# Putting this decoder into a media stack

Three questions decide how an MQA unfold fits into any player:

1. Can the stack load a plugin at runtime? GStreamer and VLC can, and
   there are working plugins here. FFmpeg cannot: it has no plugin ABI
   at all, so the filter here is one you build into a tree, and there is
   a pipe recipe that needs no rebuild.

2. Can the plugin double the sample rate? This is the awkward one. An
   unfold turns 44.1 kHz into 88.2 and 48 into 96, and each stack decides
   its output format at a different moment. GStreamer lets a filter
   settle its output caps late, which is what is needed. VLC
   accepts a rate-changing filter but has already opened the audio output
   at the source rate, and resamples back down. FFmpeg settles formats
   before any sample flows, so the filter must commit to the doubled rate
   up front.

3. Does anything touch the samples first? MQA lives in the *low bits*
   of the carrier. Any volume, gain, dither, mixing or resampling stage
   before the unfold destroys the stream. Float is fine as long as
   nothing scales it: a 24-bit sample survives a round trip through
   `float` exactly, and a volume of 0.999 does not. Put the unfold
   first and keep the path bit-exact up to it.

## The shared part

`common/mqa_adapter.[ch]` is the glue every host needs, so it is written
once: it sniffs for the stream, holds the output until it knows whether
this is MQA, and keeps the two-for-one bookkeeping so a filter's
timestamps stay simple. `common/adapter-demo.c` is a 60-line host that
reads and writes raw PCM; with `-p raw` its output is what `mqad decode`
produces, byte for byte, which is how the adapter is tested.

* Detection. Nothing in a file says it is MQA. The stream is found
  only when its magic turns up in the carrier's low bits, and on real
  material that can be well over a second in (69632 frames on one
  real track, where the stream itself opens late). The adapter
  sniffs for a configurable window, five seconds by default, before
  deciding.

* The run-in. Even in an MQA file, the frames before the stream opens
  are passed through, one output frame per input frame. In a doubled
  stream that stretch would run at twice speed, so by default the adapter
  repeats each of those frames (`hold`); it can drop them instead, or
  emit them once (`raw`) for bit-exactness with the reference decoder.

* Sample convention. The library wants 24-bit values left-justified in
  32-bit words. That is what a standard 16- or 24-bit to 32-bit
  conversion produces, so hosts mostly need no shifting at all. See the
  note on MQA CDs below.

## GStreamer

A shared library exporting a plugin descriptor, dropped anywhere on
`GST_PLUGIN_PATH`.

```
cd gstreamer && make
GST_PLUGIN_PATH=$PWD gst-inspect-1.0 mqadec
GST_PLUGIN_PATH=$PWD gst-launch-1.0 filesrc location=track.flac ! flacparse ! flacdec \
    ! mqadec ! audioconvert ! autoaudiosink
```

`make install` puts it in `~/.local/share/gstreamer-1.0/plugins`, which
GStreamer scans without being told.

The element accepts S16LE, S24_32LE and S32LE stereo at 44.1 or 48 kHz
and emits S32LE at the doubled rate. It does not negotiate its output
caps until it has sniffed the carrier, so a file that is not MQA comes
out unchanged at its own rate, which is what you want from an
element sitting in a general playback pipeline.

Properties: `signalling` (embed the renderer signalling, off by default),
`passthrough` (`hold`, `drop` or `raw`), `sniff-time` in ms.

### mqaenc, the other direction

The same directory builds `libgstmqaenc.so`, an element that goes the
other way: 88.2 or 96 kHz stereo in, an MQA carrier at half the rate out
(see `../encoder`). It cannot authenticate.

```
GST_PLUGIN_PATH=$PWD gst-launch-1.0 filesrc location=master.wav ! wavparse \
    ! audioconvert ! mqaenc ! wavenc ! filesink location=carrier.wav
```

The output rate is half the input's and known as soon as the caps
arrive, so nothing has to be held back. The properties are the stream's
parameters: `scale` (the residual quantiser, and so the bitrate), `bit`,
`level`, `orig-rate`, `variant`, `salt`, `render-filter`,
`render-depth`, `gain` and `auth-level`. `gain` defaults to a little
headroom, because a pipeline cannot measure a stream before encoding
it; the decoder puts it back exactly.

```
gst-launch-1.0 filesrc location=master.wav ! wavparse ! audioconvert \
    ! mqaenc ! mqadec ! audioconvert ! wavenc ! filesink location=back.wav
```

