CFLAGS = -std=c99 -Wpedantic -Wall -Wextra -Wconversion -Wshadow -O2
TARGET = xpipe
OBJECTS = xpipe.o

.PHONY: test clean

$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) $(OBJECTS) -o $@ $(LDFLAGS)

test: $(TARGET)
	@PATH=${PWD}:${PATH} tests/run

clean:
	rm -f $(TARGET) $(OBJECTS)
