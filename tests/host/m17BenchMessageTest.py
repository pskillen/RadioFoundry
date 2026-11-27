#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Host vectors for M17 bench packet generation and decode-message."""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
TOOL = os.path.join(ROOT, "tools", "bench", "m17tool.py")

M17_CRC_POLY = 0x5935
M17_CRC_INIT = 0xFFFF
M17_ADDR_ALPHABET = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-/."
M17_ADDR_BASE = 40

failures = 0
checks = 0


def check(cond, msg):
    global checks, failures
    checks += 1
    if not cond:
        failures += 1
        print(f"FAIL {msg}")


def crc16(data):
    crc = M17_CRC_INIT
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ M17_CRC_POLY) if crc & 0x8000 else (crc << 1)
            crc &= 0xFFFF
    return crc


def check_crc_vectors():
    check(crc16(b"") == 0xFFFF, "crc vector empty")
    check(crc16(b"A") == 0x206E, "crc vector A")
    check(crc16(b"123456789") == 0x772B, "crc vector 123456789")
    check(crc16(bytes(range(256))) == 0x1C31, "crc vector 0..255")


def base40_encode(call):
    if call == "ALL":
        return bytes([0xFF] * 6)
    v = 0
    for i, ch in enumerate(call):
        if "a" <= ch <= "z":
            ch = ch.upper()
        try:
            d = M17_ADDR_ALPHABET.index(ch)
        except ValueError:
            return None
        v += d * (M17_ADDR_BASE ** i)
    return v.to_bytes(6, "big")


def assert_packet_wire(msg, text):
    pkt = bytes.fromhex(msg.get("packet_hex", ""))
    body = bytes([0x05]) + text.encode("utf-8") + b"\x00"
    expect = crc16(body)
    got_crc = (pkt[-2] << 8) | pkt[-1]
    check(pkt[:-2] == body, f"packet payload expected {body.hex()} got {pkt[:-2].hex()}")
    check(got_crc == expect, f"packet crc expected {expect:04x} got {got_crc:04x}")


def assert_addrs(msg, src, dst):
    exp_src = base40_encode(src)
    exp_dst = base40_encode(dst)
    got_src = bytes.fromhex(msg.get("src_raw", ""))
    got_dst = bytes.fromhex(msg.get("dst_raw", ""))
    check(got_src == exp_src, f"src_raw expected {exp_src.hex()} got {got_src.hex()}")
    check(got_dst == exp_dst, f"dst_raw expected {exp_dst.hex()} got {got_dst.hex()}")


def run_tool(args):
    return subprocess.run([sys.executable, TOOL] + args, capture_output=True, text=True, cwd=ROOT)


def parse_message(stdout):
    out = {}
    for line in stdout.splitlines():
        if not line.startswith("m17-message/1 "):
            continue
        key, _, val = line.partition("=")
        out[key.split()[-1]] = val
    return out


def gen(path, **kw):
    args = ["gen", path, "--packet", "--src", kw.get("src", "AB1CD"), "--dst", kw.get("dst", "ALL")]
    if "can" in kw:
        args += ["--can", str(kw["can"])]
    if "text" in kw:
        args += ["--text", kw["text"]]
    elif "data_hex" in kw:
        args += ["--data-hex", kw["data_hex"]]
    r = run_tool(args)
    check(r.returncode == 0, f"gen failed: {r.stderr.strip()}")
    return r


def decode(path, **kw):
    args = ["decode-message", path]
    for key in ("expect_src", "expect_dst", "expect_can"):
        if key in kw:
            flag = "--" + key.replace("_", "-")
            args += [flag, str(kw[key])]
    return run_tool(args)


def main():
    check_crc_vectors()

    with tempfile.TemporaryDirectory() as tmp:
        one = os.path.join(tmp, "one.dibits")
        gen(one, src="AB1CD", dst="AB2CD", can=5, text="Hi")
        r = decode(one, expect_src="AB1CD", expect_dst="AB2CD", expect_can=5)
        msg = parse_message(r.stdout)
        check(r.returncode == 0 and msg.get("ok") == "1", "single-frame round trip")
        body = bytes.fromhex(msg.get("text_hex", ""))
        expect = b"Hi\x00"
        check(body == expect, "text body")
        assert_packet_wire(msg, "Hi")
        assert_addrs(msg, "AB1CD", "AB2CD")

        multi = os.path.join(tmp, "multi.dibits")
        gen(multi, src="W1AW", dst="ALL", can=0, text="x" * 200)
        r = decode(multi, expect_src="W1AW", expect_dst="ALL", expect_can=0)
        msg = parse_message(r.stdout)
        check(r.returncode == 0 and int(msg.get("frames", "0")) >= 2, "multi-frame")
        assert_packet_wire(msg, "x" * 200)
        assert_addrs(msg, "W1AW", "ALL")

        utf = os.path.join(tmp, "utf.dibits")
        gen(utf, text="犬")
        r = decode(utf)
        msg = parse_message(r.stdout)
        check(r.returncode == 0 and msg.get("text_hex") == "e78aac00", "utf-8 shiba")

        bad = os.path.join(tmp, "expect.dibits")
        gen(bad, text="nope")
        r = decode(bad, expect_src="ZZ9ZZ")
        msg = parse_message(r.stdout)
        check(r.returncode != 0 and msg.get("field_disagreements") == "1", "field disagreement")

        sync_only = os.path.join(tmp, "sync.dibits")
        open(sync_only, "wb").write(b"\x77" * 48)
        r = decode(sync_only)
        check(r.returncode != 0, "sync only")

        raw = open(one, "rb").read()
        no_pkt = os.path.join(tmp, "lsf.dibits")
        open(no_pkt, "wb").write(raw[:96])
        check(decode(no_pkt).returncode != 0, "lsf without packet")

        torn = bytearray(raw)
        del torn[96:144]
        torn_path = os.path.join(tmp, "torn.dibits")
        open(torn_path, "wb").write(torn)
        check(decode(torn_path).returncode != 0, "torn sequence")

        noeot = os.path.join(tmp, "noeot.dibits")
        open(noeot, "wb").write(raw[:-48])
        check(decode(noeot).returncode != 0, "missing eot")

    print(f"ok: {checks} checks, {failures} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())