CC=gcc
CFLAGS=-Wall -Wextra -std=c99 -pedantic -O2
TARGET=assembler
SRC=assembler.c

ifeq ($(DEBUG),1)
CFLAGS += -DDEBUG
endif

.PHONY: all clean test test-illegal test-c64 test-compare

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)

test: $(TARGET) tests/test.asm
	./$(TARGET) tests/test.asm tests/test.bin

test-illegal: $(TARGET) tests/illegal.asm
	./$(TARGET) --illegal-opcodes tests/illegal.asm tests/illegal.bin

test-c64: $(TARGET) tests/c64.asm
	./$(TARGET) tests/c64.asm tests/c64.prg

test-compare: all
	python3 tools/compare_assemblers.py

clean:
	rm -f $(TARGET) tests/test.bin tests/illegal.bin tests/c64.prg
	rm -rf build
