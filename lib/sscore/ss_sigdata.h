// Signature data tables. Facts only (OUIs, UUIDs, company IDs, name patterns);
// matching logic lives in ss_classify.cpp.
//
// Flock / Raven data is carried over from Rob Neyhard's SkySentinel flock.h,
// with its original research credits:
//   Wi-Fi OUI list (31):  OrdoOuroborous/@NitekryDPaul (30) + Michael/DeFlockJoplin (82:6b:f2)
//   BLE OUI list (32):    colonelpanichacks/ouispy-detector (@NitekryDPaul + @konradit)
//   Raven UUIDs:          GainSec (raven_configurations.json), fw 1.1.7, 1.2.0, 1.3.1
//   BLE mfgr ID 0x09C8:   Will Greenberg/@wgreenberg
//   BLE names:            colonelpanichacks/flock-you
//   SSID patterns:        VirtuallyScott/flock-you + nyanBOX
//   Flock BLE UUID, Flipper ids, camera makers, Meta FEB7/FEB8, Find My FD44:
//                         valleytechsolutions/Dragon-Detector (identifiers only; camera
//                         blocks re-checked against the IEEE registry)
//   Skimmer module blocks, HC-03, newer AirPods ids, Apple hotspot / AirPlay messages:
//                         7h30th3r0n3/Evil-M5Project (MIT; identifiers only)
//   Hacking-firmware default names: read from the Evil-M5Project, Bruce, GhostESP and
//                         ESP32 Marauder sources; Chameleon names from RfidResearchGroup
//
// Changes from flock.h:
//  * 00:25:DF moved out of the Flock BLE list into Axon (it is Axon/TASER's OUI).
//  * A4:CF:12 and 3C:71:BF are Espressif chip OUIs (the same chips SkySentinel
//    runs on). Kept, but scored as "generic vendor" so they never trigger an
//    alert alone. TODO: run both OUI lists against the IEEE registry and tag
//    every other generic chip vendor the same way.
//  * Oct 2026, checked against IEEE: nearly every Wi-Fi OUI here is Liteon (24 of 33),
//    the rest Silicon Labs, UGSI, Samsung (48:27:EA, generic), Espressif, Raspberry Pi.
//    Flock uses those modules, but so do countless laptops and TVs, so an OUI match only
//    counts from an access point (Flock cameras run one) and never alerts on its own.
//  * OUI rules only apply to globally-unique addresses (public BLE, non-LAA
//    Wi-Fi). The first bytes of a random address are random, so matching them
//    against an OUI list produces false hits.
#pragma once

#include <stdint.h>

