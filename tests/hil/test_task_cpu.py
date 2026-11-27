# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Read two task CPU samples on a bench build with CONFIG_TASK_CPU=y."""

import time

import pytest

from rfoundry_debug import DebugLinkError, ST_BAD_ARG
from rfoundry_hil import skip


@pytest.mark.feature("task_cpu")
def test_task_cpu_window(radio):
    try:
        _, _, first = radio.link.view_section("tasks_cpu")
    except DebugLinkError as exc:
        if exc.status == ST_BAD_ARG:
            skip("not_built", "firmware without the tasks_cpu view section")
        raise
    time.sleep(2)
    _, _, second = radio.link.view_section("tasks_cpu")
    assert "truncated" not in second
    assert int(second["tasks_cpu.omitted"]) == 0
    cycles = int(second["tasks_cpu.cycles"])
    assert cycles > 0
    for task in ("IDLE", "host"):
        key = f"tcycles.{task}"
        delta = (int(second[key]) - int(first[key])) & 0xFFFFFFFF
        assert int(second[f"tcpu.{task}"]) == min(1000, delta * 1000 // cycles)
    assert second["tasks_cpu.idle"] == second["tcpu.IDLE"]
