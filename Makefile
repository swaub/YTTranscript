# ------------------------------------------------------------------ #
#  YTTranscript.exe -- mingw-w64 cross build from WSL.
#
#  Prereq (one time):
#    sudo apt-get install -y gcc-mingw-w64-x86-64 binutils-mingw-w64-x86-64
#
#  Usage:
#    make          # build dist/YTTranscript.exe
#    make clean     # remove build/ and dist/
# ------------------------------------------------------------------ #

CC      := x86_64-w64-mingw32-gcc
WINDRES := x86_64-w64-mingw32-windres

CFLAGS  := -std=c11 -municode -mwindows -static -O2 \
           -DUNICODE -D_UNICODE -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00 \
           -Wall -Wextra

LDLIBS  := -lcomctl32 -lwinhttp -lshlwapi -ldxgi -ldxguid \
           -luser32 -lgdi32 -lkernel32

SRCDIR  := src
RESDIR  := res
OBJDIR  := build
DISTDIR := dist

SRCS := $(SRCDIR)/main.c $(SRCDIR)/ui.c $(SRCDIR)/util.c $(SRCDIR)/proc.c \
        $(SRCDIR)/pipeline.c $(SRCDIR)/strutil.c $(SRCDIR)/summarize.c \
        $(SRCDIR)/download.c $(SRCDIR)/bootstrap.c $(SRCDIR)/clipboard.c \
        $(SRCDIR)/hwdetect.c $(SRCDIR)/settings.c $(SRCDIR)/progressbar.c

OBJS := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(SRCS))
RES  := $(OBJDIR)/app.res
TARGET := $(DISTDIR)/YTTranscript.exe

.PHONY: all clean
all: $(TARGET)

$(TARGET): $(OBJS) $(RES) | $(DISTDIR)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(RES) $(LDLIBS)
	@echo BUILT $@

$(OBJDIR)/%.o: $(SRCDIR)/%.c $(SRCDIR)/common.h | $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

# -I res so bare resource names in app.rc resolve to res/.
$(RES): $(RESDIR)/app.rc $(RESDIR)/app.manifest | $(OBJDIR)
	$(WINDRES) -c 65001 -I $(RESDIR) $< -O coff -o $@

$(OBJDIR):
	mkdir -p $(OBJDIR)

$(DISTDIR):
	mkdir -p $(DISTDIR)

# Only removes build artifacts. NEVER deletes the downloaded bin/ + models/
# data under dist/ (re-downloading that is ~2.3 GB).
clean:
	rm -rf $(OBJDIR)
	rm -f $(TARGET) $(DISTDIR)/app.res
