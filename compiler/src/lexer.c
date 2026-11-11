#include "ltng.h"

static ErrorState *lexer_errors = NULL;

void lexer_init(Lexer *lexer, char *source) {
    lexer->source = source;
    lexer->source_len = strlen(source);
    lexer->pos = 0;
    lexer->line = 1;
    lexer->col = 1;
    lexer->count = 0;
    lexer->current = 0;
    lexer->token_capacity = MAX_TOKENS;
    lexer->tokens = malloc(MAX_TOKENS * sizeof(Token));
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
    int i = 0;

    while (lexer->pos < lexer->source_len && lexer_peek_char(lexer) != '"' && lexer_peek_char(lexer) != '\n') {
        char c = lexer_advance(lexer);
        if (c == '\\' && lexer->pos < lexer->source_len) {
            c = lexer_advance(lexer);
            switch (c) {
                case 'n': token->value[i++] = '\n'; break;
                case 't': token->value[i++] = '\t'; break;
                case '\\': token->value[i++] = '\\'; break;
                case '"': token->value[i++] = '"'; break;
                case '\'': token->value[i++] = '\''; break;
                case '<': token->value[i++] = '<'; break;
                case '>': token->value[i++] = '>'; break;
                case '0': token->value[i++] = '\0'; break;
                default: token->value[i++] = c; break;
            }
        } else {
            token->value[i++] = c;
        }
        if (i >= MAX_STRING_LEN - 1) {
            if (lexer_errors) {
                error_state_add(lexer_errors, lexer->line, lexer->col, "String too long");
            }
            break;
        }
    }

    if (lexer->pos < lexer->source_len && lexer_peek_char(lexer) == '"') {
        token->value[i] = '\0';
        /* Check for format specifiers */
        for (int j = 0; token->value[j]; j++) {
            if (token->value[j] == '%') {
                if (token->value[j + 1] == '%') {
                    j++;
                    continue;
                }
                if (lexer_errors) {
                    error_state_add(lexer_errors, token->line, token->col, "Format specifiers are not allowed");
                }
                break;
            }
        }
        lexer_advance(lexer);
    } else {
        token->value[i] = '\0';
        for (int j = 0; token->value[j]; j++) {
            if (token->value[j] == '%') {
                if (token->value[j + 1] == '%') {
                    j++;
                    continue;
                }
                if (lexer_errors) {
                    error_state_add(lexer_errors, token->line, token->col, "Format specifiers are not allowed");
                }
                break;
            }
        }
        if (lexer_errors) {
            error_state_add(lexer_errors, token->line, token->col, "Unterminated string literal");
        }
    }
}

static void lexer_read_char(Lexer *lexer, Token *token) {
    token->col = lexer->col;
    lexer_advance(lexer);
    int i = 0;

    if (lexer->pos >= lexer->source_len || lexer_peek_char(lexer) == '\'') {
        if (lexer_errors) {
            error_state_add(lexer_errors, lexer->line, lexer->col, "Empty char literal");
        }
        if (lexer->pos < lexer->source_len) lexer_advance(lexer);
        token->value[0] = '\0';
        return;
    }

    char c = lexer_advance(lexer);
    if (c == '\\' && lexer->pos < lexer->source_len) {
        c = lexer_advance(lexer);
        switch (c) {
            case 'n': token->value[i++] = '\n'; break;
            case 't': token->value[i++] = '\t'; break;
            case '\\': token->value[i++] = '\\'; break;
            case '"': token->value[i++] = '"'; break;
            case '\'': token->value[i++] = '\''; break;
            case '0': token->value[i++] = '\0'; break;
            default: token->value[i++] = c; break;
        }
    } else {
        token->value[i++] = c;
    }

    if (lexer->pos >= lexer->source_len || lexer_peek_char(lexer) != '\'') {
        if (lexer_errors) {
            error_state_add(lexer_errors, lexer->line, lexer->col, "Unterminated char literal");
        }
        if (lexer->pos < lexer->source_len && lexer_peek_char(lexer) != '\'') {
            token->value[i] = '\0';
            return;
        }
    }
    if (lexer->pos < lexer->source_len) lexer_advance(lexer);

    token->value[i] = '\0';
}

