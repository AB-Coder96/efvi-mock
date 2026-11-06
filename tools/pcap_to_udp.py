#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Replay Ethernet/IPv4/UDP payloads from a classic PCAP through Linux UDP.

Useful for testing efvi-mock on a machine without an RX-capable NIC. The
receiver synthesizes the Ethernet/IP/UDP headers; only the payload is replayed.
"""
import argparse
import socket
import struct
import time
from pathlib import Path


def read_udp_packets(path):
    with Path(path).open("rb") as stream:
        gh = stream.read(24)
        if len(gh) != 24:
            raise ValueError("missing classic PCAP global header")
        magic = gh[:4]
        formats = {
            b"\xd4\xc3\xb2\xa1": ("<", 1_000_000),
            b"\xa1\xb2\xc3\xd4": (">", 1_000_000),
            b"\x4d\x3c\xb2\xa1": ("<", 1_000_000_000),
            b"\xa1\xb2\x3c\x4d": (">", 1_000_000_000),
        }
        if magic not in formats:
            raise ValueError("unsupported PCAP magic (PCAPNG unsupported)")
        endian, precision = formats[magic]
        _, version_major, version_minor, _, _, snaplen, linktype = struct.unpack(
            endian + "IHHiIII", gh
        )
        if (version_major, version_minor) != (2, 4) or linktype != 1:
            raise ValueError("requires classic PCAP v2.4 with Ethernet linktype")
        if snaplen > 16 * 1024 * 1024:
            raise ValueError("unreasonable PCAP snaplen")
        while True:
            header = stream.read(16)
            if not header:
                return
            if len(header) != 16:
                raise ValueError("truncated PCAP record header")
            sec, subsec, size, original_size = struct.unpack(endian + "IIII", header)
            if size > snaplen or size > original_size:
                raise ValueError("malformed PCAP captured length")
            frame = stream.read(size)
            if len(frame) != size:
                raise ValueError("truncated PCAP record")
            if len(frame) < 14:
                continue
            kind = int.from_bytes(frame[12:14], "big")
            offset = 14
            while kind in (0x8100, 0x88A8, 0x9100):
                if len(frame) < offset + 4:
                    raise ValueError("truncated VLAN frame")
                kind = int.from_bytes(frame[offset + 2:offset + 4], "big")
                offset += 4
            if kind != 0x0800 or len(frame) < offset + 20:
                continue
            ip = frame[offset:]
            ihl = (ip[0] & 0x0F) * 4
            if ip[0] >> 4 != 4 or ihl < 20 or len(ip) < ihl + 8:
                continue
            if ip[9] != socket.IPPROTO_UDP:
                continue
            fragments = int.from_bytes(ip[6:8], "big")
            if fragments & 0x3FFF:
                raise ValueError("fragmented UDP packets cannot be replayed independently")
            total = int.from_bytes(ip[2:4], "big")
            if total < ihl + 8 or total > len(ip):
                raise ValueError("truncated IPv4 packet")
            udp = ip[ihl:total]
            udp_length = int.from_bytes(udp[4:6], "big")
            if udp_length < 8 or udp_length > len(udp):
                raise ValueError("invalid UDP length")
            port = int.from_bytes(udp[2:4], "big")
            yield sec + subsec / precision, port, udp[8:udp_length]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pcap", help="classic Ethernet PCAP file")
    parser.add_argument("--host", default="127.0.0.1",
                        help="destination host; default 127.0.0.1")
    parser.add_argument("--port", type=int, default=None,
                        help="override original UDP destination port")
    parser.add_argument("--speed", type=float, default=1.0,
                        help="capture timing multiplier; 0 = maximum speed")
    args = parser.parse_args()
    if args.port is not None and not 1 <= args.port <= 65535:
        parser.error("port must be 1..65535")
    if args.speed < 0 or not args.speed < float("inf"):
        parser.error("speed must be finite and nonnegative")
    count = 0
    first = None
    started = None
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        for at, port, payload in read_udp_packets(args.pcap):
            if first is None:
                first, started = at, time.monotonic()
            if args.speed > 0:
                remaining = (at - first) / args.speed - (time.monotonic() - started)
                if remaining > 0:
                    time.sleep(remaining)
            sock.sendto(payload, (args.host, args.port or port))
            count += 1
    print(f"Replayed {count} UDP datagrams from {args.pcap}")


if __name__ == "__main__":
    main()
