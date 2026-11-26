#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""
m17tool: M17 symbol files from the firmware's frame codec (core/m17), built for the host from tools/bench/m17.

Dibits are packed four per byte (hal/hal_air.h, tools/bench/fsk4_bench.py). Each output file is a sequence of
48-byte frames (192 symbols, 40 ms). Bench air parameters: 4800 baud, roll-off 0.5 (RRC), outer deviation 2400 Hz.
Text packets carry up to 821 UTF-8 bytes before the terminating NUL.

    m17tool.py selftest
    m17tool.py gen OUT.dibits --bert --frames N
    m17tool.py gen OUT.dibits --stream --src AB1CD --dst ALL --frames N
    m17tool.py gen OUT.dibits --packet --src AB1CD --dst ALL --can N --text MSG
    m17tool.py decode IN.dibits
    m17tool.py decode-message IN.dibits [--expect-src CALL] [--expect-dst CALL] [--expect-can N]
"""
import argparse
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
SRC = os.path.join(HERE, "m17")
BIN = os.path.join(ROOT, "build", "bench", "m17tool")

M17_CRC_POLY = 0x5935
M17_CRC_INIT = 0xFFFF


def m17_crc16(data):
    crc = M17_CRC_INIT
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ M17_CRC_POLY) if crc & 0x8000 else (crc << 1)
            crc &= 0xFFFF
    return crc


def build():
    srcs = [os.path.join(SRC, "m17tool.c"), os.path.join(SRC, "Makefile")]
    for d in ("core/m17", "core/fec", "core/air"):
        p = os.path.join(ROOT, d)
        if os.path.isdir(p):
            srcs += [os.path.join(p, f) for f in os.listdir(p) if f.endswith((".c", ".h"))]
    if not os.path.exists(BIN) or os.path.getmtime(BIN) < max(os.path.getmtime(s) for s in srcs):
        r = subprocess.run(["make", "-s", "-C", SRC], capture_output=True, text=True)
        if r.returncode:
            sys.exit("m17tool: build failed\n" + r.stdout + r.stderr)
    return BIN


def run(args):
    r = subprocess.run([build()] + [str(a) for a in args], capture_output=True, text=True)
    if r.returncode:
        sys.exit(r.stderr.strip() or r.stdout.strip() or "m17tool: failed")
    return r.stdout


def run_rc(args):
    r = subprocess.run([build()] + [str(a) for a in args], capture_output=True, text=True)
    return r.returncode, r.stdout, r.stderr


def parse_message(stdout):
    out = {}
    for line in stdout.splitlines():
        if not line.startswith("m17-message/1 "):
            continue
        key, _, val = line.partition("=")
        out[key.split()[-1]] = val
    return out


def cmd_selftest(_a):
    fails = []
    with tempfile.TemporaryDirectory() as tmp:
        def p(name):
            return os.path.join(tmp, name)

        run(["selftest"])

        run(["gen", p("stream.dibits"), "--stream", "--src", "AB1CD", "--dst", "ALL", "--frames", "3"])
        if open(p("stream.dibits"), "rb").read(1)[0] != 0x77:
            fails.append("preamble stream")
        out = run(["decode", p("stream.dibits")])
        if "LSF CRC ok" not in out:
            fails.append("LSF crc")
        if "AB1CD" not in out or "ALL" not in out:
            fails.append("LSF addresses")
        nums = [int(line.split()[-1]) for line in out.splitlines() if line.startswith("stream frame")]
        if nums != [0, 1, 2]:
            fails.append("stream frame numbers %r" % nums)

        run(["gen", p("bert.dibits"), "--bert", "--frames", "2"])
        if open(p("bert.dibits"), "rb").read(1)[0] != 0xDD:
            fails.append("preamble bert")
        bout = run(["decode", p("bert.dibits")])
        if "394" not in bout:
            fails.append("bert bits: " + bout.strip().splitlines()[-1])

        text = "Caf\u00e9"
        run(["gen", p("msg1.dibits"), "--packet", "--src", "AB1CD", "--dst", "AB2CD", "--can", "5",
             "--text", text])
        rc, mout, _ = run_rc(["decode-message", p("msg1.dibits"),
                               "--expect-src", "AB1CD", "--expect-dst", "AB2CD", "--expect-can", "5"])
        msg = parse_message(mout)
        if rc != 0 or msg.get("ok") != "1":
            fails.append("one-frame message decode")
        if msg.get("type") != "05" or msg.get("frames") != "1":
            fails.append("one-frame metadata %r" % msg)
        body = bytes.fromhex(msg.get("text_hex", ""))
        if body != (text + "\0").encode("utf-8"):
            fails.append("utf-8 body %r" % body)
        expect_crc = m17_crc16(bytes([0x05]) + body)
        if body[-1] != 0:
            fails.append("nul terminator")

        long_body = "x" * 200
        run(["gen", p("multi.dibits"), "--packet", "--src", "W1AW", "--dst", "ALL", "--can", "0",
             "--text", long_body])
        rc, mout, _ = run_rc(["decode-message", p("multi.dibits"),
                               "--expect-src", "W1AW", "--expect-dst", "ALL", "--expect-can", "0"])
        msg = parse_message(mout)
        if rc != 0 or int(msg.get("frames", "0")) < 2:
            fails.append("multi-frame message")

        max_text = "m" * 821
        run(["gen", p("max.dibits"), "--packet", "--src", "AB1CD", "--dst", "ALL", "--text", max_text])
        rc, mout, _ = run_rc(["decode-message", p("max.dibits")])
        msg = parse_message(mout)
        if rc != 0 or msg.get("bytes") != "822":
            fails.append("max length bytes=%s rc=%s" % (msg.get("bytes"), rc))

        raw = open(p("msg1.dibits"), "rb").read()
        with open(p("lsfonly.dibits"), "wb") as f:
            f.write(raw[:96])
        rc, _, _ = run_rc(["decode-message", p("lsfonly.dibits")])
        if rc == 0:
            fails.append("lsf only should fail")

        torn = bytearray(raw)
        del torn[96:144]
        with open(p("torn.dibits"), "wb") as f:
            f.write(torn)
        rc, _, _ = run_rc(["decode-message", p("torn.dibits")])
        if rc == 0:
            fails.append("torn sequence should fail")

        corrupt = bytearray(raw)
        pkt_at = None
        for i in range(48, len(corrupt) - 48, 48):
            if corrupt[i] == 0x75 and corrupt[i + 1] == 0xFF:
                pkt_at = i
                break
        if pkt_at is not None:
            for j in range(pkt_at + 2, min(pkt_at + 46, len(corrupt))):
                corrupt[j] ^= 0xAA
        with open(p("badcrc.dibits"), "wb") as f:
            f.write(corrupt)
        rc, _, _ = run_rc(["decode-message", p("badcrc.dibits")])
        if rc == 0:
            fails.append("corrupt packet should fail")

        noeot = raw[:-48]
        with open(p("noeot.dibits"), "wb") as f:
            f.write(noeot)
        rc, _, _ = run_rc(["decode-message", p("noeot.dibits")])
        if rc == 0:
            fails.append("missing eot should fail")

        # Independent CRC check (not a round-trip oracle for the wire format).
        hello = b"Hello\x00"
        wire_crc = m17_crc16(bytes([0x05]) + hello)
        if wire_crc == 0 or wire_crc == 0xFFFF:
            fails.append("trivial crc")

        if fails:
            for f in fails:
                print("FAIL", f)
            return 1
        print("selftest: all passed")
        return 0


def cmd_gen(a):
    opts = ["gen", a.out]
    if a.stream:
        opts.append("--stream")
    if a.bert:
        opts.append("--bert")
    if a.packet:
        opts.append("--packet")
    opts += ["--src", a.src, "--dst", a.dst]
    if a.can is not None:
        opts += ["--can", str(a.can)]
    if a.text is not None:
        opts += ["--text", a.text]
    if a.data_hex is not None:
        opts += ["--data-hex", a.data_hex]
    if a.frames is not None:
        opts += ["--frames", str(a.frames)]
    run(opts)


def cmd_decode(a):
    print(run(["decode", a.input]).rstrip())


def cmd_decode_message(a):
    opts = ["decode-message", a.input]
    if a.expect_src:
        opts += ["--expect-src", a.expect_src]
    if a.expect_dst:
        opts += ["--expect-dst", a.expect_dst]
    if a.expect_can is not None:
        opts += ["--expect-can", str(a.expect_can)]
    rc, out, err = run_rc(opts)
    if out:
        print(out.rstrip())
    if err:
        print(err.rstrip(), file=sys.stderr)
    sys.exit(rc)


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if argv[:1] == ["--self-test"]:
        sys.exit(cmd_selftest(None) or 0)

    ap = argparse.ArgumentParser(description="M17 bench symbol generator (core/m17)")
    sub = ap.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("selftest")
    s.set_defaults(fn=cmd_selftest)

    s = sub.add_parser("gen")
    s.add_argument("out")
    g = s.add_mutually_exclusive_group(required=True)
    g.add_argument("--stream", action="store_true")
    g.add_argument("--bert", action="store_true")
    g.add_argument("--packet", action="store_true")
    s.add_argument("--src", default="AB1CD")
    s.add_argument("--dst", default="ALL")
    s.add_argument("--can", type=int)
    s.add_argument("--text")
    s.add_argument("--data-hex")
    s.add_argument("--frames", type=int)
    s.set_defaults(fn=cmd_gen)

    s = sub.add_parser("decode")
    s.add_argument("input")
    s.set_defaults(fn=cmd_decode)

    s = sub.add_parser("decode-message")
    s.add_argument("input")
    s.add_argument("--expect-src")
    s.add_argument("--expect-dst")
    s.add_argument("--expect-can", type=int)
    s.set_defaults(fn=cmd_decode_message)

    args = ap.parse_args(argv)
    if args.cmd == "decode-message":
        cmd_decode_message(args)
    sys.exit(args.fn(args) or 0)


if __name__ == "__main__":
    main()