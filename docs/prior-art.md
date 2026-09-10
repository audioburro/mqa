# What this decoder's parts are called elsewhere

Almost every stage of the stage-1 decode is a published technique, and
several were published by the people who designed MQA. This note maps
the library's modules onto those names, so that a reader who knows audio
coding can recognise the pieces.

Three levels of confidence are used:

* **identified** -- the constants or structure match a published
  definition exactly, and can be checked from this repository;
* **likely** -- the structure is that of a named technique, with the
  usual freedom in parameters;
* **resembles** -- a family resemblance, not a claim of derivation.

## The system

MQA came out of work by Bob Stuart and Peter Craven, introduced in their
2014 AES paper *A Hierarchical Approach to Archiving and Distribution*.
The encoder side is described in the patent family *Digital encapsulation
of audio signals* (MQA Limited; inventors Peter Graham Craven and Malcolm
James Law), which covers burying encoded ultrasonic content in the lower
bits beneath a recording's noise floor, and the design of the short
downsampling and upsampling filters the scheme depends on.

Both authors have earlier work that the decoder keeps meeting:
Meridian Lossless Packing, published as *The MLP Lossless Compression
System for PCM Audio* (Gerzon, Craven, Stuart, Law and Wilson, JAES 2004),
and *Optimal Noise Shaping and Dither of Digital Signals* (Gerzon and
Craven, AES 87th Convention, 1989). MLP is the direct ancestor of the
lossless-buried-data idea; the 1989 paper is the direct ancestor of how
the decoder requantises what it sends on.

Earlier reverse engineering exists: Måns Rullgård's `mqa` repository, which documents the control bitstream this library's
scanner also parses, and the `MQA_identifier` project, which detects MQA
and its original sample rate.

## The parts

