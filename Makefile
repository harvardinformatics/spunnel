CC      ?= gcc
CFLAGS  ?= -g 

all: stunnel.so

default: stunnel.so

stunnel.so:
	$(CC) $(CFLAGS) -shared -fPIC -o stunnel.so $(LDFLAGS) stunnel-plug.c


install: all
	install -m 755 stunnel.so $(BUILDROOT)/n/sw

clean:
	rm -f stunnel.so
