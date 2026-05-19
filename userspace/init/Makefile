CC      = musl-gcc
CFLAGS  = -static -O2

INIT_ELF = ../../kernel/init/init.elf

.PHONY: all clean

all: $(INIT_ELF)

$(INIT_ELF): init.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(INIT_ELF)
