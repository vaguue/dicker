OBJECTS += sender.o
DEPS += sender.h
CFLAGS += -g
CC = gcc

mane: $(OBJECTS)
	$(CC) $(OBJECTS) $(DEPS)

%.o: %.c %.h
	$(CC) $< $(CFLAGS) -o $@ 

clean:
	rm -f ./*.o ./*.*.swp ./*.*.gch a.out
