CC     ?= gcc
CFLAGS ?= -O2 -Wall
LDLIBS ?= -lnuma

hbm-drain: hbm-drain.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f hbm-drain

.PHONY: clean