| This library | What it does | Known as | Confidence |
| --- | --- | --- | --- |
| `bitstream.h` | a control channel hidden in one bit of the sample pair | lossless buried data (MLP, MQA); *Digital encapsulation of audio signals* | identified |
| `entropy.h` range coder | 32-bit interval, renormalised a byte at a time while `range <= 2^24` | range coding (Martin 1979; Pasco 1976), byte-wise renormalisation; carryless variants after Subbotin | identified |
| `residual_stage.h` digit coder | the same coder over a non-binary alphabet (radix 243, 256 or 64) | range coding's defining property: "coding is done with digits in any base" | identified |
| `entropy.h` quantiser | decoded values quantised to a step with a pseudo-random offset that is added back | subtractive dither | identified |
| `conditioner.h` | requantisation with subtractive dither and error feedback through a short filter | Gerzon-Craven noise shaping | likely |
| `lifting.h`, `predictor.h` | reversible two-step butterflies with a gain term | the lifting scheme (Sweldens 1996); integer-to-integer transforms (Calderbank, Daubechies, Sweldens, Yeo 1998) | likely |
| `predictor.h` adaptation | four filter taps updated per block against a history of magnitudes | backward-adaptive LMS prediction, as in lossless audio coders (Monkey's Audio, MPEG-4 ALS RLS-LMS) | resembles |
| `reconstruct.h` | 2x interpolation as a predictive lifting filter over carrier and residual | MLP's IIR lossless prediction and matrixing, run as a synthesis filter | resembles |
| `recon2.h` | the same, with the interpolation split into a short 4-tap section and a 14-tap shaping section ahead of the lifting steps | a cascaded (two-stage) interpolation filter, as in Crochiere and Rabiner's multirate design; the split into a low-order predictor and a longer shaping filter is MLP's arrangement | resembles |
| `refine.h` | per-sample corrections coded with a two-radix range coder | range coding again, with a data-dependent alphabet | likely |
| `watermark.h` | one message bit per frame, carried by the parity of the sample pair, embedded by choosing between quantiser offsets | quantisation index modulation / dither modulation (Chen and Wornell 2001); at its simplest, parity LSB steganography | likely |
| `descrambler.h` | the data channel whitened by a keyed generator | stream-cipher whitening; no standard identified | resembles |
| `crc32.h` | checksum of the packet records and sample blocks | CRC-32, reflected polynomial `0xEDB88320` (normal `0x04C11DB7`), the ITU-T V.42 / Ethernet one | identified |
| `crc24.h` | one 24-bit register, clocked eight bits at a time, used twice: to select the LSB correction during decoding and to spread the signalling's nudges | a Galois LFSR, reflected `0x8100C9` (normal `0x930081`); not one of the catalogued CRC-24 polynomials (OpenPGP `0x864CFB`, 3GPP `0x800063`, `0xB2B117`) | identified as an LFSR, unidentified as a standard |
| `lcg.h` seeds | `state * 1664525 + 1013904223` | the Numerical Recipes linear congruential generator (`ranqd1`) | identified |
| `lcg.h` dither | `state * 0x17385CA9 + 0x47502932` | a linear congruential generator with constants I could not attribute | unidentified |

## What looks specific to MQA

Two things did not map onto anything I could name.

* **The carrier's own digits as an entropy-coder alphabet.** For carrier
  classes with more than one digit level, the decoder quantises each
  carrier sample to a digit (3, 4 or 8 values), packs several digits per
  byte, and feeds that as a non-binary symbol stream to a second range
  coder that supplies part of the residual data. The audio the listener
  hears and the side channel that reconstructs it are the same numbers
  read two ways. The pieces are standard; I did not find the
  arrangement described elsewhere.

* **Renderer signalling that survives requantisation.** The output's
  message is carried by the parity of each frame, chosen through a table
  of nudges of at most one step, so a renderer downstream recovers it
  from the audio itself with no side channel and no framing. This is
  quantisation index modulation in miniature, applied to signalling
  rather than to watermarking for provenance.

## Checking these claims

Everything marked *identified* can be verified from this repository:

* the range coder's renormalisation and base: `src/entropy.c`
  (`range_refill`, `mqa_range_normalize`);
* the polynomials: `src/crc32.c` and `src/watermark.c`, whose
  `build_table` regenerates both tables from the polynomials given above
  and reproduces the vendor's own tables byte for byte;
* the generator constants: `include/mqa/lcg.h`, and
  `include/mqa/crc24.h`, whose polynomial serves both the LSB correction
  and the renderer signalling;
* the buried control channel: `include/mqa/bitstream.h`, and
  `build/mqad info FILE`, which prints the packets it finds.

## Sources

* Stuart and Craven, *A Hierarchical Approach to Archiving and
  Distribution*, AES 137th Convention, 2014.
* Craven and Law, *Digital encapsulation of audio signals*, MQA Limited:
  <https://patents.google.com/patent/US10867614B2/en>,
  <https://patents.google.com/patent/WO2014108677A1/en>.
* Gerzon, Craven, Stuart, Law and Wilson, *The MLP Lossless Compression
  System for PCM Audio*, JAES 52(3), 2004:
  <https://www.semanticscholar.org/paper/The-MLP-Lossless-Compression-System-Gerzon-Craven/50783c57cdeea9aa87244d195f1dcef75678d837>.
* Gerzon and Craven, *Optimal Noise Shaping and Dither of Digital
  Signals*, AES 87th Convention, 1989 (preprint 2822).
* Martin, *Range encoding: an algorithm for removing redundancy from a
  digitised message*, 1979; see also
  <https://en.wikipedia.org/wiki/Range_coding>.
* Subbotin's carryless range coder:
  <https://gist.github.com/richgel999/d522e318bf3ad67e019eabc13137350a>.
* Sweldens, *The Lifting Scheme*, ACHA, 1996; Calderbank, Daubechies,
  Sweldens and Yeo, *Wavelet Transforms That Map Integers to Integers*,
  ACHA, 1998:
  <https://www.sciencedirect.com/science/article/pii/S1063520397902384>.
* Chen and Wornell, *Quantization index modulation*, IEEE Trans. Inf.
  Theory 47(4), 2001.
* Crochiere and Rabiner, *Multirate Digital Signal Processing*,
  Prentice-Hall, 1983 (cascaded interpolation).
* Måns Rullgård, `mqa`: <https://code.videolan.org/mansr/mqa>.
* `MQA_identifier`: <https://github.com/purpl3F0x/MQA_identifier>.

This note says what the techniques are called. It is not a view on
which patents are in force or on what anyone may build.
