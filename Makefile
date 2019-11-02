OBJECTS += dicker.o
DEPS += dicker.h
CFLAGS += -g -O3
CC = gcc

mane: $(OBJECTS)
	$(CC) $(OBJECTS) $(DEPS)

%.o: %.c %.h
	$(CC) $< $(CFLAGS) -o $@ 

clean:
	rm -f ./*.o ./*.*.swp ./*.*.gch a.out
