# An MQA encoder

A sub-project of `mqa-decode`, written for research. It shares the
decoder's library and its constants, and it is tested by decoding what
it produces with a decoder that reproduces the vendor's bit for bit.

**It cannot make an authenticated stream.** That needs keys nobody
outside MQA has (see "Provenance" below). Streams from here decode and
say what they are, but no MQA decoder will show an authentication light
for them, and this project does not try to change that.

## What the official encoder does

From public material, and from what the format forces.

### Publicly documented

* Encoding happens in the studio, in a label's supply chain, or by a
  service on a retailer's behalf, and the further down that chain it
  happens the weaker the provenance claim gets.
* Two provenance levels: **MQA Studio** (the "blue light"), asserted by
  the mastering engineer, producer or artist, and plain **MQA** (green),
  meaning the stream is genuine but its provenance is less certain.
* Output is always **44.1 or 48 kHz**, chosen by the source family:
  88.2/176.4/352.8 kHz material encodes to 44.1, and 96/192/384 to 48.
  The carrier is a normal FLAC or WAV; on a CD it is 16-bit (**MQA-CD**).
* The encoder applies **de-blurring**, a compensating filter for the
  chain that made the source, and can estimate a suitable one by
  analysis when the actual converter is not known. White-glove projects
  get manual control over de-blurring and encapsulation.
* There are **classes of encoder** by application, with more facilities
  the closer to the studio you are.
* Editing, normalising or otherwise processing a file after encoding
  breaks the signalling: it will neither authenticate nor decode.

### What the format itself forces

Everything the decoder reads is something the encoder had to decide, so
the specification is also the encoder's feature list:

| Choice | Where it lands | Range |
| --- | --- | --- |
| carrier rate | datasync `src_rate` | 44.1 or 48 kHz |
| original rate | datasync `orig_rate` | any rate code, e.g. 352.8 kHz |
| render filter | datasync, 5 bits | 0..31 -- what the DAC-side renderer applies |
| render bit depth | datasync, 2 bits | 0..3 |
| provenance | datasync `auth_level`/`auth_info`, type-4 packets | 9 = studio |
| carrier class | datasync item 1 | 0..3: where the residual data travels |
| reconstruction kernel | datasync item 1 `variant` | 1 = short filter, 0 = the long kernel |
| residual scale | datasync item 1 `scale_index` | 0..63, a scale table index |
| refinement level | datasync item 0 | 7 bits, sets the refinement gain |
| output gain | datasync item 0 `gain_index` | 16 steps of 1/32 octave |
| dither salt | datasync item 1 `salt_select` | 0 silence, 1 or 2 |
| stage-2 dither mode | datasync item 0 | 2 bits |
| channel bit | the carrier itself | one of bits 8..15 |
| metadata | type-7 packets | fragmented, up to 256 bytes each |
| resync cadence | datasyncs with positions | how often a joiner can start |

Two of those are not free choices.

* **Carrier class** decides where the residual data goes. Class 0 puts it
  in the data channel, the low byte of every sample, which needs a
  24-bit carrier. Classes 1 to 3 code it into the carrier's own digits
  instead. That is what makes a **16-bit MQA CD** possible: a 16-bit
  carrier has no spare low byte, so the residual rides in the samples
  themselves.
* **The channel bit** is not signalled anywhere. A decoder finds it by
  trying all eight, so an encoder may pick any of them, but it must then
  arrange every frame's `(L ^ R)` parity at that bit, and the choice
  interacts with the noise shaping that fills the low bits.

## What is here

Every piece of the format's encoding side, each tested by decoding
what it produces with the library:

| Header | Does | Test |
| --- | --- | --- |
| `mqae/bits.h` | writes the control bitstream: packets, checksums, datasyncs, metadata, terminate | `test_bits`, through the decoder's scanner |
| `mqae/auth.h` | writes authentication packets, with the signature left to a caller-supplied signer | (none can verify one) |
| `mqae/chan.h` | frames and scrambles the data channel's messages | `test_chan`, through the decoder's parser; `tools/channel-dump` reads a real stream's channel the same way |
| `mqae/carrier.h`, `mqae/encoder.h` | put the control bit and the data byte into the PCM and assemble a stream | `test_container`: the library finds, joins and unfolds it |
| `mqae/analyse.h` | the reconstruction filter run backwards: source to carrier and split | `test_analyse`, against the library's filter |
| `mqae/coder.h` | the range coder run backwards | `test_coder`, against the library's coder |
| `mqae/entropy.h` | chooses what the entropy decoder will produce | `test_symbols` |
| `mqae/residual.h` | the residual stage run backwards | `test_residual`, against the library's stage |
| `mqae/refine.h` | the carrier refinement run backwards | `test_refine`, against the library's refinement |
| `mqae/encode.h` | the whole encoder | `test_encode`, decoded and measured |

`tools/mqae` is the command line: `encode`, `wrap` (a stream over an
existing carrier, with nothing encoded) and `verify`.

## What it does now

`mqae encode` takes an 88.2 or 96 kHz stereo file and writes a carrier
at half its rate that a decoder unfolds back. A 6.5-minute 96 kHz
track encodes to FLAC in ten seconds and `mqad decode` brings it back in
eight, at **66.6 dB** against the source, using 11.5 of the data
channel's 16 bits a frame.

On a five-second excerpt the error is shaped rather than flat:

| band | source | error | margin |
| --- | --- | --- | --- |
| 0-2 kHz | -0.1 dBFS | -73.7 | 73.6 dB |
| 2-8 kHz | -13.2 | -79.7 | 66.5 |
| 8-16 kHz | -23.6 | -76.9 | 53.3 |
| 16-20 kHz | -40.8 | -75.4 | 34.6 |
| 20-24 kHz | -50.0 | -68.7 | 18.7 |
| 24-48 kHz | -57.8 | -72.0 | 14.2 |

