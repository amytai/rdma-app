OBJECTS := controller exokernel
HELLO := helloworld
ELF := elf

all: helper $(OBJECTS) $(HELLO) $(ELF)

helper:
	gcc -c -I/usr/include/ helper.c

$(OBJECTS): %: %.c
	gcc -g -I/usr/include/ $< -o $@ helper.o -libverbs

$(HELLO): %: %.c
	gcc -g -static -o $(HELLO) $< -static

$(ELF): %: %.c
	gcc -g -o $(ELF) $<

clean:
	rm -f $(OBJECTS) $(HELLO) $(ELF)
