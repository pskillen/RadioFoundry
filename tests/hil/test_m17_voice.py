# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""M17 voice receive over the air (docs/spec/m17.md 8.1, 11).

test_m17_voice_rx (opt-in: --allow-rf --m17-mhz MHz; only the bench transmits): on
the VFO at --m17-mhz in M17 with codec memory available (`m17.voice_mem=1`), the bench plays packed 4FSK dibits (4800 baud,
RRC roll-off 0.5, 2400 Hz outer deviation) from a voice stream file. The file must carry a plain-voice link setup
frame matching the VFO CAN filter, Codec 2 payload frames for known speech, and an end marker. `m17tool.py gen --stream` emits only zeroed TYPE and dummy
payload, so it is not a compatible source yet.

Set `RFOUNDRY_HIL_M17_VOICE_DIBITS` to such a file (the same packed dibit format as `m17tool.py gen` and
`test_m17_message.py`). Until the repository ships one, the test skips with kind `opt_in` and the owner can tune the selected image to a compatible M17 source, confirm speech, and compare the same counters manually.

Pass (automated): `m17v.rx_frames` increases by at least `MIN_RX_FRAMES` with `m17v.rx_lost=0`; `m17.voice_mem` stays
1 before and after; lost and underrun counters are printed. Pass (owner): intelligible speech from the speaker (counters
alone prove receive-path activity, not audio quality).
"""
import os

import pytest

from hil_config import rig
from rfoundry_hil import as_int, skip
from test_m17 import m17_view

AIR_FIT_RX = 0x1
BAUD = 4800
DEV_HZ = 2400
FSK4_HEX_MAX = 8192
MIN_RX_FRAMES = 16
VOICE_DIBITS_ENV = "RFOUNDRY_HIL_M17_VOICE_DIBITS"


def voice_dibits():
    path = os.environ.get(VOICE_DIBITS_ENV)
    if not path:
        skip("opt_in", "no compatible M17 voice stream dibits: set "
                      f"{VOICE_DIBITS_ENV} to packed voice dibits (plain-voice LSF and Codec 2 payload); "
                      "m17tool.py gen --stream is not voice-capable yet")
    path = os.path.expanduser(path)
    if not os.path.isfile(path):
        skip("opt_in", f"M17 voice dibits file missing or not a file: {path}")
    return path


def m17v_counts(view):
    counts = {k: as_int(view.get(k)) for k in ("m17v.rx_frames", "m17v.rx_lost", "m17v.rx_underruns")}
    assert all(value is not None for value in counts.values()), view
    return counts


@pytest.mark.feature("m17_voice", owner="confirm intelligible speech from the speaker after the counters pass")
@pytest.mark.rf
@pytest.mark.bench_service
@pytest.mark.needs_switch("M17")
@pytest.mark.needs_switch("M17_VOICE")
@pytest.mark.needs_switch("VFO")
def test_m17_voice_rx(radio, sdr_bench, pytestconfig):
    path = voice_dibits()
    mhz = rig(pytestconfig, "m17-mhz",
              "the M17 voice test frequency (the radio's VFO; test_m17_voice_rx)")
    hz = round(mhz * 1e6)
    m17_view(radio)
    with open(path, "rb") as f:
        data = f.read()
    if not data:
        skip("state", f"the voice dibits file is empty: {path}")
    n = len(data) * 4
    if n > FSK4_HEX_MAX:
        skip("state", f"the voice stream holds {n} symbols, past {FSK4_HEX_MAX}")

    with radio.vfo_session():
        radio.link.vfo_set(hz)
        radio.wait_for(lambda x: x.get("rx_hz") == str(hz), what="VFO tuned")
        radio.link.dmr_fields(mode="m17")
        v0 = radio.wait_for(lambda x: x.get("mode") == "m17" and x.get("m17.on") == "1", 3.0,
                            sections=("core", "m17"), what="M17 on the VFO")
        if (as_int(v0.get("m17.fit")) or 0) & AIR_FIT_RX == 0:
            skip("no_hw", "the radio's air interface can't receive M17")
        if v0.get("m17.voice_mem") != "1":
            skip("state", "the tuned M17 channel has no codec memory")
        c0 = m17v_counts(v0)
        with sdr_bench.lease():
            tx = sdr_bench.tx_start("fsk4", mhz, None, max_s=n / BAUD + 2, symbols=n, hex=data.hex(),
                                    shape="rrc", baud=BAUD, dev_hz=DEV_HZ, clip_hz=4064)
            done = tx.wait(n / BAUD + 30)
            assert not done.get("cut"), done
        v1 = radio.wait_for(lambda x: (as_int(x.get("m17v.rx_frames")) or 0) >= c0["m17v.rx_frames"] + MIN_RX_FRAMES
                            and x.get("state") == "idle", 5.0, sections=("core", "m17"),
                            what="M17 voice received and the radio back to idle")
        c1 = m17v_counts(v1)
        print(f"\nm17 voice rx from {path}: symbols {n}, m17v {c0} -> {c1}")
        assert v1.get("m17.voice_mem") == "1", v1
        assert c1["m17v.rx_lost"] == c0["m17v.rx_lost"], f"M17 lost frames changed: {c0} -> {c1}"
