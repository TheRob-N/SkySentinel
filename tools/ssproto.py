"""SkySentinel wire protocol v1, Python side.

Decodes the same frames the ESP32 nodes and Core send, for the future
single-board-computer master (and for debugging today):

    python3 tools/ssproto.py /dev/ttyACM0        # read COBS frames from a serial port
    python3 tools/ssproto.py --hex <frame-hex>   # decode one frame

Mirrors lib/sscore/ss_proto.h. Keep the two in sync; tools/test_ssproto.py
checks them against each other byte for byte.
"""
from __future__ import annotations

import struct
import sys
from dataclasses import dataclass, field

MAGIC = 0x5353
VERSION = 1
MAX_FRAME = 250
HEADER_SIZE = 8
SIGHTING_SIZE = 24

MSG = {1: "hello", 2: "welcome", 3: "config", 4: "heartbeat", 5: "sightings", 6: "name", 7: "event", 8: "command"}
CATEGORIES = ["none", "flock", "bodycam", "tracker", "glasses", "drone", "skimmer", "pentest",
              "phone", "audio", "wearable", "other_ble", "other_wifi", "lora", "vehicle"]
KINDS = {0x01: "wifi_beacon", 0x02: "wifi_probe_req", 0x03: "wifi_probe_resp", 0x04: "wifi_data",
         0x05: "wifi_other", 0x10: "ble_adv", 0x20: "bt_classic", 0x30: "lora_meshtastic", 0x31: "lorawan", 0x32: "lora_meshcore"}
CHANNELS = list(range(1, 14)) + [36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124,
                                 128, 132, 136, 140, 144, 149, 153, 157, 161, 165]


class ProtocolError(ValueError):
    pass


def crc16(data: bytes, crc: int = 0xFFFF) -> int:
    """CRC-16/CCITT-FALSE."""
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def auth_tag(key: bytes, data: bytes) -> int:
    """First 4 bytes of HMAC-SHA256(key, data), read little-endian (0 if no key)."""
    if not key:
        return 0
    import hashlib
    import hmac

    return int.from_bytes(hmac.new(key, data, hashlib.sha256).digest()[:4], "little")


def fmt_mac(b: bytes) -> str:
    return ":".join(f"{x:02X}" for x in b)


@dataclass
class Frame:
    type: int
    node_id: int
    seq: int
    payload: bytes

    @property
    def type_name(self) -> str:
        return MSG.get(self.type, f"type{self.type}")


def decode_frame(raw: bytes) -> Frame:
    if len(raw) < HEADER_SIZE + 2 or len(raw) > MAX_FRAME:
        raise ProtocolError(f"bad length {len(raw)}")
    magic, version, mtype, node_id, seq = struct.unpack_from("<HBBHH", raw, 0)
    if magic != MAGIC or version != VERSION:
        raise ProtocolError("bad magic/version")
    (want,) = struct.unpack_from("<H", raw, len(raw) - 2)
    if crc16(raw[:-2]) != want:
        raise ProtocolError("crc mismatch")
    return Frame(mtype, node_id, seq, bytes(raw[HEADER_SIZE:-2]))


def encode_frame(mtype: int, node_id: int, seq: int, payload: bytes) -> bytes:
    body = struct.pack("<HBBHH", MAGIC, VERSION, mtype, node_id, seq) + payload
    if len(body) + 2 > MAX_FRAME:
        raise ProtocolError("frame too large")
    return body + struct.pack("<H", crc16(body))


@dataclass
class Sighting:
    addr: str
    kind: str
    channel: int
    rssi_max: int
    rssi_last: int
    hits: int
    category: str
    sig_id: int
    conf: int
    flags: int
    company: int | None
    fp: int
    age_ms: int

    @property
    def random_addr(self) -> bool:
        return bool(self.flags & 0x01)


_SIGHTING = struct.Struct("<6sBBbbBBHBBHIH")
assert _SIGHTING.size == SIGHTING_SIZE


def decode_sightings(p: bytes) -> list[Sighting]:
    if not p:
        raise ProtocolError("empty sightings payload")
    count = p[0]
    if len(p) != 1 + count * SIGHTING_SIZE:
        raise ProtocolError("sightings length mismatch")
    out = []
    for i in range(count):
        (addr, kind, ch, rmax, rlast, hits, cat, sig, conf, flags, company, fp, age) = \
            _SIGHTING.unpack_from(p, 1 + i * SIGHTING_SIZE)
        out.append(Sighting(fmt_mac(addr), KINDS.get(kind, hex(kind)), ch, rmax, rlast, hits,
                            CATEGORIES[cat] if cat < len(CATEGORIES) else str(cat), sig, conf, flags,
                            None if company == 0xFFFF else company, fp, age))
    return out


