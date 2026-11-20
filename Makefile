CC   = gcc

BINDIR = /usr/bin

TARGET_MACHINE := $(shell $(CC) -dumpmachine)

# i386 with SSE2 maths (CFLAGS="-msse2 -mfpmath=sse" in the environment) needs
# a 16-byte aligned stack: -mstackrealign on gcc 4.2 and later, else every
# frame kept 16-byte aligned (gcc 3.4 of uClibc toolchains). No cost on x87.
I386_CFLAGS = $(shell $(CC) -mstackrealign -E -x c /dev/null >/dev/null 2>&1 && echo -mstackrealign || echo -mpreferred-stack-boundary=4)

ifeq ($(findstring x86_64,$(TARGET_MACHINE)),x86_64)

ELF_FORMAT = elf64-x86-64
ELF_ARCH   = i386:x86-64

else ifneq ($(findstring i386,$(TARGET_MACHINE)),)

ELF_ARCH   = i386
STATIC = -static
ARCH_CFLAGS = $(I386_CFLAGS)

else ifneq ($(findstring i686,$(TARGET_MACHINE)),)

ELF_FORMAT = elf32-i386
ELF_ARCH   = i386
ARCH_CFLAGS = $(I386_CFLAGS)

else

$(error Unsupported target: $(TARGET_MACHINE))

endif

SRCS = sop.c mmidi/mmidi.c mmidi/mmmodel.c audio/audio.c audio/pcm.c

OBJS = $(SRCS:.c=.o)
TARGET = sop

# mmidi: .sf2 banks only (-DMM_NO_MODELS), sop has its own audio output
# (-DMM_NO_OSS). More options for mmidi (-DMM_NO_REVERB, -DMM_NO_CHORUS,
# -DMAXVOICE=n): see mmidi/README.md.
#-Wshadow
CFLAGS += -Wall  -Wextra --std=gnu99 -Os -ffunction-sections -fdata-sections \
	-Wno-deprecated-declarations -I./audio -I./mmidi -DAUDIO_HAVE_OSS -DAUDIO_HAVE_TINYALSA \
	-DMM_NO_MODELS -DMM_NO_OSS
override CFLAGS += $(ARCH_CFLAGS)

LDFLAGS += $(STATIC) -Wl,--gc-sections,--sort-common,-s -lm

.PHONY: all clean install

all: $(TARGET)

sopbank.h: sop.sf2
	xxd -i sop.sf2 sopbank.h

sop.o: sopbank.h

$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS) $(CFLAGS)

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/

clean:
	rm -f $(TARGET) $(OBJS)
