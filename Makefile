UNAME := $(shell uname)

ifeq ($(UNAME), Darwin)
all: macos
else
all: x11
endif

# --- X11 backend (Linux) ---
CC       = gcc
CFLAGS   = -Wall -Wextra -O2 -g
CFLAGS  += $(shell pkg-config --cflags x11 xcomposite xdamage xfixes xrender xext gl)
LDFLAGS  = $(shell pkg-config --libs x11 xcomposite xdamage xfixes xrender xext gl)
LDFLAGS += -lm

x11: screenshader screenshader-preview

screenshader: screenshader.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

screenshader-preview: screenshader-preview.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# --- macOS backend ---
macos: macos/screenshader-macos

macos/screenshader-macos: macos/screenshader-macos.swift
	swiftc -O -o $@ $<

# --- Clean ---
.PHONY: all clean x11 macos

clean:
	rm -f screenshader screenshader-preview macos/screenshader-macos
