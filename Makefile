OBJECTS := test
HELLO := helloworld
ELF := elf

all: $(OBJECTS) $(HELLO) $(ELF)

$(OBJECTS): %: %.c
	gcc -g -I/usr/include/ $< -o $@ -libverbs

$(HELLO): %: %.c
	gcc -g -static -o $(HELLO) $< -static

$(ELF): %: %.c
	gcc -g -o $(ELF) $<

clean:
	rm -f $(OBJECTS) $(HELLO) $(ELF)
