CC=gcc
CFLAGS=-Wall -Wextra -std=c99 -pedantic -O2
TARGET=assembler
SRC=assembler.c

ifeq ($(DEBUG),1)
CFLAGS += -DDEBUG
endif

.PHONY: all clean test test-illegal test-compare

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)

test: $(TARGET) test.asm
	./$(TARGET) test.asm test.bin

test-illegal: $(TARGET) illegal.asm
	./$(TARGET) --illegal-opcodes illegal.asm illegal.bin

test-compare: all
	python3 tools/compare_assemblers.py

clean:
	rm -f $(TARGET) test.bin illegal.bin
	rm -rf build
