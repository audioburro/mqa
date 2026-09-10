# MQA stage-1 ("first unfold") decoding

Revision 1, 2026-09-09. Accompanies the `mqa-decode` library.

---

## Status of this document

This document specifies how to decode the first unfold of an MQA stream:
how a 44.1 or 48 kHz PCM carrier conceals a control channel, a data
channel and a residual signal, and how a decoder turns those back into
twice as many samples per second.

Nothing here comes from the format's designers. Every rule was recovered
by exploratory testing of a decoder, and every rule stated normatively
has been shown to reproduce that decoder bit for bit on real material,
some 4 million groups of it. The
document therefore describes *what a decoder must do to agree with the
existing one*, not what anyone intended.

Where behaviour has been observed but not explained, the text says so and
marks the section "informative". Where a path exists in the reference
decoder that no material to hand exercises, the text says that too.

## 1. Introduction

### 1.1 Scope

An MQA-encoded file is an ordinary PCM file: 44.1 or 48 kHz, stereo,
24-bit samples in a WAV or FLAC container, playable as it stands. Below
the audio's noise floor it also carries a coded representation of the
signal's upper octave. A stage-1 decoder recovers that octave and emits
PCM at twice the input rate: 88.2 or 96 kHz.

This document specifies the stage-1 decode and nothing else. In
particular it does **not** specify:

* The encoder. What a compliant encoder must produce follows from this
  document, but the choices an encoder makes (filter design, bit
  allocation, where to place packets) are not described.
* The renderer, the second, DAC-side stage that oversamples further
  and applies the filter the stream names. Section 11 specifies the
  signalling a stage-1 decoder embeds for it, not what the renderer does
  with it.
* Authentication. Section 12 describes the fields and the observed
  behaviour, but the hashing behind them was not recovered.

### 1.2 Conformance and notation

The key words MUST, MUST NOT, SHALL, SHOULD and MAY are to be
interpreted as in RFC 2119. They apply to a decoder that intends to
produce output identical to the reference decoder's, which is the only
useful conformance target for a format defined by an implementation.

Notation used throughout:

| Notation | Meaning |
| --- | --- |
| `x >> n`, `x << n` | arithmetic shift on signed values, logical on unsigned |
| `hi32(a * b)` | the high 32 bits of a 64-bit product, i.e. `(int64_t)a * b >> 32` |
| `lo32(x)` | the low 32 bits, i.e. the value taken modulo 2^32 |
| Q31 | a fixed-point fraction with 31 bits after the point |
| `u32`, `i32` | unsigned and signed 32-bit integers |
| `M[a..b]` | bits a through b inclusive of M, a the least significant |

All arithmetic in this specification is integer arithmetic on 32-bit
words that wraps on overflow, unless a 64-bit intermediate is written
explicitly. This is not an implementation detail: the reference
decoder's output depends on the wrapping in several places, and a decoder
that promotes to wider types will diverge. Where a computation must be
performed in 64 bits and truncated, the text says so.

Samples are handled as signed 32-bit words holding a 24-bit value in
their low 24 bits, i.e. the range [-2^23, 2^23). Where the carrier's own
words are described as "left-justified", that refers to the reference
decoder's internal convention, noted in section 2.1.

### 1.3 The chain at a glance

```
   PCM in  ─┬─▶ control channel (§4) ──▶ stream parameters, packets
            │
            ├─▶ data channel (§5) ─────▶ messages ─┬─▶ symbol ring
            │                                      ├─▶ auxiliary ring
            │                                      └─▶ parameter records
            │
            ├─▶ carrier digits (§2.4) ─────────────────▶ digit FIFO
            │
            └─▶ carrier samples ──▶ refinement (§6) ──▶ reconstruction (§8)
                                                             ▲
                          residual stage (§7) ── P, Q ───────┘
                                                             │
                                        output stage (§9) ◀──┘ ──▶ PCM out
```

Everything is driven in groups of 32 carrier frames. One group of 32
input frames yields 64 output frames per channel. The control channel is
read 480 frames ahead of the group being reconstructed; the data channel
is descrambled up to 480 samples ahead; the carrier digits come from half
a group ahead. Section 3 specifies this timing exactly, and it matters:
the stages are stateful and a decoder that reads ahead by a different
amount produces different output.

### 1.4 How to read this document

The document is long because the arithmetic is written out. Not all of
it is needed at once.

* To decode a file: sections 2, 3 and 4 (the carrier and the control
  channel), then 5 (the data channel), then 7 and 8 in that order, with
  Appendix A open beside section 7.5. Section 9 is short. Section 6 can
  wait until the rest works, since a wrong refinement produces
  recognisable audio and a wrong residual stage does not.
* To check an implementation: Appendix B.
* To understand what the format is: sections 1 and 2, the first
  paragraph of each later section, and `docs/prior-art.md`.

### 1.5 Terms

Several words below have a narrower meaning than usual.

| Term | Meaning here |
| --- | --- |
| **carrier** | the 44.1 or 48 kHz PCM as stored, which also hides the stream |
| **frame** | one stereo sample pair of the carrier |
| **group** | 32 carrier frames; the unit the decoder advances by |
| **tap** | one step of the reconstruction filter: one carrier frame in, two output frames out |
| **block** | 4096 carrier frames; the residual coder and the refinement restart at each |
| **packet** | a unit of the control channel (section 4) |
| **message** | a unit of the data channel (section 5) |
| **datasync** | the packet that opens a stream and carries its parameters |
| **record** (residual) | one entropy decoder; each channel has two |
| **record** (parameter) | a type-4 message carrying the stream id and the stages' initial state |
| **symbol** | one output of an entropy decoder; sixteen make a record's share of a group |
| **residual** | the P (left) or Q (right) value the filter adds to a carrier sample |
| **level** | the entropy decoder's quantiser step, which adapts per symbol |
| **scale** | the fixed-point scale of a gain record; 203 in every stream seen |
| **variant** | which reconstruction filter a stream uses: 1 the short one, 0 the long kernel |
| **carrier class** | where a stream keeps its residual data: the data channel (class 0) or the carrier's digits (1..3) |
| **digit** | a small value read from each carrier sample, modulo the class's level count |
| **refinement** | the correction applied to the carrier before reconstruction |
| **conditioning** | the requantisation the intake applies to the carrier after taking its low byte |
| **run-in** | the frames before a stream opens, passed through undecoded |
| **resync** | a datasync with a position, at which a decoder may join |

## 2. The carrier

### 2.1 Sample conventions

The carrier is stereo PCM, 44.1 or 48 kHz, nominally 24-bit. A decoder
MUST treat each sample as a signed 24-bit value. The reference decoder
holds samples left-justified in 32-bit words (the 24-bit value shifted up
by 8); this specification uses the 24-bit value directly and notes bit
numbers accordingly. An implementation that follows the reference's
convention must shift the bit numbers in sections 2.2 and 2.3 up by 8.

Three separate things are hidden in the carrier:

| What | Where | Section |
| --- | --- | --- |
| control channel | one bit per frame, from bits 8..15 | 2.2, 4 |
| data channel | the low 8 bits of every sample | 2.3, 5 |
| carrier digits | the sample value above bit 8, modulo a small M | 2.4 |

### 2.2 The control channel bit

The control channel carries **one bit per stereo frame**: the
exclusive-or of bit *k* of the left sample with bit *k* of the right
sample, for one fixed *k* in 8..15:

```
    channel_bit(frame) = (L[k] ^ R[k]) & 1
```

*k* is not signalled anywhere ahead of the stream; a decoder MUST find it
by trial (section 4.1). The value in force is called the stream's
**channel bit**; the reference decoder keeps `xbit = k - 8` in 0..7 and
uses it as the carrier's shift, which appears in the digit extraction of
section 2.4 and in the conditioning of section 10.

### 2.3 The data channel byte

The low 8 bits of every carrier sample, both channels, every frame,
are the data channel. They are not audio: the encoder puts them there and
the decoder takes them out (section 5.1) and replaces them (section 10)
before passing the samples on.

### 2.4 Carrier digits

Every carrier sample also carries one small digit, read from the value
*above* the data-channel byte. With the stream's carrier class giving a
level count M and the stream's `shift`:

```
    v     = (sample >> (shift + 8)) + 0x690000
    digit = v mod M
```

M is 1, 3, 4 or 8 according to the class (section A.1). A decoder MUST
compute the reduction as the reference does, by multiplying with a 32-bit
reciprocal and taking the high word:

```
    digit = hi32( (v * recip) * M )        recip = ceil(2^32 / M)
```

which equals `v mod M` over the carrier's range and is bit-exact outside
it. For M = 1 the digit is always 0 and `recip` is unused.

Digits are produced interleaved, one pair per frame: `digits[2i]` from
the left channel, `digits[2i+1]` from the right. Classes with M > 1 feed
them to the residual stage (section 7.8); for M = 1 they are inert.

*(Reference: `mqa/carrier.h`.)*

### 2.5 The carrier rings

A decoder MUST buffer the carrier, because the control channel runs ahead
of reconstruction. The reference decoder keeps two rings of **832 words**
each (left and right), with a 64-word tail mirroring the head so that any
group plus its lookahead can be read contiguously. A decoder MAY buffer
differently, but 832 is not arbitrary: the descrambler's read position
wraps at 832 (section 5.1) and the ring length appears in the flush
behaviour at the end of a stream (section 9.5).

