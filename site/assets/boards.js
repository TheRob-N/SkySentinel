// Supported boards: the grid and table on the home page and the board picker on the
// flash page both come from this list.
//   adds: what the board brings to a cluster.  only: things no other board does.
//   env:  PlatformIO environment = firmware file name (firmware/<env>.app.bin / .factory.bin)
//   master: can it run the cluster, and what it gives up doing so (shown on the site)
const CAPS = [
  ["wifi24", "Wi-Fi 2.4 GHz"], ["wifi5", "Wi-Fi 5 GHz"], ["ble", "Bluetooth LE"], ["classic", "Classic Bluetooth"],
  ["lora", "LoRa 915 MHz"], ["subghz", "Sub-GHz 300-928 MHz"], ["gps", "GPS (time + place)"], ["screen", "Screen"],
  ["touch", "Touch dashboard"], ["sd", "SD card log"], ["speaker", "Speaker alerts"], ["battery", "Battery"], ["ap", "Phone page (own Wi-Fi)"],
];
const BOARDS = [
  { id: "c5", env: "c5", name: "ESP32-C5 DevKitC-1", chip: "ESP32-C5", roles: "Master · Standalone · Node",
    caps: { wifi24: 1, wifi5: 1, ble: 1, ap: 1 }, only: ["wifi5"],
    master: { ok: true, best: true, limits: "Tracks 256 devices at once (2048 on a module with PSRAM). No screen, GPS or SD card of its own: add nodes for those." },
    why: "The best master: the only supported chip that hears 5 GHz Wi-Fi, and it serves the console to your phone over its own Wi-Fi. Also works alone as a single detector." },
  { id: "heltec", env: "heltec", name: "Heltec Wireless Tracker", chip: "ESP32-S3", roles: "Node · Master · Standalone",
    caps: { wifi24: 1, ble: 1, lora: 1, gps: 1, screen: 1, battery: 1 }, only: ["lora", "gps"],
    master: { ok: true, limits: "Tracks 256 devices at once (no PSRAM). 2.4 GHz only: add a C5 node for 5 GHz. Brings its own LoRa and GPS, so the log gets positions with no extra node. The small screen shows status, not the device list." },
    why: "Adds LoRa (Meshtastic and MeshCore nodes, LoRaWAN trackers) and GPS, which puts the time and place on every log entry and draws your route." },
  { id: "cyd", env: "cyd", name: "CYD / ESP32-32E 2.8\"", chip: "ESP32", roles: "Node + dashboard",
    caps: { wifi24: 1, classic: 1, screen: 1, touch: 1, sd: 1, speaker: 1, gps: "add-on", battery: "32E" }, only: ["touch", "sd"],
    master: { ok: false, limits: "Node only: Classic Bluetooth, the screen and the SD card use nearly all of the original ESP32's memory, leaving no room for the device list and the phone page." },
    why: "A touch dashboard for the whole cluster, an SD card copy of the log, alert beeps, an optional $5 GPS, and Classic Bluetooth: headphones and the HC-05 radios found in card skimmers." },
  { id: "classic", env: "classic", name: "ESP32-WROOM-32 DevKit", chip: "ESP32", roles: "Node",
    caps: { wifi24: 1, classic: 1 }, only: [],
    master: { ok: false, limits: "Node only: Classic Bluetooth needs Espressif's larger Bluetooth stack, which leaves too little memory to run the cluster." },
    why: "The cheapest node. Classic Bluetooth inquiry (skimmer radios, older headsets) plus another set of Wi-Fi ears." },
  { id: "v8", env: "v8", name: "Marauder V8", chip: "ESP32-C5", roles: "Master · Standalone · Node", status: "beta",
    caps: { wifi24: 1, wifi5: 1, ble: 1, ap: 1, screen: 1, sd: "soon", battery: 1 }, only: [],
    master: { ok: true, best: true, limits: "Tracks 2048 devices at once (PSRAM). Its screen and SD log are still being added." },
    why: "A C5 master in a handheld with its own screen and battery. Screen and SD logging support are in progress." },
  { id: "tembed", env: "tembed", name: "LilyGO T-Embed CC1101", chip: "ESP32-S3", roles: "Node · Master · Standalone", status: "beta",
    caps: { wifi24: 1, ble: 1, subghz: 1, screen: 1, battery: 1 }, only: ["subghz"],
    master: { ok: true, limits: "Tracks 256 devices at once for now (its PSRAM isn't switched on yet). 2.4 GHz only: add a C5 node for 5 GHz. Keeps its sub-GHz watch, so jammer alerts come from the master itself." },
    why: "Sub-GHz ears for the cluster: watches 315 / 345 / 390 / 433.92 / 915 MHz for key fobs, garage remotes, sensors and the jammers used in car theft, with its own screen and jog wheel. Tire-sensor and fob IDs are next." },
  { id: "cardputer", name: "M5Stack Cardputer ADV + Cap LoRa-1262", chip: "ESP32-S3", roles: "Master · Standalone · Node", status: "planned",
    caps: { wifi24: 1, ble: 1, lora: "cap", subghz: "CC1101 cap", gps: "cap", screen: 1, sd: 1, speaker: 1, battery: 1 }, only: [],
    master: { ok: true, limits: "Tracks 256 devices at once (no PSRAM). 2.4 GHz only: add a C5 node for 5 GHz. With the LoRa cap it brings its own LoRa and GPS, and its SD card holds the full log. Keyboard for setup and pairing without a laptop." },
    why: "A pocket master or node with a keyboard, screen and SD card on the chip itself. Swap caps for the job: the LoRa cap adds LoRa and GPS, the CC1101 cap adds the T-Embed's sub-GHz watch. (The caps also fit the CardputerZero.)" },
];

