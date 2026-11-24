#include "ltng.h"

static ErrorState *lexer_errors = NULL;

char *ltng_strdup(const char *s) {
    if (!s) { char *p = (char*)malloc(1); if (p) p[0] = 0; return p; }
    size_t n = strlen(s);
    char *p = (char*)malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n + 1);
    return p;
}

Token token_create(TokenType type, const char *value, int line, int col) {
    Token t;
    t.type = type;
    t.value = ltng_strdup(value ? value : "");
    t.line = line;
    t.col = col;
    return t;
}

void token_free(Token *t) {
    if (!t) return;
    free(t->value);
    t->value = NULL;
}

void lexer_init(Lexer *lexer, char *source) {
    lexer->source = source;
    lexer->source_len = strlen(source);
    lexer->pos = 0;
    lexer->line = 1;
    lexer->col = 1;
    lexer->count = 0;
    lexer->current = 0;
    lexer->token_capacity = 4096;
    lexer->tokens = malloc(4096 * sizeof(Token));
    if (!lexer->tokens) {
        fprintf(stderr, "Error: Cannot allocate token buffer\n");
        exit(1);
    }
}

static char lexer_peek_char(Lexer *lexer) {
    if (lexer->pos >= lexer->source_len) return '\0';
    return lexer->source[lexer->pos];
}

static char lexer_advance(Lexer *lexer) {
    char c = lexer->source[lexer->pos];
    lexer->pos++;
    if (c == '\n') {
        lexer->line++;
        lexer->col = 1;
    } else {
        lexer->col++;
    }
    return c;
}

static void lexer_skip_whitespace(Lexer *lexer) {
    while (lexer->pos < lexer->source_len) {
        char c = lexer_peek_char(lexer);
        if (c == ' ' || c == '\t' || c == '\r') {
            lexer_advance(lexer);
        } else if (c == '#') {
            while (lexer->pos < lexer->source_len && lexer_peek_char(lexer) != '\n') {
                lexer_advance(lexer);
            }
        } else {
            break;
        }
    }
}

static void lexer_read_string(Lexer *lexer, Token *token) {
    token->col = lexer->col;
    lexer_advance(lexer);
    int cap = 64, i = 0;
    char *buf = (char*)malloc(cap);
    if (!buf) { free(token->value); token->value = ltng_strdup(""); return; }
    while (lexer->pos < lexer->source_len && lexer_peek_char(lexer) != '"' && lexer_peek_char(lexer) != '\n') {
        char c = lexer_advance(lexer);
        if (c == '\\' && lexer->pos < lexer->source_len) {
            c = lexer_advance(lexer);
            switch (c) {
                case 'n': c = '\n'; break; case 't': c = '\t'; break; case '\\': c = '\\'; break;
                case '"': c = '"'; break; case '\'': c = '\''; break; case '<': c = '<'; break;
                case '>': c = '>'; break; case '0': c = '\0'; break; default: break;
            }
        }
        if (i + 2 >= cap) { cap *= 2; char *nb = (char*)realloc(buf, cap); if(!nb){free(buf); free(token->value); token->value=ltng_strdup(""); return;} buf=nb; }
        buf[i++] = c;
    }
    buf[i] = '\0';
    for (int j = 0; buf[j]; j++) { if (buf[j] == '%') { if (buf[j+1]=='%'){j++; continue;} if (lexer_errors) { error_state_add(lexer_errors, token->line, token->col, "Format specifiers are not allowed"); } break; } }
    if (lexer->pos < lexer->source_len && lexer_peek_char(lexer) == '"') lexer_advance(lexer);
    else if (lexer_errors) error_state_add(lexer_errors, token->line, token->col, "Unterminated string literal");
    free(token->value); token->value = buf;
}

