# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Receive-only codec bench on a selected M17 channel. Opt in with --m17-bench."""
import pytest

from rfoundry_hil import skip


@pytest.mark.feature("m17_voice")
def test_m17_bench(radio, pytestconfig):
    if not pytestconfig.getoption("--m17-bench"):
        skip("opt_in", "select an M17 channel and pass --m17-bench")
    view = radio.view(["m17"])
    if "c2.scratch" not in view:
        skip("not_built", "built without CONFIG_M17_VOICE")
    if view.get("m17.on") != "1":
        skip("state", "select a running M17 channel")
    if view.get("m17.voice_mem") != "1":
        skip("state", "the M17 channel has no lent codec memory")

    result = radio.link.m17_bench()
    assert int(result["c2.dec_crc"], 16) == 0xB012FF9E
    assert int(result["c2.dec_cycles"]) > 0
    assert result["c2.enc"] == "0"