The adapter used to double everything it had queued when it found a
stream, decoded frames included, which is wrong for a stream that opens
at its first frame. Only the run-in is doubled now. `raw` still matches
`mqad decode` byte for byte on real material.

## VLC

VLC scans its plugin path at startup; no VLC rebuild is needed, only its plugin
headers (`libvlccore-dev`).

```
cd vlc && make                # or: VLC_SDK=/path/to/sdk/usr make
VLC_PLUGIN_PATH=$PWD vlc --audio-filter=mqa track.flac
```

```
  audio output debug: output 'f32l' 44100 Hz Stereo        <- opened before the filters
  audio filter debug: using audio filter module "mqa"
  audio output debug: conversion: 'f32l'->'f32l' 88200 Hz->88200 Hz
  audio resampler debug: using audio resampler module "samplerate"
```

VLC 3 opens the audio output from the *decoder's* format, before the user
filter chain exists. A filter that doubles the rate therefore has its
work resampled straight back down to 44.1 kHz. The unfold happens, and
what reaches the device is a resampled version of it.

To get the higher rate to the device, the doubled format has to be
declared before the audio output is created. In VLC that means a decoder
module (capability `audio decoder`) that decodes FLAC or WAV and unfolds
in one step, rather than an audio filter. That is a bigger piece of work
and is not attempted here; the filter shows where the limit is.

Note also that VLC hands user filters `f32l`, not integers. The module
accepts both and converts; a 24-bit carrier survives the float round trip
exactly, but a non-unity volume ahead of the filter would not. In the
chain observed here VLC's volume stage (`float_mixer`) runs *after* the
user filters, so the default configuration is safe.

## FFmpeg

FFmpeg has deliberately never had a plugin interface for codecs or filters:
everything is compiled in.  `af_ladspa` and `af_lv2` do host out-of-tree
plugins, but neither LADSPA nor LV2 can change the sample rate, so an unfold
cannot be expressed as one.

That leaves two routes.

### A filter built into the tree

`ffmpeg/af_mqa.c` is a libavfilter filter. Registering it takes three
lines, all in `ffmpeg/register.patch`:

```
libavfilter/allfilters.c   extern const AVFilter ff_af_mqa;
libavfilter/Makefile       OBJS-$(CONFIG_MQA_FILTER) += af_mqa.o mqa_adapter.o
```

then point configure at this library:

```
cp integration/ffmpeg/af_mqa.c integration/common/mqa_adapter.[ch] ffmpeg/libavfilter/
cd ffmpeg && patch -p1 < .../register.patch
./configure --extra-cflags="-I/path/to/mqa-decode/include" \
            --extra-ldflags="-L/path/to/mqa-decode" --extra-libs="-lmqadecode"
make
ffmpeg -i track.flac -af mqa -c:a flac out.flac
```

configure picks the filter up from `allfilters.c` by itself; there is
nothing to add to configure.

Because a filter graph settles its formats before any sample flows, the
filter declares the doubled rate up front and keeps two output frames per
input frame whatever the carrier holds. Applied to audio that is not MQA
it is therefore a sample-and-hold upsample, not a stream that runs fast.

### No rebuild: a pipe

Stock ffmpeg can carry the audio to and from the decoder. `-f s32le`
gives exactly the sample convention the library wants:

```
ffmpeg -v error -i track.flac -f s32le -c:a pcm_s32le - \
  | mqa-decode/build/adapter-demo -p raw -r 44100 \
  | ffmpeg -f s32le -ar 88200 -ac 2 -i - -c:a flac out.flac
```

The middle stage's output is byte-identical to `mqad decode` on the same file.
Mind the two rate arguments: the first `-r` is the carrier's rate, the second
`-ar` twice it.

## What about MQA CDs?

Some material is 16-bit, not 24. A 16-bit carrier is converted to 32-bit words
the ordinary way (the sample shifted up by 16), which puts the control channel
where the decoder looks for it.  What such a stream cannot have is the data
channel, which lives in the low byte a 16-bit carrier does not have, so those
streams carry their residual data in the carrier's own digits instead. The
library handles both; a host has to do nothing about it beyond not scaling the
samples.

## Licensing

The library and everything here is MIT. GStreamer and VLC are LGPL and load MIT
plugins without trouble; the GStreamer plugin declares `MIT/X11`. FFmpeg's LGPL
build links an MIT static library without affecting its own licence.
