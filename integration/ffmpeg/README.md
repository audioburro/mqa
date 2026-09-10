# FFmpeg

FFmpeg has no plugin ABI: filters and codecs are compiled in. So there
are two ways to get an MQA unfold out of it, and both are described in
`../README.md`:

* `af_mqa.c` - a libavfilter filter to build into a tree.
  `register.patch` has the two lines that register it and the configure
  flags that point at this library. Built and run against FFmpeg 7.1
  here; its output is identical to the library's own.

* a pipe - stock ffmpeg, no rebuild:

  ```
  ffmpeg -v error -i track.flac -f s32le -c:a pcm_s32le - \
    | ../../build/adapter-demo -p raw -r 44100 \
    | ffmpeg -f s32le -ar 88200 -ac 2 -i - -c:a flac out.flac
  ```

`-f s32le` is what makes this work: it hands over samples in exactly the
alignment the library wants, for both 16- and 24-bit sources.
