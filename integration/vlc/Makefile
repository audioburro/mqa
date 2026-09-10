# An out-of-tree VLC plugin. VLC loads plugins from its plugin path at
# runtime, so this needs no VLC rebuild -- only its plugin headers
# (libvlccore-dev on Debian, vlc-devel elsewhere).
#
#     make
#     VLC_PLUGIN_PATH=$PWD vlc --audio-filter=mqa file.flac
#
# `make install` puts it in ~/.local/lib/vlc/plugins/audio_filter, which
# VLC scans as well.

CC       ?= cc
CFLAGS   ?= -O2 -g
CFLAGS   += -std=gnu11 -Wall -Wextra -fPIC -DPIC
PREFIX   ?= $(HOME)/.local
PLUGINDIR ?= $(PREFIX)/lib/vlc/plugins/audio_filter

TOP       = ../..
# VLC_SDK lets you build against headers unpacked somewhere else, e.g.
#   VLC_SDK=/tmp/sdk/usr make
ifdef VLC_SDK
VLC_CFLAGS := -I$(VLC_SDK)/include/vlc/plugins -I$(VLC_SDK)/include/vlc
VLC_LIBS   :=
else
VLC_CFLAGS := $(shell pkg-config --cflags vlc-plugin)
VLC_LIBS   := $(shell pkg-config --libs vlc-plugin)
endif

PLUGIN  = libmqa_plugin.so
SRCS    = mqa_vlc.c $(TOP)/integration/common/mqa_adapter.c
LIB     = $(TOP)/libmqadecode.a

all: $(PLUGIN)

$(LIB):
	$(MAKE) -C $(TOP)

$(PLUGIN): $(SRCS) $(LIB)
	$(CC) $(CFLAGS) $(VLC_CFLAGS) -DMODULE_STRING=\"mqa\" -D__PLUGIN__ \
		-I$(TOP)/include -I$(TOP)/integration/common \
		-shared -o $@ $(SRCS) $(LIB) $(VLC_LIBS) -lm

install: $(PLUGIN)
	mkdir -p $(PLUGINDIR)
	cp $(PLUGIN) $(PLUGINDIR)/

clean:
	rm -f $(PLUGIN)

.PHONY: all install clean