## 3. Groups, positions and timing

The decoder advances in groups of `MQA_GROUP = 32` carrier frames. Three
positions move together:

| Position | Advances by | Points at |
| --- | --- | --- |
| stream position | 32 per group | frames since the stream opened |
| ring position | 32 per group, mod 832 | the group being reconstructed |
| descramble position | as needed, mod 832 | the data channel's read point |

Per group, in order:

1. the control channel is read up to **480 frames** ahead of the
   reconstruction point (section 4);
2. the data channel is descrambled up to 480 samples ahead, in chunks the
   message parser's 128-byte FIFO can take, parsing after each chunk
   (section 5). Parameter records and sync messages take effect here,
   which is why they act on a *later* group than the one being decoded;
3. the carrier digits of the window **half a group ahead** (ring position
   + 16, for 32 frames) are extracted (section 2.4);
4. the group's own 32 frames are refined in place (section 7), its
   residuals decoded (section 8) and its 64 output frames per channel
   reconstructed (section 9);
5. positions advance.

A decoder MUST preserve these relative offsets. They are what the
reference's own state machines assume, and the stages are adaptive: a
decoder that refines a group before the parameter record that belongs to
it, or that takes digits from the wrong window, diverges within a few
groups and stays wrong.

*(Reference: `mqa/decoder.h`, `mqa/intake.h`.)*

## 4. The control bitstream

*(Reference: `mqa/bitstream.h`, `src/bitstream.c`.)*

### 4.1 Finding the stream

The control channel is read least-significant-bit-first as a sequence of
packets. A stream is recognised by its opening packet: type 5 followed by
a 36-bit magic, which together form the 40-bit pattern

```
    0x11319207d5        (bit 0 first: 1,0,1,0,1,0,1,1,...)
```

A decoder does not know the channel bit *k* in advance and MUST search:
maintain eight candidate shift registers, one per *k* in 8..15, feed each
frame's `(L[k] ^ R[k])` bit into every candidate, and declare the stream
open on the first candidate whose last 40 bits equal the magic. The
frame at which the magic *began* is the stream's frame 0 for the purposes
of section 4.3.

A decoder MUST continue to accept a stream whose channel bit changes
between streams in one file (different tracks concatenated, for example);
in practice the bit is constant for the life of a stream.

If packet parsing later fails a checksum, the decoder MUST treat the
stream as lost, discard buffered bits and resume the search one bit after
the failed packet's start.

### 4.2 Packet framing and the checksum

Every packet is:

```
    4 bits    type
    ...       type-dependent payload (section 4.4)
    4 bits    checksum
```

The checksum is computed with a 32-bit shift register clocked by every
bit of the packet, payload included but the checksum field excluded:

```
    seed:  reg = position_of_packet_in_bits & 15
    step:  x   = reg & 1
           reg = (reg >> 1) | (bit << 31)
           if x: reg ^= 3
```

