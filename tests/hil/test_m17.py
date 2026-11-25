# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""M17 on the radio (docs/spec/m17.md 8.1, 10).

test_m17_mode_switch: the channel on view becomes M17 by a temporary mode change (the debug link's mode request,
debuglink.md 3.8 op 31); the m17 view section must show the engine running (`m17.on=1`, no new `begin_fails`).
Undoing the change brings the channel's own mode back with the engine stopped. Nothing is sent.

test_m17_bert_rx_from_bench, test_m17_bert_tx_off_air, test_m17_lsf_off_air: opt-in `--allow-rf`. The bench
transmitter and receiver are not wired into this file yet, so the bodies skip rather than transmit.
"""
import pytest

from rfoundry_hil import as_int, skip


def m17_view(radio):
    radio.go_home()
    if radio.core().get("hotspot") == "1":
        skip("state", "the radio is in hotspot mode")
    v = radio.view(["m17"])
    if v.get("m17.built") != "1":
        skip("not_built", "built without CONFIG_M17")
    return v


@pytest.mark.feature("m17")
def test_m17_mode_switch(radio):
    v = m17_view(radio)
    snap = radio.snapshot()
    mode0 = radio.core().get("mode")
    if mode0 == "m17":
        skip("state", "the channel is M17 already")
    fails0 = as_int(v.get("m17.begin_fails")) or 0
    try:
        radio.link.dmr_fields(mode="m17")
        try:
            v = radio.wait_for(lambda x: x.get("mode") == "m17" and x.get("m17.on") == "1", 3.0,
                               sections=("core", "m17"), what="M17 running on the channel")
        except AssertionError:
            v = radio.view(["core", "m17"])
            if v.get("mode") != "m17":
                skip("state", f"the radio kept mode {v.get('mode')} (a listen-only band, or the VFO without "
                              "both modes)")
            raise
        assert (as_int(v.get("m17.begin_fails")) or 0) == fails0, "the engine did not start (begin_fails)"
        assert radio.core().get("state") in ("idle", "rx")
    finally:
        if snap["vfo"] == "1":      # the VFO's mode is its own: restore() only drops a channel's mode for now
            radio.link.dmr_fields(mode=mode0)
        radio.restore(snap)
    radio.wait_for(lambda x: x.get("mode") == mode0 and x.get("m17.on") == "0", 3.0, sections=("core", "m17"),
                   what=f"mode {mode0} back, M17 stopped")


@pytest.mark.feature("m17")
@pytest.mark.radio_tx
def test_m17_bert_rx_from_bench():
    """Opt-in `--allow-rf`. The bench has no M17 transmitter wired here, so this skips instead of sending."""
    skip("opt_in", "the bench fixture has no M17 transmitter")


@pytest.mark.feature("m17")
@pytest.mark.radio_tx
def test_m17_bert_tx_off_air():
    """Opt-in `--allow-rf`. The bench receiver is not wired here, so this skips instead of keying."""
    skip("opt_in", "the bench fixture has no M17 receiver")


@pytest.mark.feature("m17")
@pytest.mark.radio_tx
def test_m17_lsf_off_air():
    """Opt-in `--allow-rf`. Would set the callsign over op 44, key, and expect the bench to decode the LSF."""
    skip("opt_in", "the bench fixture has no M17 receiver")
