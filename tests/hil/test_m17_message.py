# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""M17 text messages over the air (docs/spec/m17.md 8.2, 11).

test_m17_message (opt-in: --allow-rf --allow-service-stop --m17-mhz MHz; the radio and the bench both transmit):
on the VFO at --m17-mhz in M17 at the lowest power, the bench plays an M17 packet from tools/bench/m17tool.py gen
--packet (4800 baud, RRC roll-off 0.5, 2400 Hz outer deviation); the radio must show a fresh m17.packet RX snapshot
(source, length, cut, rx_hex). The radio then queues a reply with the debug link (RADIO op 44 cmd 4); the bench's 4FSK
receiver (sync 7777777755f7, 24 symbols) must capture preamble through EOT, and m17tool.py decode-message must report
ok=1 with CRC, addresses (src_raw, dst_raw, packet_hex) and text matching the distinct payloads each way. Skipped without
CONFIG_M17_PACKET, when the air interface lacks RX or TX (m17.fit), or when the bench board lacks fsk4 tx or rx.
"""
import os
import subprocess
import sys
import tempfile
import time

import pytest

from hil_config import rig
from rfoundry_hil import as_int, skip
from test_m17 import RADIO_CALL, STORED_CAN, m17_view

AIR_FIT_RX, AIR_FIT_TX = 0x1, 0x2
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
M17_TOOL = os.path.join(ROOT, "tools", "bench", "m17tool.py")
BAUD = 4800
DEV_HZ = 2400
M17_LSF_SYNC = "7777777755f7"
M17_SYNC_LEN = 24
FSK4_HEX_MAX = 8192
BENCH_CALL = "W1AW"
BENCH_TEXT = "bench: m17 hello"
RADIO_TEXT = "radio: m17 reply"
WAIT_TX_S = 20
M17_CRC_POLY = 0x5935
M17_CRC_INIT = 0xFFFF
M17_ADDR_ALPHABET = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-/."
M17_ADDR_BASE = 40


def m17_tool(args):
    return subprocess.run([sys.executable, M17_TOOL] + args, capture_output=True, text=True, cwd=ROOT)


def parse_message(stdout):
    out = {}
    for line in stdout.splitlines():
        if not line.startswith("m17-message/1 "):
            continue
        key, _, val = line.partition("=")
        out[key.split()[-1]] = val
    return out


def crc16(data):
    crc = M17_CRC_INIT
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ M17_CRC_POLY) if crc & 0x8000 else (crc << 1)
            crc &= 0xFFFF
    return crc


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
    assert pkt[:-2] == body, f"packet body {pkt[:-2].hex()} != {body.hex()}"
    assert got_crc == expect, f"packet crc {got_crc:04x} != {expect:04x}"


def assert_addrs(msg, src, dst):
    got_src = bytes.fromhex(msg.get("src_raw", ""))
    got_dst = bytes.fromhex(msg.get("dst_raw", ""))
    exp_src = base40_encode(src)
    exp_dst = base40_encode(dst)
    assert got_src == exp_src, f"src_raw {got_src.hex()} != {exp_src.hex()}"
    assert got_dst == exp_dst, f"dst_raw {got_dst.hex()} != {exp_dst.hex()}"


def decode_capture(path, **expect):
    args = ["decode-message", path]
    for key, val in expect.items():
        args += ["--" + key.replace("_", "-"), str(val)]
    r = m17_tool(args)
    return r.returncode, parse_message(r.stdout), r.stdout, r.stderr


def packed_dibits(job):
    parts = [bytes.fromhex(r["dibits"]) for r in job.records if r.get("dibits")]
    if not parts:
        skip("bench", "the bench sent no sliced dibits while the radio keyed the M17 message")
    return b"".join(parts)


def bench_to_radio(radio, sdr_bench, mhz, radio_call):
    v0 = radio.view(["m17"])
    seq0 = as_int(v0.get("m17.packet.rx_seq")) or 0
    pkts0 = as_int(v0.get("m17.packets")) or 0
    with tempfile.TemporaryDirectory() as tmp, sdr_bench.lease():
        path = os.path.join(tmp, "bench.dibits")
        try:
            r = m17_tool(["gen", path, "--packet", "--src", BENCH_CALL, "--dst", radio_call,
                          "--can", str(STORED_CAN), "--text", BENCH_TEXT])
        except OSError as e:
            pytest.fail(f"m17tool gen: {e}")
        if r.returncode != 0:
            pytest.fail(f"m17tool gen failed: {r.stderr.strip() or r.stdout}")
        with open(path, "rb") as f:
            data = f.read()
        n = len(data) * 4
        assert n <= FSK4_HEX_MAX, f"the bench message holds {n} symbols, past {FSK4_HEX_MAX}"
        tx = sdr_bench.tx_start("fsk4", mhz, None, max_s=n / BAUD + 2, symbols=n, hex=data.hex(), shape="rrc",
                                baud=BAUD, dev_hz=DEV_HZ, clip_hz=4064)
        tx.wait(n / BAUD + 30)
    time.sleep(0.5)
    v1 = radio.wait_for(
        lambda x: (as_int(x.get("m17.packet.rx_seq")) or 0) > seq0 and x.get("m17.packet.rx_src") == BENCH_CALL,
        8.0, sections=("m17",), what="the bench message in the M17 packet RX snapshot")
    wire_len = 1 + len(BENCH_TEXT.encode("utf-8"))
    assert as_int(v1.get("m17.packet.rx_len")) == wire_len, v1
    assert v1.get("m17.packet.rx_cut") == "0", v1
    expect_hex = bytes([0x05]) + BENCH_TEXT.encode("utf-8")
    got_hex = v1.get("m17.packet.rx_hex") or ""
    assert got_hex == expect_hex.hex(), f"rx_hex {got_hex!r}"
    v2 = radio.view(["m17"])
    assert (as_int(v2.get("m17.packets")) or 0) > pkts0, "m17.packets did not count a CRC-good packet"


def radio_to_bench(radio, sdr_bench, mhz, radio_call):
    radio.lowest_power()
    v0 = radio.view(["m17"])
    tx_seq0 = as_int(v0.get("m17.packet.tx_seq")) or 0
    link = radio.link
    with sdr_bench.lease():
        job = sdr_bench.fsk4_rx_start(mhz, WAIT_TX_S + 5, baud=BAUD, shape="rrc", sync=M17_LSF_SYNC,
                                      sync_len=M17_SYNC_LEN, nominal_dev_hz=DEV_HZ)
        time.sleep(0.4)
        link.m17_message(BENCH_CALL, RADIO_TEXT)
        v = radio.wait_for(lambda x: (as_int(x.get("m17.packet.tx_seq")) or 0) > tx_seq0, 3.0,
                           sections=("m17",), what="the message send admitted (m17.packet.tx_seq)")
        req = as_int(v.get("m17.packet.tx_seq"))
        assert v.get("m17.packet.tx_dst") == BENCH_CALL, v
        radio.wait_for(lambda x: x.get("state") != "tx", timeout=WAIT_TX_S, sections=("core",),
                       what="the radio finished keying")
        v = radio.wait_for(lambda x: as_int(x.get("m17.packet.tx_result")) not in (None, 0), WAIT_TX_S,
                           sections=("m17",), what="the M17 message TX terminal result")
        assert as_int(v.get("m17.packet.tx_result")) == 1, f"tx_result {v.get('m17.packet.tx_result')} (want sent)"
        assert as_int(v.get("m17.packet.tx_seq")) == req, "tx_seq changed before completion"
        done = job.wait(WAIT_TX_S + 25)
        if done.get("summary", {}).get("carrier") is False:
            skip("bench", f"the bench board heard no carrier while the radio keyed: {done.get('summary')}")
    with tempfile.TemporaryDirectory() as tmp:
        cap = os.path.join(tmp, "radio.dibits")
        raw = packed_dibits(job)
        with open(cap, "wb") as f:
            f.write(raw)
        rc, msg, out, err = decode_capture(cap, expect_src=radio_call, expect_dst=BENCH_CALL, expect_can=STORED_CAN)
        if rc != 0 and msg.get("reason") in ("incomplete packet", "no eot", "packet continuity"):
            skip("bench", f"the bench capture is incomplete (can't keep preamble through EOT): {msg.get('reason')}")
        assert rc == 0 and msg.get("ok") == "1", f"decode-message failed: {err.strip() or out.strip() or msg}"
        assert msg.get("crc") == "ok" and msg.get("eot") == "1", msg
        body = bytes.fromhex(msg.get("text_hex", ""))
        assert body == (RADIO_TEXT + "\0").encode("utf-8"), f"text_hex {body!r}"
        assert_packet_wire(msg, RADIO_TEXT)
        assert_addrs(msg, radio_call, BENCH_CALL)
        fec = msg.get("fec_disagreements", "?")
        print(f"\nm17 off-air decode: fec_disagreements={fec} bytes={msg.get('bytes')} frames={msg.get('frames')}")


@pytest.mark.feature("m17_packet")
@pytest.mark.rf
@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.sdr_rx
@pytest.mark.needs_switch("M17")
@pytest.mark.needs_switch("M17_PACKET")
@pytest.mark.needs_switch("VFO")
def test_m17_message(radio, sdr_bench, pytestconfig):
    mhz = rig(pytestconfig, "m17-mhz",
              "the M17 message test frequency (the radio's VFO; test_m17_message)")
    hz = round(mhz * 1e6)
    v = m17_view(radio)
    if "M17_PACKET" not in radio.features():
        skip("not_built", "built without CONFIG_M17_PACKET")
    if v.get("m17.packet.built") != "1":
        skip("not_built", "the firmware has no M17 packet view keys (CONFIG_M17_PACKET off on the radio)")
    fit = as_int(v.get("m17.fit")) or 0
    if (fit & AIR_FIT_RX) == 0:
        skip("no_hw", "the radio's air interface can't receive M17")
    if (fit & AIR_FIT_TX) == 0:
        skip("no_hw", "the radio's air interface can't transmit M17")
    modes = (sdr_bench._api_info().get("signal_modes") or {})
    if "fsk4" not in (modes.get("tx") or []):
        skip("bench", f"the bench board {sdr_bench.api} has no 4FSK transmit")
    if not sdr_bench.can_fsk4_rx():
        skip("bench", f"the bench board {sdr_bench.api_rx} has no 4FSK receive")
    radio_call = pytestconfig.getoption("--callsign") or RADIO_CALL
    with radio.vfo_session():
        L = radio.link
        try:
            L.vfo_set(hz)
            radio.wait_for(lambda x: x.get("rx_hz") == str(hz), what="VFO tuned")
            L.m17_settings(can=STORED_CAN, squelch=0, dst="ALL", call=radio_call)
            if radio.core().get("mode") != "m17":
                L.dmr_fields(mode="m17")
            radio.wait_for(lambda x: x.get("mode") == "m17" and x.get("m17.on") == "1", 3.0,
                           sections=("core", "m17"), what="M17 on the VFO")
            core = radio.core()
            assert core.get("rx_hz") == str(hz) and core.get("tx_hz") == str(hz), core
            bench_to_radio(radio, sdr_bench, mhz, radio_call)
            radio_to_bench(radio, sdr_bench, mhz, radio_call)
        finally:
            L.m17_bert("stop")
            L.m17_settings(can=0xFF, squelch=0xFF, dst=None, call=None)