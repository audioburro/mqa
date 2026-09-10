# The encoder: a separate sub-project that links the decoding library,
# both to share its constants and to be tested against it.
#
#     make          build libmqaencode.a and the tools
#     make check    run the tests

CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c99 -Wall -Wextra -pedantic -fPIC -Iinclude -I../include
TOP      = ..

LIB      = libmqaencode.a
SRCS     = $(wildcard src/*.c)
OBJS     = $(SRCS:.c=.o)
TESTS    = $(patsubst tests/%.c,build/%,$(wildcard tests/test_*.c))
TOOLS    = $(patsubst tools/%.c,build/%,$(wildcard tools/*.c))
DECLIB   = $(TOP)/libmqadecode.a

FLAC_CFLAGS := $(shell pkg-config --cflags flac 2>/dev/null && echo -DHAVE_FLAC)
FLAC_LIBS   := $(shell pkg-config --libs flac 2>/dev/null)

all: $(LIB) $(TOOLS)

$(DECLIB):
	$(MAKE) -C $(TOP)

$(LIB): $(OBJS)
	$(AR) rcs $@ $^

src/%.o: src/%.c $(wildcard include/mqae/*.h)
	$(CC) $(CFLAGS) -c -o $@ $<

build:
	@mkdir -p build

build/test_%: tests/test_%.c $(LIB) $(DECLIB) | build
	$(CC) $(CFLAGS) -Itests -o $@ $< $(LIB) $(DECLIB) -lm

build/%: tools/%.c $(LIB) $(DECLIB) | build
	$(CC) $(CFLAGS) $(FLAC_CFLAGS) -I$(TOP)/tools -o $@ $< $(TOP)/tools/audio_io.c \
		$(LIB) $(DECLIB) $(FLAC_LIBS) -lm

check: $(TESTS)
	@fail=0; for t in $(TESTS); do ./$$t || fail=1; done; \
	if [ $$fail = 0 ]; then echo "ENCODER TESTS PASSED"; else echo "ENCODER TESTS FAILED"; exit 1; fi

clean:
	rm -f $(LIB) $(OBJS)
	rm -rf build

.PHONY: all check clean tools

tools: $(TOOLS)
