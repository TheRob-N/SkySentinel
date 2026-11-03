#ifdef SS_CYD
#include "cyd_sd.h"

#include <SPI.h>
#include <diskio.h>
#include <ff.h>
#include <sd_diskio.h>   // Arduino-ESP32's SD driver, used directly (see below)
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>

#include "sd_format.h"
#include "ss_gps.h"

namespace ss {
namespace cydsd {

static constexpr int kSck = 18, kMiso = 19, kMosi = 23, kCs = 5;
static constexpr uint32_t kRetryMs = 30000;   // no card: look again this often (mounting stalls the screen briefly)
static constexpr uint32_t kFlushMs = 3000;
static constexpr size_t kMaxPending = 8192;   // card busy or missing: keep this much, then drop
static constexpr const char* kDir = "/SkySentinel";

static SPIClass sdSpi(VSPI);
static uint8_t state_ = SD_NO_CARD;
static bool mounted_ = false;
static uint32_t lastTry_ = 0, lastFlush_ = 0;
static uint32_t saved_ = 0, missed_ = 0, lastSeq_ = 0, dropped_ = 0;
static String pending_;
static char pendingFile_[40] = "";
static char file_[40] = "";
static uint32_t formatAt_ = 0;     // 0 = no format waiting
static char formatMsg_[48] = "";
static bool formatOk_ = false;
static uint32_t cardMB_ = 0;
static uint8_t pdrv_ = 0xFF;

// Not <SD.h>: the Adafruit display libraries pull Arduino's AVR "SD" library into
// .pio/libdeps, and PlatformIO picks that one for <SD.h> (it doesn't build for the
// ESP32). <sd_diskio.h> only exists in Arduino-ESP32's own SD library, so including it
// gets the right one. Files are then plain C files under /sd.
static constexpr const char* kMount = "/sd";

static void closeRemote();
static void unmount() {
  closeRemote();
  if (pdrv_ != 0xFF) {
    sdcard_unmount(pdrv_);
    sdcard_uninit(pdrv_);
    pdrv_ = 0xFF;
  }
  mounted_ = false;
}

// Free space in MB (0 if unknown)
static uint32_t freeMB() {
  if (pdrv_ == 0xFF) return 0;
  FATFS* fs = nullptr;
  DWORD clusters = 0;
  char drv[3] = {(char)('0' + pdrv_), ':', 0};
  if (f_getfree(drv, &clusters, &fs) != FR_OK || !fs) return 0;
  return (uint32_t)((uint64_t)clusters * fs->csize * sdcard_sector_size(pdrv_) / (1024ULL * 1024ULL));
}

static bool mount() {
  unmount();
  static uint8_t lastFail = 0;   // print each kind of failure once, not every 30 s
  pdrv_ = sdcard_init(kCs, &sdSpi, 10000000);
  if (pdrv_ == 0xFF) {
    if (lastFail != 1) Serial.println("[sd] SD driver didn't start");
    lastFail = 1;
    state_ = SD_NO_CARD; cardMB_ = 0; return false;
  }
  // The card is only talked to when mounting (sdcard_type() says CARD_NONE until then).
  if (!sdcard_mount(pdrv_, kMount, 3, false)) {
    if (sdcard_type(pdrv_) == CARD_NONE) {   // it never answered
      if (lastFail != 2) Serial.println("[sd] no card answers (pins 18/19/23/5): none inserted, or not seated");
      lastFail = 2;
      sdcard_uninit(pdrv_); pdrv_ = 0xFF;
      state_ = SD_NO_CARD; cardMB_ = 0; return false;
    }
    LBA_t sectors = 0;
    cardMB_ = disk_ioctl(pdrv_, GET_SECTOR_COUNT, &sectors) == RES_OK
                  ? (uint32_t)((uint64_t)sectors * sdcard_sector_size(pdrv_) / (1024ULL * 1024ULL)) : 0;
    // The card is there but has no FAT file system: cards over 32 GB come as exFAT,
    // which this can't read. FORMAT fixes it.
    if (lastFail != 3) Serial.printf("[sd] %lu MB card found, but it isn't FAT32 (exFAT?): format it on the CYD (SETTINGS > SD) or a computer\n", (unsigned long)cardMB_);
    lastFail = 3;
    sdcard_uninit(pdrv_); pdrv_ = 0xFF;
    state_ = SD_UNFORMATTED;
    return false;
  }
  lastFail = 0;
  cardMB_ = (uint32_t)((uint64_t)sdcard_num_sectors(pdrv_) * sdcard_sector_size(pdrv_) / (1024ULL * 1024ULL));
  mounted_ = true;
  char dir[24];
  snprintf(dir, sizeof(dir), "%s%s", kMount, kDir);
  struct stat st;
  if (stat(dir, &st) != 0) mkdir(dir, 0777);
  state_ = SD_LOGGING;
  Serial.printf("[sd] card mounted: %lu MB, %lu MB free\n", (unsigned long)cardMB_, (unsigned long)freeMB());
  return true;
}

static void pathFor(const LogRec& r, char* out, size_t cap) {
  if (r.utc) {
    char t[20];
    formatUtc(r.utc, t);
    snprintf(out, cap, "%s/%.10s.csv", kDir, t);   // the date part
  } else {
    snprintf(out, cap, "%s/undated.csv", kDir);
  }
}

static void flush() {
  if (!pending_.length() || !pendingFile_[0]) return;
  if (!mounted_ && !mount()) return;  // keep it for when a card shows up
  char path[48];
  snprintf(path, sizeof(path), "%s%s", kMount, pendingFile_);
  struct stat st;
  const bool fresh = stat(path, &st) != 0;
  FILE* f = fopen(path, "a");
  if (!f) {
    state_ = SD_ERROR;
    unmount();   // pulled out? mount again next time
    return;
  }
  bool ok = true;
  if (fresh) ok = fwrite(kLogCsvHeader, 1, strlen(kLogCsvHeader), f) == strlen(kLogCsvHeader);
  ok = ok && fwrite(pending_.c_str(), 1, pending_.length(), f) == pending_.length();
  ok = (fclose(f) == 0) && ok;
  if (!ok) {
    state_ = freeMB() == 0 ? SD_FULL : SD_ERROR;
    unmount();
    return;
  }
  state_ = SD_LOGGING;
  strncpy(file_, pendingFile_, sizeof(file_) - 1);
  pending_ = "";
}

void begin() {
  pinMode(kCs, OUTPUT);
  digitalWrite(kCs, HIGH);
  sdSpi.begin(kSck, kMiso, kMosi, kCs);
  if (!mount()) Serial.println("[sd] no card (logging starts when one is put in)");
  lastTry_ = millis();
}

void onRecord(const LogRec& r) {
  if (r.seq == lastSeq_) return;  // a repeat
  if (lastSeq_ && r.seq > lastSeq_ + 1 && r.seq - lastSeq_ < 100000) missed_ += r.seq - lastSeq_ - 1;
  lastSeq_ = r.seq;                // (lower than before: the master restarted)
  char path[40];
  pathFor(r, path, sizeof(path));
  if (strcmp(path, pendingFile_) != 0) {  // new day (or the clock just arrived): write out the old one first
    flush();
    if (pending_.length()) { ++dropped_; pending_ = ""; }
    strncpy(pendingFile_, path, sizeof(pendingFile_) - 1);
  }
  char line[200];
  const size_t n = formatLogCsv(r, line, sizeof(line));
  if (!n) return;
  if (pending_.length() + n > kMaxPending) { ++dropped_; return; }
  pending_ += line;
  ++saved_;
  // Alerts go to the card right away; the rest every few seconds.
  if (r.kind != LK_DEVICE && r.kind != LK_TRACK) flush();
}

static void runFormat() {
  formatAt_ = 0;
  Serial.println("[sd] formatting the card (erases everything on it)...");
  pending_ = "";        // what was waiting would have gone to the old file system
  pendingFile_[0] = '\0';
  unmount();
  const uint32_t t0 = millis();
  formatOk_ = sdFormatCard(kCs, sdSpi, 10000000, formatMsg_, sizeof(formatMsg_));
  Serial.printf("[sd] %s (%lu s)\n", formatMsg_, (unsigned long)((millis() - t0) / 1000));
  file_[0] = '\0';
  mount();
  lastTry_ = millis();
}

void requestFormat() {
  formatAt_ = millis() + 500;
  if (!formatAt_) formatAt_ = 1;
  state_ = SD_FORMATTING;
}
bool formatPending() { return formatAt_ != 0; }
const char* formatResult() { return formatMsg_; }
bool formatOk() { return formatOk_; }
uint32_t cardMB() { return cardMB_; }

// ---------------------------------------------------------------------------
// Reading files back over USB (the web page's SD CARD > FILES), a few lines per loop
// pass so the radio link and the screen keep running during a long download.

static FILE* dumpF_ = nullptr;
static uint32_t dumpLines_ = 0;

static void listFiles() {
  Serial.print("{\"t\":\"sd_ls\",\"files\":[");
  char dir[24];
  snprintf(dir, sizeof(dir), "%s%s", kMount, kDir);
  DIR* d = mounted_ ? opendir(dir) : nullptr;
  bool first = true;
  if (d) {
    while (dirent* e = readdir(d)) {
      if (e->d_name[0] == '.') continue;
      char path[300];
      snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
      struct stat st;
      if (stat(path, &st) != 0 || S_ISDIR(st.st_mode)) continue;
      Serial.printf("%s{\"n\":\"%s\",\"b\":%lu}", first ? "" : ",", e->d_name, (unsigned long)st.st_size);
      first = false;
    }
    closedir(d);
  }
  Serial.printf("],\"mounted\":%s}\n", mounted_ ? "true" : "false");
}

static void startDump(const String& name) {
  if (dumpF_) { fclose(dumpF_); dumpF_ = nullptr; }
  if (!mounted_ || name.indexOf('/') >= 0 || name.indexOf("..") >= 0 || !name.length()) {
    Serial.println("{\"t\":\"log_end\",\"lines\":0,\"ok\":false}");
    return;
  }
  flush();   // what's waiting goes in first
  char path[300];
  snprintf(path, sizeof(path), "%s%s/%s", kMount, kDir, name.c_str());
  dumpF_ = fopen(path, "r");
  dumpLines_ = 0;
  Serial.printf("{\"t\":\"log_begin\",\"name\":\"%s\"}\n", name.c_str());
  if (!dumpF_) Serial.println("{\"t\":\"log_end\",\"lines\":0,\"ok\":false}");
}

static void pumpDump() {
  if (!dumpF_) return;
  char line[256], out[540];
  for (int n = 0; n < 8; ++n) {
    if (Serial.availableForWrite() < (int)sizeof(out)) return;   // USB busy: next pass
    if (!fgets(line, sizeof(line), dumpF_)) {
      fclose(dumpF_);
      dumpF_ = nullptr;
      Serial.printf("{\"t\":\"log_end\",\"lines\":%lu,\"ok\":true}\n", (unsigned long)dumpLines_);
      return;
    }
    size_t o = 0;
    o += snprintf(out, sizeof(out), "{\"t\":\"log\",\"l\":\"");
    for (const char* c = line; *c && o < sizeof(out) - 6; ++c) {
      if (*c == '\n' || *c == '\r') continue;
      if (*c == '"' || *c == '\\') out[o++] = '\\';
      if ((uint8_t)*c >= 0x20) out[o++] = *c;
    }
    memcpy(out + o, "\"}\n", 3);
    Serial.write((const uint8_t*)out, o + 3);
    ++dumpLines_;
  }
}

// Remote downloads: the master asks for a file a few chunks at a time. One file stays
// open between requests and closes after a few seconds of quiet.
static FILE* remoteF_ = nullptr;
static char remoteName_[kMaxFileName + 1] = "";
static uint32_t remoteAt_ = 0;
static void closeRemote() {
  if (remoteF_) fclose(remoteF_);
  remoteF_ = nullptr;
  remoteName_[0] = 0;
}

namespace {
class SdFiles : public FileSource {
 public:
  bool ready() override {
    if (formatAt_) return false;
    if (!mounted_ && millis() - lastTry_ > 5000) { lastTry_ = millis(); mount(); }
    return mounted_;
  }
  void forEach(void (*fn)(const char*, uint32_t, void*), void* ctx) override {
    flush();   // what's waiting goes in first, so sizes are current
    char dir[24];
    snprintf(dir, sizeof(dir), "%s%s", kMount, kDir);
    DIR* d = mounted_ ? opendir(dir) : nullptr;
    if (!d) return;
    while (dirent* e = readdir(d)) {
      if (e->d_name[0] == '.') continue;
      char path[300];
      snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
      struct stat st;
      if (stat(path, &st) != 0 || S_ISDIR(st.st_mode)) continue;
      fn(e->d_name, (uint32_t)st.st_size, ctx);
    }
    closedir(d);
  }
  int32_t readAt(const char* name, uint32_t off, uint8_t* buf, size_t len, uint32_t* size) override {
    if (!mounted_ || !fileNameOk(name)) return -1;
    remoteAt_ = millis();
    if (!remoteF_ || strcmp(remoteName_, name) != 0) {
      closeRemote();
      flush();
      char path[80];
      snprintf(path, sizeof(path), "%s%s/%s", kMount, kDir, name);
      remoteF_ = fopen(path, "r");
      if (!remoteF_) return -1;
      strncpy(remoteName_, name, sizeof(remoteName_) - 1);
    }
    // Today's file keeps growing while it's read: the size is taken fresh each time.
    if (fseek(remoteF_, 0, SEEK_END) != 0) { closeRemote(); return -1; }
    const long sz = ftell(remoteF_);
    *size = sz > 0 ? (uint32_t)sz : 0;
    if (off >= *size) return 0;
    if (fseek(remoteF_, (long)off, SEEK_SET) != 0) { closeRemote(); return -1; }
    const size_t got = fread(buf, 1, len, remoteF_);
    if (!got && ferror(remoteF_)) { closeRemote(); return -1; }
    return (int32_t)got;
  }
};
SdFiles sdFiles_;
}  // namespace

FileSource& files() { return sdFiles_; }

void loop(uint32_t now) {
  pumpDump();
  if (remoteF_ && now - remoteAt_ > 5000) closeRemote();
  if (formatAt_ && (int32_t)(now - formatAt_) >= 0) { runFormat(); return; }
  if (formatAt_) return;
  if (!mounted_ && now - lastTry_ > kRetryMs) {
    lastTry_ = now;
    mount();
  }
  if (pending_.length() && now - lastFlush_ > kFlushMs) {
    lastFlush_ = now;
    flush();
  }
}

uint8_t state() { return formatAt_ ? SD_FORMATTING : state_; }
uint32_t saved() { return saved_; }
uint32_t missed() { return missed_; }
const char* fileName() { return file_; }

bool handleCommand(const String& line) {
  if (line == "sd format") {
    Serial.println("[sd] this ERASES EVERYTHING on the SD card, logs included. Type \"sd format yes\" to go ahead.");
    return true;
  }
  if (line == "sd format yes") {
    requestFormat();
    Serial.println("{\"t\":\"ack\",\"cmd\":\"sd format\"}");
    return true;
  }
  if (line == "sd ls") { listFiles(); return true; }
  if (line.startsWith("sd get ")) { String n = line.substring(7); n.trim(); startDump(n); return true; }
  if (line != "sd" && line != "sd status") return false;
  Serial.printf("[sd] %s, %lu records written since power on, %lu missed on the radio, %lu dropped, file %s\n",
                sdStateName(state_), (unsigned long)saved_, (unsigned long)missed_, (unsigned long)dropped_,
                file_[0] ? file_ : "(none yet)");
  if (mounted_) Serial.printf("[sd] card %lu MB, %lu MB free\n", (unsigned long)cardMB_, (unsigned long)freeMB());
  return true;
}

}  // namespace cydsd
}  // namespace ss
#endif  // SS_CYD