static void lexer_read_char(Lexer *lexer, Token *token) {
    token->col = lexer->col;
    lexer_advance(lexer);
    int cap = 8, i = 0;
    char *buf = (char*)malloc(cap);
    if (!buf) { free(token->value); token->value = ltng_strdup(""); return; }
    if (lexer->pos >= lexer->source_len || lexer_peek_char(lexer) == '\'') {
        if (lexer_errors) error_state_add(lexer_errors, lexer->line, lexer->col, "Empty char literal");
        if (lexer->pos < lexer->source_len) lexer_advance(lexer);
        buf[0] = '\0'; free(token->value); token->value = buf; return;
    }
    char c = lexer_advance(lexer);
    if (c == '\\' && lexer->pos < lexer->source_len) {
        c = lexer_advance(lexer);
        switch (c) { case 'n': c='\n'; break; case 't': c='\t'; break; case '\\': c='\\'; break; case '"': c='"'; break; case '\'': c='\''; break; case '0': c='\0'; break; default: break; }
    }
    if (i + 2 >= cap) { cap *= 2; char *nb=(char*)realloc(buf,cap); if(!nb){free(buf); free(token->value); token->value=ltng_strdup(""); return;} buf=nb; }
    buf[i++] = c;
    if (lexer->pos >= lexer->source_len || lexer_peek_char(lexer) != '\'') {
        if (lexer_errors) error_state_add(lexer_errors, lexer->line, lexer->col, "Unterminated char literal");
        buf[i] = '\0'; free(token->value); token->value = buf; return;
    }
    if (lexer->pos < lexer->source_len) lexer_advance(lexer);
    buf[i] = '\0'; free(token->value); token->value = buf;
}

static void lexer_read_identifier(Lexer *lexer, Token *token) {
    token->col = lexer->col;
    int cap = 32, i = 0;
    char *buf = (char*)malloc(cap);
    if (!buf) { free(token->value); token->value = ltng_strdup(""); return; }
    while (lexer->pos < lexer->source_len) {
        char c = lexer_peek_char(lexer);
        if (isalnum(c) || c == '_') {
            if (i + 2 >= cap) { cap *= 2; char *nb=(char*)realloc(buf,cap); if(!nb){free(buf); free(token->value); token->value=ltng_strdup(""); return;} buf=nb; }
            buf[i++] = lexer_advance(lexer);
        } else break;
    }
    buf[i] = '\0'; free(token->value); token->value = buf;

    if (strcmp(token->value, "let") == 0) {
        token->type = TOKEN_LET;
    } else if (strcmp(token->value, "const") == 0) {
        token->type = TOKEN_CONST;
    } else if (strcmp(token->value, "set") == 0) {
        token->type = TOKEN_SET;
    } else if (strcmp(token->value, "write") == 0) {
        token->type = TOKEN_WRITE;
    } else if (strcmp(token->value, "read") == 0) {
        token->type = TOKEN_READ;
    } else if (strcmp(token->value, "if") == 0) {
        token->type = TOKEN_IF;
    } else if (strcmp(token->value, "else") == 0) {
        token->type = TOKEN_ELSE;
    } else if (strcmp(token->value, "switch") == 0) {
        token->type = TOKEN_SWITCH;
    } else if (strcmp(token->value, "case") == 0) {
        token->type = TOKEN_CASE;
    } else if (strcmp(token->value, "default") == 0) {
        token->type = TOKEN_DEFAULT;
    } else if (strcmp(token->value, "break") == 0) {
        token->type = TOKEN_BREAK;
    } else if (strcmp(token->value, "continue") == 0) {
        token->type = TOKEN_CONTINUE;
    } else if (strcmp(token->value, "while") == 0) {
        token->type = TOKEN_WHILE;
    } else if (strcmp(token->value, "do") == 0) {
        token->type = TOKEN_DO;
    } else if (strcmp(token->value, "for") == 0) {
        token->type = TOKEN_FOR;
    } else if (strcmp(token->value, "from") == 0) {
        token->type = TOKEN_FROM;
    } else if (strcmp(token->value, "to") == 0) {
        token->type = TOKEN_TO;
    } else if (strcmp(token->value, "step") == 0) {
        token->type = TOKEN_STEP;
    } else if (strcmp(token->value, "in") == 0) {
        token->type = TOKEN_IN;
    } else if (strcmp(token->value, "function") == 0) {
        token->type = TOKEN_FUNCTION;
    } else if (strcmp(token->value, "return") == 0) {
        token->type = TOKEN_RETURN;
    } else if (strcmp(token->value, "exit") == 0) {
        token->type = TOKEN_EXIT;
    } else if (strcmp(token->value, "try") == 0) {
        token->type = TOKEN_TRY;
    } else if (strcmp(token->value, "catch") == 0) {
        token->type = TOKEN_CATCH;
    } else if (strcmp(token->value, "finally") == 0) {
        token->type = TOKEN_FINALLY;
    } else if (strcmp(token->value, "throw") == 0) {
        token->type = TOKEN_THROW;
    } else if (strcmp(token->value, "and") == 0) {
        token->type = TOKEN_AMP_AMP;
    } else if (strcmp(token->value, "or") == 0) {
        token->type = TOKEN_PIPE_PIPE;
    } else if (strcmp(token->value, "not") == 0) {
        token->type = TOKEN_BANG;
    } else {
        token->type = TOKEN_IDENT;
    }
}