// How to flash each build (same text as the console's FIRMWARE window)
const GUIDES = {
    c5: { title: "ESP32-C5 board (DevKitC-1 or any C5)", steps: [
      "Plug into the board's own USB port. On the DevKitC-1 that's the one labeled <b>USB</b>; the <b>UART</b> port works too but may need the buttons.",
      "Close anything else using the port: serial monitor, PlatformIO, Arduino IDE. (If this page is connected, it hands the port over.)",
      "Choose <b>UPDATE</b> to keep its settings, or <b>FRESH INSTALL</b> for a new board or to start over. Press <b>FLASH</b> and pick <b>USB JTAG/serial debug unit</b>.",
      "Won't connect? Hold <b>BOOT</b>, tap <b>RST</b>, let go of BOOT, then press FLASH again.",
      "When it finishes the board restarts and this page reconnects. After a fresh install the setup screen asks what the board is: <b>STANDALONE</b> for a single board, <b>MASTER</b> or <b>NODE</b> in a cluster." ] },
    v8: { title: "Marauder V8", steps: [
      "Plug the V8 in with a data USB-C cable.",
      "Close anything else using the port (serial monitor, PlatformIO).",
      "Choose <b>UPDATE</b> or <b>FRESH INSTALL</b>, press <b>FLASH</b> and pick the V8's port (<b>USB JTAG/serial debug unit</b>).",
      "Won't connect? Hold the V8's BOOT button while plugging it in, then press FLASH.",
      "A fresh install starts the V8 as the <b>MASTER</b>; change it in SETTINGS &gt; ROLE if needed (STANDALONE, or NODE under another master)." ] },
    heltec: { title: "Heltec Wireless Tracker (LoRa node)", steps: [
      "Plug it in with a data USB-C cable. It uses the ESP32-S3's own USB and shows up as <b>USB JTAG/serial debug unit</b>.",
      "Close anything else using the port (serial monitor, PlatformIO, the Meshtastic app or web flasher).",
      "Choose <b>UPDATE</b> or <b>FRESH INSTALL</b>, press <b>FLASH</b> and pick that port. This replaces Meshtastic firmware if it had it.",
      "Won't connect? Hold <b>BOOT</b> (the USER button on some versions), tap <b>RST</b>, let go of BOOT, then press FLASH again.",
      "A fresh install starts it as a <b>NODE</b> waiting to be paired: on the master's page press CLUSTER &gt; <b>PAIR A NODE</b> and check the code on its screen. Turn on the <b>LoRa / Meshtastic</b> category on the master to use its LoRa radio.",
      "Want it as the <b>master</b> instead (LoRa and GPS built in)? Connect this page to it over USB, then <b>SETTINGS &gt; ROLE &gt; MASTER</b>. See <a href=\"docs.html#master\">Choosing a master</a> for what it gives up." ] },
    classic: { title: "Original ESP32 (WROOM-32 DevKit): Classic Bluetooth node", steps: [
      "Plug it in with a <b>data</b> cable (some USB cables only charge). Its port goes through a USB-serial chip and shows up as <b>CP2102</b>, <b>CH340</b> or <b>USB Serial</b>.",
      "On Linux, if no port shows up: <span class=\"mono\">sudo usermod -aG dialout $USER</span>, then log out and back in.",
      "Close anything else using the port. Choose <b>UPDATE</b> or <b>FRESH INSTALL</b>, press <b>FLASH</b> and pick that port.",
      "Stuck on \u201cConnecting\u201d? Hold the <b>BOOT</b> button and keep holding until the progress bar moves, then let go.",
      "It's always a node. On the master's page press CLUSTER &gt; <b>PAIR A NODE</b> and follow the steps with the ESP32 close by: it joins within seconds of ACCEPT.",
      "A board with a screen (CYD, ESP32-32E)? Use the <b>CYD</b> build instead: same node, plus the touch dashboard." ] },
    tembed: { title: "LilyGO T-Embed CC1101: sub-GHz node with screen and jog wheel", steps: [
      "This replaces whatever it runs now (Bruce, the factory demo). You can put Bruce back later from its own web flasher.",
      "Plug it in with a <b>data</b> cable. It uses the ESP32-S3's own USB and shows up as <b>USB JTAG/serial debug unit</b> (Espressif).",
      "Choose <b>FRESH INSTALL</b> the first time, press <b>FLASH</b> and pick that port. Stuck on \u201cConnecting\u201d? Hold the jog wheel pressed in (it's BOOT) while you plug it in.",
      "It starts as a <b>NODE</b>: on the master's page press CLUSTER &gt; <b>PAIR A NODE</b> and check the code on its screen.",
      "Turn the wheel for pages: home, the sub-GHz bands (level, noise floor, bursts) and the latest bursts. The back button turns the screen off and on.",
      "The sub-GHz watch runs while the <b>Pentest gear</b> or <b>Vehicles</b> category is on. A jammed band raises an alert on the master.",
      "Want it as the <b>master</b>? Connect this page to it over USB, then <b>SETTINGS &gt; ROLE &gt; MASTER</b>. See <a href=\"docs.html#master\">Choosing a master</a> for what it gives up." ] },
    cyd: { title: "CYD (ESP32-2432S028) or ESP32-32E 2.8\": Classic node + touch dashboard", steps: [
      "Plug it in with a <b>data</b> cable. The 2-port CYD flashes through either port; the ESP32-32E through its USB-C. It shows up as <b>CH340</b> or <b>USB Serial</b>.",
      "Close anything else using the port. Choose <b>FRESH INSTALL</b> the first time (or <b>UPDATE</b> later), press <b>FLASH</b> and pick that port.",
      "Stuck on \u201cConnecting\u201d? Hold <b>BOOT</b> until the progress bar moves. (The 2-port CYD's BOOT button is on the back, next to RST.)",
      "First start, on the screen: tap three crosses (use a fingernail), pick your board, then check the colors and tap <b>LOOKS GOOD</b>.",
      "Screen stays white or garbled? Hold <b>BOOT</b> for 3 seconds: it switches display chip (ILI9341 / ST7789) and restarts.",
      "Then pair it: on the master's page press CLUSTER &gt; <b>PAIR A NODE</b>, check the code on the CYD matches, ACCEPT. The dashboard fills in right after." ] },
  };
const guideHtml = (key) => {
  const g = GUIDES[key];
  // The console's FIRMWARE window calls its button FLASH; here it's CONNECT AND INSTALL
  return g ? `<ol>${g.steps.map((t) => `<li>${t.replaceAll("<b>FLASH</b>", "<b>CONNECT AND INSTALL</b>").replaceAll("press FLASH", "press CONNECT AND INSTALL")}</li>`).join("")}</ol>` : "";
};
