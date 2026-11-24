#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""
m17tool: M17 symbol files from the firmware's frame codec (core/m17), built for the host from tools/bench/m17.

    m17tool.py selftest
    m17tool.py gen OUT.dibits --bert --frames N
    m17tool.py gen OUT.dibits --stream --src AB1CD --dst ALL --frames N
    m17tool.py decode IN.dibits
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


def build():
    srcs = [os.path.join(SRC, "m17tool.c"), os.path.join(SRC, "Makefile")]
    for d in ("core/m17", "core/fec"):
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
        nums = [int(line.split()[-1]) for line in out.splitlines() if line.startswith("stream frame")]
        if nums != [0, 1, 2]:
            fails.append("stream frame numbers %r" % nums)

        run(["gen", p("bert.dibits"), "--bert", "--frames", "2"])
        if open(p("bert.dibits"), "rb").read(1)[0] != 0xDD:
            fails.append("preamble bert")
        bout = run(["decode", p("bert.dibits")])
        if "394" not in bout:
            fails.append("bert bits: " + bout.strip().splitlines()[-1])

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
    opts += ["--src", a.src, "--dst", a.dst, "--frames", str(a.frames)]
    run(opts)


def cmd_decode(a):
    print(run(["decode", a.input]).rstrip())


def main(argv=None):
    ap = argparse.ArgumentParser(description="M17 bench symbol generator (core/m17)")
    sub = ap.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("selftest")
    s.set_defaults(fn=cmd_selftest)

    s = sub.add_parser("gen")
    s.add_argument("out")
    g = s.add_mutually_exclusive_group(required=True)
    g.add_argument("--stream", action="store_true")
    g.add_argument("--bert", action="store_true")
    s.add_argument("--src", default="AB1CD")
    s.add_argument("--dst", default="ALL")
    s.add_argument("--frames", type=int, default=1)
    s.set_defaults(fn=cmd_gen)

    s = sub.add_parser("decode")
    s.add_argument("input")
    s.set_defaults(fn=cmd_decode)

    args = ap.parse_args(argv)
    sys.exit(args.fn(args) or 0)


if __name__ == "__main__":
    main()