The tool reports where its loss went:

```
  loss: the carrier the decoder sees is 66.8 dB from the one the
        analysis wanted,
        the refinement corrected it to 67.0 dB, 300850 of its
        symbols clipped
        the residuals are coded 17.7 dB down, 72 clipped
```

Two thirds of the refinement's symbols clip. Its correction can move a
sample two or three steps and no further (the radix follows from the
stream's parameters), and the carrier it is handed is further out than
that. The carrier error is the conditioner's: the data channel takes the
low byte of every sample and the conditioner refills it with shaped
dither whose total power is well above the byte it replaced, although it
sits where the music is not. A real encoder faces the same arithmetic
with the same radix, so some of that noise is meant to survive. How much
of the rest could be removed by choosing the carrier's bits more
carefully is open.

Three mechanics matter to anyone changing the encoder.

* **The model.** The encoder keeps the library's own conditioner and
  refinement, set up as a packet start sets them up, and works back
  from the carrier that model says the decoder will see rather than the
  one it wrote. `tests/test_encode.c` compares the model against a real
  decoder's `on_layer` report sample for sample. They must agree
  exactly.
* **Headroom.** A full-scale source makes a full-scale carrier, and one
  clipped carrier sample costs tens of dB because the filter's history
  carries it forward. The datasync's gain index lets the encoder
  attenuate by up to 2.8 dB and the decoder put it back, so `mqae
  encode` measures the carrier's peak in an analysis-only first pass and
  takes what it needs. On the track above that is index 5.
* **Pacing.** A decoder reads the data channel a few hundred frames
  ahead of what it is decoding, into a 2048-byte ring for the residual
  symbols and a 1024-byte one for the refinement. A block's bytes have
  to arrive at about the rate they are consumed, so each block's frames
  are cut into segments and each stream gets its share of every one.
  That is why a real stream is a steady interleave of small messages.

### Running the decoder backwards

The decoder's reconstruction is a lifting scheme: predict, then update,
each step reversible on its own (see `docs/prior-art.md`). Given the
output pair a lifting step produced and the dither it used, the inputs
follow. The same is true of the P/Q predictor. So the encoder runs the
decoder's own steps in reverse rather than guessing at an inverse
filter.

Two of those inverses have to run backwards in *time* as well. Solved
in sample order, both the reconstruction analysis and the predictor's
inverse amplify each rounding by about three a step and diverge within
a few dozen samples; solved from the end of a block towards its start
they contract. The entropy coder is the same: the decoder's range coder
turns out to be an asymmetric numeral coder, so each block is encoded
by choosing every step forwards, then building the value from the last
step back to the first, shedding a byte wherever the decoder pulled
one (`mqae/coder.h`).

What is not invertible is where the loss is: the quantiser in the
entropy coder, and the carrier, which has to be a listenable 44.1 kHz
signal as well as a container.

## Open: joining a stream part way through

A stream is found only where a datasync is, so a player can only start
at the beginning of what this encoder writes; seek into the middle and
there is nothing to find. Real streams carry a resync datasync every few
thousand frames, and writing them was tried.

It does not work yet. A decoder finds the packet, takes the stream's
parameters from it and starts decoding, but what it decodes is wrong,
and the vendor decoder produces the same wrong audio, so the fault is in
the stream. What is not right is one of: the byte offset the datasync
gives for the data channel (item 1's offset and the bit beside it), the
sync mode in that item, or the conditioner's resync marker.

Three things were learned.

* The marker words are **zeros**, whatever the packet holds. A decoder
  reads them from the packet's own bits at an offset past item 0's
  fields, but it does so while acting on item 0, when it has copied only
  the bits up to the end of that item. The marker carries its position
  and nothing else.
* A decoder joining part way through has to count from the position the
  datasync announces, not from zero. The control channel already does
  this (it rebases its checksum seeds from the same field); the intake
  does not, and needs changing before any of this can be tested end to
  end.
* A resync point needs a reconstruction packet behind it or nothing can
  join there: a decoder will not start a stream until it has met one.

Two pieces from the attempt are kept, because real streams have them
and they cost nothing: a sync message at the head of every block's data
(the message that tells a decoder where to pick the residual stage up),
and control-channel holes sized to end on a block boundary, so that a
packet can be placed at a chosen frame.

The way in is to watch a decoder join a real stream at one of its
resync points and compare its state, group by group, with what it does
on one of ours.

## Provenance, and why this cannot authenticate

The datasync says what a stream claims; type-4 packets carry the proof.
Each is 384 bytes, the size of a 3072-bit signature, though the scheme
was not recovered. A decoder that verifies one keeps the stream
authenticated for 327680 frames.

`mqae/auth.h` writes those packets and hands their contents to a signer
you supply, along with the block's carrier samples so it can hash
whatever it needs to. With no signer they carry zeros. Nothing here
attempts to forge or work around the scheme.

## Building

```
make            # libmqaencode.a
make check      # the tests, which decode what they encode
```

It links `../libmqadecode.a` and builds it if needed.

## Sources

* Wikipedia, *Master Quality Authenticated*:
  <https://en.wikipedia.org/wiki/Master_Quality_Authenticated>
* Bob Stuart, *MQA Authentication* and *Appendix 4: MQA Encoding those
  files*, bobtalks.co.uk.
* Sound on Sound, *MQA: Time-domain Accuracy & Digital Audio Quality*:
  <https://www.soundonsound.com/techniques/mqa-time-domain-accuracy-digital-audio-quality>
* Create MQA, service FAQ: <https://www.createmqa.io/faq>
* And, for everything in "what the format itself forces",
  `../docs/mqa-stage1-spec.md`.
