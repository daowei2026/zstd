#!/usr/bin/env python3
"""Observer behavior checks with generated packets kept entirely in memory."""
import json
import random
import struct
import subprocess
import sys


def checksum(data):
    data += b"\0" * (len(data) % 2)
    value = sum(struct.unpack("!" + "H" * (len(data) // 2), data))
    while value >> 16:
        value = (value & 65535) + (value >> 16)
    return (~value) & 65535


def frame(payload, sequence, reverse=False):
    src, dst = bytes([192, 0, 2, 1]), bytes([192, 0, 2, 2])
    sport, dport = 40000, 443
    if reverse:
        src, dst, sport, dport = dst, src, dport, sport
    tcp = struct.pack("!HHIIBBHHH", sport, dport, sequence, 1, 80, 24, 65535, 0, 0)
    tcp_sum = checksum(src + dst + struct.pack("!BBH", 0, 6, len(tcp) + len(payload)) + tcp + payload)
    tcp = tcp[:16] + struct.pack("!H", tcp_sum) + tcp[18:]
    ip = struct.pack("!BBHHHBBH4s4s", 69, 0, 40 + len(payload), sequence & 65535, 16384, 64, 6, 0, src, dst)
    ip = ip[:10] + struct.pack("!H", checksum(ip)) + ip[12:]
    return b"\x02\0\0\0\0\x02\x02\0\0\0\0\x01\x08\0" + ip + tcp + payload


def pcap(frames, endian="<", nano=False):
    magic = 0xA1B23C4D if nano else 0xA1B2C3D4
    result = bytearray(struct.pack(endian + "IHHIIII", magic, 2, 4, 0, 0, 65535, 1))
    for i, data in enumerate(frames):
        result.extend(struct.pack(endian + "IIII", i + 1, 123, len(data), len(data)))
        result.extend(data)
    return bytes(result)


def run(data, success=True, every=1, capacity=16384):
    result = subprocess.run([sys.argv[1], str(capacity), str(every), "192.0.2.1"], input=data, capture_output=True)
    assert b"AddressSanitizer" not in result.stderr and b"runtime error:" not in result.stderr, result.stderr.decode()
    if success:
        assert result.returncode == 0, (result.returncode, result.stderr.decode())
        rows = [json.loads(line) for line in result.stdout.splitlines()]
        assert rows[-1]["complete"] is True
        for row in rows:
            if "partitions" in row:
                assert sum(p["allocated"] for p in row["partitions"]) == row["payload_allocated"]
                assert all(p["extent"] <= p["capacity"] for p in row["partitions"])
                assert sum(row["header_matched_bytes"]) + sum(row["transport_payload_matched_bytes"]) <= sum(row["matched_bytes"])
        return rows
    assert result.returncode == 1 and b"workload check failed" in result.stderr
    assert b'"complete":true' not in result.stdout


def main():
    rng = random.Random(71237)
    content = rng.randbytes(1400)
    packets = [frame(content, 1), frame(content, 1, reverse=True), frame(content, 1401)]
    for endian in ("<", ">"):
        for nano in (False, True):
            rows = run(pcap(packets, endian, nano))
            finals = {row["direction"]: row for row in rows if row["kind"] == "final"}
            outgoing = finals["from_client"]
            assert outgoing["appends"] == 1
            assert sum(outgoing["transport_payload_matched_bytes"]) == 1400
            assert sum(finals["toward_client"]["matched_bytes"]) == 0
            assert any(row.get("frames") == 1 and sum(row["matched_bytes"]) == 0 for row in rows if "matched_bytes" in row)
    # Every payload is new, with stable packet headers. Header hits must not
    # silently prevent learning of the previously unseen remaining bytes.
    unique = [frame(rng.randbytes(1400), i * 1400) for i in range(240)]
    all_rows = run(pcap(unique))
    last = [r for r in all_rows if r["kind"] == "final"][0]
    assert last["appends"] == 240
    assert sum(last["transport_payload_matched_bytes"]) == 0
    assert last["rotations"][2] > 5 and last["payload_freed"] > 0
    assert last["model_ratio_R2"] > 1
    sampled = [r for r in run(pcap(unique), every=16) if r["kind"] == "final"][0]
    assert sampled["appends"] == 15
    large = [r for r in run(pcap(packets), capacity=30000000) if r["kind"] == "final" and r["direction"] == "from_client"][0]
    assert large["appends"] == 1 and sum(large["transport_payload_matched_bytes"]) >= 1300
    uneven = [r for r in run(pcap(unique), capacity=17000) if r["kind"] == "final"][0]
    assert uneven["rotations"][2] > 5
    # Reject partial captures, unsupported link types, GRO-sized frames and
    # invalid transport headers; none is successful workload evidence.
    good = pcap(packets)
    run(good[:-1], False)
    bad_link = bytearray(good); bad_link[20:24] = struct.pack("<I", 113); run(bad_link, False)
    snap = bytearray(good); snap[36:40] = struct.pack("<I", len(packets[0]) + 1); run(snap, False)
    run(pcap([frame(rng.randbytes(3000), 1)]), False)
    bad_tcp = bytearray(packets[0]); bad_tcp[46] = 0; run(pcap([bad_tcp]), False)
    run(good[:24], False)
    print(json.dumps({"test": "workload_observer_synthetic_behavior", "seed": 71237,
                      "passed": True, "real_application_traffic": False}))


if __name__ == "__main__":
    main()
