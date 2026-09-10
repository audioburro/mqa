# The adapter, and the smallest possible host for it.
#
#     make
#     ./adapter-demo -p raw -r 44100 < in.raw > out.raw

CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c99 -Wall -Wextra -pedantic
TOP      = ../..

all: adapter-demo

$(TOP)/libmqadecode.a:
	$(MAKE) -C $(TOP)

adapter-demo: adapter-demo.c mqa_adapter.c mqa_adapter.h $(TOP)/libmqadecode.a
	$(CC) $(CFLAGS) -I$(TOP)/include -I. -o $@ adapter-demo.c mqa_adapter.c $(TOP)/libmqadecode.a

clean:
	rm -f adapter-demo

.PHONY: all clean
