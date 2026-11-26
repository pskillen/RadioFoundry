# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# The M17 bench tool (m17tool.c) on the host, from the firmware's frame codec and packet engine.
#   make -C tools/bench/m17           -> build/bench/m17tool
HERE    := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
ROOT    := $(abspath $(HERE)/../../..)
CORE    := $(ROOT)/core
OUT     ?= $(ROOT)/build/bench
CC      ?= cc
M17B    := $(CORE)/m17
CFLAGS  := -std=gnu99 -O2 -Wall -Wextra -Wshadow \
	   -DCONFIG_M17=1 -DCONFIG_M17_PACKET=1 -DCONFIG_AIR=1 \
	   -I$(CORE) -I$(ROOT)/hal '-D__attribute__(x)=' -U_FORTIFY_SOURCE
FRAME   := $(M17B)/m17_frame.c $(CORE)/fec/conv.c $(CORE)/fec/golay.c
ENGINE  := $(M17B)/m17_engine.c $(M17B)/m17_packet.c $(M17B)/m17_bert.c $(M17B)/m17_air.c \
	   $(M17B)/m17_rx.c $(CORE)/air/air_framer.c
SRC     := $(HERE)/m17tool.c $(FRAME) $(ENGINE)
HDRS    := $(wildcard $(M17B)/*.h $(CORE)/fec/*.h $(CORE)/air/*.h) $(ROOT)/hal/hal_air.h

$(OUT)/m17tool: $(SRC) $(HDRS)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -o $@ $(SRC)

.PHONY: clean
clean:
	rm -f $(OUT)/m17tool