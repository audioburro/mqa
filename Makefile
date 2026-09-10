# mqa-decode: a from-scratch, portable C implementation of MQA stage-1
# ("first unfold") decoding.

CC      ?= cc
AR      ?= ar
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c99 -Wall -Wextra -pedantic -Iinclude
LDFLAGS ?=

LIB      = libmqadecode.a
SRCS     = $(wildcard src/*.c)
OBJS     = $(SRCS:.c=.o)

all: $(LIB)

$(LIB): $(OBJS)
	$(AR) rcs $@ $^

src/%.o: src/%.c $(wildcard include/mqa/*.h)
	$(CC) $(CFLAGS) $(OPTFLAGS) -c -o $@ $<

build:
	@mkdir -p build

TOOLS    = build/mqad

# mqad reads and writes FLAC through libFLAC when pkg-config finds it
FLAC_CFLAGS := $(shell pkg-config --cflags flac 2>/dev/null && echo -DHAVE_FLAC)
FLAC_LIBS   := $(shell pkg-config --libs flac 2>/dev/null)

build/spec-vectors: tools/spec-vectors.c $(LIB) | build
	$(CC) $(CFLAGS) -o $@ tools/spec-vectors.c $(LIB)

build/spec-tables: tools/spec-tables.c $(LIB) | build
	$(CC) $(CFLAGS) -o $@ tools/spec-tables.c $(LIB)

build/mqad: tools/mqad.c tools/audio_io.c tools/audio_io.h $(LIB) | build
	$(CC) $(CFLAGS) $(OPTFLAGS) $(FLAC_CFLAGS) -o $@ tools/mqad.c tools/audio_io.c $(LIB) $(FLAC_LIBS)

tools: $(TOOLS)

check:
	$(MAKE) -C encoder check

clean:
	rm -f $(LIB) $(OBJS)
	rm -rf build

.PHONY: all check clean tools