namespace ss {
namespace sigdata {

struct Oui { uint8_t b[3]; bool generic; };

static const Oui kFlockWifiOuis[] = {
  {{0x70, 0xC9, 0x4E}, false}, {{0x3C, 0x91, 0x80}, false}, {{0xD8, 0xF3, 0xBC}, false},
  {{0x80, 0x30, 0x49}, false}, {{0xB8, 0x35, 0x32}, false}, {{0x14, 0x5A, 0xFC}, false},
  {{0x74, 0x4C, 0xA1}, false}, {{0x08, 0x3A, 0x88}, false}, {{0x9C, 0x2F, 0x9D}, false},
  {{0xC0, 0x35, 0x32}, false}, {{0x94, 0x08, 0x53}, false}, {{0xE4, 0xAA, 0xEA}, false},
  {{0xF4, 0x6A, 0xDD}, false}, {{0xF8, 0xA2, 0xD6}, false}, {{0x24, 0xB2, 0xB9}, false},
  {{0x00, 0xF4, 0x8D}, false}, {{0xD0, 0x39, 0x57}, false}, {{0xE8, 0xD0, 0xFC}, false},
  {{0xE0, 0x4F, 0x43}, false}, {{0xB8, 0x1E, 0xA4}, false}, {{0x70, 0x08, 0x94}, false},
  {{0x58, 0x8E, 0x81}, false}, {{0xEC, 0x1B, 0xBD}, false}, {{0x3C, 0x71, 0xBF}, true},
  {{0x58, 0x00, 0xE3}, false}, {{0x90, 0x35, 0xEA}, false}, {{0x5C, 0x93, 0xA2}, false},
  {{0x64, 0x6E, 0x69}, false}, {{0x48, 0x27, 0xEA}, true}, {{0xA4, 0xCF, 0x12}, true},
  {{0x82, 0x6B, 0xF2}, false},  // DeFlockJoplin
  {{0xE0, 0x0A, 0xF6}, false},  // Liteon, 8 Flock- APs in flock-you's 2024 WiGLE export
  {{0xB8, 0x27, 0xEB}, true},   // Raspberry Pi: 25 Flock- APs there, but every Pi has it
};

// ouispy-detector's BLE list, checked against the IEEE registry (Oct 2026). Most of it
// was not Flock: Ring, DJI, Parrot, Skydio, Luxottica (Ray-Ban Meta), Apple and HPE
// entries were moved to kOuiSigs below under their real category, and Apple / HPE
// dropped. What's left is either unregistered in the public lists or Flock's own.
static const Oui kFlockBleOuis[] = {
  {{0x24, 0x2B, 0xD6}, false}, {{0xAC, 0x9F, 0xC3}, false}, {{0xC4, 0xDB, 0xAD}, false},
  {{0xCC, 0x3B, 0xFB}, false}, {{0x0C, 0x9A, 0xE6}, false}, {{0x8C, 0x58, 0x23}, false},
  {{0x04, 0xA8, 0x5A}, false}, {{0x7C, 0x2A, 0x9E}, false},
};

static const char* const kFlockBleNames[] = {"FS Ext Battery", "Penguin", "Pigvision", "Flock"};
static const char* const kFlockSsidPrefixes[] = {"FLOCK-S3-", "FLOCK-"};
static const uint16_t kFlockBleCompany = 0x09C8;  // XUNTONG

// Raven version-specific 16-bit service UUIDs (0x3100..0x3500, exact values only).
// The standard SIG UUIDs Raven also uses (0x180A, 0x1809, 0x1819) are deliberately
// absent: they appear on huge numbers of consumer devices.
static const uint16_t kRavenUuids[] = {0x3100, 0x3200, 0x3300, 0x3400, 0x3500};

// Pentest gear
static const uint8_t kHak5Oui[3] = {0x00, 0x13, 0x37};                 // Wi-Fi Pineapple default
static const uint8_t kPwnagotchiMac[6] = {0xDE, 0xAD, 0xBE, 0xEF, 0xDE, 0xAD};
// Flipper Zero. Its BLE advert carries a service UUID by case color (0x3080..0x3083),
// Flipper Devices' company id, and its own appearance value; its public address is in
// Flipper Devices' block. The name ("Flipper <name>") is the user's to change.
static const uint8_t kFlipperOui[3] = {0x0C, 0xFA, 0x22};
static const uint16_t kCoFlipper = 0x0E29;            // Flipper Devices Inc.
static const uint16_t kFlipperUuidFirst = 0x3080, kFlipperUuidLast = 0x3083;
static const uint16_t kFlipperAppearance = 0x8600;

// ESP32 hacking firmwares by the names they broadcast out of the box, read from their
// source (Nov 2026): Evil-M5Project (device and portal names), Bruce (its web AP, karma,
// shell and sniffer APs, its BLE name), GhostESP (its AP and BLE bridge), ESP32 Marauder
// (its evil portal's default network). exact: the whole name, else a prefix. A user can
// rename all of these, so a miss means nothing.
struct ToolName { const char* s; bool exact; uint8_t conf; };
static const ToolName kAttackFwBleNames[] = {
  {"Evil-Cardputer", false, 85}, {"Evil-M5", false, 85}, {"Evil-StickC", false, 85},
  {"Evil-AtomS3", false, 85}, {"Evil-CYD", false, 85}, {"Evil-Face", false, 85},
  {"Bruce", true, 75}, {"BruceSniffer", true, 85}, {"BRUCE-PN532", false, 85},
  {"GhostESP", false, 85},
};
static const ToolName kAttackFwSsids[] = {
  {"Evil-Cardputer", false, 85}, {"Evil-M5", false, 85}, {"Evil-StickC", false, 85},
  {"Evil-AtomS3", false, 85}, {"Evil-CYD", false, 85}, {"Evil-Face", false, 85},
  {"EvilRelay", true, 85}, {"EvilChatMesh", false, 85},
  {"BruceNet", true, 85}, {"BruceKarma", true, 90}, {"BruceShell", true, 90},
  {"BruceSniffer", true, 90}, {"BruceAttack", true, 90}, {"BRUCE-UDP", true, 90}, {"BruceBeacon", false, 80},
  {"GhostNet", true, 80}, {"GhostESP", false, 85},
  {"PORTAL", true, 45},   // Marauder's evil portal default: weak, it's a plain word
};
// Chameleon Ultra / Lite (RFID and NFC card cloners) advertise their model name
// (RfidResearchGroup firmware, common/device_info.h).
static const char* const kChameleonNames[] = {"ChameleonUltra", "ChameleonLite"};

// Flock: a 128-bit service UUID seen on Flock BLE accessories,
// e8ccbb38-9532-46a8-9fe5-1814df172e6f, as it sits in an advert (byte-reversed).
static const uint8_t kFlockBleUuid128[16] = {0x6F, 0x2E, 0x17, 0xDF, 0x14, 0x18, 0xE5, 0x9F,
                                             0xA8, 0x46, 0x32, 0x95, 0x38, 0xBB, 0xCC, 0xE8};

// Camera makers by IEEE address block (registry, Nov 2026). A camera maker's block says
// "a camera, probably", not "surveillance": neighbors' doorbells and NVRs land here too,
// so these are weak and never alert alone. Genetec makes AutoVu ALPR; only "Genetec Inc."
// blocks are listed (00:0A:B1 is GENETEC Corporation, an unrelated Japanese firm).
// Avigilon has no block of its own (Motorola Solutions, already listed as body cams).
enum CamMaker : uint8_t { CAM_HIKVISION, CAM_VERKADA, CAM_WYZE, CAM_ARLO, CAM_AXIS, CAM_BLINK, CAM_GENETEC };
struct CamOui { uint32_t oui; uint8_t maker; };
static const CamOui kCameraOuis[] = {   // sorted by OUI
  {0x00408C, CAM_AXIS}, {0x00BF15, CAM_GENETEC}, {0x040312, CAM_HIKVISION},
  {0x085411, CAM_HIKVISION}, {0x08A189, CAM_HIKVISION}, {0x08CC81, CAM_HIKVISION}, {0x0C75D2, CAM_HIKVISION},
  {0x0CBF15, CAM_GENETEC}, {0x1012FB, CAM_HIKVISION}, {0x1868CB, CAM_HIKVISION}, {0x188025, CAM_HIKVISION},
  {0x240F9B, CAM_HIKVISION}, {0x2428FD, CAM_HIKVISION}, {0x2432AE, CAM_HIKVISION}, {0x244845, CAM_HIKVISION},
  {0x24B105, CAM_HIKVISION}, {0x2857BE, CAM_HIKVISION}, {0x2CA59C, CAM_HIKVISION}, {0x2CAA8E, CAM_WYZE},
  {0x340962, CAM_HIKVISION}, {0x3C1BF8, CAM_HIKVISION}, {0x40ACBF, CAM_HIKVISION}, {0x4419B6, CAM_HIKVISION},
  {0x4447CC, CAM_HIKVISION}, {0x44A642, CAM_HIKVISION}, {0x4C62DF, CAM_HIKVISION}, {0x4CBD8F, CAM_HIKVISION},
  {0x4CF5DC, CAM_HIKVISION}, {0x50E538, CAM_HIKVISION}, {0x548C81, CAM_HIKVISION}, {0x54C415, CAM_HIKVISION},
  {0x5803FB, CAM_HIKVISION}, {0x5850ED, CAM_HIKVISION}, {0x5C345B, CAM_HIKVISION}, {0x64DB8B, CAM_HIKVISION},
  {0x686DBC, CAM_HIKVISION}, {0x743FC2, CAM_HIKVISION}, {0x74AB93, CAM_BLINK}, {0x7C78B2, CAM_WYZE},
  {0x80482C, CAM_WYZE}, {0x807C62, CAM_HIKVISION}, {0x80BEAF, CAM_HIKVISION}, {0x80F5AE, CAM_HIKVISION},
  {0x849A40, CAM_HIKVISION}, {0x8CE748, CAM_HIKVISION}, {0x94E1AC, CAM_HIKVISION}, {0x988B0A, CAM_HIKVISION},
  {0x989DE5, CAM_HIKVISION}, {0x98DF82, CAM_HIKVISION}, {0x98F112, CAM_HIKVISION}, {0xA0FF0C, CAM_HIKVISION},
  {0xA41162, CAM_ARLO}, {0xA41437, CAM_HIKVISION}, {0xA4D5C2, CAM_HIKVISION}, {0xACB92F, CAM_HIKVISION},
  {0xACCB51, CAM_HIKVISION}, {0xACCC8E, CAM_AXIS}, {0xB4A382, CAM_HIKVISION}, {0xB8A44F, CAM_AXIS},
  {0xBC5E33, CAM_HIKVISION}, {0xBC9B5E, CAM_HIKVISION}, {0xBCAD28, CAM_HIKVISION}, {0xBCBAC2, CAM_HIKVISION},
  {0xC0517E, CAM_HIKVISION}, {0xC056E3, CAM_HIKVISION}, {0xC06DED, CAM_HIKVISION}, {0xC42F90, CAM_HIKVISION},
  {0xD03F27, CAM_WYZE}, {0xD4E853, CAM_HIKVISION}, {0xDC07F8, CAM_HIKVISION}, {0xDCD26A, CAM_HIKVISION},
  {0xE0A700, CAM_VERKADA}, {0xE0BAAD, CAM_HIKVISION}, {0xE0CA3C, CAM_HIKVISION}, {0xE0DF13, CAM_HIKVISION},
  {0xE82725, CAM_AXIS}, {0xE8A0ED, CAM_HIKVISION}, {0xECA971, CAM_HIKVISION}, {0xECC89C, CAM_HIKVISION},
  {0xF84DFC, CAM_HIKVISION}, {0xFC9C98, CAM_ARLO}, {0xFC9FFD, CAM_HIKVISION},
};


// Drones: ASTM F3411 Remote ID
static const uint16_t kRemoteIdBleUuid = 0xFFFA;
static const uint32_t kRemoteIdWifiVendor = (0xFA0BBCu << 8) | 0x0D;   // OUI FA:0B:BC, type 0x0D

// Trackers
static const uint16_t kSmartTagUuid = 0xFD5A;
static const uint16_t kTileUuids[] = {0xFEED, 0xFEEC, 0xFD84};
// Chipolo ONE / CARD (app models). Spot models use Apple Find My, POINT uses Google Find Hub.
static const uint16_t kChipoloUuids[] = {0xFE33, 0xFE65};
static const uint16_t kCoChipolo = 0x08C3;
static const uint16_t kEddystoneUuid = 0xFEAA;  // Google Find Hub (FMDN) frames 0x40 / 0x41
static const uint16_t kDultUuid = 0xFCB2;       // cross-vendor unwanted-tracking spec
static const uint16_t kFindMyAccessoryUuid = 0xFD44;   // Apple: Find My network accessories
// Wi-Fi Alliance P2P (Wi-Fi Direct) vendor element: OUI 50:6F:9A, type 9
static const uint32_t kWfaP2pVendor = 0x506F9A09u;

// More Bluetooth makers (2.1.6). Company ids from the Bluetooth SIG list; GoPro's two
// service UUIDs (0xFEA5, 0xFEA6) from its member UUID list.
static const uint16_t kCoGoPro = 0x02F2;
static const uint16_t kGoProUuids[] = {0xFEA5, 0xFEA6};
static const uint16_t kCameraMakersBle[] = {
  0x0870,  // Wyze Labs
  0x03D5,  // Wyzelink Systems
  0x0C19,  // Arlo Technologies
};
static const uint16_t kCarMakersBle[] = {   // phone-as-key and app links in cars
  0x0068,  // General Motors
  0x011F,  // Volkswagen
  0x017C,  // Mercedes-Benz
  0x05EB,  // BMW
  0x0723,  // Ford
  0x0826,  // Hyundai
  0x0915,  // Honda
  0x0941,  // Rivian
  0x0977,  // Toyota
  0x0A10,  // Subaru
  0x0BA6,  // Nissan
  0x10BC,  // Harley-Davidson
  0x0501,  // Polaris
};
static const uint16_t kEbikeScooterMakers[] = {
  0x0F1F,  // Ninebot (Segway-Ninebot scooters)
  0x0A4F,  // VanMoof
  0x044A,  // Shimano (e-bike drive units, Di2)
};
static const uint16_t kCoAmazon = 0x0171;   // Amazon.com Services

// Audio / wearables
static const uint16_t kFastPairUuid = 0xFE2C;
static const uint16_t kHeartRateUuid = 0x180D;

// Bluetooth SIG company identifiers
static const uint16_t kCoMicrosoft = 0x0006;
static const uint8_t kSwiftPairBeacon = 0x03;  // Microsoft beacon id for Swift Pair

// Makers whose BLE gear is (almost) all audio or all wearables. Bluetooth SIG company
// identifiers; a device sending one of these in its maker data is filed accordingly.
static const uint16_t kAudioMakers[] = {
  0x00CC,  // Beats Electronics
  0x0067,  // GN Hearing (ReSound hearing aids)
  0x0494,  // Sennheiser
  0x07C9,  // Skullcandy
  0x0CE9,  // JLab Audio
  0x10AE,  // Raycon
  0x0055,  // Plantronics (Poly headsets)
  0x0089,  // GN Hearing A/S (ReSound hearing aids)
  0x05A7,  // Sonos
};
static const uint16_t kWearableMakers[] = {
  0x009F,  // Suunto
  0x00D1,  // Polar Electro Europe
  0x0157,  // Anhui Huami (Amazfit / Zepp)
  0x01FC,  // Wahoo Fitness
  0x02B2,  // Oura Health Oy
  0x0304,  // Oura Health Ltd
  0x03FF,  // Withings
};

// GAP appearance categories (Bluetooth Assigned Numbers 2.6; value >> 6)
static const uint8_t kAppearPhone = 0x01, kAppearComputer = 0x02, kAppearWatch = 0x03,
                     kAppearHeartRate = 0x0D, kAppearHid = 0x0F, kAppearRunWalk = 0x11,
                     kAppearCycling = 0x12, kAppearPulseOx = 0x31, kAppearAudioSink = 0x21,
                     kAppearWearableAudio = 0x25, kAppearHearingAid = 0x29;

// LE Audio services: Audio Stream Control, Broadcast Audio Scan, Published Audio
// Capabilities, Common Audio, Hearing Access, Telephony and Media Audio
static const uint16_t kLeAudioUuids[] = {0x184E, 0x184F, 0x1850, 0x1853, 0x1854, 0x1855};

// AirPods / Beats model numbers in Apple's proximity-pairing advert. Community
// documented (the same table BLE-spam tools use to imitate them), not from Apple.
struct AppleModel { uint16_t id; const char* name; };
static const AppleModel kAppleAudioModels[] = {
  {0x0220, "AirPods"}, {0x0F20, "AirPods (2nd gen)"}, {0x1320, "AirPods (3rd gen)"},
  {0x0E20, "AirPods Pro"}, {0x1420, "AirPods Pro (2nd gen)"}, {0x0A20, "AirPods Max"},
  {0x1920, "AirPods 4"}, {0x1B20, "AirPods 4 (ANC)"}, {0x2420, "AirPods Pro (2nd gen, USB-C)"},
  {0x2720, "AirPods Pro 3"}, {0x1F20, "AirPods Max (USB-C)"},
  {0x0320, "Powerbeats3"}, {0x0B20, "Powerbeats Pro"}, {0x0520, "BeatsX"}, {0x1020, "Beats Flex"},
  {0x0620, "Beats Solo3"}, {0x0C20, "Beats Solo Pro"}, {0x0920, "Beats Studio3"},
  {0x1720, "Beats Studio Pro"}, {0x1120, "Beats Studio Buds"}, {0x1620, "Beats Studio Buds+"},
  {0x1220, "Beats Fit Pro"},
};
static const uint16_t kCoApple = 0x004C;
static const uint16_t kCoHarman = 0x0057;
static const uint16_t kCoPolar = 0x006B;
static const uint16_t kCoSamsung = 0x0075;
static const uint16_t kCoGarmin = 0x0087;
static const uint16_t kCoBose = 0x009E;
static const uint16_t kCoGoogle = 0x00E0;
static const uint16_t kCoSony = 0x012D;

// Apple Continuity message types (first byte of Apple manufacturer data)
static const uint8_t kAppleProximityPairing = 0x07;  // AirPods / Beats
static const uint8_t kAppleHandoff = 0x0C;
static const uint8_t kAppleNearbyAction = 0x0F;
static const uint8_t kAppleNearbyInfo = 0x10;
static const uint8_t kAppleFindMy = 0x12;            // offline finding
static const uint8_t kAppleAirPlayTarget = 0x09;     // Apple TV, HomePod, AirPlay speakers
static const uint8_t kAppleTetheringSource = 0x0E;   // an iPhone / iPad offering Instant Hotspot

// Cheap serial-over-BLE modules seen in some card skimmers (names are their defaults).
// Low confidence on their own: hobby projects use the same modules.
static const char* const kSerialModuleNames[] = {"HC-08", "BT05", "MLT-BT05", "JDY-", "HMSoft"};
// Classic serial (SPP) modules, default names. HC-05 / HC-06 are what most card
// skimmers found in gas pumps use.
static const char* const kClassicSerialNames[] = {"HC-05", "HC-06", "HC-03", "linvor", "RNBT-", "BT04", "JDY-30", "JDY-31", "SPP-CA"};
// Address blocks those modules ship with (as listed by Evil-M5Project's skimmer
// detector): 00:18:E4 (YIGUANG), 20:16:04 (unregistered, HC-06 clones), 00:11:22.
// Only raise a name match's confidence: hobby boards carry them too.
static const uint8_t kSerialModuleOuis[][3] = {{0x00, 0x18, 0xE4}, {0x20, 0x16, 0x04}, {0x00, 0x11, 0x22}};

// ---------------------------------------------------------------------------
// Vendor signatures (body cams, glasses, drones, ALPR). Sources:
//   OUIs: IEEE MA-L registry (via the netaddr and bluetooth-numbers packages).
//   Company IDs and 16-bit service UUIDs: Bluetooth SIG Assigned Numbers.
//   Glasses company IDs: yjeanrenaud's "Nearby Glasses" app (0x01AB, 0x058E, 0x0D53, 0x03C2).
//   Ray-Ban Meta service UUID 0xFD5F: Eye Spy (simeononsecurity).
//   DJI DroneID Wi-Fi vendor IE (OUI 26:37:12): "DJI drone IDs are not encrypted"
//   (Schiller et al.), dji-joe (hugsy), CYM wiki.
// Checked and NOT used: D4:BB:E6 (Huawei), 3C:61:05 (Espressif) and 00:0E:58 (Sonos),
// published elsewhere as Flock / Vigilant OUIs; 00:01:21 and 00:90:7F (WatchGuard
// Technologies, the firewall company, not WatchGuard Video).

// One OUI -> what it means. Applied to public BLE addresses and non-LAA Wi-Fi only.
struct OuiSig { uint8_t b[3]; uint8_t cat; uint16_t sig; uint8_t conf; bool strong; };

// Bluetooth company identifier or 16-bit service UUID -> what it means.
struct IdSig { uint16_t id; uint8_t cat; uint16_t sig; uint8_t conf; bool strong; };

// The tables use category numbers from ss_proto.h and SigIds from ss_classify.h, so
// they live in ss_classify.cpp (kOuiSigs, kCompanySigs, kUuidSigs).

// DJI's proprietary DroneID / flight-info vendor IE in drone beacons.
static const uint32_t kDjiDroneIdOui = 0x263712;

// Drone hotspot SSIDs (consumer drones that fly over their own Wi-Fi), case-insensitive.
static const char* const kDroneSsidPrefixes[] = {"TELLO-", "DJI-", "Mavic", "Anafi", "Bebop", "Skydio"};

// Vehicles. Car makers' built-in hotspots and head units name themselves after the
// brand (GM: "myChevrolet 1234"; Audi: "Audi_MMI_1234"; Mercedes: "MB Hotspot 12345";
// VW group: "VW WLAN 1234"); OBD-II dongles use a handful of stock names. Matched at
// the start, case-insensitive; short brand words need the separator to avoid
// "Kiara's iPhone" or "Ramirez Home".
static const char* const kCarWifiPrefixes[] = {
  "myChevrolet", "myGMC", "myBuick", "myCadillac", "OnStar", "Audi_MMI", "Audi MMI", "MB Hotspot", "MBUX",
  "VW WLAN", "VW_WLAN", "SKODA_WLAN", "SEAT_WLAN", "Porsche", "FordPass", "Ford ", "SYNC ", "Uconnect",
  "Toyota ", "Lexus ", "HondaLink", "Honda ", "Subaru", "Hyundai", "KIA ", "Volvo", "BMW ", "BMW_", "MINI_",
  "Jeep ", "Nissan", "Infiniti", "Mazda", "Rivian", "Polestar", "Lucid", "Jaguar", "Land Rover", "Range Rover"};
static const char* const kCarBtPrefixes[] = {
  "SYNC", "Uconnect", "MyLink", "Chevrolet", "GMC", "Buick", "Cadillac", "My Car", "MY CAR", "CAR MULTIMEDIA",
  "Audi MMI", "Audi_MMI", "MB Bluetooth", "Mercedes", "BMW ", "MINI ", "VW BT", "VW Radio", "Toyota", "Lexus",
  "Honda", "Acura", "Subaru", "HYUNDAI", "Hyundai", "KIA", "Kia ", "Nissan", "Infiniti", "Mazda", "MAZDA",
  "Volvo", "Jeep", "Dodge", "Chrysler", "RAM ", "Ford ", "Lincoln", "Porsche", "Tesla", "Rivian", "Polestar"};
// Default hotspot names phones use (matched anywhere in the SSID, case-insensitive).
static const char* const kHotspotSsidWords[] = {"iPhone", "iPad", "AndroidAP", "Android_", "Galaxy ", "Galaxy_",
                                                "Pixel ", "Pixel_", "OnePlus", "Redmi", "POCO ", "moto g", "motorola edge",
                                                "Xiaomi", "HUAWEI P", "Nothing Phone"};
static const char* const kObdNames[] = {"OBDII", "OBD II", "OBD2", "OBDLink", "Vgate", "V-LINK", "vLinker", "ELM327",
                                        "BlueDriver", "Viecar", "Veepeak", "FIXD", "Carista", "KONNWEI"};

}  // namespace sigdata
}  // namespace ss