static void lexer_read_identifier(Lexer *lexer, Token *token) {
    token->col = lexer->col;
    int i = 0;
    while (lexer->pos < lexer->source_len) {
        char c = lexer_peek_char(lexer);
        if (isalnum(c) || c == '_') {
            token->value[i++] = lexer_advance(lexer);
        } else {
            break;
        }
        if (i >= MAX_IDENTIFIER_LEN - 1) break;
    }
    token->value[i] = '\0';

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
    } else if (strcmp(token->value, "function") == 0) {
        token->type = TOKEN_FUNCTION;
    } else if (strcmp(token->value, "return") == 0) {
        token->type = TOKEN_RETURN;
    } else if (strcmp(token->value, "exit") == 0) {
        token->type = TOKEN_EXIT;
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
    int i = 0;
    int is_float = 0;

    while (lexer->pos < lexer->source_len && isdigit(lexer_peek_char(lexer))) {
        token->value[i++] = lexer_advance(lexer);
    }

    if (lexer->pos < lexer->source_len && lexer_peek_char(lexer) == '.') {
        char next = (lexer->pos + 1 < lexer->source_len) ? lexer->source[lexer->pos + 1] : '\0';
        if (isdigit(next)) {
            is_float = 1;
            token->value[i++] = lexer_advance(lexer);
            while (lexer->pos < lexer->source_len && isdigit(lexer_peek_char(lexer))) {
                token->value[i++] = lexer_advance(lexer);
            }
        }
    }

    token->value[i] = '\0';
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
            Token token = {TOKEN_NEWLINE, "", lexer->line, lexer->col};
            lexer_advance(lexer);
            lexer_add_token(lexer, token);
        } else if (c == '"') {
            Token token = {TOKEN_STRING, "", lexer->line, lexer->col};
            lexer_read_string(lexer, &token);
            lexer_add_token(lexer, token);
        } else if (c == '\'') {
            Token token = {TOKEN_CHAR, "", lexer->line, lexer->col};
            lexer_read_char(lexer, &token);
            lexer_add_token(lexer, token);
        } else if (c == '(') {
            Token token = {TOKEN_LPAREN, "(", lexer->line, lexer->col};
            lexer_advance(lexer);
            lexer_add_token(lexer, token);
        } else if (c == ')') {
            Token token = {TOKEN_RPAREN, ")", lexer->line, lexer->col};
            lexer_advance(lexer);
            lexer_add_token(lexer, token);
        } else if (c == '{') {
            Token token = {TOKEN_LBRACE, "{", lexer->line, lexer->col};
            lexer_advance(lexer);
            lexer_add_token(lexer, token);
        } else if (c == '}') {
            Token token = {TOKEN_RBRACE, "}", lexer->line, lexer->col};
            lexer_advance(lexer);
            lexer_add_token(lexer, token);
        } else if (c == '[') {
            Token token = {TOKEN_LBRACKET, "[", lexer->line, lexer->col};
            lexer_advance(lexer);
            lexer_add_token(lexer, token);
        } else if (c == ']') {
            Token token = {TOKEN_RBRACKET, "]", lexer->line, lexer->col};
            lexer_advance(lexer);
            lexer_add_token(lexer, token);
        } else if (c == ':') {
            Token token = {TOKEN_COLON, ":", lexer->line, lexer->col};
            lexer_advance(lexer);
            lexer_add_token(lexer, token);
        } else if (c == ',') {
            Token token = {TOKEN_COMMA, ",", lexer->line, lexer->col};
            lexer_advance(lexer);
            lexer_add_token(lexer, token);
        } else if (c == '.') {
            Token token = {TOKEN_DOT, ".", lexer->line, lexer->col};
            lexer_advance(lexer);
            lexer_add_token(lexer, token);
        } else if (c == '=') {
            lexer_advance(lexer);
            if (lexer_peek_char(lexer) == '=') {
                lexer_advance(lexer);
                Token token = {TOKEN_EQ_EQ, "==", lexer->line, lexer->col};
                lexer_add_token(lexer, token);
            } else {
                Token token = {TOKEN_EQUALS, "=", lexer->line, lexer->col};
                lexer_add_token(lexer, token);
            }
        } else if (c == '!' && lexer->pos + 1 < lexer->source_len && lexer->source[lexer->pos + 1] == '=') {
            lexer_advance(lexer);
            lexer_advance(lexer);
            Token token = {TOKEN_BANG_EQ, "!=", lexer->line, lexer->col};
            lexer_add_token(lexer, token);
        } else if (c == '!') {
            lexer_advance(lexer);
            Token token = {TOKEN_BANG, "!", lexer->line, lexer->col};
            lexer_add_token(lexer, token);
        } else if (c == '<' && lexer->pos + 1 < lexer->source_len && lexer->source[lexer->pos + 1] == '=') {
            lexer_advance(lexer);
            lexer_advance(lexer);
            Token token = {TOKEN_LT_EQ, "<=", lexer->line, lexer->col};
            lexer_add_token(lexer, token);
        } else if (c == '<') {
            lexer_advance(lexer);
            Token token = {TOKEN_LT, "<", lexer->line, lexer->col};
            lexer_add_token(lexer, token);
        } else if (c == '>' && lexer->pos + 1 < lexer->source_len && lexer->source[lexer->pos + 1] == '=') {
            lexer_advance(lexer);
            lexer_advance(lexer);
            Token token = {TOKEN_GT_EQ, ">=", lexer->line, lexer->col};
            lexer_add_token(lexer, token);
        } else if (c == '>') {
            lexer_advance(lexer);
            Token token = {TOKEN_GT, ">", lexer->line, lexer->col};
            lexer_add_token(lexer, token);
        } else if (c == '&' && lexer->pos + 1 < lexer->source_len && lexer->source[lexer->pos + 1] == '&') {
            lexer_advance(lexer);
            lexer_advance(lexer);
            Token token = {TOKEN_AMP_AMP, "&&", lexer->line, lexer->col};
            lexer_add_token(lexer, token);
        } else if (c == '|' && lexer->pos + 1 < lexer->source_len && lexer->source[lexer->pos + 1] == '|') {
            lexer_advance(lexer);
            lexer_advance(lexer);
            Token token = {TOKEN_PIPE_PIPE, "||", lexer->line, lexer->col};
            lexer_add_token(lexer, token);
        } else if (c == '^' && lexer->pos + 1 < lexer->source_len && lexer->source[lexer->pos + 1] == '^') {
            lexer_advance(lexer);
            lexer_advance(lexer);
            Token token = {TOKEN_CARET_CARET, "^^", lexer->line, lexer->col};
            lexer_add_token(lexer, token);
        } else if (c == '+') {
            lexer_advance(lexer);
            if (lexer_peek_char(lexer) == '+') {
                lexer_advance(lexer);
                Token token = {TOKEN_PLUS_PLUS, "++", lexer->line, lexer->col};
                lexer_add_token(lexer, token);
            } else if (lexer_peek_char(lexer) == '=') {
                lexer_advance(lexer);
                Token token = {TOKEN_PLUS_EQUALS, "+=", lexer->line, lexer->col};
                lexer_add_token(lexer, token);
            } else {
                Token token = {TOKEN_PLUS, "+", lexer->line, lexer->col};
                lexer_add_token(lexer, token);
            }
        } else if (c == '-') {
            lexer_advance(lexer);
            if (lexer_peek_char(lexer) == '-') {
                lexer_advance(lexer);
                Token token = {TOKEN_MINUS_MINUS, "--", lexer->line, lexer->col};
                lexer_add_token(lexer, token);
            } else if (lexer_peek_char(lexer) == '=') {
                lexer_advance(lexer);
                Token token = {TOKEN_MINUS_EQUALS, "-=", lexer->line, lexer->col};
                lexer_add_token(lexer, token);
            } else {
                Token token = {TOKEN_MINUS, "-", lexer->line, lexer->col};
                lexer_add_token(lexer, token);
            }
        } else if (c == '*') {
            lexer_advance(lexer);
            if (lexer_peek_char(lexer) == '=') {
                lexer_advance(lexer);
                Token token = {TOKEN_STAR_EQUALS, "*=", lexer->line, lexer->col};
                lexer_add_token(lexer, token);
            } else {
                Token token = {TOKEN_STAR, "*", lexer->line, lexer->col};
                lexer_add_token(lexer, token);
            }
        } else if (c == '/') {
            lexer_advance(lexer);
            if (lexer_peek_char(lexer) == '=') {
                lexer_advance(lexer);
                Token token = {TOKEN_SLASH_EQUALS, "/=", lexer->line, lexer->col};
                lexer_add_token(lexer, token);
            } else {
                Token token = {TOKEN_SLASH, "/", lexer->line, lexer->col};
                lexer_add_token(lexer, token);
            }
        } else if (c == '%') {
            lexer_advance(lexer);
            if (lexer_peek_char(lexer) == '=') {
                lexer_advance(lexer);
                Token token = {TOKEN_PERCENT_EQUALS, "%=", lexer->line, lexer->col};
                lexer_add_token(lexer, token);
            } else {
                Token token = {TOKEN_PERCENT, "%", lexer->line, lexer->col};
                lexer_add_token(lexer, token);
            }
        } else if (isdigit(c)) {
            Token token = {TOKEN_INT_LIT, "", lexer->line, lexer->col};
            lexer_read_number(lexer, &token);
            lexer_add_token(lexer, token);
        } else if (isalpha(c) || c == '_') {
            Token token = {TOKEN_IDENT, "", lexer->line, lexer->col};
            lexer_read_identifier(lexer, &token);
            lexer_add_token(lexer, token);
        } else if (c == ';') {
            error_state_add(errors, lexer->line, lexer->col,
                "Semicolons are not allowed in ltng");
            lexer_advance(lexer);
        } else {
            char msg[256];
            snprintf(msg, sizeof(msg), "Unexpected character '%c'", c);
            error_state_add(errors, lexer->line, lexer->col, msg);
            lexer_advance(lexer);
        }
    }

    Token eof = {TOKEN_EOF, "", lexer->line, lexer->col};
    lexer_add_token(lexer, eof);
    lexer_errors = NULL;
}

Token lexer_next(Lexer *lexer) {
    if (lexer->current >= lexer->count) {
        Token eof = {TOKEN_EOF, "", lexer->line, lexer->col};
        return eof;
    }
    return lexer->tokens[lexer->current++];
}

Token lexer_peek(Lexer *lexer) {
    if (lexer->current >= lexer->count) {
        Token eof = {TOKEN_EOF, "", lexer->line, lexer->col};
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
