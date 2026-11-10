"""Live console view of SkySentinel's JSON stream (solo board or Core).

    pip install pyserial
    python3 tools/ssmon.py /dev/ttyUSB0        # Linux   (Windows: COM5, macOS: /dev/cu.usbserial-*)
    python3 tools/ssmon.py /dev/ttyUSB0 --all  # also print every device event, not just the summary

Shows per-category now/total counts once a second, plus each interesting device
as it appears. Anything that isn't JSON (boot log, command replies) is printed as is.
Type a command (e.g. "cats all", "devices") and press Enter to send it to the board.
"""
import json
import sys
import threading

ORDER = ["flock", "bodycam", "tracker", "glasses", "drone", "skimmer", "pentest",
         "phone", "audio", "wearable", "other_ble", "other_wifi", "lora", "vehicle", "subghz"]


def fmt_status(s):
    if s.get("role") == "node":  # a node keeps no device table; show its link instead
        link = f"paired with {s['core']} as slot {s['slot']}" if s.get("paired") else "looking for a master"
        return (f"[node {s.get('id', '')}] {link} | scan {s.get('scan')} ch {s['ch']} "
                f"({s['channels']} ch, BLE {s['ble_duty']}%) | wifi {s['wifi_frames']} ble {s['ble_adv']}"
                f" | send fail {s.get('send_fail', 0)}")
    parts = [f"{c}:{s['now'].get(c, 0)}/{s['total'].get(c, 0)}" for c in ORDER
             if s["now"].get(c) or s["total"].get(c)]
    head = f"[{s.get('role', s.get('mode', 'core'))}{'' if s.get('setup', True) else ', setup needed'}] now {s['devices_now']} / session {s['devices_total']}"
    if "ch" in s:
        head += f" | ch {s['ch']} ({s['channels']} ch, BLE {s['ble_duty']}%) | wifi {s['wifi_frames']} ble {s['ble_adv']}"
    if s.get("nodes"):
        head += " | nodes " + " ".join(f"{n['id']}:{n['role']}{'' if n['online'] else '(off)'}" for n in s["nodes"])
    head += f" | stitched {s['stitches']}"
    if s.get("spoofed"):
        head += f" | spoofed {s['spoofed']}" + (" (SPAM ACTIVE)" if s.get("spam_active") else "")
    return head + "\n    " + "  ".join(parts)


def fmt_dev(d):
    tag = "STRONG" if d["strong"] else f"{d['conf']}%"
    rot = f" rot {d['rot']}" if d["rot"] else ""
    name = f" \"{d['name']}\"" if d.get("name") else ""
    return f"  {d['why'].upper():8} {d['cat']:10} {d['sig']:34} {d['mac']} {d['rssi']:4} dBm {tag}{rot}{name}"


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    import serial

    import time

    # Don't reset the board on open (see tools/sslog.py for the DTR/RTS order). Retry while the port
    # re-enumerates after a flash.
    port = serial.Serial()
    port.port, port.baudrate, port.timeout = sys.argv[1], 115200, 0.5
    for attempt in range(30):
        try:
            port.open()          # Linux raises DTR and RTS together here: no reset
            port.rts = False     # then RTS first (DTR alone only touches the boot pin),
            port.dtr = False     # never DTR first (RTS alone = reset)
            break
        except serial.SerialException:
            if attempt == 0:
                print(f"waiting for {sys.argv[1]} ...")
            time.sleep(0.5)
    else:
        print(f"could not open {sys.argv[1]}")
        return 1
    print(f"connected to {sys.argv[1]} (type a command + Enter, Ctrl+C to quit)")
    threading.Thread(target=lambda: [port.write((l.strip() + "\n").encode()) for l in sys.stdin],
                     daemon=True).start()
    buf = b""
    while True:
        buf += port.read(1024)
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            text = line.decode("utf-8", "replace").strip()
            if not text:
                continue
            try:
                msg = json.loads(text)
            except ValueError:
                print(text)
                continue
            t = msg.get("t")
            if t == "status":
                print(fmt_status(msg))
            elif t == "dev":
                print(fmt_dev(msg))
            elif t == "event" and msg.get("event") == "ble_spam":
                print(f"  !! BLE SPAM: {msg['count']} fake devices from one transmitter at ~{msg['rssi']} dBm "
                      f"(e.g. {msg['src']}); excluded from counts")
            elif t == "event":
                print(f"  !! {msg['event']} on ch {msg['ch']} ({msg['count']}/s) from {msg['src']}")
            else:
                print(text)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print()
