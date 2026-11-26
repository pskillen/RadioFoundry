# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""M17 on the radio (docs/spec/m17.md 8.1, 10).

test_m17_mode_switch: the channel on view becomes M17 by a temporary mode change (the debug link's mode request,
debuglink.md 3.8 op 31); the m17 view section must show the engine running (`m17.on=1`, no new `begin_fails`).
Undoing the change brings the channel's own mode back with the engine stopped. Nothing is sent.

test_m17_stored_channel: opt-in `--allow-codeplug-write`. Writes a kind-6 channel tag and the M17 callsign into the
codeplug settings area over CPS (m17-codeplug.md), receive only, and checks the radio runs them: M17 mode, the stored
CAN with the squelch off, the destination and the callsign in force (`m17.run.*`). The codeplug is put back after.
Nothing is sent.

test_m17_callsign_on_radio: opt-in `--allow-codeplug-write`. Types a callsign on Settings > My radio and checks
`m17.run.call`. The radio region is restored after. Nothing is sent.

test_m17_channel_on_radio: opt-in `--allow-codeplug-write`. Plants a receive-only kind-6 channel, then sets CAN 15
in the channel editor and checks `m17.run.can`. The codeplug is put back after. Nothing is sent.

test_m17_talk_to_on_radio: opt-in `--allow-codeplug-write`. Chooses INFO and Default in Quick on the stored
receive-only M17 channel. The codeplug is put back after. Nothing is sent.