def decode_payload(f: Frame) -> dict:
    p = f.payload
    try:
        if f.type == 1:
            mac, fw, hw, caps, role, auth = struct.unpack("<6sHBBBI", p)
            return {"mac": fmt_mac(mac), "fw": fw, "hw": hw, "caps": caps, "role_pref": role, "auth": auth}
        if f.type == 2:
            mac, idx, core_ms, auth = struct.unpack("<6sBII", p)
            return {"core_mac": fmt_mac(mac), "node_index": idx, "core_ms": core_ms, "auth": auth}
        if f.type == 3:
            ver, role, cats, chmask, dwell, report, floor, duty = struct.unpack("<BBHQHHbB", p[:18])
            node_flags = p[18] if len(p) > 18 else 0  # optional trailing fields
            label = p[20:20 + p[19]].decode("utf-8", "replace") if len(p) > 19 else ""
            chans = [c for i, c in enumerate(CHANNELS) if chmask >> i & 1]
            enabled = [CATEGORIES[c] for c in range(1, len(CATEGORIES)) if cats >> c & 1]
            return {"cfg_version": ver, "role": role, "categories": enabled, "channels": chans,
                    "dwell_ms": dwell, "report_ms": report, "rssi_floor": floor, "ble_duty_pct": duty,
                    "display_off": bool(node_flags & 0x01), "label": label}
        if f.type == 4:
            up, ver, ch, batt, temp, dropped, wf, ba = struct.unpack("<IBBBbHII", p[:18])
            gps = p[18] if len(p) > 18 else 0  # optional trailing fields
            sats = p[19] if len(p) > 19 else 0
            health = {}
            if len(p) >= 27:
                rst, heap, heapmin, losses = struct.unpack("<BHHH", p[20:27])
                health = {"reset": ["unknown", "power", "software", "crash", "watchdog", "brownout", "no_master", "deepsleep", "usb", "radio", "stuck_tx"][rst] if rst < 11 else rst,
                          "heap_kb": heap, "heap_min_kb": heapmin, "losses": losses}
            return {"uptime_s": up, "cfg_version": ver, "channel": ch,
                    "batt_pct": None if batt == 255 else batt, "temp_c": None if temp == -128 else temp,
                    "dropped": dropped, "wifi_frames": wf, "ble_adv": ba,
                    "gps": ["none", "silent", "searching", "2D", "3D"][gps] if gps < 5 else gps, "gps_sats": sats, **health}
        if f.type == 5:
            return {"sightings": decode_sightings(p)}
        if f.type == 10:
            return {"dash_cmd": {1: "toggle_category", 2: "deploy"}.get(p[0], p[0]), "arg": p[1]}
        if f.type == 9:
            return {"dash": len(p)}  # summary for a CYD (see encodeDash); not decoded here
        if f.type == 8:
            cmd, arg, ln = struct.unpack("<BBB", p[:3])
            return {"cmd": {1: "reboot", 2: "identify", 3: "label"}.get(cmd, cmd), "arg": arg,
                    "text": p[3:3 + ln].decode("utf-8", "replace")}
        if f.type == 6:
            addr, kind, n = struct.unpack_from("<6sBB", p)
            if n > 32 or len(p) != 8 + n:
                raise ProtocolError("bad name length")
            return {"addr": fmt_mac(addr), "kind": KINDS.get(kind, hex(kind)),
                    "name": p[8:8 + n].decode("utf-8", "replace")}
        if f.type == 7:
            et, ch, count, src, dst, bssid = struct.unpack("<BBH6s6s6s", p)
            return {"event": {1: "deauth_flood", 2: "disassoc_flood", 3: "ble_spam"}.get(et, et), "channel": ch,
                    "count": count, "src": fmt_mac(src), "dst": fmt_mac(dst), "bssid": fmt_mac(bssid)}
    except struct.error as e:
        raise ProtocolError(str(e)) from e
    return {"raw": p.hex()}


# --- COBS -----------------------------------------------------------------

def cobs_encode(data: bytes) -> bytes:
    out = bytearray([0])
    code_idx, code = 0, 1
    for b in data:
        if b == 0:
            out[code_idx] = code
            code_idx, code = len(out), 1
            out.append(0)
        else:
            out.append(b)
            code += 1
            if code == 0xFF:
                out[code_idx] = code
                code_idx, code = len(out), 1
                out.append(0)
    out[code_idx] = code
    return bytes(out)


def cobs_decode(data: bytes) -> bytes:
    out = bytearray()
    i = 0
    while i < len(data):
        code = data[i]
        i += 1
        if code == 0:
            raise ProtocolError("zero in COBS data")
        block = data[i:i + code - 1]
        if len(block) != code - 1 or 0 in block:
            raise ProtocolError("truncated COBS block")
        out += block
        i += code - 1
        if code != 0xFF and i < len(data):
            out.append(0)
    return bytes(out)


@dataclass
class Deframer:
    """Feed a serial byte stream; yields decoded Frames, counts bad ones."""
    buf: bytearray = field(default_factory=bytearray)
    errors: int = 0

    def feed(self, chunk: bytes):
        for b in chunk:
            if b != 0:
                if len(self.buf) < 2 * MAX_FRAME:
                    self.buf.append(b)
                continue
            if self.buf:
                try:
                    yield decode_frame(cobs_decode(bytes(self.buf)))
                except ProtocolError:
                    self.errors += 1
            self.buf.clear()


def _main(argv: list[str]) -> int:
    if len(argv) >= 2 and argv[0] == "--hex":
        f = decode_frame(bytes.fromhex(argv[1]))
        print(f.type_name, f"node={f.node_id:04X}", f"seq={f.seq}", decode_payload(f))
        return 0
    if len(argv) == 1:
        import serial  # pip install pyserial

        port = serial.Serial(argv[0], 921600, timeout=0.2)
        df = Deframer()
        while True:
            for f in df.feed(port.read(512)):
                print(f.type_name, f"node={f.node_id:04X}", f"seq={f.seq}", decode_payload(f), flush=True)
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(_main(sys.argv[1:]))