After the last payload bit the register is clocked further with the same
rule but no input bits, `pad` times, where `pad = (-(packet_bits)) mod
32`: the packet is padded with zero bits to a multiple of 32.
The low four bits of the register are then the expected checksum, which
MUST equal the 4-bit field that follows. (`reg ^= 3` is a 2-bit reflected
CRC with polynomial 0x3; the same register appears again, byte-wise, in
the data channel's message check, section 5.2.)

The packet's bit position for the seed is a running count. It starts at
zero for a stream whose first datasync carries no stream position (the
usual case) and at the declared position otherwise, and advances by each
packet's total length in bits (type, payload and checksum). Every later
datasync that carries a stream position resets the count to it before
that advance. Because the control channel carries one bit per frame, the
count is also a frame position, which is how a decoder that joins a
stream mid-file recovers the seed.

A decoder MUST reject a packet whose type field is 9..15: those are not
packets, and their appearance means synchronisation was lost.

### 4.3 Packet types

| Type | Name | Payload |
| --- | --- | --- |
| 0 | hole | 4-bit size; if 15, a further 12-bit size; then `size` bits skipped |
| 1 | reconstruction | 12-bit size, then `size` bits (the span they cover is meaningful to the encoder, not the decoder) |
| 2 | data | 8-bit unknown, 12-bit size, then `size` bits |
| 3 | terminate | 17 bits: how many bits of stream remain after this packet |
| 4 | authentication | 4-bit level, then 384 **bytes** (3072 bits, the size of a 3072-bit signature) |
| 5 | datasync | section 4.4 |
| 6 | (unnamed) | 12-bit size, then `size` bits |
| 7 | metadata | 7-bit type, 1-bit last flag, 12-bit fragment number, 8-bit `size - 1`, then `size` bytes |
| 8 | key | 12-bit size, 32-bit word, then `size - 32` bits |

Only three types affect a stage-1 decode:

* **datasync (5)** carries the stream's parameters, and every one after
  the first also carries a resynchronisation position;
* **terminate (3)** says where the stream ends: the decoder stops
  decoding `bits_to_end` frames after the packet;
* **authentication (4)** carries the level the stream claims (section
  12).

The rest are consumed and discarded by a stage-1 decoder. Type 1
("reconstruction") is the bulk of a stream by volume and is *not* the
residual data, which travels in the data channel (section 5), not here.

### 4.4 The datasync packet

After the 4-bit type and the 36-bit magic:

| Bits | Field |
| --- | --- |
| 1 | stream-position flag (0 on the stream's first datasync) |
| 1 | (unknown) |
| 5 | original sample rate, as a rate code (section 4.5) |
| 5 | carrier (stream) sample rate, as a rate code |
| 5 | render filter |
| 2 | (unknown) |
| 2 | render bit depth |
| 4 | (unknown) |
| 4 | authentication info |
| 4 | authentication level |
| 7 | item count *n* |
| 8*n* | one size byte per item, in order |
| 8*n* | one type byte per item, in order |
| 32 | stream position, **only when the flag is set** |
| ... | the items themselves, in order |

Each item's size byte gives its length in bits; a decoder MUST use it to
skip items it does not understand rather than assuming the layouts below.
Three item types are known:

**Type 0, base band.** A 2-bit stage-2 dither selector (section 10), a
4-bit output gain index (1/32-octave steps, section A.8), a 7-bit
refinement level (the level word of section 6.1) and a 7-bit feedback
strength, 127 meaning none; and, only when the datasync carries a
stream position, a 27-bit start position, which is the resynchronisation
point in units of 32 frames, and one reserved bit.

**Type 1, the stream's parameters.** A 6-bit residual scale index
(section A.6's scale table), a 2-bit carrier class (section A.1), a
1-bit reconstruction variant (1 selects the short filter of section 8.2,
0 the kernel of section 8.3) and a 2-bit dither salt selector (section
9.1); and when the datasync carries a position, an 8-bit sync mode, one
flag and a **signed** 12-bit offset.

**Type 2, the same without a class.** The 6-bit scale index and the
2-bit salt selector; and when positioned, a 3-bit sync mode, one flag
and the signed 12-bit offset.

These are the encoder's choices, which is how they were named: see
`encoder/README.md`.

**Type 3, cipher.** A 64-bit initialisation vector and a 27-bit start
position. Streams carrying it were not available; a decoder that meets
one and cannot use it SHOULD treat the stream as undecodable rather than
produce output.

The fields a stage-1 decoder MUST act on are: both rate codes, the two
authentication fields, the render filter and bit depth (for the
signalling of section 11), the stream position, and from item type 0 the
stage-2 dither selector and gain index (section 10).


**Example.** The opening datasync of a 48 kHz stream, as the 147
control-channel bits of its first 147 frames, bit 0 first:

```
    1010101111100000010010011000110010001000010101000010000100001000
    0000010010100000001010001101000000000010000000010000111100011111
    11100110001100111
```

Read least significant bit first from position 0, that is:

| Bit | Width | Field | Value |
| --- | --- | --- | --- |
| 0 | 4 | type | 5 |
| 4 | 36 | magic | 0x11319207d |
| 40 | 1 | stream position flag | 0 |
| 41 | 1 | (skipped; 1 in every stream seen) | 1 |
| 42 | 5 | original rate code | 10 |
| 47 | 5 | carrier rate code | 8 |
| 52 | 5 | render filter | 8 |
| 57 | 2 | (unknown, 2 bits) | 0 |
| 59 | 2 | render bit depth | 2 |
| 61 | 4 | (unknown, 4 bits) | 0 |
| 65 | 4 | auth info | 0 |
| 69 | 4 | auth level | 9 |
| 73 | 7 | item count | 2 |
| 80 | 8 | item 0 size | 20 |
| 88 | 8 | item 1 size | 11 |
| 96 | 8 | item 0 type | 0 |
| 104 | 8 | item 1 type | 1 |
| 112 | 2 | item 0: stage-2 dither | 2 |
| 114 | 4 | item 0: gain index | 0 |
| 118 | 7 | item 0: refinement level | 15 |
| 125 | 7 | item 0: feedback (127: none) | 127 |
| 132 | 6 | item 1: scale index | 25 |
| 138 | 2 | item 1: carrier class | 0 |
| 140 | 1 | item 1: variant | 1 |
| 141 | 2 | item 1: salt | 1 |
| 143 | 4 | checksum | 14 |

Rate code 10 is 192 kHz and 8 is 48 kHz (section 4.5); auth level 9 is
studio provenance; item 0's feedback field of 127 means no feedback
term; item 1 asks for scale index 25 (a scale of 203, Appendix A.3),
carrier class 0, the short reconstruction filter and dither salt 1.

The checksum: seed the register with the packet's position (0) and
clock in the 143 bits before the checksum field with the rule of
section 4.2, then clock it 17 more times with no input to reach a
multiple of 32. The register's low nibble is 14, which is the field.

### 4.5 Rate codes

A 5-bit rate code is a 2-bit base index and a 3-bit power of two:

```
    rate_hz(code) = base[code >> 3] << (code & 7)
    base[] = { 44100, 48000, 64000, - }
```

Code 31 means "unset". The original-rate code is the rate the encoder
started from (352.8 kHz, say); the carrier code is the file's own rate.

### 4.6 What the decoder keeps

From the packets a decoder MUST maintain:

* **the stream position**, in frames from the stream's start, advanced by
  32 per group;
* **the end position**, if a terminate packet has been seen: the stream
  ends `bits_to_end` frames after the terminate packet's own position,
  and the decoder MUST stop decoding there and pass later samples through
  untouched (section 9.5);
* **a pending sync position**, from every datasync after the first: the
  position it announces takes effect when the stream reaches it, not when
  the packet is parsed;
* **the stream parameters** the group path reads: carrier class, shift,
  level, scale index, variant, salt selector, descriptor and format
  fields. These come from the datasync and its items, and change only at
  a packet start.

Because the control channel is read 480 frames ahead (section 3), all of
this is known well before the group it applies to is reconstructed.


## 5. The data channel

*(Reference: `mqa/descrambler.h`, `mqa/stream.h`.)*

The data channel is where the residual signal travels. It is a byte
stream, one byte per carrier sample, scrambled; unscrambled it is a
sequence of framed messages, whose payloads feed the residual stage's
entropy coders and the carrier refinement.

### 5.1 Descrambling

Each carrier word contributes one byte, exclusive-ored with the top byte
of a generator state. There are two generators, one per channel, stepped
once per frame:

```
    out[2i]     = (A[pos + i] ^ (lcgA >> 24)) & 0xff
    out[2i + 1] = (B[pos + i] ^ (lcgB >> 24)) & 0xff
    lcgA = lcgA * 0x17385ca9 + 0x47502932
    lcgB = lcgB * 0x17385ca9 + 0x47502932
```

so a group of 32 frames yields 64 bytes. Reads wrap at the ring's end
(index 832).

The generators are re-seeded every **2048 byte pairs**. A decoder keeps a
budget, decremented per pair; when it reaches zero, both states are
re-seeded from a running count of previous reseeds:

```
    s      = 0x2082352c + reseeds
    s      = s * s                      (32-bit, wrapping)
    lcgA   = nr(s)                      nr(x) = x * 1664525 + 1013904223
    lcgB   = nr(lcgA)
    reseeds = reseeds + 1
    budget  = 2048
```

`nr` is the Numerical Recipes generator, distinct from the scrambler's own
recurrence above. A fresh decoder starts with a zero budget, so its first
fill reseeds immediately with `reseeds = 0`.

To start at an arbitrary sample position (joining a stream, section 6.2)
a decoder MUST reproduce the state that continuous scrambling would have
reached:

```
    within  = position & 0xffe            (an even offset within the batch)
    reseeds = (position >> 12) + 1
    s       = 0x2082352c + (position >> 12);  s = s * s
    lcgA    = nr(s), then advanced `within / 2` steps of the scrambler
    lcgB    = nr(lcgA)
    budget  = (0x1000 - within) >> 1
```

Advancing the generator by *n* steps at once is exact modular arithmetic;
the reference decoder uses a table of 2^k-step constants, and any
equivalent jump-ahead is acceptable.

### 5.2 Message framing

The descrambled bytes form messages:

```
    byte 0      (check << 4) | type
    byte 1..    a type-dependent header
    payload     `size` bytes, for the types that have one
```

| Type | Header after byte 0 | Payload |
| --- | --- | --- |
| 0 | none | none |
| 1, 2 | 1 size byte | `size` bytes |
| 3 | 1, 3, 5 or 7 bytes; bits 0-1 of byte 1 select | none |
| 4 | 5 bytes: a 24-bit stream id and two record lengths | the record |
| 5..15 | 1 size byte | `size` bytes |

Every message carries a 4-bit check in the high nibble of its first byte.
The check register is 32 bits, **seeded with the message's byte offset in
the stream**, and absorbs every byte of the message after the first:

```
    reg = ((byte << 24) | (reg >> 8)) ^ crc2[reg & 0xff]
```

where `crc2` is the 256-entry table of the 2-bit reflected CRC with
polynomial 0x3, the same polynomial as the control channel's packet
checksum (section 4.2), applied a byte at a time instead of a bit at a
time. After the last payload byte the register's low nibble MUST equal
the check nibble.

A check failure MUST take the payload rings offline: the decoder stops
routing payloads and continues parsing. A later type-3 sync message
carrying a stream id puts them back online. A stream MUST open, at byte
offset 0, with a type-4 record; a decoder that sees anything else there
MUST treat the data channel as not started.

The parser owns a FIFO of at least 128 bytes, topped up by the caller
(64 bytes per group) and drained message by message, keeping partial
state across calls.

**Example.** The first 48 bytes of the same stream's data channel,
descrambled from the low bytes of its first 24 frames:

```
    24 40 96 65 08 20  00 00 00 00 01 00 00 00  cb 00 00 00 c7 02 00 00
    cb 00 00 00 c6 02 00 00  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
    21 ff ...
```

Byte 0 is `0x24`: type 4, a parameter record, with check nibble 2. The
header's next five bytes are the stream id `0x659640` (three 6-bit
fields, each 25, the scale index the datasync asked for) and the part
lengths 8 and 32. Part A, `00 00 00 00 01 00 00 00`, seeds the two
predictors' symbol memories (section 7.3), each 16-bit value scaled by
the record's scale when installed: (0, 0) for the left channel and
(203, 0) for the right. Part B is the reconstruction filter's history
(section 8.4): for the left channel `pred[1] = 203`, `pred[0] = 711`,
`corr[1] = 203`, `corr[0] = 710`, and zeros for the right.

The check: seed the register with the message's byte offset, 0, and
absorb the 45 bytes after the first with the step above. The register's
low nibble is 2. The next message starts at byte 46: `0x21` is type 1
with check 2, and `0xff` says it carries 255 bytes of payload, which
this decoder discards.

### 5.3 Routing

Payloads are routed by message type:

| Type | Goes to |
| --- | --- |
| 2 | the **auxiliary ring**, read by carrier refinement (section 6) |
| 4 | the **parameter record** handler (section 5.4) |
| 5 | the **symbol ring**, read by the residual stage (section 7) |
| other | checked and discarded |

Both rings are byte rings with a write position and a read cursor; the
reference's are 2048 bytes. The residual stage's range coder consumes
from the symbol ring's cursor, so a stream that codes more residual data
than the rings can hold would overrun; see section 7.10.

### 5.4 Parameter records and sync messages

A **type-4 parameter record** carries a 24-bit stream id and two parts:
part A (up to 8 bytes) and part B (up to 32 bytes). Its arrival:

* selects the residual decoders' gain records, through two 6-bit indices
  taken from the stream id (section 7.3);
* restarts the output stage, installing the reconstruction variant, the
  dither salt and scale, and either loading the reconstruction filter's
  history from part B or warming it from the carrier pair ring
  (sections 8.4 and 9.3);
* seeds the residual decoders' predictors from part A when the stream is
  at position 0.

A **type-3 sync message** announces a position at which the residual
stage's window is to be re-based (section 7.9). Its header's bits select
whether it carries a 24-bit stream id and a further 32-bit word; the
decoder acts on it when the stream reaches the announced position, not
when the message arrives.


## 6. Carrier refinement

*(Reference: `mqa/refine.h`.)*

Before a group is reconstructed, its 32 carrier samples per channel are
refined in place: scaled, then corrected by a value decoded from the
auxiliary ring. The correction is what recovers the resolution the
encoder gave up when it put the data channel in the low byte.

### 6.1 Parameters

The stage is (re)started at every packet start with:

* **gain**, derived from the packet's level word `x`:

  ```
      f     = x << 16
      third = hi32(0x55555555 * f)          (unsigned)
      sq    = hi32(third * f)               (unsigned)
      gain  = (i32)(third + 0x80000000 - (sq >> 2)) >> (x >> 16)
  ```

  The reference computes exactly this and a decoder MUST reproduce it.

* **param**, a stream parameter feeding the bias;
* a **coefficient set** index, selecting one of four FIR sets (section
  A.4), each with a tap count and a coarse-symbol width `shift`;
* a **dither salt** (0 disables dither).

Whenever a parameter record or sync message announces a new quantiser
step, the step and its derived constants are recomputed, but only at
the next 4096-sample block boundary:

```
    k     = floor(log2(step))                (31 if step < 0)
    q     = (1 << (32 + k)) / (u32)(step * 2)
    recip = -q
    t     = -hi32(-gain * (param << 11))
    bias  = (t << 1) >> 1
    radix_a = hi32(recip * ~(t << 2)) >> k
    threshold = (t << 1) - step * radix_a
    radix_b = radix_a + 1
    recip_x = 0x7fffffff / radix_x + 1       (0 + 1 when radix_x is 0)
```

A step of 0 disables refinement: the samples are scaled and nothing else.

### 6.2 The group

```
    for each sample: s = hi32((s << 4) * gain)          both channels
```

Then, at a block boundary (`counter mod 4096 == 0`) only: a pending step
change is adopted, the ring's read position is set if one was announced,
the dither generators are re-seeded from the block index

```
    seed = (block + salt)^2                  (32-bit)
    lcg0 = nr(seed), lcg1 = nr(lcg0)
```

the coder's value register is cleared, and the first `2 * (taps + 1)`
samples of the group are marked as carrying a **coarse symbol** (none in
the stream's first block).

Dither for the group is drawn before the samples are processed, two
values per sample:

```
    d[2i]   = hi32(lcg0 * step)   (unsigned), then lcg0 = lcg0 * 0x17385ca9 + 0x47502932
    d[2i+1] = hi32(lcg1 * step)   (unsigned), then lcg1 stepped likewise
```

With salt 0 all dither is zero. Channel A uses `d[2i]`, channel B
`d[2i+1]`.

### 6.3 The range coder

The refinement's coder is not the residual stage's. It keeps a single
32-bit value register, normalised by pulling bytes from the auxiliary
ring whenever the register has less than 16 radices of room:

```
    normalize(value, radix):
        while (radix << 4) > value:
            value = (value << 8) + next_ring_byte()
        return value

    decode(radix, recip):
        value  = normalize(value, radix)
        q      = hi32((value << 1) * recip)     (unsigned)
        value' = q
        return value - radix * q
```

### 6.4 Per sample

For each sample `x` with dither `d`:

* **Coarse samples** (the first `2 * (taps + 1)` of a block after the
  first) take a `shift`-bit symbol straight out of the value register
  first:

  ```
      value = normalize(value, 1 << shift)
      value' = value >> shift
      rem   = value - (value' << shift)
      err   = hi32( ~((bias_shifted + x - d) << 1) * recip ) >> k
      pred  = err * step + d
      radix = radix_b;   sym = radix_b * rem
  ```

* **Ordinary samples** predict from the channel's FIR accumulator:

  ```
      t     = x + bias - (acc >> 11)
      err   = hi32( ~((t - d) << 1) * recip ) >> k
      pred  = step * err + d
      resid = t - pred
      radix = (resid >= threshold) ? radix_a : radix_b
      sym   = 0
  ```

Then, in both cases:

```
    sym += decode(radix, recip_of_that_radix)
    y    = pred - step * sym
    output the sample as y, and feed (y - x) to the channel's predictor
```

### 6.5 The delta predictor

Each channel keeps a 50-entry history of correction deltas, written
downwards, and a FIR of `2 * taps + 2` coefficients (section A.4). It is
evaluated once per *pair* of samples, under a phase bit that is cleared
at the start of every group and toggled before each delta is taken:

```
    phase becomes 1 (the first sample of a pair):
        acc  = coef[0] * delta + last
        last = delta
    phase becomes 0 (the second):
        hist_pos -= 2
        history[hist_pos]     = delta          (this sample's)
        history[hist_pos + 1] = last           (the previous sample's)
        acc  = sum over j < 2*taps + 2 of coef[j]     * history[hist_pos + j]
        last = sum over j < 2*taps + 1 of coef[j + 1] * history[hist_pos + j]
```

So the pair is pushed and the filter evaluated on the second sample of
each pair (twice: once for the next sample and once, tail-shifted, for
the one after), and the first sample of the next pair merely completes
the stored tail with its own delta.

`acc` is the prediction used by the *next* sample, shifted down by 11 in
section 6.4. At the end of every group the newest 18 entries are copied
back to the top of the history and `hist_pos` reset to 32, which is how
the reference keeps a bounded buffer; a decoder MUST reproduce the fold
because the filter reads across it.


## 7. The residual stage

*(Reference: `mqa/residual_stage.h`, `mqa/residuals.h`, `mqa/entropy.h`,
`mqa/predictor.h`.)*

The residual stage turns the symbol ring's bytes into the 32 P and 32 Q
values one group's reconstruction consumes, one per output tap per
channel.

### 7.1 Shape

```
    symbol ring ──▶ range coder ──┬─▶ L record 0 ─┐
                                  ├─▶ L record 1 ─┴▶ L predictor ─▶ P[0..31]
                                  ├─▶ R record 0 ─┐
                                  └─▶ R record 1 ─┴▶ R predictor ─▶ Q[0..31]
```

Each channel has **two entropy decoders** ("records") and one P/Q
predictor. Per group each record produces 16 symbols and the two records'
outputs drive the predictor, which emits 32 residuals. All records share
one range coder and one byte ring, except for the carrier-digit classes
of section 7.8, where the leading records read a second coder instead.

A channel's `records` field says how many of its two decoders actually
run (0, 1 or 2); a record that does not run contributes an all-zero
16-symbol record.

### 7.2 The range coder

The coder is Martin's range coder over an arbitrary base:

```
    normalize(rc):
        while rc.range <= 0xffffff:
            rc.value = rc.value * base + next_source_symbol()
            rc.range = rc.range * base
```

`base` is 256 for a byte ring and the carrier-digit radix for the digit
coder (section 7.8). A coder whose `range` is 0 has never been started
and cannot be renormalised; the sequences that start one are given in
sections 7.3 and 7.8.

### 7.3 Blocks and gain records

The stream is divided into blocks of **4096 carrier samples**. Two things
happen only at a block boundary: a gain record announced since the last
one takes effect, and the block itself starts.

A **gain record** arrives as a type-4 parameter message (section 5.4).
Its 24-bit stream id carries two 6-bit indices, which select scales from
the 64-entry scale table (section A.6). Each channel's first entropy
decoder is given the record for index 0 and its second the record for
index 1; the P/Q predictors are given it only when *both* indices name
the announced record, and otherwise keep a neutral gain of
`{scale = 2, gain = 1/2, shift = 0}`. A record's part A, when the stream
is at position 0, also seeds the predictors' symbol memories:
`sym_prev[0] = scale * (i16)(a[0] | a[1] << 8)` and `sym_prev[1]` from
bytes 2 and 3, per channel.

Then, before any symbol of the block is decoded:

1. the **digit coder is restarted** unconditionally: `value = 0, range =
   1` (section 7.8; harmless for classes without digits);
2. if the stage is starting (its mode is not yet 2), the **data-channel
   coder** is restarted the same way, both channels' `records` set to 2
   and the mode set to 2; otherwise, if a channel has no records at all,
   the block header is skipped entirely;
3. the **block header** is read through the coder the first record of the
   left channel reads (the digit coder for a digit class, the data
   channel's otherwise):

   ```
       normalize(rc)
       v = rc.value
       rc.range >>= 3;  rc.value >>= 3
       spread      = (v & 4) ? (class_word & 0xff) >> 1 : 0
       header_bits = v & 3
       rate        = rate_table[v & 3]
   ```

4. every entropy decoder is re-seeded from the block index and reads its
   own header:

   ```
       s = salt_of_channel + block_index
       predictor noise[0] = nr(s * s), noise[1] = nr(noise[0])
       for record i:  rng = (s + 32 * (i + 1))^2
   ```

   and then, per record, with `bits` = 7 in block 0 and 11 thereafter:

   ```
       level = 0x200;  level_rate = rate
       normalize(rc)
       v = rc.value & ((1 << bits) - 1)
       rc.range >>= bits;  rc.value >>= bits
       low = v & 0x7f
       if block != 0:  hi = v >> 7
                       level = level_table[(-hi) & 3] >> ((hi + 3) >> 2)
       var = (i32)((u32)level_table[low & 3] << (low >> 2)) >> 8
       if low > 0x5b or var >= 2 * variance:  the stage resets (§7.10)
       variance = var
   ```

5. the level bounds are set from the carrier class and the group's window
   position (section 7.4).

The salts are fixed per channel (section A.5).

### 7.4 Level bounds

Before each group's decode the two records of each channel are given
level bounds derived from the carrier class's word `w` and the stage's
window position `at`:

```
    base = cls.param[0] ? 8*w - ((4 * w * at) >> 8) : 4*w
    lo   = base - spread
    hi   = base + spread
    record 0: level_max = lo, level_min = (u32)lo >> 2
    record 1: level_max = hi, level_min = (u32)hi >> 2
```

and at stream position 0 the levels themselves are set to `lo` and `hi`.
`at` is the number of bytes the digit FIFO holds (section 7.8); for
classes without digits it is 0 and the ramp term vanishes.

### 7.5 One symbol

Symbols are decoded in blocks of four; `pos` below is the symbol's index
in its block of four.

**Two random draws.** The record's own generator is stepped twice:

```
    s1 = nr(rng);  s2 = nr(s1);  rng = s2
    dither  = s2 >> 24                      (0..255)
    dscaled = hi32(s1 * scale)              (unsigned)
```

**The magnitude.** The coder is normalised and a 32-bit code word formed:

```
    normalize(rc)
    code = (rc.value << 13) | 0x1fff
```

If `rc.value & 0x7ffff` is below 32 the *escape* form applies:

```
    rc.range = (((rc.range - 32) >> 19) + 1) << 5
    rc.value = (rc.value & 0x7ffff) + ((rc.value >> 19) << 5)
    normalize(rc)
    res = (i32)(rc.value & 0xff) - 0x80
    rc.range >>= 8;  rc.value >>= 8
    if res == 0:                            (a 16-bit residual follows)
        refill(rc) once;  normalize(rc)
        res = (i16)rc.value
        rc.range >>= 16;  rc.value >>= 16
    q = dither + res * 256
    a = level * q;  b = a + level * 256;  t = a + b
    m = |t| >> 7
```

Otherwise the *table* form applies. The 32-bin table (section A.6) gives
each bin a base value, a cumulative threshold, a width in code-space
units per value, and a weight:

```
    rec = table[bin_by_nibble[code >> 28]]
    while code < rec.threshold: rec--
    off = code - rec.threshold
    q   = hi32(off * rec.recip)             (unsigned)
    if off < rec.width * q: q--
    v   = q + rec.base
```

`v` is then quantised to a multiple of `level`, keeping the dither in the
low byte:

```
    s  = v >> 31
    qv = ((|v| / level) ^ s)                (unsigned division)
    qv = ((qv - dither) & ~0xff) + dither
    a  = level * qv;  b = a + level * 256;  t = a + b
```

and the coder's interval is narrowed to the bins of `a` and `b`:

```
    ra = table[bin_of(a) + 16];  rb = table[bin_of(b) + 16]
    m  = ra.weight + rb.weight
    lo = (ra.threshold + (a - ra.base) * ra.width) >> 13
    hi = (rb.threshold + (b - rb.base) * rb.width) >> 13
    span = hi - lo
    rc.value = (rc.value >> 19) * span + ((rc.value & 0x7ffff) - lo)
    rc.range = (((rc.range - hi) >> 19) + 1) * span
```

`bin_of(x)` is the magnitude class of a signed value: with `s = x >> 31`
and `c = (x >> 14) ^ s`, it is `class_by_magnitude[min(c, 31)] ^ s`
(section A.6).

**Variance and step adaptation.**

```
    sa = (i32)(a * variance >> 16)          64-bit product, low 32 kept
    sb = (i32)(b * variance >> 16)
    variance = (i32)(m * variance >> 12)
    history push: clamp(t >> 9, -512, 512) as an i16
    pred  = (sa + sb) >> 1
    level = clamp(level_rate * level / m, level_min, level_max)
```

The division by `m` can be a division by zero: the reference decoder does
it anyway, and the platform's helper returns 0. A conforming decoder MUST
return 0 for that case rather than trapping.

**The two AR stages.** Four taps, rotated by the symbol's position in its
block, over the last four predictions of each stage:

```
    acc_a = sum over k of (i64)taps[(k + pos) mod 4] * pred_a[k]
    acc_b = sum over k of (i64)taps[(k + pos) mod 4] * pred_b[k]
    p1 = pred - (i32)(acc_a >> 32) * 16
    p2 = p1   - (i32)(acc_b >> 32) * 16
    pred_a[3 - pos] = p1;  pred_b[3 - pos] = p2
```

**The range-coded residual.** Only when the gain-shaped interval between
the two scaled magnitudes is wider than one unit:

```
    hi_in = 2 * (sb - dscaled) + 1
    lo_in = 2 * (sa - dscaled) - 1
    hi_g  = (hi32(hi_in * gain)) >> shift
    lo_g  = (hi32(lo_in * gain)) >> shift
    n     = hi_g - lo_g
    resid = hi_g
    if n > 1:
        recip = 0xffffffff / n
        normalize(rr)                       (rr is the residual coder)
        quot = hi32(rr.value * recip)       (unsigned)
        rem  = rr.value - quot * n
        top  = hi32(recip * (rr.range - 1))
        if rem >= n: rem -= n
        rr.range = top + 1
        rr.value = quot
        resid -= rem
```

**The symbol.**

```
    pv = p2 - pred
    if scale2 == scale:  pv = gain_step(scale + 2 * pv)     (§8.1)
    out = scale * resid + dscaled + pv
```

### 7.6 The per-block filter update

After every four symbols the record's four int16 filter coefficients are
updated by an LMS step against its 8-entry magnitude history (newest
first at `head`), and the AR taps refreshed:

```
    h[k] = history[(head + k) mod 8]                    k = 0..7
    for j = 4..7:  e[j-4] = h[j-4] + (sum over k of coef[k] * h[j-3+k]) >> 12
    for k = 0..3:  g = sum over j = 4..7 of h[j-3+k] * e[j-4]
                   g += dither << 4                     (the block's last draw)
                   coef[k] = (i16)(coef[k] - (g >> 12))
                   taps[k] = decay[k] * coef[k]
```

`decay` is a fixed four-entry set (section A.6). After the update the
variance is floored at `2 * scale2`.

### 7.7 The P/Q predictor

Each channel's two 16-symbol records are combined into 32 residuals by a
lifting butterfly with its own pair of dither generators. For each
`i = 0..15`, with symbols `wa` and `wb` from the two records:

```
    ea = wa - y1_prev
    eb = wb - y2h_prev
    h1 = wa_prev + hi32(K1 * eb + K0 * ea)      64-bit accumulation
    h2 = wb_prev + hi32(K2 * ea + K0 * eb)
    y1  = dx + gain_step(scale + 2 * (h1 - dx))
    y2h = dy - (y1 >> 1) + gain_step(scale + 2 * (h2 - dy + (y1 >> 1)))
    out[2i]     = y1 - (y2h + (y1 >> 1))
    out[2i + 1] =       y2h + (y1 >> 1)
    wa_prev = wa;  wb_prev = wb;  y1_prev = y1;  y2h_prev = y2h
```

`dx` and `dy` are the i-th draws of the predictor's two generators
(section A.5), `K0..K2` are fixed Q31 constants (section A.6), and
`gain_step` is the shared step of section 8.1. The state carried between
groups is `(wa_prev, wb_prev, y1_prev, y2h_prev)`; note that it is `y2h`
(the second output *minus half the first*) that is carried, not the
output itself.

Left-channel outputs are the P residuals, right-channel the Q.

### 7.8 The carrier-digit FIFO

For a carrier class with `param[0] > 0`, the leading `param[0]` records
of each channel do not read the data channel at all: they read the
carrier's own digits (section 2.4) through a second range coder.

Digits are packed into bytes, `param[1]` of them per byte, most
significant first, in base `levels`:

```
    byte = ((digit0 * M + digit1) * M + digit2) ... 
```

so a byte is one symbol of radix `param[2] + 1` (243 for class 1: five
base-3 digits). A group contributes `2 * 32` digits, which rarely end on
a byte boundary; the remainder stays in a carry with a phase counter
saying how many more digits it wants.

The FIFO is a byte ring of 256 bytes holding at most `size - 1` of them:
**a write into a full FIFO drops the oldest byte**, moving the read
position up with the write position. That is what keeps the read position
a fixed depth behind the carrier once a stream is running, and it happens
per byte written.

The digit coder is restarted (`value = 0, range = 1`) at every block
boundary (section 7.3), and its base is `param[2] + 1`.

When a stream is **joined**, at a packet start, a group of *zero*
digits is packed in: the join establishes the FIFO's write position and
packing phase, not its contents. Only a **realignment** (section 7.9)
packs real digits.

### 7.9 Resynchronisation

A sync message announces a position and a depth. At the next block
boundary at or after that position:

* if the stage is inactive, the FIFO's read position is re-based to
  `wpos - depth` (modulo the ring);
* if it is active and its window is already exactly `depth`, nothing
  happens;
* if it is active and the window differs, the stage restarts (its
  decoders return to their initial state) and reports resynchronisation.

In all three cases the stage becomes active and the pending sync is
cleared.

### 7.10 Status

The stage reports, per group: 0, or resynchronisation (4), window
exhausted (0xe), reset (0x11) or abort (0x12). Reset and abort take the
stage out of decoding until a later packet start or sync; while it is not
decoding the P and Q residuals MUST be zero, and reconstruction continues
with them.


## 8. Reconstruction

*(Reference: `mqa/lifting.h`, `mqa/reconstruct.h`, `mqa/recon2.h`.)*

Reconstruction turns one carrier sample pair and one residual pair into
**two output samples per channel**: the unfold. A stream selects one of
two filters with its `variant` field: variant 1 the short filter of
section 8.2, variant 0 the longer kernel of section 8.3.

### 8.1 The shared gain step

Both filters, and the P/Q predictor of section 7.7, lift through the same
fixed-point step. With a gain record `(scale, gain, shift)`:

```
    gain_input(x) = scale + 2 * x                       (32-bit, wrapping)
    gain_step(x)  = (u32)(hi32(gain * x) >> shift) * scale
```

A gain record is derived from a single `scale` value:

```
    if scale <= 0:   the record is inactive (scale = 0)
    k     = floor(log2(scale - 1)) + 1        (bits needed for scale - 1)
    shift = k
    gain  = (1 << (32 + k)) / (2 * scale)
```

The value 203 has been the only scale observed, giving `gain =
0x50b59897` and `shift = 7`.

### 8.2 The short filter (variant 1)

Per tap and per channel, with carrier sample `a`, residual `p`, and the
tap's two dither samples `(d0, d1)`:

```
    s = -s                                   sign flips before every tap
    t = s * p + (a >> 1)                     the correction
    n = a - t                                the prediction

    h1 = hi32( c0 * (n - out0[t-2]) + c1 * (pred[t-1] - out0[t-1]) )
    h2 = hi32( c2 * (t - out1[t-2]) + c3 * (corr[t-1] - out1[t-1]) )
    h  = h1 + h2

    a2 = pred[t-2] + corr[t-2]                the carrier sample two taps back
    y1 = d0 + d1 + gain_step(gain_input(a2 - d0 - d1 + 2 * h))
    y2 = d1      + gain_step(gain_input(corr[t-2] - d1 + (y1 >> 1)
                                        - ((a2 >> 1) + h) + 2 * h2))
    out0 = y1 - y2      (the first output sample)
    out1 = y2           (the second)
```

The differences inside the accumulators are formed in 64 bits, not
wrapped to 32 first: the reference keeps them as (low, sign) register
pairs, which matters once outputs exceed 2^31. The history advanced after
each tap is `pred`, `corr`, `out0` and `out1`, two taps deep each.

`c0..c3` are 64-bit fixed-point coefficients; in every stream seen their
high words are zero and their low words are entries 0..3 of the
coefficient table (section A.7). The sign `s` starts such that the first
tap after a restart uses -1.

### 8.3 The alternative kernel (variant 0)

The other filter runs two FIR stages ahead of two lifting steps, and
works **eight taps behind** the input. Per tap and per channel, with
carrier sample `x` and residual `p`:

```
    1. interpolate: the current sample at unit gain plus four filtered ones
       u = hi64( (x << 32) + c4*x[-1] + c5*x[-2] + c6*x[-3] + c7*x[-4] )

    2. shape: fourteen taps of that filter's own output
       v = hi64( c8*u + c9*u[-1] + c10*u[-2] + ... + c21*u[-13] )

    3. the second output sample, lifting v with the residual of eight
       taps back and the alternating sign:
       y2 = d1 + gain_step(gain_input(s * p[-8] + v - d1))

    4. a 4-tap FIR over the second output samples, doubled in 64 bits,
       and a short filter over the first output's residue z:
       g = hi64( 2 * (c22*y2 + c23*y2[-1] + c24*y2[-2] + c25*y2[-3]) )
       r = m0*z[-1] + m1*z[-2] + m2*z[-3]              (32-bit, wrapping)
       w = (u[-8] - g) - d0 + (r >> 11)

    5. the first output sample and the residue the next taps feed on:
       y1 = d0 + gain_step(gain_input(w))
       z  = (u[-8] - g) - y1

    out0 = y1,  out1 = y2
```

`hi64(...)` denotes a 64-bit accumulation of which only the high 32 bits
are kept; every product is a full 64-bit product of a 32-bit value with a
32-bit coefficient. `c4..c25` are entries 4..25 of the coefficient table
(section A.7), the entries the short filter leaves unused, and
`m0..m2` are one of four triples (section A.7) selected by the stream's
kernel parameter as `set = (kp & 7) * 3 + (kp >> 3)`, clamped to 3.

The sign `s` alternates every tap, as in section 8.2, and is the same
state: a stream that switches variants restarts it.

Delay lines needed: 4 carrier samples, 13 interpolator outputs, 3 second
output samples, 3 residues, 8 residuals. The reference keeps them
overlaid in one sliding array of pairs, which is an implementation
choice, not a requirement.

### 8.4 Warm-up, skip and flush

The alternative kernel needs three things the short filter does not:

* **Warm-up.** When a parameter record restarts the stage, the 16 most
  recent carrier pairs are pushed through steps 1 and 2 only, oldest
  first, producing no output. The pairs come from the output stage's pair
  ring (section 9.3) and are scaled by `(3 * x) >> 1` on the way in, the
  inverse of the gain the ring stores them with. During the warm-up the
  shaping filter's output takes the place of the output sample a tap
  would have produced, and the sign is **not** advanced.

* **Skip.** The first **eight** taps of every group are pushed through
  the same way (no output, sign not advanced), so a group of 32
  carrier samples produces 48 output samples per channel, not 64.

* **Flush.** At the end of a stream the eight carrier samples that follow
  the group are pushed through as a full tap each, with zero residual and
  scaled by `x + (x >> 1)`, producing sixteen further output samples per
  channel. These are not clamped (section 9.2).

The short filter of section 8.2 uses the same 16-pair warm-up, but as
*taps*: each stored pair is run through the linear half of the filter
(both prediction and correction are the pair itself, the outputs are the
ideal predictions, no dither and no gain step) and the sign is not
advanced.

## 9. The output stage

*(Reference: `mqa/output_stage.h`.)*

### 9.1 Dither

Both filters consume two dither samples per carrier sample per channel.
They come from a pair of generators (one per channel) whose output is
scaled by the gain record's `scale`:

```
    value = hi32(state * scale)              (unsigned)
    state = state * 0x17385ca9 + 0x47502932
```

The pair is re-seeded every **2048 carrier samples**, counted from the
stream's start:

```
    s        = salt + (counter >> 11)
    state_L  = nr(s * s)
    state_R  = nr(state_L)
```

A salt of 0 means silence: all dither samples are zero. Otherwise the
salt is one of two fixed constants chosen by the stream's salt selector
(section A.5). A restart that begins mid-block MUST advance the left
generator by `2 * (counter mod 2048)` steps before deriving the right
one, which is what continuous generation would have produced.

### 9.2 The group

Per group the output stage:

1. pushes every carrier pair into the **pair ring** (section 9.3);
2. gives the first `skip` carrier samples to the reconstruction filter
   without output (8 for the alternative kernel, 0 for the short one);
3. fills the output buffers with dither for the remaining samples;
4. runs the filter, turning each dither pair into an output pair;
5. **clamps** every output sample to 24 bits, i.e. to
   [-0x800000, 0x7fffff];
6. at the end of a stream only, draws `lookahead` further dither samples
   past the group's output and, for the alternative kernel, flushes it
   (section 8.4). Flushed samples are not clamped;
7. accumulates every output sample into the output CRC (section 9.4);
8. advances the ring position by the group's carrier count.

### 9.3 The carrier pair ring

The stage keeps a ring of the 66 most recent carrier pairs, each scaled
on the way in:

```
    stored = (sample * pair_gain) >> 16
    pair_gain = 0x8000 for variant 1, 0xaab1 for variant 0
```

written downwards (newest at the lowest index, wrapping) and mirrored so
that a run of pairs can be read contiguously. Its consumers are the
filter warm-up of section 8.4 and the passthrough resampler's history.

### 9.4 The output CRC

Every output sample pair feeds a CRC-32 register (reflected polynomial
0xedb88320), four zero steps then an exclusive-or with the sample:

```
    for k in 0..3:  crc = table[crc & 0xff] ^ (crc >> 8)
    crc ^= (u32)sample
```

left sample then right. The stream's sync messages can carry an expected
value; a decoder MAY compare and report, and the reference does so only
while the authentication indicator is set. The register is cleared after
each comparison.

### 9.5 Passthrough

A group with no packet in it (the stream is over, or the file was never
MQA) passes its carrier samples through unchanged, and so does the
flush at the end of a file. Such a group can carry as much as the whole
carrier ring, so an implementation MUST size its output buffers for that,
not for a group.

The reference follows an output-rate state machine here: it tracks the
descriptor's rate code, format and low nibble, and when the situation
changes it selects a resampler record and reconfigures a polyphase
resampler whose history is the pair ring. On every stream to hand the
selected ratio is 1:1 and the samples are copied out unchanged. A decoder
MUST copy them unchanged in that case; the resampling itself is out of
scope for this document (no material to hand exercises it, so its rules
were not recovered).


## 10. Sample conditioning

*(Reference: `mqa/conditioner.h`. This section is **normative in effect
but abbreviated**: see the note at its end.)*

Capturing the data channel leaves every carrier sample's low byte empty.
The reference decoder does not pass such hard-truncated samples on. Before
a group reaches the decoder proper it runs two per-sample stages over it,
and because the decoder's output is built from these samples, their effect
is audible in the output. A decoder that skips them will not match.

**Stage 1, requantisation.** The pair is taken to mid/side, each part
requantised to its top bits with subtractive dither (one noise byte per
sample from a generator re-seeded every 4096 samples) and the rounding
error fed back through a short noise-shaping filter (3 or 6 taps
according to the carrier rate, section A.8). The noise byte is added back
after rounding, so the empty low byte is filled and the rounding error is
uniform:

```
    round(x, noise) = ((u32)(x + 128 - noise) & ~255) + noise
```

Samples within a few thousand steps of full scale are folded back
instead, with bits 0..8 cleared.

**Stage 2, signalling.** The stream's level gain is applied, a leaky
feedback term subtracted, and the sample dithered and requantised once
more with its own error feedback. This time the rounding direction of the
larger of the two errors is chosen so that the sample pair carries one
bit of the control bitstream that the intake extracted 480 frames
earlier: the decoder re-embeds the control channel in what it sends on.
Samples near full scale are coded specially, with a small
variable-length code.

**Markers.** At a resynchronisation both stages write a marker: a handful
of samples quantised to coarse steps with bits of the datasync packet in
their low bits, from which the bitstream's read position is set. Sample
counters and marker positions count frames from the stream's start.

The full per-sample arithmetic runs to some four hundred lines of
wrapping 32-bit operations, and a prose transcription would be as long
and easier to get wrong. This document specifies it by reference: the
module `src/conditioner.c` is the normative statement, and it is
verified against the original decoder over every group of the test
material. The
constants it uses (filter taps, generator seeds, thresholds) are in
section A.8.

## 11. Renderer signalling

*(Reference: `mqa/watermark.h`.)*

A stage-1 decoder embeds a message for the renderer downstream in the
*parity* of its output frames. It is off by default in this library
because it perturbs the samples; a decoder that wants output identical to
the official decoder delivered files MUST embed it.

One message bit per output frame is carried as

```
    parity(frame) = (L ^ R) & 1
```

To make a frame carry the bit it must, each sample is nudged by 0, +1 or
-1 from a 32-entry table indexed by

```
    index = ((state & 14) + carried) * 2
    carried = ((L ^ R) ^ message_bit) & 1
```

where `state` is a 24-bit shift register clocked once per frame with the
reflected polynomial 0x8100c9 (the same register the LSB correction of
section A.9 uses), and the table's entries come in pairs that differ in
parity, so the frame always ends up carrying the bit. The nudge table is
in section A.9.

The message is a repeating cycle of six records, each `type, length,
payload..., checksum`:

| Record | Payload |
| --- | --- |
| 0 | the fixed identifier `8c 49 e0 ab` |
| 1 | a 32-bit configuration word |
| 2 | a 16-bit second word |
| 4 | one byte: renderer profile 4, then 8, then 16 |

The configuration word is

```
    render_filter | (authenticated ? 1 << 8 : 0)
                  | orig_rate_code << 20 | render_bitdepth << 25
```

with the fields as the datasync gave them (section 4.4). Each record's
checksum byte is the low byte of a CRC-32 register (reflected polynomial
0xedb88320) stepped four times per 32-bit word of the record and
exclusive-ored with each word, then run out for three further steps.

Bits are sent most significant first within each byte, and the records
follow one another without framing: a renderer finds the cycle by looking
for record 0's identifier.

## 12. Authentication (informative)

*(Reference: `mqa/intake.h`.)*

A stream declares an authentication level and an info field in every
datasync (section 4.4), and carries type-4 authentication packets whose
384-byte payloads the reference decoder hashes. The hashing was not
recovered, and this document does not specify it.

What is known and reproducible:

* the decoder derives an indicator from the datasync's fields as
  `auth_byte = auth_info | (auth_level << 4)`, and the indicator it
  carries into its output is 3, 2 or 5 according to that byte;
* authentication is evaluated over blocks of 65536 frames; a block that
  passes keeps the stream authenticated for a window of 327680 frames,
  and a decoder starts with a full window;
* **authentication required for decoding.** Removing every authentication
  packet from a stream still yields decodable audio: the reference
  decoder restarts the stream once per missing packet, losing about 2% of
  its output and dropping the "authored" status it reports, but the audio
  that remains is bit-identical. A decoder that cannot authenticate can
  therefore decode; it MUST NOT claim the stream is authenticated.

This library takes every block as authenticating, which is what the
reference's own checks do on authentic material, and derives the
indicator from the datasync. On a stream that failed authentication the
reference would report a lower indicator; this library would not notice.


## Appendix A: constants

The tables marked *generated* below come straight from the reference
implementation's own definitions, printed by `tools/spec-tables`; the
rest are transcribed here.

### A.1 Carrier classes

| Class | M (levels) | param[0] | param[1] | param[2] | word | reciprocal |
| --- | --- | --- | --- | --- | --- | --- |
| 0 | 1 | 0 | 1 | 255 | 0x00000029 | 0x00000000 |
| 1 | 3 | 1 | 5 | 242 | 0x00000024 | 0x55555556 |
| 2 | 4 | 1 | 4 | 255 | 0x00000015 | 0x40000000 |
| 3 | 8 | 2 | 2 | 63 | 0x00000029 | 0x20000000 |

`param[0]` is how many of each channel's two records read the
carrier's digits, `param[1]` how many digits pack into one FIFO
byte, and `param[2] + 1` the radix of the digit coder. `word`'s
low byte is the level-bound base of section 7.4.

### A.2 Rate codes

```
    rate_hz(code) = base[code >> 3] << (code & 7)
    base[4] = { 44100, 48000, 64000, - }        code 31: unset
```


### A.3 Gain records

A gain record is three numbers derived from one `scale` (section 8.1):

```
    shift = number of bits in (scale - 1)
    gain  = (1 << (32 + shift)) / (2 * scale)
```

Every stream examined uses `scale = 203`, giving `gain = 0x50b59897`
and `shift = 7`. A `scale` of 0 or less makes the record inactive.


### A.4 Refinement coefficient sets

Four sets; a stream selects one with `(kernel_param & 7) * 3 +
(kernel_param >> 3)`, clamped to 3. `taps` is the FIR's pair
count, so it has `2 * taps + 2` coefficients; `shift` is the
coarse symbol's width in bits.

Set 0 (taps 9, shift 3):

```
    2887, 956, -296, -202, -1247, -1975, -826, 129, 118, 369, 667, 405, 117, 62, -42, -140, -114, -76, -59, -24
```

Set 1 (taps 9, shift 3):

```
    3190, 1617, -177, -230, -645, -1940, -1856, -474, 191, 139, 425, 735, 503, 148, 42, -18, -135, -166, -95, -27
```

Set 2 (taps 3, shift 4):

```
    5282, 6571, 3875, -439, -2659, -2134, -822, -134
```

Set 3 (taps 3, shift 4):

```
    5144, 7493, 7151, 4495, 1625, 99, -180, -59
```

### A.5 Generators, seeds and salts

Two linear congruential generators appear throughout. The **scrambler**
recurrence, used for dither and for the data channel:

```
    lcg(x) = x * 0x17385ca9 + 0x47502932
```

and the **Numerical Recipes** recurrence, used to derive seeds:

```
    nr(x)  = x * 0x0019660d + 0x3c6ef35f
```

| Where | Salt or seed | Reseeded |
| --- | --- | --- |
| data channel (§5.1) | `0x2082352c + reseeds` | every 2048 byte pairs |
| residual records (§7.3) | `0xa1e24bba` (left), `0xa1e24cba` (right) | every 4096-sample block |
| refinement (§6.2) | the stream's salt, 0 for none | every 4096-sample block |
| output dither (§9.1) | `0xf807b7df` or `0xe9d30005`, by the stream's selector; 0 means silence | every 2048 carrier samples |
| conditioner stage 1 (§10) | `0xe7e1faee` | every 4096 samples |
| conditioner stage 2 (§10) | `0xf807b7df` or `0xd5c31f79`, by the stream's dither mode | every 4096 samples |
| refinement setup (§6.1) | `0x3895afe1` for a decoding packet, `0xf807b7df` or `0x3895afe1` otherwise | at a packet start |

The reseed recipe is the same everywhere: square the salt plus the block
index (32-bit, wrapping), then `nr` it for the first state and `nr` that
for the second.


### A.6 Entropy coder tables

The 32-bin cumulative distribution:

| Bin | base | threshold | width | reciprocal | weight |
| --- | --- | --- | --- | --- | --- |
| 0 | -1073741824 | 0x00000000 | 0 | 0x00000000 | 9251 |
| 1 | -524288 | 0x00040000 | 2 | 0x80000000 | 7173 |
| 2 | -393216 | 0x00080000 | 9 | 0x1c71c71d | 4424 |
| 3 | -327680 | 0x00110000 | 76 | 0x035e50d8 | 3408 |
| 4 | -262144 | 0x005d0000 | 277 | 0x00ec9792 | 2945 |
| 5 | -229376 | 0x00e78000 | 683 | 0x005ff402 | 2731 |
| 6 | -196608 | 0x023d0000 | 1586 | 0x00295252 | 2520 |
| 7 | -163840 | 0x05560000 | 3390 | 0x00135509 | 2323 |
| 8 | -131072 | 0x0bf50000 | 5547 | 0x000bd08f | 2192 |
| 9 | -114688 | 0x115fc000 | 7461 | 0x0008c8a8 | 2141 |
| 10 | -98304 | 0x18a90000 | 9693 | 0x0006c2dc | 2080 |
| 11 | -81920 | 0x22204000 | 12116 | 0x000568b8 | 2027 |
| 12 | -65536 | 0x2df54000 | 14546 | 0x00048164 | 1987 |
| 13 | -49152 | 0x3c29c000 | 19039 | 0x00037134 | 1954 |
| 14 | -32768 | 0x4ec18000 | 23020 | 0x0002d8d0 | 1925 |
| 15 | -16384 | 0x653c8000 | 27414 | 0x000263ff | 1917 |
| 16 | 0 | 0x80020000 | 27414 | 0x000263ff | 1917 |
| 17 | 16384 | 0x9ac78000 | 23020 | 0x0002d8d0 | 1925 |
| 18 | 32768 | 0xb1428000 | 19039 | 0x00037134 | 1954 |
| 19 | 49152 | 0xc3da4000 | 14546 | 0x00048164 | 1987 |
| 20 | 65536 | 0xd20ec000 | 12116 | 0x000568b8 | 2027 |
| 21 | 81920 | 0xdde3c000 | 9693 | 0x0006c2dc | 2080 |
| 22 | 98304 | 0xe75b0000 | 7461 | 0x0008c8a8 | 2141 |
| 23 | 114688 | 0xeea44000 | 5547 | 0x000bd08f | 2192 |
| 24 | 131072 | 0xf40f0000 | 3390 | 0x00135509 | 2323 |
| 25 | 163840 | 0xfaae0000 | 1586 | 0x00295252 | 2520 |
| 26 | 196608 | 0xfdc70000 | 683 | 0x005ff402 | 2731 |
| 27 | 229376 | 0xff1c8000 | 277 | 0x00ec9792 | 2945 |
| 28 | 262144 | 0xffa70000 | 76 | 0x035e50d8 | 3408 |
| 29 | 327680 | 0xfff30000 | 9 | 0x1c71c71d | 4424 |
| 30 | 393216 | 0xfffc0000 | 2 | 0x80000000 | 7173 |
| 31 | 524288 | 0x00000000 | 0 | 0x00000000 | 9251 |

Bin selection by the code word's top nibble:

```
    bin_by_nibble[16] = { 8, 10, 12, 13, 14, 14, 15, 15,
                          16, 17, 17, 18, 19, 21, 23, 30 }
```

Magnitude class by `|x| >> 14`, capped at 31:

```
    class_by_magnitude[32] = {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 8, 9, 9, 10, 10, 11, 11,
        12, 12, 12, 12, 13, 13, 13, 13, 14, 14, 14, 14, 14, 14, 14, 14 }
```

The AR taps' decay weights, and the P/Q predictor's constants:

```
    decay[4] = { 0xf000, 0xe100, 0xd2f0, 0xc5c1 }
    K0 = 0x51147576, K1 = 0x6c1b4748, K2 = 0x1b06d1d2
```

The level, rate and scale tables of section 7.3:

```
    level_table[4] = { 256, 304, 362, 431 }
    rate_table[4]  = { 4176, 4256, 4416, 4704 }
    scale_table[64] = {
        0, 1, 2, 4, 8, 11, 16, 21, 25, 32,
        35, 41, 45, 51, 57, 64, 71, 81, 91, 101,
        115, 128, 143, 161, 181, 203, 229, 256, 287, 323,
        363, 407, 457, 512, 575, 645, 725, 813, 913, 1024,
        1149, 1291, 1449, 1625, 1825, 2048, 2299, 2581, 2897, 3251,
        3649, 4096, 4597, 5161, 5793, 6501, 7299, 8192, 9195, 10321,
        11585, 13003, 14597, 16384 }
```

A record's scale is `scale_table[index & 63]`, from which its gain
record follows by the recipe of section 8.1.

### A.7 Reconstruction coefficients

One table of 26 Q31 coefficients. The short filter of section 8.2
uses entries 0..3 as `c0..c3`; the alternative kernel of section
8.3 uses 4..25 as `c4..c25`.

```
    0x01540000, 0x2ea50000, 0x0bf00000, 0x6b140000, 0x53510000,
    0x08520000, 0xf9690000, 0x005a0000, 0xffce0000, 0x011a0000,
    0xfcc60000, 0x06ee0000, 0xf33c0000, 0x15ed0000, 0xd9e90000,
    0x4e550000, 0x73ff0000, 0xbfe80000, 0x1d900000, 0xf6800000,
    0x02550000, 0xffb10000, 0x47500000, 0x2a100000, 0x15f00000,
    0xf8b00000,
```

The alternative kernel's three shaping integers, one triple per
stream parameter set:

```
    set 0: 2402, 8, -614
    set 1: 2564, 484, -412
    set 2: 3067, 1898, 406
    set 3: 3307, 2964, 1300
```

### A.8 Conditioning constants

The noise-shaping filters, selected by the carrier's rate code as
`row = (code & 7) * 3 + ((i8)code >> 3)`, clamped to 3. The first entry
of a row says whether the filter has six taps or three; the taps
themselves start at the row's third entry:

```
    { 1, 0, 2866,  811, 305, 396, -647, -551 }
    { 1, 0, 3006,  946, 162, 446, -351, -445 }
    { 0, 0, 3797, 2667, 668,   0,    0,    0 }
    { 0, 0, 2867, 1834, 446,   0,    0,    0 }
```

Full scale for the stage is `0x800000 >> shift`, with the special-coding
thresholds `full - 8192` and `full - 1024`.

The level gain (2^(-level/256) in Q31) and the datasync's gain index
(2^(index/32) in Q15) are computed as:

```
    gain(level):  frac = level << 16;  e = level >> 16
                  t = hi32(0x55555555 * frac)
                  u = hi32(t * frac)
                  return (i32)(t + 0x80000000 - (u >> 2)) >> e

    gain_index[16] = { 32768, 33486, 34219, 34968, 35734, 36516, 37316,
                       38133, 38968, 39821, 40693, 41584, 42495, 43425,
                       44376, 45348 }
```


### A.9 Output-side tables

The LSB correction's 16 (dL, dR) pairs, indexed by
`(scrambler & 0xe) | ((L ^ R ^ priming_bit) & 1)`:

```
    {0,0}, {0,1}, {0,0}, {1,0}, {0,0}, {0,-1}, {0,0}, {-1,0}, {1,1}, {0,1}, {1,-1}, {1,0}, {-1,-1}, {0,-1}, {-1,1}, {-1,0}
```

Its scrambler is a 24-bit register with reflected polynomial
0x8100c9, seeded 0xffffff and clocked with a zero byte per sample.
The renderer signalling of section 11 clocks the same register.

The signalling's nudge table, sixteen (left, right) pairs:

```
     0,  0,   0,  1,   0,  0,   1,  0,
     0,  0,   0, -1,   0,  0,  -1,  0,
     1,  1,   0,  1,   1, -1,   1,  0,
    -1, -1,   0, -1,  -1,  1,  -1,  0
```

## Appendix B: test vectors

The files under `docs/vectors/` are machine-readable vectors for the
stages a decoder is most likely to get subtly wrong. Each is a text file
of `name: value value ...` lines; values are decimal where the quantity
is signed and `0x`-prefixed where it is a state word. Comment lines begin
with `#`.

They are generated by `tools/spec-vectors`, which also checks an
implementation against them:

```
    build/spec-vectors docs/vectors        write them
    build/spec-vectors -c docs/vectors     check this library against them
```

The inputs are synthetic patterns, so the files carry no audio.

| File | Stage | Section |
| --- | --- | --- |
| `lcg.txt` | both generators, and the jump-ahead | 5.1, A.5 |
| `descrambler.txt` | data-channel descrambling, fresh and at a position | 5.1 |
| `entropy.txt` | 16 symbols from a known ring, and the state after | 7.2, 7.5, 7.6 |
| `predictor.txt` | 32 residuals from two 16-symbol records | 7.7 |
| `reconstruct.txt` | eight taps of the short filter | 8.2 |
| `recon2.txt` | a 16-pair warm-up and 24 taps of the alternative kernel | 8.3, 8.4 |
| `signalling.txt` | 64 frames of renderer signalling and the message bytes | 11 |

## Appendix C: differences from the reference decoder

The reference decoder does several things this document does not specify,
because the material to hand never exercised them and their rules could
not be recovered:

1. **Rate conversion in the passthrough** (section 9.5). The state
   machine that selects a resampler is specified; the resampling is not.
2. **The alignment mode** a packet start can leave the decoder in
   (`mode 1`).
3. **Authentication hashing** (section 12).
4. **The LSB correction's refresh** (section A.9). The mechanism is
   specified and verified; where a *second* priming segment comes from,
   once the first is exhausted, is characterised structurally but not
   verified against a stream that needs one.

A decoder that meets any of these SHOULD report that it cannot decode the
stream rather than emit approximate audio.

## Appendix D: references

* Bob Stuart and Peter Craven, *A Hierarchical Approach to Archiving and
  Distribution*, AES 137th Convention, 2014.
* Peter Craven and Malcolm Law, *Digital encapsulation of audio signals*
  (MQA Limited), e.g. US 10,867,614 B2.
* Michael Gerzon, Peter Craven, J. Robert Stuart, Malcolm Law and Rhonda
  Wilson, *The MLP Lossless Compression System for PCM Audio*, JAES
  52(3), 2004.
* Michael Gerzon and Peter Craven, *Optimal Noise Shaping and Dither of
  Digital Signals*, AES 87th Convention, 1989.
* G. N. N. Martin, *Range encoding: an algorithm for removing redundancy
  from a digitised message*, 1979.
* Wim Sweldens, *The Lifting Scheme*, ACHA, 1996.
* Brian Chen and Gregory Wornell, *Quantization index modulation*, IEEE
  Trans. Inf. Theory 47(4), 2001.
* `docs/prior-art.md` in this repository maps each stage onto the
  published technique it implements.