static void lexer_read_number(Lexer *lexer, Token *token) {
    token->col = lexer->col;
    int cap = 32, i = 0, is_float = 0;
    char *buf = (char*)malloc(cap);
    if (!buf) { free(token->value); token->value = ltng_strdup(""); token->type = TOKEN_INT_LIT; return; }
    while (lexer->pos < lexer->source_len && isdigit(lexer_peek_char(lexer))) {
        if (i + 2 >= cap) { cap *= 2; char *nb=(char*)realloc(buf,cap); if(!nb){free(buf); free(token->value); token->value=ltng_strdup(""); token->type=TOKEN_INT_LIT; return;} buf=nb; }
        buf[i++] = lexer_advance(lexer);
    }
    if (lexer->pos < lexer->source_len && lexer_peek_char(lexer) == '.') {
        char next = (lexer->pos + 1 < lexer->source_len) ? lexer->source[lexer->pos + 1] : '\0';
        if (isdigit(next)) {
            is_float = 1;
            if (i + 2 >= cap) { cap *= 2; char *nb=(char*)realloc(buf,cap); if(!nb){free(buf); free(token->value); token->value=ltng_strdup(""); token->type=TOKEN_FLOAT_LIT; return;} buf=nb; }
            buf[i++] = lexer_advance(lexer);
            while (lexer->pos < lexer->source_len && isdigit(lexer_peek_char(lexer))) {
                if (i + 2 >= cap) { cap *= 2; char *nb=(char*)realloc(buf,cap); if(!nb){free(buf); free(token->value); token->value=ltng_strdup(""); token->type=TOKEN_FLOAT_LIT; return;} buf=nb; }
                buf[i++] = lexer_advance(lexer);
            }
        }
    }
    buf[i] = '\0'; free(token->value); token->value = buf;
    token->type = is_float ? TOKEN_FLOAT_LIT : TOKEN_INT_LIT;
}

static void lexer_add_token(Lexer *lexer, Token token) {
    if (lexer->count >= lexer->token_capacity) {
        int new_cap = lexer->token_capacity * 2;
        Token *new_tokens = realloc(lexer->tokens, new_cap * sizeof(Token));
        if (!new_tokens) {
            if (lexer_errors) {
                error_state_add(lexer_errors, lexer->line, lexer->col, "Out of memory");
            }
            return;
        }
        lexer->tokens = new_tokens;
        lexer->token_capacity = new_cap;
    }
    lexer->tokens[lexer->count++] = token;
}

