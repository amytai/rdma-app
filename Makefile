OBJECTS := test
HELLO := helloworld

all: $(OBJECTS) $(HELLO)

$(OBJECTS): %: %.c
	gcc -g -I/usr/include/ $< -o $@ -libverbs

$(HELLO): %: %.c
	gcc -g -o $(HELLO) $<

clean:
	rm -f $(OBJECTS) $(HELLO)
