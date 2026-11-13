CC = cc
CFLAGS = -Wall -Wextra -pedantic -std=c99
SRC = src/main.c src/lexer.c src/parser.c src/codegen.c src/errors.c src/ir.c
OUT = ltng

all: $(OUT)

$(OUT): $(SRC) src/ltng.h
	$(CC) $(CFLAGS) -o $(OUT) $(SRC)

clean:
	rm -f $(OUT)

.PHONY: all clean
