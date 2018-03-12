prefix      ?= /usr/local
exec_prefix ?= $(prefix)
bindir      ?= $(exec_prefix)/bin

.PHONY: donkey clean install uninstall docker

CPPFLAGS +=  -D_FORTIFY_SOURCE=2
CFLAGS   += -static -s -std=c11 -pedantic -O2 -fstack-protector -Wall -Wextra -Wcast-align -Wpointer-arith \
            -Wwrite-strings -Wlogical-op -Wformat=2 -Wmissing-format-attribute -Winit-self -Wshadow \
            -Wstrict-prototypes -Wunreachable-code -Wconversion -Wsign-conversion

donkey: donkey.c
	$(CC) $(CFLAGS) $(CPPFLAGS) $(OUTPUT_OPTION) $<

clean:
	rm -f donkey

install: donkey
	install -d -m 755 $(DESTDIR)$(bindir)
	install -m 755 donkey $(DESTDIR)$(bindir)

uninstall:
	rm -f $(DESTDIR)$(bindir)/donkey

docker:
	docker build -t donkey:1.0.0 .
	docker run --rm donkey:1.0.0 > donkey
	chmod +x donkey
