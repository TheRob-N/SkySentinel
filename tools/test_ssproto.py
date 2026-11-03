"""Checks tools/ssproto.py against the C++ encoder (test/host/test_bin --golden)."""
import os
import random
import subprocess
import sys
import unittest

sys.path.insert(0, os.path.dirname(__file__))
import ssproto as sp  # noqa: E402

HOST = os.path.join(os.path.dirname(__file__), "..", "test", "host")


def golden():
    subprocess.run(["make", "-s", "-C", HOST, "test_bin"], check=True)
    out = subprocess.run([os.path.join(HOST, "test_bin"), "--golden"], check=True,
                         capture_output=True, text=True).stdout
    return {k: bytes.fromhex(v) for k, v in (line.split() for line in out.splitlines())}


class TestProtocol(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.g = golden()

    def test_crc(self):
        self.assertEqual(sp.crc16(b"123456789"), 0x29B1)

    def test_hello(self):
        f = sp.decode_frame(self.g["hello"])
        self.assertEqual((f.type_name, f.node_id, f.seq), ("hello", 0x2233, 1))
        p = sp.decode_payload(f)
        self.assertEqual(p["mac"], "24:58:7C:11:22:33")
        self.assertEqual(p["fw"], 0x0102)
        self.assertEqual(p["role_pref"], 2)
        self.assertEqual(p["auth"], 0x11223344)

    def test_config(self):
        p = sp.decode_payload(sp.decode_frame(self.g["config"]))
        self.assertEqual(p["channels"], list(range(1, 14)))
        self.assertEqual(p["categories"], ["flock", "bodycam", "tracker", "glasses", "drone", "skimmer", "pentest"])
        self.assertEqual(p["rssi_floor"], -90)
        self.assertEqual(p["ble_duty_pct"], 50)

    def test_sightings(self):
        f = sp.decode_frame(self.g["sightings"])
        self.assertEqual(f.seq, 65535)
        s = sp.decode_payload(f)["sightings"]
        self.assertEqual(len(s), 2)
        self.assertEqual(s[0].addr, "5A:3F:C2:9B:14:07")
        self.assertEqual((s[0].kind, s[0].category, s[0].rssi_max, s[0].hits), ("ble_adv", "tracker", -61, 12))
        self.assertEqual((s[0].company, s[0].fp, s[0].age_ms, s[0].channel), (0x004C, 0x3C7E0001, 250, 37))
        self.assertTrue(s[0].random_addr)
        self.assertEqual((s[1].kind, s[1].category, s[1].company, s[1].channel), ("wifi_probe_req", "phone", None, 149))

    def test_encode_matches_cpp(self):
        f = sp.decode_frame(self.g["sightings"])
        self.assertEqual(sp.encode_frame(f.type, f.node_id, f.seq, f.payload), self.g["sightings"])

    def test_cobs_matches_cpp(self):
        self.assertEqual(sp.cobs_encode(self.g["sightings"]), self.g["sightings_cobs"])
        self.assertEqual(sp.cobs_decode(self.g["sightings_cobs"]), self.g["sightings"])

    def test_cobs_roundtrip(self):
        rnd = random.Random(1)
        for _ in range(300):
            data = bytes(rnd.choice([0, 0, rnd.randrange(256)]) for _ in range(rnd.randrange(1, 300)))
            enc = sp.cobs_encode(data)
            self.assertNotIn(0, enc)
            self.assertEqual(sp.cobs_decode(enc), data)

    def test_deframer(self):
        stream = b"\x01\x02\x00" + sp.cobs_encode(self.g["hello"]) + b"\x00" + \
                 sp.cobs_encode(self.g["config"][:-1] + b"\x00") + b"\x00" + \
                 sp.cobs_encode(self.g["sightings"]) + b"\x00"
        df = sp.Deframer()
        frames = []
        for i in range(0, len(stream), 7):  # arbitrary chunking
            frames += list(df.feed(stream[i:i + 7]))
        self.assertEqual([f.type_name for f in frames], ["hello", "sightings"])
        self.assertEqual(df.errors, 2)  # leading garbage + corrupted config

    def test_rejects_corruption(self):
        raw = bytearray(self.g["hello"])
        raw[9] ^= 0xFF
        with self.assertRaises(sp.ProtocolError):
            sp.decode_frame(bytes(raw))


if __name__ == "__main__":
    unittest.main(verbosity=1)
