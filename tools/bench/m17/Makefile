# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# The M17 bench tool (m17tool.c) on the host, from the firmware's frame codec. tools/bench/m17tool.py runs this.
#   make -C tools/bench/m17           -> build/bench/m17tool
HERE    := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
ROOT    := $(abspath $(HERE)/../../..)
CORE    := $(ROOT)/core
OUT     ?= $(ROOT)/build/bench
CC      ?= cc
CFLAGS  := -std=gnu99 -O2 -Wall -Wextra -Wshadow -I$(CORE) -I$(ROOT)/hal '-D__attribute__(x)=' -U_FORTIFY_SOURCE
SRC     := $(HERE)/m17tool.c $(CORE)/m17/m17_frame.c $(CORE)/fec/conv.c $(CORE)/fec/golay.c
HDRS    := $(wildcard $(CORE)/m17/*.h $(CORE)/fec/*.h) $(ROOT)/hal/hal_air.h

$(OUT)/m17tool: $(SRC) $(HDRS)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -o $@ $(SRC)

.PHONY: clean
clean:
	rm -f $(OUT)/m17tool