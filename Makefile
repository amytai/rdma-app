OBJECTS := test test2

all: $(OBJECTS)

$(OBJECTS): %: %.c
	gcc -g -I/usr/include/ $< -o $@ -libverbs

clean:
	rm -f $(OBJECTS)
