# The library, module by module

Each header under `include/mqa/` is one stage of the decode. The
specification section that defines the stage is given in the last
column; the header's own comment says what the spec does not, which is
how the code is arranged.

| Header | Stage | Spec |
| --- | --- | --- |
| `bitstream.h` | finds the control channel in bits 8..15 of the samples and parses its packets | 4 |
| `intake.h` | turns packets and samples into the per-group record the decoder runs on; conditions the samples | 3, 4, 10, 12 |
| `descrambler.h` | recovers the data channel: one byte per sample, XOR-scrambled by an LCG | 5.1 |
| `stream.h` | frames those bytes into checked messages and routes the payloads | 5.2-5.4 |
| `carrier.h` | the small digit every carrier sample carries | 2.4 |
| `refine.h` | the correction applied to the carrier before reconstruction | 6 |
| `entropy.h` | the adaptive arithmetic decoder that produces symbol records | 7.2, 7.5, 7.6 |
| `predictor.h` | turns two 16-symbol records into 32 residuals | 7.7 |
| `residuals.h` | two entropy decoders and one predictor per channel | 7.1 |
| `residual_stage.h` | the controller: blocks, gain records, level bounds, the digit FIFO, status | 7.3, 7.4, 7.8-7.10 |
| `lifting.h` | the fixed-point gain step the filters and the predictor share | 8.1 |
| `reconstruct.h` | the short reconstruction filter (variant 1) | 8.2 |
| `recon2.h` | the long reconstruction kernel (variant 0) | 8.3, 8.4 |
| `output_stage.h` | dither, the filter per sample, clamping, the output CRC | 9 |
| `resampler.h` | the passthrough's rate-status machine (resampling itself is not implemented) | 9.5 |
| `conditioner.h` | the requantisation and signalling the intake applies to the carrier | 10 |
| `watermark.h` | the renderer signalling carried in the parity of output frames | 11 |
| `lsb_correction.h`, `crc24.h` | the final per-sample +-1 correction | A.9 |
| `crc32.h`, `lcg.h` | the CRC-32 and the two generators used throughout | 9.4, A.5 |
| `decoder.h` | the per-group orchestration of everything above | 3 |
| `stream_decoder.h` | the whole decoder behind one feed/run interface | -- |

