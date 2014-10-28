#!/usr/bin/make

CC      = gcc
CFLAGS  ?= -g 
LDFLAGS ?= -D"X11_LIBEXEC_PROG=\"/usr/lib/slurm/slurm-spank-x11\""

all: slurm-spank-x11

default: slurm-spank-x11

x11.so:
	$(CC) $(CFLAGS) -shared -fPIC -o x11.so $(LDFLAGS) slurm-spank-x11-plug.c


slurm-spank-x11: x11.so
	$(CC) $(CFLAGS) -o slurm-spank-x11 $(LDFLAGS) slurm-spank-x11.c

install: all
	mkdir -p $(BUILDROOT)/usr/lib/slurm/
	install -m 755 slurm-spank-x11 $(BUILDROOT)/usr/lib/slurm/
	install -m 755 x11.so $(BUILDROOT)/usr/lib/slurm/

clean:
	rm -f slurm-spank-x11 x11.so

mrproper: clean

