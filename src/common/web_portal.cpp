#include "web_portal.h"

#include <DNSServer.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <lwip/sockets.h>

#include "core_common.h"
#include "webui_gz.h"  // generated from webui/index.html by tools/embed_webui.py

namespace ss {
namespace portal {

static constexpr uint8_t kMaxViewers = 3;
// Per-viewer backlog. A device list is ~20 KB; if a phone falls further behind than
// this, lines are dropped (the next status/devs line a second later replaces them).
static constexpr size_t kMaxBacklog = 24 * 1024;
static constexpr uint32_t kRequestTimeoutMs = 300;

struct Viewer {
  NetworkClient client;
  String backlog;
  bool used = false;
};

static NetworkServer server(80);
static DNSServer dns;
static Viewer viewers[kMaxViewers];
static CommandHandler handler = nullptr;
static bool running = false;
static IPAddress apIp;

static void refreshWebFlag() {
  bool any = false;
  for (auto& v : viewers) any = any || v.used;
  g_webOn = any;
}

static void dropViewer(Viewer& v) {
  v.client.stop();
  v.backlog = "";
  v.used = false;
  refreshWebFlag();
}

// Sends as much backlog as the socket takes right now, without blocking the radio loop.
static void flushViewer(Viewer& v) {
  if (!v.used) return;
  if (!v.client.connected()) { dropViewer(v); return; }
  if (!v.backlog.length()) return;
  const int fd = v.client.fd();
  const int n = ::send(fd, v.backlog.c_str(), v.backlog.length(), MSG_DONTWAIT);
  if (n > 0) {
    v.backlog.remove(0, n);
  } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
    dropViewer(v);
  }
}

static FileWriter logWriter = nullptr;
void setLogWriter(FileWriter w) { logWriter = w; }

void pushLine(const char* line, size_t len) {
  for (auto& v : viewers) {
    if (!v.used) continue;
    if (v.backlog.length() + len + 8 > kMaxBacklog) continue;  // behind: skip this line
    v.backlog += "data: ";
    v.backlog.concat(line, len);
    v.backlog += "\n\n";
  }
}

static void sendSimple(NetworkClient& c, const char* status, const char* type, const char* body) {
  c.printf("HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %u\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
           status, type, (unsigned)strlen(body));
  c.print(body);
}

static void redirectToUi(NetworkClient& c) {
  c.printf("HTTP/1.1 302 Found\r\nLocation: http://%s/\r\nCache-Control: no-store\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
           apIp.toString().c_str());
}

static void serveUi(NetworkClient& c) {
  c.printf("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Encoding: gzip\r\n"
           "Content-Length: %u\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n", (unsigned)kWebUiGzLen);
  size_t off = 0;
  while (off < kWebUiGzLen && c.connected()) {
    const size_t chunk = kWebUiGzLen - off > 1436 ? 1436 : kWebUiGzLen - off;
    const size_t n = c.write(kWebUiGz + off, chunk);
    if (!n) break;
    off += n;
  }
}

static void startStream(NetworkClient& c) {
  Viewer* slot = nullptr;
  for (auto& v : viewers) if (!v.used) { slot = &v; break; }
  if (!slot) { sendSimple(c, "503 Service Unavailable", "text/plain", "Too many viewers (3 max)\n"); return; }
  c.print("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-store\r\n"
          "Connection: keep-alive\r\n\r\nretry: 2000\n\n");
  c.setNoDelay(true);
  slot->client = c;
  slot->backlog = "";
  slot->used = true;
  refreshWebFlag();
}

static void handleRequest(NetworkClient& c) {
  c.setTimeout(kRequestTimeoutMs);
  const String request = c.readStringUntil('\n');  // "GET /path HTTP/1.1"
  size_t contentLength = 0;
  for (;;) {                                         // headers
    String h = c.readStringUntil('\n');
    h.trim();
    if (!h.length()) break;
    h.toLowerCase();
    if (h.startsWith("content-length:")) contentLength = (size_t)h.substring(15).toInt();
  }
  const int sp1 = request.indexOf(' ');
  const int sp2 = request.indexOf(' ', sp1 + 1);
  if (sp1 < 0 || sp2 < 0) { c.stop(); return; }
  const String method = request.substring(0, sp1);
  String path = request.substring(sp1 + 1, sp2);
  const int q = path.indexOf('?');
  if (q >= 0) path = path.substring(0, q);

  if (method == "GET" && (path == "/" || path == "/index.html")) {
    serveUi(c);
  } else if (method == "GET" && path == "/events") {
    startStream(c);
    return;  // keep the connection
  } else if (method == "POST" && path == "/cmd") {
    String body;
    while (body.length() < contentLength && body.length() < 128) {
      const int ch = c.read();
      if (ch < 0) { if (!c.available() && !c.connected()) break; delay(1); continue; }
      body += (char)ch;
    }
    body.trim();
    sendSimple(c, "204 No Content", "text/plain", "");
    c.stop();
    if (body.length() && handler) handler(body);
    return;
  } else if (method == "GET" && path == "/info") {
    sendSimple(c, "200 OK", "application/json", "{\"skysentinel\":true,\"transport\":\"sse\"}");
  } else if (method == "GET" && path == "/log.csv") {
    if (!logWriter) { sendSimple(c, "404 Not Found", "text/plain", "No log on this board\n"); c.stop(); return; }
    c.print("HTTP/1.1 200 OK\r\nContent-Type: text/csv; charset=utf-8\r\n"
            "Content-Disposition: attachment; filename=\"skysentinel-log.csv\"\r\n"
            "Cache-Control: no-store\r\nConnection: close\r\n\r\n");
    logWriter(c);
  } else if (method == "GET" && path == "/favicon.ico") {
    sendSimple(c, "404 Not Found", "text/plain", "");
  } else {
    redirectToUi(c);  // captive-portal checks and anything else
  }
  c.stop();
}

bool begin(const char* ssid, const char* password, uint8_t channel, CommandHandler onCommand) {
  handler = onCommand;
  const bool open = !password || !password[0];
  if (!WiFi.softAP(ssid, open ? nullptr : password, channel, 0, 4)) return false;
  apIp = WiFi.softAPIP();
  dns.setErrorReplyCode(DNSReplyCode::NoError);
  dns.start(53, "*", apIp);
  server.begin();
  running = true;
  Out.setSink(&pushLine);
  return true;
}

void loop() {
  if (!running) return;
  dns.processNextRequest();
  NetworkClient c = server.accept();
  if (c) handleRequest(c);
  for (auto& v : viewers) flushViewer(v);
}

bool active() { return running; }

uint8_t stationCount() { return running ? WiFi.softAPgetStationNum() : 0; }

uint8_t viewerCount() {
  uint8_t n = 0;
  for (auto& v : viewers) n += v.used;
  return n;
}

}  // namespace portal
}  // namespace ss