void lexer_tokenize(Lexer *lexer, ErrorState *errors) {
    lexer_errors = errors;

    while (lexer->pos < lexer->source_len) {
        lexer_skip_whitespace(lexer);
        if (lexer->pos >= lexer->source_len) break;

        char c = lexer_peek_char(lexer);

        if (c == '\n') {
            Token token = token_create(TOKEN_NEWLINE, "", lexer->line, lexer->col);
            lexer_advance(lexer);
            lexer_add_token(lexer, token);
        } else if (c == '"') {
            Token token = token_create(TOKEN_STRING, "", lexer->line, lexer->col);
            lexer_read_string(lexer, &token);
            lexer_add_token(lexer, token);
        } else if (c == '\'') {
            Token token = token_create(TOKEN_CHAR, "", lexer->line, lexer->col);
            lexer_read_char(lexer, &token);
            lexer_add_token(lexer, token);
        } else if (c == '(') {
            Token token = token_create(TOKEN_LPAREN, "(", lexer->line, lexer->col);
            lexer_advance(lexer);
            lexer_add_token(lexer, token);
        } else if (c == ')') {
            Token token = token_create(TOKEN_RPAREN, ")", lexer->line, lexer->col);
            lexer_advance(lexer);
            lexer_add_token(lexer, token);
        } else if (c == '{') {
            Token token = token_create(TOKEN_LBRACE, "{", lexer->line, lexer->col);
            lexer_advance(lexer);
            lexer_add_token(lexer, token);
        } else if (c == '}') {
            Token token = token_create(TOKEN_RBRACE, "}", lexer->line, lexer->col);
            lexer_advance(lexer);
            lexer_add_token(lexer, token);
        } else if (c == '[') {
            Token token = token_create(TOKEN_LBRACKET, "[", lexer->line, lexer->col);
            lexer_advance(lexer);
            lexer_add_token(lexer, token);
        } else if (c == ']') {
            Token token = token_create(TOKEN_RBRACKET, "]", lexer->line, lexer->col);
            lexer_advance(lexer);
            lexer_add_token(lexer, token);
        } else if (c == ':') {
            Token token = token_create(TOKEN_COLON, ":", lexer->line, lexer->col);
            lexer_advance(lexer);
            lexer_add_token(lexer, token);
        } else if (c == '?') {
            Token token = token_create(TOKEN_QUESTION, "?", lexer->line, lexer->col);
            lexer_advance(lexer);
            lexer_add_token(lexer, token);
        } else if (c == ',') {
            Token token = token_create(TOKEN_COMMA, ",", lexer->line, lexer->col);
            lexer_advance(lexer);
            lexer_add_token(lexer, token);
        } else if (c == '.') {
            lexer_advance(lexer);
            if (lexer_peek_char(lexer) == '.' &&
                lexer->pos + 1 < lexer->source_len && lexer->source[lexer->pos + 1] == '.') {
                lexer_advance(lexer);
                lexer_advance(lexer);
                Token token = token_create(TOKEN_ELLIPSIS, "...", lexer->line, lexer->col);
                lexer_add_token(lexer, token);
            } else {
                Token token = token_create(TOKEN_DOT, ".", lexer->line, lexer->col);
                lexer_add_token(lexer, token);
            }
        } else if (c == '=') {
            lexer_advance(lexer);
            if (lexer_peek_char(lexer) == '=') {
                lexer_advance(lexer);
                Token token = token_create(TOKEN_EQ_EQ, "==", lexer->line, lexer->col);
                lexer_add_token(lexer, token);
            } else {
                Token token = token_create(TOKEN_EQUALS, "=", lexer->line, lexer->col);
                lexer_add_token(lexer, token);
            }
        } else if (c == '!' && lexer->pos + 1 < lexer->source_len && lexer->source[lexer->pos + 1] == '=') {
            lexer_advance(lexer);
            lexer_advance(lexer);
            Token token = token_create(TOKEN_BANG_EQ, "!=", lexer->line, lexer->col);
            lexer_add_token(lexer, token);
        } else if (c == '!') {
            lexer_advance(lexer);
            Token token = token_create(TOKEN_BANG, "!", lexer->line, lexer->col);
            lexer_add_token(lexer, token);
        } else if (c == '<' && lexer->pos + 1 < lexer->source_len && lexer->source[lexer->pos + 1] == '=') {
            lexer_advance(lexer);
            lexer_advance(lexer);
            Token token = token_create(TOKEN_LT_EQ, "<=", lexer->line, lexer->col);
            lexer_add_token(lexer, token);
        } else if (c == '<') {
            lexer_advance(lexer);
            Token token = token_create(TOKEN_LT, "<", lexer->line, lexer->col);
            lexer_add_token(lexer, token);
        } else if (c == '>' && lexer->pos + 1 < lexer->source_len && lexer->source[lexer->pos + 1] == '=') {
            lexer_advance(lexer);
            lexer_advance(lexer);
            Token token = token_create(TOKEN_GT_EQ, ">=", lexer->line, lexer->col);
            lexer_add_token(lexer, token);
        } else if (c == '>') {
            lexer_advance(lexer);
            Token token = token_create(TOKEN_GT, ">", lexer->line, lexer->col);
            lexer_add_token(lexer, token);
        } else if (c == '&' && lexer->pos + 1 < lexer->source_len && lexer->source[lexer->pos + 1] == '&') {
            lexer_advance(lexer);
            lexer_advance(lexer);
            Token token = token_create(TOKEN_AMP_AMP, "&&", lexer->line, lexer->col);
            lexer_add_token(lexer, token);
        } else if (c == '|' && lexer->pos + 1 < lexer->source_len && lexer->source[lexer->pos + 1] == '|') {
            lexer_advance(lexer);
            lexer_advance(lexer);
            Token token = token_create(TOKEN_PIPE_PIPE, "||", lexer->line, lexer->col);
            lexer_add_token(lexer, token);
        } else if (c == '^' && lexer->pos + 1 < lexer->source_len && lexer->source[lexer->pos + 1] == '^') {
            lexer_advance(lexer);
            lexer_advance(lexer);
            Token token = token_create(TOKEN_CARET_CARET, "^^", lexer->line, lexer->col);
            lexer_add_token(lexer, token);
        } else if (c == '+') {
            lexer_advance(lexer);
            if (lexer_peek_char(lexer) == '+') {
                lexer_advance(lexer);
                Token token = token_create(TOKEN_PLUS_PLUS, "++", lexer->line, lexer->col);
                lexer_add_token(lexer, token);
            } else if (lexer_peek_char(lexer) == '=') {
                lexer_advance(lexer);
                Token token = token_create(TOKEN_PLUS_EQUALS, "+=", lexer->line, lexer->col);
                lexer_add_token(lexer, token);
            } else {
                Token token = token_create(TOKEN_PLUS, "+", lexer->line, lexer->col);
                lexer_add_token(lexer, token);
            }
        } else if (c == '-') {
            lexer_advance(lexer);
            if (lexer_peek_char(lexer) == '>') {
                lexer_advance(lexer);
                Token token = token_create(TOKEN_ARROW, "->", lexer->line, lexer->col);
                lexer_add_token(lexer, token);
            } else if (lexer_peek_char(lexer) == '-') {
                lexer_advance(lexer);
                Token token = token_create(TOKEN_MINUS_MINUS, "--", lexer->line, lexer->col);
                lexer_add_token(lexer, token);
            } else if (lexer_peek_char(lexer) == '=') {
                lexer_advance(lexer);
                Token token = token_create(TOKEN_MINUS_EQUALS, "-=", lexer->line, lexer->col);
                lexer_add_token(lexer, token);
            } else {
                Token token = token_create(TOKEN_MINUS, "-", lexer->line, lexer->col);
                lexer_add_token(lexer, token);
            }
        } else if (c == '*') {
            lexer_advance(lexer);
            if (lexer_peek_char(lexer) == '=') {
                lexer_advance(lexer);
                Token token = token_create(TOKEN_STAR_EQUALS, "*=", lexer->line, lexer->col);
                lexer_add_token(lexer, token);
            } else {
                Token token = token_create(TOKEN_STAR, "*", lexer->line, lexer->col);
                lexer_add_token(lexer, token);
            }
        } else if (c == '/') {
            lexer_advance(lexer);
            if (lexer_peek_char(lexer) == '=') {
                lexer_advance(lexer);
                Token token = token_create(TOKEN_SLASH_EQUALS, "/=", lexer->line, lexer->col);
                lexer_add_token(lexer, token);
            } else {
                Token token = token_create(TOKEN_SLASH, "/", lexer->line, lexer->col);
                lexer_add_token(lexer, token);
            }
        } else if (c == '%') {
            lexer_advance(lexer);
            if (lexer_peek_char(lexer) == '=') {
                lexer_advance(lexer);
                Token token = token_create(TOKEN_PERCENT_EQUALS, "%=", lexer->line, lexer->col);
                lexer_add_token(lexer, token);
            } else {
                Token token = token_create(TOKEN_PERCENT, "%", lexer->line, lexer->col);
                lexer_add_token(lexer, token);
            }
        } else if (isdigit(c)) {
            Token token = token_create(TOKEN_INT_LIT, "", lexer->line, lexer->col);
            lexer_read_number(lexer, &token);
            lexer_add_token(lexer, token);
        } else if (isalpha(c) || c == '_') {
            Token token = token_create(TOKEN_IDENT, "", lexer->line, lexer->col);
            lexer_read_identifier(lexer, &token);
            lexer_add_token(lexer, token);
        } else if (c == ';') {
            error_state_add(errors, lexer->line, lexer->col,
                "Semicolons are not allowed in ltng");
            lexer_advance(lexer);
        } else {
            char *msg = (char*)malloc(64);
            if (msg) { snprintf(msg, 64, "Unexpected character '%c'", c); error_state_add(errors, lexer->line, lexer->col, msg); free(msg); }
            else { error_state_add(errors, lexer->line, lexer->col, "Unexpected character"); }
            lexer_advance(lexer);
        }
    }

    Token eof = token_create(TOKEN_EOF, "", lexer->line, lexer->col);
    lexer_add_token(lexer, eof);
    lexer_errors = NULL;
}

Token lexer_next(Lexer *lexer) {
    if (lexer->current >= lexer->count) {
        Token eof = token_create(TOKEN_EOF, "", lexer->line, lexer->col);
        return eof;
    }
    return lexer->tokens[lexer->current++];
}

Token lexer_peek(Lexer *lexer) {
    if (lexer->current >= lexer->count) {
        Token eof = token_create(TOKEN_EOF, "", lexer->line, lexer->col);
        return eof;
    }
    return lexer->tokens[lexer->current];
}

void lexer_free(Lexer *lexer) {
    if (lexer && lexer->tokens) {
        free(lexer->tokens);
        lexer->tokens = NULL;
        lexer->token_capacity = 0;
        lexer->count = 0;
        lexer->current = 0;
    }
}
