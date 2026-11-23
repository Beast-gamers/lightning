CC = cc
CFLAGS = -Wall -Wextra -pedantic -std=c99
SRCDIR = source/src
SRC = $(SRCDIR)/main.c $(SRCDIR)/lexer.c $(SRCDIR)/parser_sym.c $(SRCDIR)/parser_expr.c $(SRCDIR)/parser_stmt.c $(SRCDIR)/codegen.c $(SRCDIR)/errors.c $(SRCDIR)/ir.c
OUT = ../builds/bin/ltng

all: $(OUT)

$(OUT): $(SRC) $(SRCDIR)/ltng.h
	@mkdir -p ../builds/bin
	$(CC) $(CFLAGS) -o $(OUT) $(SRC)

clean:
	rm -f $(OUT)

.PHONY: all clean
