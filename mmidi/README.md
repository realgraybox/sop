# mmidi (bundled)

Version: v2.1.0 (API 2.1.0)

These files are `v2/mmidi.c`, `v2/mmidi.h`, `v2/mmmodel.c`, `v2/mmmodel.h` and
`LICENSE` from [mmidi](https://github.com/realgraybox/mmidi), unchanged, at
the version above (v2.1.0 is commit 6411ce4). sop builds them with
`-DMM_NO_MODELS -DMM_NO_OSS`.

To update them to another released version of mmidi:

    mmidi/update.sh v2.1.1                 # downloads from GitHub
    mmidi/update.sh v2.1.1 ../mmidi        # or from a local clone

The script copies the files and sets the Version line above; then rebuild,
test, and check `git diff`. mmidi's versions are listed in its
`v2/README.txt`, section 3.3.

The full documentation, build options and API reference are in mmidi's
`v2/README.txt`; section 3.4 maps TinySoundFont and TinyMidiLoader calls to
mmidi.

i386 builds with SSE2 maths (`-msse2 -mfpmath=sse`) need a 16-byte aligned
stack: the Makefile adds `-mstackrealign` (gcc 4.2 and later) or
`-mpreferred-stack-boundary=4` (older gcc).
