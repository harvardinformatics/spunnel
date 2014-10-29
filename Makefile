CC      ?= gcc
CFLAGS  ?= -g 
#LDFLAGS ?= -D"X11_LIBEXEC_PROG=\"/usr/lib/slurm/slurm-spank-x11\""

all: stunnel

default: stunnel

stunnel.so:
	$(CC) $(CFLAGS) -shared -fPIC -o stunnel.so $(LDFLAGS) stunnel-plug.c


stunnel: stunnel.so
	$(CC) $(CFLAGS) -o stunnel $(LDFLAGS) stunnel.c

install: all
	mkdir -p $(BUILDROOT)/usr/lib/slurm/
	install -m 755 stunnel $(BUILDROOT)/usr/lib/slurm/
	install -m 755 stunnel.so $(BUILDROOT)/usr/lib/slurm/

clean:
	rm -f stunnel stunnel.so

mrproper: clean

