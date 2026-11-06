# SPDX-License-Identifier: BSD-2-Clause
"""stdlib-only unit test for classic PCAP payload extraction."""
import importlib.util
import struct
import tempfile
import unittest
from pathlib import Path


class PcapReplayTest(unittest.TestCase):
    def test_extract_single_udp_datagram(self):
        script = Path(__file__).resolve().parents[1] / "tools" / "pcap_to_udp.py"
        spec = importlib.util.spec_from_file_location("pcap_to_udp", script)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        payload = b"mold64" + bytes(range(10))
        udp = struct.pack("!HHHH", 1111, 26477, 8 + len(payload), 0) + payload
        ip = (bytes.fromhex("4500") + struct.pack("!H", 20 + len(udp)) +
              bytes.fromhex("0000400040110000") +
              bytes([127, 0, 0, 1, 127, 0, 0, 1]))
        ethernet = bytes(12) + bytes.fromhex("0800")
        frame = ethernet + ip + udp
        global_header = struct.pack("<IHHIIII", 0xA1B2C3D4, 2, 4, 0, 0, 65535, 1)
        record = struct.pack("<IIII", 100, 250000, len(frame), len(frame)) + frame
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "test.pcap"
            path.write_bytes(global_header + record)
            packets = list(module.read_udp_packets(path))
        self.assertEqual(packets, [(100.25, 26477, payload)])


if __name__ == "__main__":
    unittest.main()
