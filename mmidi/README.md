# mmidi (bundled)

These files are `v2/mmidi.c`, `v2/mmidi.h`, `v2/mmmodel.c`, `v2/mmmodel.h` and
`LICENSE` from [mmidi](https://github.com/realgraybox/mmidi), unchanged,
at commit 1309f07 (API 2.1.0). sop builds them with `-DMM_NO_MODELS -DMM_NO_OSS`.

The full documentation, build options and API reference are in mmidi's
`v2/README.txt`; section 3.4 maps TinySoundFont and TinyMidiLoader calls to
mmidi.

i386 builds with SSE2 maths (`-msse2 -mfpmath=sse`) need a 16-byte aligned
stack: the Makefile adds `-mstackrealign` (gcc 4.2 and later) or
`-mpreferred-stack-boundary=4` (older gcc).