test_m17_bert_rx_from_bench, test_m17_bert_tx_off_air, test_m17_lsf_off_air: opt-in `--allow-rf`. The bench
transmitter and receiver are not wired into this file yet, so the bodies skip rather than transmit.
"""
import time

import pytest

from rfoundry_hil import LONG, as_int, skip


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


# --- A stored M17 channel (m17-codeplug.md, channel-modes.md 2) ---

KIND_M17 = 6
KEY_CH_TAG = 0x0400         # codeplug-settings.md 3: channel tags, 0x0400 + channel - 1
KEY_CALL = 0x0A00 + KIND_M17    # the radio-wide record: the callsign (channel-modes.md 5.5)
STORED_CAN = 3
STORED_DST = "ECHO"
STORED_DST_BYTES = bytes.fromhex("0000000ED87D")
RADIO_CALL = "AB1CD"


def make_m17(img, number):
    """Channel number of img made a stored M17 channel: the stand-in record, then its tag. Receive only, the CAN
    squelch off, and the radio-wide callsign record"""
    import cps_client
    from test_nxdn import new_area
    from test_p25 import area_with, name_check
    _, a = cps_client.channel_addr(number)
    img[a + 0x18] = 1                   # DMR
    img[a + 0x33] |= 0x04               # receive only: the stand-in, whatever the tag allows
    tag = bytes([KIND_M17, 0xFF]) + name_check(img[a:a + 16]).to_bytes(2, "little") + bytes([0])
    block = bytes([0x10 | STORED_CAN]) + STORED_DST_BYTES
    lo, hi = cps_client.SETTINGS_AREA
    area = img[lo:hi]
    if cps_client.area_state(area) != "valid":
        area = new_area(hi - lo)
    area = area_with(area, KEY_CH_TAG + number - 1, tag + block)
    img[lo:hi] = area_with(area, KEY_CALL, RADIO_CALL.encode() + bytes(10 - len(RADIO_CALL)))


@pytest.fixture
def stored_m17(radio, backup_dir):
    m17_view(radio)
    if "M17" not in radio.features():
        skip("not_built", "M17 isn't in the radio's feature list")
    import cps_client
    from test_cps import codeplug_edited, pick_channel, read_backup
    regions = cps_client.REGIONS["radio"]      # with the settings area, where the tag goes
    probe, _ = read_backup(radio, backup_dir, "m17probe", regions)
    lo, hi = cps_client.SETTINGS_AREA
    if cps_client.area_state(probe[lo:hi]) != "valid":
        skip("state", "the codeplug settings area isn't valid: the restore couldn't take the tag back")
    core = radio.core()
    number = pick_channel(radio, probe)
    if core.get("vfo") == "1" or number != as_int(core.get("channel_number"), 0):
        skip("state", "the radio isn't on a stored channel")
    if core.get("am") == "1" or core.get("bcast") == "1":
        skip("state", "the channel is in a listen-only band: no M17 there")
    with codeplug_edited(radio, backup_dir, lambda img: make_m17(img, number), "m17", regions):
        v = radio.wait_for(lambda x: x.get("ch.kind") == str(KIND_M17) and x.get("m17.on") == "1", 5.0,
                           sections=("core", "channel", "m17"), what="the stored M17 channel running")
        yield v
    radio.wait_for(lambda x: x.get("ch.kind") == "0" and x.get("m17.on") != "1", 5.0,
                   sections=("core", "channel", "m17"), what="the channel back as it was, M17 stopped")


@pytest.mark.feature("m17")
@pytest.mark.codeplug_write
def test_m17_stored_channel(stored_m17):
    v = stored_m17
    assert v.get("mode") == "m17", v
    assert v.get("m17.run.can") == str(STORED_CAN) and v.get("m17.run.squelch") == "0", v
    assert v.get("m17.run.dst") == STORED_DST, v
    assert v.get("m17.run.call") == RADIO_CALL, v


@pytest.mark.feature("m17")
@pytest.mark.codeplug_write
def test_m17_talk_to_on_radio(radio, stored_m17):
    from test_ui import wait_screen

    assert stored_m17.get("m17.run.dst") == STORED_DST, stored_m17
    radio.go_home()
    link = radio.view(["link"])
    if link.get("ui.dual") == "1":
        skip("state", "the stored-channel fixture requires single-channel Home")
    key = radio.unlock_key(link)
    if key == "none":
        skip("no_hw", "the fixture needs a Quick or Back key")
    if key == "quick":
        radio.press("quick")
    else:
        radio.press("ok", LONG)
    wait_screen(radio, "quick")

    for destination, row in (("INFO", 3), (STORED_DST, 0)):
        radio.press("8")
        picker = wait_screen(radio, "m17-talk-to")
        assert picker.get("ui.focus") == "0" and as_int(picker.get("ui.count"), 0) >= 5, picker
        for _ in range(row):
            radio.press("down")
        radio.press("ok")
        wait_screen(radio, "quick")
        v = radio.wait_for(lambda x: x.get("m17.run.dst") == destination, 5.0,
                           sections=("m17",), what=f"M17 destination {destination}")
        assert v.get("m17.run.can") == str(STORED_CAN) and v.get("m17.run.squelch") == "0", v
        assert v.get("m17.run.call") == RADIO_CALL, v
    radio.press("back")
    wait_screen(radio, "home")


@pytest.mark.feature("m17")
@pytest.mark.codeplug_write
def test_m17_channel_on_radio(radio, backup_dir):
    """The channel editor saves CAN on a stored M17 channel. Nothing is sent."""
    m17_view(radio)
    if "CHANNEL_EDIT" not in radio.features() and "UI_SETTINGS" not in radio.features():
        skip("not_built", "built without the channel editor")
    import cps_client
    from test_cps import codeplug_edited, pick_channel, read_backup
    regions = cps_client.REGIONS["radio"]
    probe, _ = read_backup(radio, backup_dir, "m17chan", regions)
    lo, hi = cps_client.SETTINGS_AREA
    if cps_client.area_state(probe[lo:hi]) != "valid":
        skip("state", "the codeplug settings area isn't valid: the restore couldn't take the tag back")
    core = radio.core()
    number = pick_channel(radio, probe)
    if core.get("vfo") == "1" or number != as_int(core.get("channel_number"), 0):
        skip("state", "the radio isn't on a stored channel")
    with codeplug_edited(radio, backup_dir, lambda img: make_m17(img, number), "m17ui", regions):
        radio.go_home()
        for key in ("ok", "5", "ok", "down", "down", "down", "ok", "1", "5", "ok"):
            radio.press(key)
            time.sleep(0.3)
        try:
            radio.wait_for(lambda v: v.get("ui.screen") == "channel", 3.0, sections=("link",),
                           what="the channel editor")
        except AssertionError:
            skip("state", "the channel editor didn't open")
        radio.press("ok", LONG)
        radio.wait_for(lambda x: x.get("m17.run.can") == "15", 5.0, sections=("m17",),
                       what="CAN 15 from the channel editor")


@pytest.mark.feature("m17")
@pytest.mark.codeplug_write
def test_m17_callsign_on_radio(radio, backup_dir):
    """Settings > My radio > M17 callsign writes the radio-wide record. Nothing is sent."""
    m17_view(radio)
    if "UI_SETTINGS" not in radio.features():
        skip("not_built", "built without CONFIG_UI_SETTINGS")
    if radio.core().get("hotspot") == "1":
        skip("state", "the radio is in hotspot mode")
    import cps_client
    from test_cps import read_backup, restore_backup
    regions = cps_client.REGIONS["radio"]
    backup, path = read_backup(radio, backup_dir, "m17call", regions)
    mode0 = radio.core().get("mode")
    radio.go_home()
    try:
        for key in ("ok", "7", "0", "down", "down", "ok"):
            radio.press(key)
        try:
            radio.wait_for(lambda v: v.get("ui.screen") == "m17-callsign", 3.0, sections=("link",),
                           what="the M17 callsign editor")
        except AssertionError:
            skip("state", "M17 callsign didn't open (no keypad, or the row isn't there)")
        # W 1 A W. Past the multi-tap window, so the keys don't cycle one character
        radio.press("9")
        time.sleep(1.2)
        radio.press("1", LONG)
        time.sleep(1.2)
        radio.press("2")
        time.sleep(1.2)
        radio.press("9")
        time.sleep(1.2)
        radio.press("ok")
        if radio.core().get("mode") != "m17":
            radio.link.dmr_fields(mode="m17")
            try:
                radio.wait_for(lambda x: x.get("mode") == "m17", 3.0, sections=("core",), what="M17 mode")
            except AssertionError:
                v = radio.core()
                skip("state", f"the radio kept mode {v.get('mode')}")
        radio.wait_for(lambda x: x.get("m17.run.call") == "W1AW", 5.0, sections=("m17",),
                       what="the typed callsign in force")
    finally:
        if radio.core().get("mode") != mode0:
            radio.link.dmr_fields(mode=mode0)
        radio.go_home()
        restore_backup(radio, backup, path, backup, regions=regions)
