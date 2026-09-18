# mqa-decode

An MQA file is an ordinary 44.1 or 48 kHz PCM file that hides a coded
copy of the audio's upper octave in the low bits of its samples. A
decoder reads that back and produces PCM at twice the rate. MQA calls
this the first unfold. The format was never published; what exists in
public is patents on the general idea.

## Quick start

```
make tools
build/mqad info  track.flac            # is it MQA, and what does it say about itself
build/mqad decode track.flac out.flac  # the first unfold, tags carried over
```

Input is FLAC or WAV; libFLAC is used if `pkg-config` finds it.

To make a stream rather than read one:

```
make -C encoder
encoder/build/mqae encode master96.flac carrier48.flac
```

## What is in the repository

| Directory | Contents |
| --- | --- |
| `include/mqa/`, `src/` | the library, one header per stage of the decode |
| `tools/` | `mqad` (scan, describe, decode), and the verification tools |
| `docs/` | the specification, a note on prior art, and a module map |
| `encoder/` | the encoder, a separate sub-project with its own README |
| `integration/` | GStreamer, VLC, FFmpeg |

## How the decode works

The carrier hides three things:

* a control channel, one bit per frame, in bits 8..15 of the
  samples: the stream's parameters, packetised.
* a data channel, the low byte of every sample: a scrambled byte
  stream framed into messages, which carry the coded residual and a
  correction for the carrier.
* a digit in every sample, for streams that keep their residual data
  in the carrier itself (16-bit MQA CDs do).

Per group of 32 carrier frames the decoder reads the control channel
480 frames ahead, descrambles and parses the data channel, refines the
group's carrier samples, decodes 32 residuals per channel from the
message payloads with an adaptive arithmetic coder and a lifting
predictor, and reconstructs 64 output frames with a lifting filter that
takes the carrier sample and the residual and produces two samples.
Every stage is stateful and every operation is 32-bit integer
arithmetic that wraps; the output depends on both.

```
carrier PCM  --intake-->  carrier rings (832 frames)
                             |                 |
              descrambler, 480 ahead      group of 32
                             |                 |
                      message parser      carrier digits
                     /       |       \         |
       parameter records  symbols  auxiliary   |
         + sync messages     |        |        |
                |            |     refinement <+  (samples in place)
                v            v        |
       stage set-up    residual stage <+-- digits
                             |
                        output stage: dither, reconstruction, clamp, CRC
                             |
                        64 output frames per channel
```

`docs/mqa-stage1-spec.md` specifies all of it, with the arithmetic
written out and test vectors. `docs/modules.md` maps each header onto
the section that defines it. `docs/prior-art.md` gives each stage its
published name: range coding, subtractive dither, Gerzon-Craven noise
shaping, the lifting scheme, quantisation index modulation.

## The mqad tool

```
build/mqad scan  DIR...                 # list the MQA files in a tree
build/mqad stats -f DIR...              # summarise them (-f reads whole files)
build/mqad info  [-v] [-m md.bin] FILE  # the stream and its packets; -m saves embedded metadata
build/mqad decode [-p] [-v] IN OUT      # decode; -p progress, -v formats
build/mqad decode -s IN OUT             # ... with the renderer signalling embedded
build/mqad decode -u 2 [-R] IN OUT      # both unfolds: render to the original rate
build/mqad decode -r layer.wav -x extra.wav IN OUT   # research outputs
```

Renderer signalling. A decoder finishes its output by nudging each frame by at
most one step so that the parity of the sample pair carries one bit of a short
message for the renderer in a DAC: identifier, rates, render filter and depth,
and one record per renderer profile, each with a CRC. To anything else it is
dither at about -138 dBFS, so it is off by default. With `-s` the output
matches the official decoder exactly.

The second unfold. `-u 2` renders as well, to the rate the stream
names as its original (2 or 4 times the unfolded rate), with the short
filter the stream chose. It recovers nothing: a renderer is an
interpolator whose filter the encoder picked, followed by a
requantiser to a coarser step with noise shaping. The requantiser only
loses precision, so it is off unless `-R` asks for it, and with it the
output is what an MQA renderer produces, sample for sample. A third
"unfold", in a DAC, is that DAC's own business.

Research outputs. `-r` writes the residual layer (P, Q) as carried,
one value per carrier sample. `-x` writes what the hidden data adds: the
decoded output minus a reconstruction of the same carrier with no
residuals and no refinement data, which is where the content above the
carrier's Nyquist comes from.

## The encoder

`encoder/` makes MQA streams. It shares the library and is tested by
decoding what it produces. A 6.5-minute 96 kHz track encodes to a 48 kHz
carrier in ten seconds; decoded back, it is 66.6 dB from the source,
with the error shaped away from where the music is.

It cannot authenticate a stream; that needs keys nobody outside MQA
has, and an MQA decoder ends an unsigned stream at its first
65536-frame block, so what it writes plays for 1.4 seconds in one and
in full in this library. Everything else in the format is there,
resync points included, so a player can seek. See `encoder/README.md`.

## Players

`integration/` has a GStreamer element (and an encoder element), a VLC filter,
a libavfilter filter for FFmpeg, with a README on what each stack allows. The
one rule they all obey: MQA lives in the low bits of the samples, so nothing
may scale them before the unfold.

## Open questions

1. Resampling in the passthrough. A group with no packet in it (the
   stream is over, or the file was never MQA) goes through a separate
   output path in the official decoder that can reconfigure a polyphase
   resampler. The rate-status machine is implemented; the resampling is
   not. No file to hand exercises it.
2. Authentication. The library does not verify: it treats every
   block as authenticating and derives the indicator from the datasync.
   The vendor decoder verifies, and ends a stream whose 65536-frame
   block has no verified packet to match. The outline of the scheme is
   public (RSA-3072 signatures under published public keys, hashes of
   the audio), so verifying, for the indicator's sake, is possible
   future work; the hashing of the audio was not recovered.
3. The alignment mode (mode 1) a packet start can leave the decoder
   in. The library reports `MQA_DECODER_UNSUPPORTED`.
4. The LSB correction's refresh. The +-1 correction is verified, but
   the source of its second priming segment is characterised, not
   verified.

