# An out-of-tree GStreamer plugin: build it, drop it where GStreamer
# looks, and `gst-inspect-1.0 mqadec` finds it. Nothing needs to be
# installed system-wide.
#
#     make
#     GST_PLUGIN_PATH=$PWD gst-inspect-1.0 mqadec
#     GST_PLUGIN_PATH=$PWD gst-inspect-1.0 mqaenc
#
# `make install` puts it in ~/.local/share/gstreamer-1.0/plugins, which
# GStreamer scans by default.

CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=gnu99 -Wall -Wextra -fPIC
LIBDIR  ?= $(HOME)/.local/share/gstreamer-1.0/plugins

TOP      = ../..
GST_CFLAGS := $(shell pkg-config --cflags gstreamer-1.0 gstreamer-audio-1.0)
GST_LIBS   := $(shell pkg-config --libs gstreamer-1.0 gstreamer-audio-1.0)

PLUGIN  = libgstmqadec.so
SRCS    = gstmqadec.c $(TOP)/integration/common/mqa_adapter.c
LIB     = $(TOP)/libmqadecode.a

ENCPLUGIN = libgstmqaenc.so
ENCLIB    = $(TOP)/encoder/libmqaencode.a

all: $(PLUGIN) $(ENCPLUGIN)

$(LIB):
	$(MAKE) -C $(TOP)

$(ENCLIB):
	$(MAKE) -C $(TOP)/encoder

$(PLUGIN): $(SRCS) $(LIB)
	$(CC) $(CFLAGS) $(GST_CFLAGS) -I$(TOP)/include -I$(TOP)/integration/common \
		-shared -o $@ $(SRCS) $(LIB) $(GST_LIBS)

$(ENCPLUGIN): gstmqaenc.c $(ENCLIB) $(LIB)
	$(CC) $(CFLAGS) $(GST_CFLAGS) -I$(TOP)/include -I$(TOP)/encoder/include \
		-shared -o $@ gstmqaenc.c $(ENCLIB) $(LIB) $(GST_LIBS) -lm

install: $(PLUGIN) $(ENCPLUGIN)
	mkdir -p $(LIBDIR)
	cp $(PLUGIN) $(ENCPLUGIN) $(LIBDIR)/

clean:
	rm -f $(PLUGIN) $(ENCPLUGIN)

.PHONY: all install clean
