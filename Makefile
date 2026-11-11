# SPDX-License-Identifier: GPL-2.0

CC      ?= gcc
CFLAGS  := -O2 -g -Wall -muintr -mgeneral-regs-only
LDLIBS  :=

PROGS := ctrl-c-uintr-test tty_isig_latency

.PHONY: all clean

all: $(PROGS)

ctrl-c-uintr-test: ctrl-c-uintr-test.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

tty_isig_latency: tty_isig_latency.c uintr_common.h
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f $(PROGS)
