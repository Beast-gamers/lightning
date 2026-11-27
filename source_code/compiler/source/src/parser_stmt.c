#include <unistd.h>
#include "parser_internal.h"


#define SNPRINTF_FULL(buf,...) do{ char *_sf=_ltng_cfmt(__VA_ARGS__); if(_sf){ size_t _sn=strlen(_sf)+1; if(_sn<65536){ char *_sg=(char*)realloc(_sf,65536); if(_sg){_sf=_sg; _sn=65536;} } free(buf); buf=_sf; } }while(0)
#define STMT_APPEND(buf,cap,pos,...) do{ int _sa_n; for(;;){ _sa_n=snprintf((buf)+(pos),(cap)-(pos),__VA_ARGS__); if(_sa_n<0){(buf)[pos]=0;break;} if(_sa_n>=(cap)-(pos)){int _sa_nc=(cap)*2; if(_sa_nc<(pos)+_sa_n+1)_sa_nc=(pos)+_sa_n+1; char*_sa_nb=(char*)realloc((buf),_sa_nc); if(!_sa_nb)break; (buf)=_sa_nb; (cap)=_sa_nc; continue;} (pos)+=_sa_n; break;} }while(0)


int try_parse_typeof(Lexer *lexer, SymbolTable *table, Token *result) {
    int saved_pos = lexer->current;
    Token typeof_tok = lexer_peek(lexer);
    if (typeof_tok.type != TOKEN_IDENT || strcmp(typeof_tok.value, "typeof") != 0)
        return 0;

    lexer_next(lexer);

    Token open = lexer_next(lexer);
    if (open.type != TOKEN_LPAREN) {
        error_state_add(parser_errors, open.line, open.col,
            "Expected '(' after typeof");
        lexer->current = saved_pos;
        return 0;
    }

    Token arg = lexer_next(lexer);

    if (arg.type == TOKEN_IDENT && strcmp(arg.value, "typeof") == 0) {
        lexer->current--;
        Token inner_result = (Token){0};
        if (try_parse_typeof(lexer, table, &inner_result)) {
            Token close = lexer_next(lexer);
            if (close.type != TOKEN_RPAREN) {
                error_state_add(parser_errors, arg.line, arg.col,
                    "Expected ')' after typeof argument");
                lexer->current = saved_pos;
                return 0;
            }
            result->type = TOKEN_STRING;
            { free(result->value); result->value = ltng_strdup("string"); }
            result->line = typeof_tok.line;
            result->col = typeof_tok.col;
            return 1;
        }
        lexer->current = saved_pos;
        return 0;
    }

    /* Type conversion call inside typeof: typeof(toint(...)) → int,
       typeof(tostring(...)) → string, typeof(tobool(...)) → bool.
       Fast-path only when the conversion call is the sole typeof argument. */
    if (arg.type == TOKEN_IDENT && is_type_conversion(arg.value) &&
        lexer_peek(lexer).type == TOKEN_LPAREN) {
        int conv_scan_saved = lexer->current; /* before consuming conversion '(' */
        lexer_next(lexer); /* consume ( */
        Token first_inner = lexer_peek(lexer);
        int inner_tokens = 0;
        int inner_depth = 0;
        int depth = 1;
        while (lexer->current < lexer->count && depth > 0) {
            Token itok = lexer_peek(lexer);
            if (itok.type == TOKEN_LPAREN) depth++;
            else if (itok.type == TOKEN_RPAREN) { depth--; if (depth == 0) break; }
            else if (itok.type == TOKEN_NEWLINE || itok.type == TOKEN_EOF) break;
            if (depth == 1 && inner_depth == 0) { inner_tokens++; }
            if (itok.type == TOKEN_LPAREN || itok.type == TOKEN_LBRACKET) inner_depth++;
            else if (itok.type == TOKEN_RPAREN || itok.type == TOKEN_RBRACKET) { if (inner_depth > 0) inner_depth--; }
            lexer_next(lexer);
        }
        if (lexer->current < lexer->count && lexer_peek(lexer).type == TOKEN_RPAREN) {
            lexer_next(lexer); /* consume inner ) of conversion */
        }
        /* Sole argument only if next token closes typeof */
        if (lexer->current < lexer->count && lexer_peek(lexer).type == TOKEN_RPAREN) {
            if ((strcmp(arg.value, "toint") == 0 || strcmp(arg.value, "int") == 0 ||
                 strcmp(arg.value, "tofloat") == 0 || strcmp(arg.value, "float") == 0) &&
                inner_tokens == 1 && first_inner.type == TOKEN_STRING) {
                const char *sv = first_inner.value;
                int slen = (int)strlen(sv);
                int ok = 0;
                if (slen > 0) {
                    int k = 0; if (sv[k]=='+'||sv[k]=='-') k++;
                    int digits = 0, dot = 0, valid = 1;
                    for (; k < slen; k++) {
                        if (sv[k] >= '0' && sv[k] <= '9') digits = 1;
                        else if (sv[k] == '.' && !dot) dot = 1;
                        else { valid = 0; break; }
                    }
                    if (valid && digits) ok = 1;
                }
                if (!ok) {
                    char *msg = _ltng_cfmt("Invalid conversion: cannot convert \"%s\" with %s", sv, arg.value);
                    error_state_add(parser_errors, arg.line, arg.col, msg ? msg : "");
                    free(msg);
                }
            }
            lexer_next(lexer); /* consume outer ) of typeof */
            VarType cvt;
            if (strcmp(arg.value, "toint") == 0 || strcmp(arg.value, "int") == 0) cvt = TYPE_INT;
            else if (strcmp(arg.value, "tofloat") == 0 || strcmp(arg.value, "float") == 0) cvt = TYPE_FLOAT;
            else if (strcmp(arg.value, "tostring") == 0 || strcmp(arg.value, "string") == 0) cvt = TYPE_STRING;
            else if (strcmp(arg.value, "tochar") == 0 || strcmp(arg.value, "char") == 0) cvt = TYPE_CHAR;
            else cvt = TYPE_BOOL; /* tobool/bool */
            result->type = TOKEN_STRING;
            { free(result->value); result->value = ltng_strdup(var_type_name(cvt)); }
            result->line = typeof_tok.line;
            result->col = typeof_tok.col;
            return 1;
        }
        /* Not a sole conversion-call argument (e.g. typeof(toint(x) + 1));
           restore and let general expression parsing handle it. */
        lexer->current = conv_scan_saved;
        goto typeof_general_expr;
    }

    /* User-defined function call inside typeof: typeof(add(1, 2)) */
    if (arg.type == TOKEN_IDENT && lexer_peek(lexer).type == TOKEN_LPAREN) {
        /* Check if this is a known/callable function */
        if (find_function_ast_by_name(arg.value) || is_callable_ident(table, arg.value) || is_function_name_scoped(table, arg.value)) {
            int call_saved = lexer->current; /* before consuming ( */
            /* Try to fully parse the call for type specialization */
            int saved_errors = parser_errors ? parser_errors->count : 0;
            lexer_next(lexer); /* consume ( */
            ASTNode *tcall = parse_call_node(lexer, table, arg.value, arg.line, arg.col, NULL);
            Token close2 = lexer_peek(lexer);
            if (close2.type == TOKEN_RPAREN) {
                lexer_next(lexer); /* consume outer ) of typeof */
                VarType ftype = infer_call_result_type(tcall);
                result->type = TOKEN_STRING;
                { free(result->value); result->value = ltng_strdup(var_type_name(ftype == TYPE_NULL ? TYPE_STRING : ftype)); }
                result->line = typeof_tok.line;
                result->col = typeof_tok.col;
                return 1;
            }
            /* Not a sole call argument (e.g. typeof(func() + 1)); restore and let general expression parsing handle it. */
            lexer->current = call_saved;
            if (parser_errors && parser_errors->count > saved_errors) parser_errors->count = saved_errors;
        }
    }

    /* Array element access: typeof(arr[i]) / typeof(arr[i][j]) */
    if (arg.type == TOKEN_IDENT && lexer_peek(lexer).type == TOKEN_LBRACKET) {
        char *mangled_arr = mangle_name_alloc(arg.value);
        if (symbol_table_has(table, mangled_arr) && symbol_table_get(table, mangled_arr) == TYPE_ARRAY) {
            int idx_saved = lexer->current;
            int ok = 1;
            int index_count = 0;
            while (lexer_peek(lexer).type == TOKEN_LBRACKET) {
                index_count++;
                lexer_next(lexer); /* [ */
                int depth = 1;
                while (lexer->current < lexer->count && depth > 0) {
                    Token itok = lexer_peek(lexer);
                    if (itok.type == TOKEN_LBRACKET) depth++;
                    else if (itok.type == TOKEN_RBRACKET) { depth--; if (depth == 0) break; }
                    else if (itok.type == TOKEN_NEWLINE || itok.type == TOKEN_EOF) { ok = 0; break; }
                    lexer_next(lexer);
                }
                if (!ok || lexer_peek(lexer).type != TOKEN_RBRACKET) { ok = 0; break; }
                lexer_next(lexer); /* ] */
            }
            Token rclose = lexer_peek(lexer);
            if (ok && rclose.type == TOKEN_RPAREN) {
                lexer_next(lexer); /* consume ) */
                int *dims = NULL;
                int dim_count = 0;
                int have_dims = symbol_table_get_array_dims(table, mangled_arr, &dims, &dim_count);
                if (have_dims && index_count != dim_count) {
                    char *errmsg = _ltng_cfmt("Array '%s' has %d dimension(s) but %d index(es) were given", arg.value, dim_count, index_count);
                    error_state_add(parser_errors, typeof_tok.line, typeof_tok.col, errmsg ? errmsg : "");
                    free(errmsg);
                }
                VarType aet = symbol_table_get_array_elem_type(table, mangled_arr);
                if (aet == TYPE_NULL) aet = TYPE_STRING;
                result->type = TOKEN_STRING;
                { free(result->value); result->value = ltng_strdup(var_type_name(aet)); }
                result->line = typeof_tok.line;
                result->col = typeof_tok.col;
                return 1;
            }
            lexer->current = idx_saved;
        }
    }

    /* Simple single-token argument followed by ) */
    Token close = lexer_peek(lexer);
    if (close.type == TOKEN_RPAREN) {
        lexer_next(lexer); /* consume ) */
        VarType vtype;
        if (arg.type == TOKEN_IDENT && is_bool_value(arg.value)) {
            vtype = TYPE_BOOL;
        } else if (arg.type == TOKEN_IDENT && is_ltng_reserved(arg.value)) {
            result->type = TOKEN_STRING;
            { free(result->value); result->value = ltng_strdup("reserved"); }
            result->line = typeof_tok.line;
            result->col = typeof_tok.col;
            return 1;
        } else if (arg.type == TOKEN_WRITE || arg.type == TOKEN_READ ||
                   arg.type == TOKEN_LET || arg.type == TOKEN_CONST ||
                   arg.type == TOKEN_SET) {
            result->type = TOKEN_STRING;
            { free(result->value); result->value = ltng_strdup("reserved"); }
            result->line = typeof_tok.line;
            result->col = typeof_tok.col;
            return 1;
        } else if (arg.type == TOKEN_IDENT || token_is_name(arg)) {
            char *mangled = mangle_name_alloc(arg.value);
            if (!symbol_table_has(table, mangled)) {
                error_state_add(parser_errors, arg.line, arg.col,
                    "Variable not declared");
                lexer->current = saved_pos;
                return 0;
            }
            vtype = effective_param_default_type(arg.value, symbol_table_get(table, mangled));
        } else if (arg.type == TOKEN_STRING) {
            vtype = TYPE_STRING;
        } else if (arg.type == TOKEN_INT_LIT) {
            vtype = TYPE_INT;
        } else if (arg.type == TOKEN_FLOAT_LIT) {
            vtype = TYPE_FLOAT;
        } else if (arg.type == TOKEN_CHAR) {
            vtype = TYPE_STRING;
        } else if (is_bool_value(arg.value)) {
            vtype = TYPE_BOOL;
        } else {
            error_state_add(parser_errors, arg.line, arg.col,
                "Expected variable or value inside typeof()");
            lexer->current = saved_pos;
            return 0;
        }

        result->type = TOKEN_STRING;
        { free(result->value); result->value = ltng_strdup(var_type_name(vtype)); }
        result->line = typeof_tok.line;
        result->col = typeof_tok.col;
        return 1;
    }

    /* General expression argument: typeof(expr) */
typeof_general_expr:
    {
        int expr_saved = saved_pos + 2; /* position after typeof and ( */
        lexer->current = expr_saved;
        int err_before = parser_errors ? parser_errors->count : 0;
        char *exprbuf = (char*)malloc(65536); if(exprbuf) exprbuf[0]=0;
        VarType etype = collect_expression(lexer, table, &exprbuf, &(int){65536}, 1, 0, NULL, NULL, NULL, NULL);
        Token rclose = lexer_peek(lexer);
        if (rclose.type == TOKEN_RPAREN) {
            lexer_next(lexer); /* consume ) */
            /* Discard errors introduced by speculative typeof arg parse */
            if (parser_errors && parser_errors->count > err_before) parser_errors->count = err_before;
            result->type = TOKEN_STRING;
            { free(result->value); result->value = ltng_strdup(var_type_name(etype)); }
            result->line = typeof_tok.line;
            result->col = typeof_tok.col;
            return 1;
        }
        if (parser_errors && parser_errors->count > err_before) parser_errors->count = err_before;
    }

    error_state_add(parser_errors, arg.line, arg.col,
        "Expected ')' after typeof argument");
    lexer->current = saved_pos;
    return 0;
}

void parse_write(Lexer *lexer, SymbolTable *table) {
    Token write_tok = lexer_next(lexer);

    Token lparen = lexer_next(lexer);
    if (lparen.type != TOKEN_LPAREN) {
        error_state_add(parser_errors, write_tok.line, write_tok.col,
            "Expected '(' after 'write'");
        skip_to_newline(lexer);
        return;
    }

    Token first = lexer_peek(lexer);

    /* Check for x.push(y) / x.pop() / x.pop(i) as write argument */
    if (first.type == TOKEN_IDENT && lexer->current + 3 < lexer->count) {
        Token second_tok = lexer->tokens[lexer->current + 1];
        Token third_tok = lexer->tokens[lexer->current + 2];
        Token fourth_tok = lexer->tokens[lexer->current + 3];
        if (second_tok.type == TOKEN_DOT &&
            (third_tok.type == TOKEN_IDENT || token_is_name(third_tok)) &&
            (strcmp(third_tok.value, "push") == 0 || strcmp(third_tok.value, "pop") == 0 ||
             strcmp(third_tok.value, "insert") == 0 || strcmp(third_tok.value, "remove") == 0 ||
             strcmp(third_tok.value, "contains") == 0 || strcmp(third_tok.value, "index_of") == 0 ||
             strcmp(third_tok.value, "sort") == 0 || strcmp(third_tok.value, "reverse") == 0) &&
            fourth_tok.type == TOKEN_LPAREN) {
            char *rm = mangle_name_alloc(first.value);
            if (!symbol_table_has(table, rm)) {
                char *errmsg = _ltng_cfmt("Variable '%s' not declared", first.value);
                error_state_add(parser_errors, first.line, first.col, errmsg ? errmsg : "");
                free(errmsg);
                skip_to_newline(lexer);
                return;
            }
            VarType vt = symbol_table_get(table, rm);
            if (vt != TYPE_ARRAY) {
                char *errmsg = _ltng_cfmt("Variable '%s' is not an array", first.value);
                error_state_add(parser_errors, first.line, first.col, errmsg ? errmsg : "");
                free(errmsg);
                skip_to_newline(lexer);
                return;
            }
            char *arrname = _ltng_cfmt("%s_data", rm);
            char *lenname = _ltng_cfmt("%s_len", rm);
            char *capname = _ltng_cfmt("%s_cap", rm);
            /* Value-consumed context: use general expression parsing so array
               methods operate on a temp copy and do not mutate the original. */
            goto _write_array_method_fallback;
            /* current points to arr; consume arr, ., method, ( */
            lexer_next(lexer); /* consume arr (already peeked as first) */
            lexer_next(lexer); /* consume . */
            lexer_next(lexer); /* consume push/pop */
            lexer_next(lexer); /* consume ( */
            if (strcmp(third_tok.value, "push") == 0) {
                char *arg_expr = (char*)malloc(65536); if(arg_expr) arg_expr[0]=0;
                VarType arg_type = collect_expression(lexer, table, &arg_expr, &(int){65536}, 1, 0, NULL, NULL, NULL, NULL);
                if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                    error_state_add(parser_errors, lexer_peek(lexer).line,
                        lexer_peek(lexer).col, "Expected ')' after push argument");
                    skip_to_newline(lexer);
                    return;
                }
                lexer_next(lexer); /* consume pop's ) */
                lexer_next(lexer); /* consume write's ) */
                char *expr = (char*)malloc(65536); if(expr) expr[0]=0;
                const char *fmt;
                if (arg_type == TYPE_STRING) {
                    fmt = "%s";
                } else if (arg_type == TYPE_INT) {
                    fmt = "%lld";
                } else if (arg_type == TYPE_FLOAT) {
                    fmt = "%g";
                } else {
                    fmt = "%s";
                }
                SNPRINTF_FULL(expr,
                    "({ char *__apv = _ltng_ds_fmt(\"%s\", %s); _ltng_arr_push(&%s, &%s, &%s, __apv); char *__r = _ltng_retbuf_fmt(\"%%s\", __apv); free(__apv); __r; })",
                    fmt, arg_expr, arrname, lenname, capname);
                ASTNode *node = create_node(NODE_WRITE, "", expr);
                node->is_variable = 0;
                node->is_expr = 1;
                node->var_type = TYPE_STRING;
                emit(node);
                return;
            } else if (strcmp(third_tok.value, "insert") == 0) {
                char *idx_expr = (char*)malloc(65536); if(idx_expr) idx_expr[0]=0;
                collect_expression(lexer, table, &idx_expr, &(int){65536}, 1, 1, NULL, NULL, NULL, NULL);
                if (lexer_peek(lexer).type != TOKEN_COMMA) {
                    error_state_add(parser_errors, lexer_peek(lexer).line,
                        lexer_peek(lexer).col, "Expected ',' after insert index");
                    skip_to_newline(lexer);
                    return;
                }
                lexer_next(lexer); /* consume , */
                char *arg_expr = (char*)malloc(65536); if(arg_expr) arg_expr[0]=0;
                VarType arg_type = collect_expression(lexer, table, &arg_expr, &(int){65536}, 1, 0, NULL, NULL, NULL, NULL);
                if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                    error_state_add(parser_errors, lexer_peek(lexer).line,
                        lexer_peek(lexer).col, "Expected ')' after insert argument");
                    skip_to_newline(lexer);
                    return;
                }
                lexer_next(lexer); /* consume insert's ) */
                lexer_next(lexer); /* consume write's ) */
                char *expr = (char*)malloc(65536); if(expr) expr[0]=0;
                const char *fmt;
                if (arg_type == TYPE_STRING) {
                    fmt = "%s";
                } else if (arg_type == TYPE_INT) {
                    fmt = "%lld";
                } else if (arg_type == TYPE_FLOAT) {
                    fmt = "%g";
                } else {
                    fmt = "%s";
                }
                SNPRINTF_FULL(expr,
                    "({ char *__apv = _ltng_ds_fmt(\"%s\", %s); _ltng_arr_insert(&%s, &%s, &%s, (int)(%s), __apv); char *__r = _ltng_retbuf_fmt(\"%%s\", __apv); free(__apv); __r; })",
                    fmt, arg_expr, arrname, lenname, capname, idx_expr);
                ASTNode *node = create_node(NODE_WRITE, "", expr);
                node->is_variable = 0;
                node->is_expr = 1;
                node->var_type = TYPE_STRING;
                emit(node);
                return;
            } else if (strcmp(third_tok.value, "remove") == 0) {
                char *idx_expr = (char*)malloc(65536); if(idx_expr) idx_expr[0]=0;
                collect_expression(lexer, table, &idx_expr, &(int){65536}, 1, 0, NULL, NULL, NULL, NULL);
                if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                    error_state_add(parser_errors, lexer_peek(lexer).line,
                        lexer_peek(lexer).col, "Expected ')' after remove index");
                    skip_to_newline(lexer);
                    return;
                }
                lexer_next(lexer); /* consume remove's ) */
                lexer_next(lexer); /* consume write's ) */
                char *expr = (char*)malloc(65536); if(expr) expr[0]=0;
                SNPRINTF_FULL(expr,
                    "({ char *__r = _ltng_arr_pop_index(%s, &%s, (int)(%s)); __r; })",
                    arrname, lenname, idx_expr);
                ASTNode *node = create_node(NODE_WRITE, "", expr);
                node->is_variable = 0;
                node->is_expr = 1;
                node->var_type = TYPE_STRING;
                emit(node);
                return;
            } else if (strcmp(third_tok.value, "contains") == 0) {
                char *arg_expr = (char*)malloc(65536); if(arg_expr) arg_expr[0]=0;
                VarType arg_type = collect_expression(lexer, table, &arg_expr, &(int){65536}, 1, 0, NULL, NULL, NULL, NULL);
                if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                    error_state_add(parser_errors, lexer_peek(lexer).line,
                        lexer_peek(lexer).col, "Expected ')' after contains argument");
                    skip_to_newline(lexer);
                    return;
                }
                lexer_next(lexer); /* consume contains's ) */
                lexer_next(lexer); /* consume write's ) */
                char *expr = (char*)malloc(65536); if(expr) expr[0]=0;
                if (arg_type == TYPE_INT) {
                    SNPRINTF_FULL(expr,
                        "({ char *__cv = _ltng_ds_fmt(\"%%lld\", (long long)(%s)); char *__r = _ltng_retbuf_fmt(\"%%s\", _ltng_arr_contains(%s, %s, __cv) ? \"true\" : \"false\"); free(__cv); __r; })",
                        arg_expr, arrname, lenname);
                } else if (arg_type == TYPE_FLOAT) {
                    SNPRINTF_FULL(expr,
                        "({ char *__cv = _ltng_ds_fmt(\"%%g\", (double)(%s)); char *__r = _ltng_retbuf_fmt(\"%%s\", _ltng_arr_contains(%s, %s, __cv) ? \"true\" : \"false\"); free(__cv); __r; })",
                        arg_expr, arrname, lenname);
                } else {
                    SNPRINTF_FULL(expr,
                        "({ char *__cv = _ltng_ds_fmt(\"%%s\", %s); char *__r = _ltng_retbuf_fmt(\"%%s\", _ltng_arr_contains(%s, %s, __cv) ? \"true\" : \"false\"); free(__cv); __r; })",
                        arg_expr, arrname, lenname);
                }
                ASTNode *node = create_node(NODE_WRITE, "", expr);
                node->is_variable = 0;
                node->is_expr = 1;
                node->var_type = TYPE_STRING;
                emit(node);
                return;
            } else if (strcmp(third_tok.value, "index_of") == 0) {
                char *arg_expr = (char*)malloc(65536); if(arg_expr) arg_expr[0]=0;
                VarType arg_type = collect_expression(lexer, table, &arg_expr, &(int){65536}, 1, 0, NULL, NULL, NULL, NULL);
                if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                    error_state_add(parser_errors, lexer_peek(lexer).line,
                        lexer_peek(lexer).col, "Expected ')' after index_of argument");
                    skip_to_newline(lexer);
                    return;
                }
                lexer_next(lexer); /* consume index_of's ) */
                lexer_next(lexer); /* consume write's ) */
                char *expr = (char*)malloc(65536); if(expr) expr[0]=0;
                if (arg_type == TYPE_INT) {
                    SNPRINTF_FULL(expr,
                        "({ char *__iv = _ltng_ds_fmt(\"%%lld\", (long long)(%s)); char *__r = _ltng_retbuf_fmt(\"%%d\", _ltng_arr_index_of(%s, %s, __iv)); free(__iv); __r; })",
                        arg_expr, arrname, lenname);
                } else if (arg_type == TYPE_FLOAT) {
                    SNPRINTF_FULL(expr,
                        "({ char *__iv = _ltng_ds_fmt(\"%%g\", (double)(%s)); char *__r = _ltng_retbuf_fmt(\"%%d\", _ltng_arr_index_of(%s, %s, __iv)); free(__iv); __r; })",
                        arg_expr, arrname, lenname);
                } else {
                    SNPRINTF_FULL(expr,
                        "({ char *__iv = _ltng_ds_fmt(\"%%s\", %s); char *__r = _ltng_retbuf_fmt(\"%%d\", _ltng_arr_index_of(%s, %s, __iv)); free(__iv); __r; })",
                        arg_expr, arrname, lenname);
                }
                ASTNode *node = create_node(NODE_WRITE, "", expr);
                node->is_variable = 0;
                node->is_expr = 1;
                node->var_type = TYPE_STRING;
                emit(node);
                return;
            } else if (strcmp(third_tok.value, "sort") == 0 || strcmp(third_tok.value, "reverse") == 0) {
                /* sort()/reverse() in write arg, with chaining support */
                int chain_ops_cap = 4096;
                char *chain_ops = (char*)malloc(chain_ops_cap);
                if(!chain_ops){ skip_to_newline(lexer); return; }
                chain_ops[0] = 0;
                int cop = 0;
                Token cmname = third_tok;
                for (;;) {
                    if (strcmp(cmname.value, "sort") == 0) {
                        if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                            error_state_add(parser_errors, lexer_peek(lexer).line,
                                lexer_peek(lexer).col, "Expected ')' after sort");
                            free(chain_ops); skip_to_newline(lexer); return;
                        }
                        lexer_next(lexer); /* consume ) */
                        STMT_APPEND(chain_ops, chain_ops_cap, cop, "_ltng_arr_sort(%s, %s); ", arrname, lenname);
                    } else if (strcmp(cmname.value, "reverse") == 0) {
                        if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                            error_state_add(parser_errors, lexer_peek(lexer).line,
                                lexer_peek(lexer).col, "Expected ')' after reverse");
                            free(chain_ops); skip_to_newline(lexer); return;
                        }
                        lexer_next(lexer); /* consume ) */
                        STMT_APPEND(chain_ops, chain_ops_cap, cop, "_ltng_arr_reverse(%s, %s); ", arrname, lenname);
                    } else {
                        break;
                    }
                    /* chain: .method() on same array */
                    if (lexer_peek(lexer).type == TOKEN_DOT) {
                        int saved_c = lexer->current;
                        lexer_next(lexer); /* . */
                        Token nm = lexer_peek(lexer);
                        if ((nm.type == TOKEN_IDENT || token_is_name(nm)) &&
                            (strcmp(nm.value, "sort") == 0 || strcmp(nm.value, "reverse") == 0)) {
                            lexer_next(lexer); /* name */
                            if (lexer_peek(lexer).type == TOKEN_LPAREN) {
                                lexer_next(lexer); /* ( */
                                cmname = nm;
                                continue;
                            }
                        }
                        lexer->current = saved_c;
                    }
                    break;
                }
                if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                    error_state_add(parser_errors, lexer_peek(lexer).line,
                        lexer_peek(lexer).col, "Expected ')' after write argument");
                    free(chain_ops); skip_to_newline(lexer); return;
                }
                lexer_next(lexer); /* consume write's ) */
                char *expr = (char*)malloc(65536*2+1); if(expr) expr[0]=0;
                SNPRINTF_FULL(expr,
                    "({ %s_ltng_retbuf_fmt(\"true\"); })",
                    chain_ops);
                ASTNode *node = create_node(NODE_WRITE, "", expr);
                node->is_variable = 0;
                node->is_expr = 1;
                node->var_type = TYPE_STRING;
                emit(node);
                free(chain_ops);
                return;
            } else if (strcmp(third_tok.value, "pop") == 0) {
                /* pop() or pop(index) */
                if (lexer_peek(lexer).type == TOKEN_RPAREN) {
                    /* pop() - no args */
                    lexer_next(lexer); /* consume pop's ) */
                    lexer_next(lexer); /* consume write's ) */
                    char *expr = (char*)malloc(65536); if(expr) expr[0]=0;
                    SNPRINTF_FULL(expr,
                        "({ char *__r = _ltng_arr_pop(%s, &%s); __r; })",
                        arrname, lenname);
                    ASTNode *node = create_node(NODE_WRITE, "", expr);
                    node->is_variable = 0;
                    node->is_expr = 1;
                    node->var_type = TYPE_STRING;
                    emit(node);
                    return;
                } else {
                    /* pop(index) */
                    char *idx_expr = (char*)malloc(65536); if(idx_expr) idx_expr[0]=0;
                    collect_expression(lexer, table, &idx_expr, &(int){65536}, 1, 0, NULL, NULL, NULL, NULL);
                    if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                        error_state_add(parser_errors, lexer_peek(lexer).line,
                            lexer_peek(lexer).col, "Expected ')' after pop index");
                        skip_to_newline(lexer);
                        return;
                    }
                    lexer_next(lexer); /* consume pop index's ) */
                    lexer_next(lexer); /* consume write's ) */
                    char *expr = (char*)malloc(65536); if(expr) expr[0]=0;
                    SNPRINTF_FULL(expr,
                        "({ char *__r = _ltng_arr_pop_index(%s, &%s, (int)(%s)); __r; })",
                        arrname, lenname, idx_expr);
                    ASTNode *node = create_node(NODE_WRITE, "", expr);
                    node->is_variable = 0;
                    node->is_expr = 1;
                    node->var_type = TYPE_STRING;
                    emit(node);
                    return;
                }
            }
        }
    _write_array_method_fallback: ;
    }

    /* Direct callable call as sole write argument: write(f(x)), write(f(x)(y)) */
    if (first.type == TOKEN_IDENT) {
        Token second_tok = lexer->tokens[lexer->current + 1];
        if (second_tok.type == TOKEN_LPAREN && is_callable_ident(table, first.value) &&
            !is_type_conversion(first.value)) {
            int saved_call = lexer->current;
            Token fname = lexer_next(lexer);
            lexer_next(lexer); /* consume ( */
            ASTNode *call = parse_call_node(lexer, table, fname.value, fname.line, fname.col, NULL);
            char *cexpr = (char*)malloc(65536); if(cexpr) cexpr[0]=0; int cexpr_cap = 65536;
            build_call_cexpr(call, &cexpr, &cexpr_cap);
            int chained = 0;
            while (lexer_peek(lexer).type == TOKEN_LPAREN) {
                chained = 1;
                int cline = lexer_peek(lexer).line, ccol = lexer_peek(lexer).col;
                lexer_next(lexer); /* consume ( */
                ASTNode *args = NULL, *last = NULL;
                if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                    while (1) {
                        char *arg_expr = (char*)malloc(65536); if(arg_expr) arg_expr[0]=0;
                        VarType at = collect_expression(lexer, table, &arg_expr, &(int){65536}, 1, 1, NULL, NULL, NULL, NULL);
                        ASTNode *an = create_node(NODE_PARAM, "", arg_expr);
                        an->var_type = at; an->is_expr = 1;
                        an->v_sl=g_expr_sl; an->v_sc=g_expr_sc; an->v_el=g_expr_el; an->v_ec=g_expr_ec;
                        if (!args) args = last = an; else { last->next = an; last = an; }
                        Token sep = lexer_peek(lexer);
                        if (sep.type == TOKEN_COMMA) { lexer_next(lexer); continue; }
                        break;
                    }
                }
                if (lexer_peek(lexer).type != TOKEN_RPAREN) { lexer->current = saved_call; goto _write_call_fallback; }
                lexer_next(lexer); /* consume ) */
                int argc = 0; for (ASTNode *a = args; a; a = a->next) argc++;
                char *next_expr = (char*)malloc(65536); if(next_expr) next_expr[0]=0; int next_expr_cap = 65536; int p = 0;
                static int _wcid = 0; int cid = _wcid++;
                STMT_APPEND(next_expr, next_expr_cap, p, "({ char *__ct%d=_ltng_ds_fmt(\"%%s\",%s); char *__ctr%d=__ltng_call_indirect(__ct%d,_ltng_retbuf(),%d,%d,%d",
                    cid,cexpr,cid,cid,cline>0?cline:1,ccol>0?ccol:1,argc);
                /* p grown dynamically */
                for (ASTNode *a = args; a ; a = a->next) {
                    if (a->var_type == TYPE_STRING && a->value[0] == '"') STMT_APPEND(next_expr, next_expr_cap, p, ",%s", a->value);
                    else if (a->is_variable && a->value[0] != '\0') STMT_APPEND(next_expr, next_expr_cap, p, ",%s", a->value);
                    else if ((a->var_type == TYPE_STRING || a->var_type == TYPE_OBJECT) && a->value[0] != '\0') STMT_APPEND(next_expr, next_expr_cap, p, ",%s", a->value);
                    else if (a->value[0] == '\0') STMT_APPEND(next_expr, next_expr_cap, p, ",\"\"");
                    else STMT_APPEND(next_expr, next_expr_cap, p, ",_ltng_ad((double)(%s))", a->value);
                    /* p grown dynamically */
                }
                STMT_APPEND(next_expr, next_expr_cap, p, "); free(__ct%d); __ctr%d; })", cid, cid);
                if (p < next_expr_cap) next_expr[p] = 0;
                if (p >= next_expr_cap) {
                    error_state_add(parser_errors, cline, ccol, "Chained call expression too deep/complex");
                    lexer->current = saved_call; goto _write_call_fallback;
                }
                SNPRINTF_FULL(cexpr, "%s", next_expr);
            }
            if (lexer_peek(lexer).type == TOKEN_RPAREN) {
                lexer_next(lexer); /* consume write's ) */
                VarType _wct = chained ? TYPE_STRING : infer_call_result_type(call);
                if (_wct == TYPE_ARRAY) {
                    char *_fmt = (char*)malloc(65536); if(_fmt) _fmt[0]=0;
                    SNPRINTF_FULL(_fmt,
                        "({ char *__ahv=_ltng_ds_new(%s); _ltng_ah *__ah=_ltng_ah_unpack(__ahv); char *__r=_ltng_ds_new(\"\"); if(__ah){ for(int _i=0;_i<__ah->len;_i++){ if(_i) _ltng_ds_append(&__r,\", \"); _ltng_ds_append(&__r,__ah->d[_i]?__ah->d[_i]:\"\"); } _ltng_ah_free(__ah); } free(__ahv); __r; })",
                        cexpr);
                    SNPRINTF_FULL(cexpr, "%s", _fmt);
                }
                ASTNode *node = create_node(NODE_WRITE, "", cexpr);
                node->is_variable = 0;
                node->is_expr = 1;
                node->var_type = TYPE_STRING;
                emit(node);
                return;
            }
            /* not a sole call (e.g. f(x) + 1) - backtrack and let expression parsing handle it */
            lexer->current = saved_call;
        }
    _write_call_fallback: ;
    }

    Token second = lexer->tokens[lexer->current + 1];
    int is_conv_call = (first.type == TOKEN_IDENT && is_type_conversion(first.value) &&
                        second.type == TOKEN_LPAREN);

    /* Simple array element access as sole write argument: write(arr[i]) / write(arr[i][j])
       Preserve raw string value for OOB compatibility (empty string) and exact element text. */
    if (first.type == TOKEN_IDENT && second.type == TOKEN_LBRACKET) {
        char *rm_arr = mangle_name_alloc(first.value);
        if (symbol_table_has(table, rm_arr) && symbol_table_get(table, rm_arr) == TYPE_ARRAY) {
            int saved_a = lexer->current;
            Token arrtok = lexer_next(lexer); /* consume arr ident */
            (void)arrtok;
            int idx_cap_w = 4, idx_n_w = 0;
            char **idx_exprs_w = (char**)malloc(idx_cap_w * sizeof(char*));
            int ok_w = idx_exprs_w != NULL;
            while (ok_w && lexer_peek(lexer).type == TOKEN_LBRACKET) {
                lexer_next(lexer); /* [ */
                if (idx_n_w >= idx_cap_w) {
                    int nc = idx_cap_w * 2;
                    char **nb = (char**)realloc(idx_exprs_w, nc * sizeof(char*));
                    if (!nb) { ok_w = 0; break; }
                    idx_exprs_w = nb; idx_cap_w = nc;
                }
                idx_exprs_w[idx_n_w] = (char*)malloc(65536);
                if (!idx_exprs_w[idx_n_w]) { ok_w = 0; break; }
                idx_exprs_w[idx_n_w][0] = 0;
                collect_expression(lexer, table, &idx_exprs_w[idx_n_w], &(int){65536}, 1, 0, NULL, NULL, NULL, NULL);
                Token rb = lexer_peek(lexer);
                if (rb.type != TOKEN_RBRACKET) { ok_w = 0; break; }
                lexer_next(lexer); /* ] */
                idx_n_w++;
            }
            if (ok_w && idx_n_w > 0 && lexer_peek(lexer).type == TOKEN_RPAREN) {
                lexer_next(lexer); /* consume write's ) */
                char *arrname_w = _ltng_cfmt("%s_data", rm_arr);
                char *lenname_w = _ltng_cfmt("%s_len", rm_arr);
                int *dims_w = NULL; int dimc_w = 0;
                int have_dims_w = symbol_table_get_array_dims(table, rm_arr, &dims_w, &dimc_w);
                /* For 1D or matching dims, emit _ltng_arr_get; for unknown dims treat as 1D */
                int edim = (have_dims_w && dimc_w > 0) ? dimc_w : 1;
                if (idx_n_w == edim) {
                    char *expr = malloc(65536);
                    if (!expr) { for(int _ix=0;_ix<idx_n_w;_ix++) free(idx_exprs_w[_ix]); free(idx_exprs_w); lexer->current = saved_a; goto _write_array_fallback; }
                    int p = 0;
                    int esz = 65536;
                    #define WAPP(...) STMT_APPEND(expr, esz, p, __VA_ARGS__)
                    WAPP("({ ");
                    for (int d = 0; d < idx_n_w; d++) {
                        if (dimc_w == 1) {
                            WAPP("int __i%d = (int)(%s); __i%d = __i%d > 0 ? __i%d - 1 : __i%d + (%s); if (__i%d < 0 || __i%d >= %s) __i%d = %s; ", d, idx_exprs_w[d], d, d, d, d, lenname_w, d, d, lenname_w, d, lenname_w);
                        } else if (have_dims_w && dims_w && d < dimc_w) {
                            WAPP("int __i%d = (int)(%s); __i%d = __i%d > 0 ? __i%d - 1 : __i%d + (%d); ", d, idx_exprs_w[d], d, d, d, d, dims_w[d]);
                        } else {
                            WAPP("int __i%d = (int)(%s); __i%d = __i%d > 0 ? __i%d - 1 : __i%d + (%s); ", d, idx_exprs_w[d], d, d, d, d, lenname_w);
                        }
                    }
                    WAPP("int __idx = __i0");
                    for (int d = 1; d < idx_n_w; d++) {
                        if (have_dims_w && dims_w && d < dimc_w)
                            WAPP("; __idx = __idx * %d + __i%d", dims_w[d], d);
                        else
                            WAPP("; __idx = __idx * %s + __i%d", lenname_w, d);
                    }
                    WAPP("; _ltng_arr_get(%s, %s, __idx); })", arrname_w, lenname_w);
                    #undef WAPP
                    expr[esz - 1] = '\0';
                    ASTNode *node = create_node(NODE_WRITE, "", expr);
                    node->is_variable = 0;
                    node->is_expr = 1;
                    node->var_type = TYPE_STRING; /* preserve raw/OOB string behavior */
                    emit(node);
                    free(expr);
                    for(int _ix=0;_ix<idx_n_w;_ix++) free(idx_exprs_w[_ix]);
                    free(idx_exprs_w);
                    return;
                }
                /* fall back to general expression handling for mismatched dims */
                for(int _ix=0;_ix<idx_n_w;_ix++) free(idx_exprs_w[_ix]);
                free(idx_exprs_w);
                lexer->current = saved_a;
                goto _write_array_fallback;
            }
            if (idx_exprs_w) { for(int _ix=0;_ix<idx_n_w;_ix++) free(idx_exprs_w[_ix]); free(idx_exprs_w); }
            _write_array_fallback:
            lexer->current = saved_a;
        }
    }

    if (first.type == TOKEN_LPAREN || first.type == TOKEN_MINUS || first.type == TOKEN_BANG ||
        first.type == TOKEN_PLUS_PLUS || first.type == TOKEN_MINUS_MINUS ||
        has_operator_ahead(lexer) || is_conv_call ||
        (first.type == TOKEN_IDENT && second.type == TOKEN_DOT) ||
        (first.type == TOKEN_IDENT && second.type == TOKEN_LPAREN && is_callable_ident(table, first.value))) {
        char *expr = (char*)malloc(65536); if(expr) expr[0]=0;
        VarType expr_type = collect_expression(lexer, table, &expr, &(int){65536}, 1, 0, NULL, NULL, NULL, NULL);
        int esl=g_expr_sl,esc=g_expr_sc,eel=g_expr_el,eec=g_expr_ec;

        Token rparen = lexer_next(lexer);
        if (rparen.type != TOKEN_RPAREN) {
            error_state_add(parser_errors, write_tok.line, write_tok.col,
                "Expected ')' after write argument");
            skip_to_newline(lexer);
            return;
        }

        ASTNode *node = create_node(NODE_WRITE, "", expr);
        node->v_sl=esl; node->v_sc=esc; node->v_el=eel; node->v_ec=eec;
        node->is_variable = 0;
        node->is_expr = 1;
        node->var_type = expr_type;
        emit(node);
        return;
    }

    Token arg = lexer_next(lexer);

    ASTNode *node;

    Token typeof_val = {0};
    if (arg.type == TOKEN_IDENT && strcmp(arg.value, "typeof") == 0) {
        lexer->current--;
        if (try_parse_typeof(lexer, table, &typeof_val)) {
            arg = typeof_val;
        }
    }

        if (arg.type == TOKEN_STRING || arg.type == TOKEN_CHAR) {
        node = create_node(NODE_WRITE, "", arg.value);
        node->is_variable = 0;
        if (arg.type == TOKEN_CHAR) node->var_type = TYPE_STRING;
        if (arg.type == TOKEN_STRING) {
            int resolved_cap = 65536; char *resolved = (char*)malloc(resolved_cap); if(resolved) resolved[0]=0;
            int rlen = 0;
            for (const char *sp = arg.value; *sp; sp++) {
                if (*sp == '$' && sp[1] == '[') {
                    const char *vstart = sp + 2;
                    sp += 2;
                    int ibd = 0;
                    while (*sp) {
                        if (*sp == '[') ibd++;
                        else if (*sp == ']') { if (ibd == 0) break; ibd--; }
                        sp++;
                    }
                    if (*sp == ']') {
                        int vlen = (int)(sp - vstart);
                        char *vname = (char*)malloc((size_t)vlen + 1); if(vname){ memcpy(vname, vstart, vlen); vname[vlen]='\0'; }
                        int skip_check = 0;
                        if (resolve_typeof_interpolation(vname, table, arg.line, arg.col, resolved, &rlen)) {
                            skip_check = 1;
                        }
                        if (!skip_check) {
                            int is_expr = 0;
                            for (const char *c = vname; *c; c++) {
                                if (*c == '+' || *c == '-' || *c == '*' || *c == '/' || *c == '%' ||
                                    *c == '(' || *c == ')' || *c == '<' || *c == '>' ||
                                    *c == '&' || *c == '|' || *c == '^' || *c == '!' ||
                                    *c == '[' || *c == ']' ||
                                    *c == ' ' || *c == '.') {
                                    is_expr = 1;
                                    break;
                                }
                            }
                            if (!is_expr) {
                                char *mvname = mangle_name_alloc(vname);
                                if (!symbol_table_has(table, mvname) && !is_type_keyword(vname) && !is_bool_value(vname)) {
                                    char *errmsg = _ltng_cfmt("Variable '%s' not declared (used in string interpolation)", vname);
                                    error_state_add(parser_errors, arg.line, arg.col, errmsg ? errmsg : "");
                                    free(errmsg);
                                }
                            } else {
                                int check_cap = 65536;
                                char *check = (char*)malloc(check_cap); if(check) check[0]=0;
                                parse_expression_from_string(vname, table, &check, &check_cap);
                            }
                            int voff = (int)(vstart - 2 - arg.value);
                            int vtotal = (int)(sp - (vstart - 2) + 1);
                            for (int ci = voff; ci < voff + vtotal ; ci++) {
                                if (rlen+1 >= resolved_cap) { int nc=resolved_cap*2; char *nr=(char*)realloc(resolved,nc); if(nr){resolved=nr;resolved_cap=nc;} }
                                if (rlen < resolved_cap-1) resolved[rlen++] = arg.value[ci];
                            }
                        }
                    } else {
                        if (rlen + 3 >= resolved_cap) { int nc=resolved_cap*2; char *nr=(char*)realloc(resolved,nc); if(nr){resolved=nr;resolved_cap=nc;} }
                        if (rlen + 2 < resolved_cap) { resolved[rlen++] = '$'; resolved[rlen++] = '['; }
                    }
                } else {
                    if (rlen+1 >= resolved_cap) { int nc=resolved_cap*2; char *nr=(char*)realloc(resolved,nc); if(nr){resolved=nr;resolved_cap=nc;} }
                    if (rlen < resolved_cap-1) resolved[rlen++] = *sp;
                }
            }
            resolved[rlen] = '\0';
            { free(node->value); node->value = ltng_strdup(resolved); }
        }
    } else if (arg.type == TOKEN_INT_LIT || arg.type == TOKEN_FLOAT_LIT) {
        node = create_node(NODE_WRITE, "", arg.value);
        node->is_variable = 0;
    } else if (arg.type == TOKEN_IDENT || token_is_name(arg)) {
        if (is_bool_value(arg.value)) {
            node = create_node(NODE_WRITE, "", bool_to_str(arg.value));
            node->is_variable = 0;
            node->var_type = TYPE_STRING;
                } else if (is_callable_ident(table, arg.value) && lexer_peek(lexer).type == TOKEN_LPAREN) {
            /* write(func(...)) */
            lexer_next(lexer); /* consume ( */
            ASTNode *call = parse_call_node(lexer, table, arg.value, arg.line, arg.col, NULL);
            char *cexpr = (char*)malloc(65536); if(cexpr) cexpr[0]=0; int cexpr_cap = 65536;
            build_call_cexpr(call, &cexpr, &cexpr_cap);
            node = create_node(NODE_WRITE, "", cexpr);
            node->is_variable = 0;
            node->is_expr = 1;
            node->var_type = TYPE_STRING;
        } else if (lexer_peek(lexer).type == TOKEN_DOT) {
            int saved_m = lexer->current;
            lexer_next(lexer); /* dot */
            Token mname = lexer_next(lexer);
            if ((mname.type == TOKEN_IDENT || token_is_name(mname)) &&
                lexer_peek(lexer).type == TOKEN_LPAREN &&
                is_function_name_scoped(table, mname.value)) {
                /* method call: receiver.method(args) */
                lexer_next(lexer); /* ( */
                ASTNode *recv = make_receiver_arg(table, arg);
                ASTNode *call = parse_call_node(lexer, table, mname.value, mname.line, mname.col, recv);
                if (call->is_base_assign) {
                    /* Value-consumed context (write arg): keep receiver intact, use return value */
                    call->is_base_assign = 0;
                }
                char *cexpr = (char*)malloc(65536); if(cexpr) cexpr[0]=0; int cexpr_cap = 65536;
                build_call_cexpr(call, &cexpr, &cexpr_cap);
                node = create_node(NODE_WRITE, "", cexpr);
                node->is_variable = 0;
                node->is_expr = 1;
                node->var_type = TYPE_STRING;
            } else if (mname.type == TOKEN_IDENT || token_is_name(mname)) {
                /* property access: obj.field, or array .length */
                char *mwname = mangle_name_alloc(arg.value);
                if (!symbol_table_has(table, mwname)) {
                    error_state_add(parser_errors, arg.line, arg.col,
                        "Variable not declared");
                    skip_to_newline(lexer);
                    return;
                }
                const char *cwname = symbol_table_get_cname(table, mwname);
                VarType avt = symbol_table_get(table, mwname);
                /* Array .length property */
                if (avt == TYPE_ARRAY && strcmp(mname.value, "length") == 0) {
                    char *lenname = _ltng_cfmt("%s_len", cwname);
                    node = create_node(NODE_WRITE, "", (char *)lenname);
                    node->is_variable = 0;
                    node->is_expr = 1;
                    node->var_type = TYPE_INT;
                } else {
                    char *fv = obj_resolve_field_alloc(cwname, mname.value);
                    node = create_node(NODE_WRITE, fv ? fv : "", "");
                    node->is_variable = 1;
                    node->var_type = TYPE_STRING; /* fields stored as strings */
                    free(fv);
                }
            } else {
                lexer->current = saved_m;
                char *mwname = mangle_name_alloc(arg.value);
                if (!symbol_table_has(table, mwname)) {
                    error_state_add(parser_errors, arg.line, arg.col,
                        "Variable not declared");
                    skip_to_newline(lexer);
                    return;
                }
                const char *cwname = symbol_table_get_cname(table, mwname);
                node = create_node(NODE_WRITE, cwname, "");
                node->is_variable = 1;
                node->var_type = symbol_table_get(table, mwname);
            }
        } else {
            char *mwname = mangle_name_alloc(arg.value);
            if (!symbol_table_has(table, mwname)) {
                error_state_add(parser_errors, arg.line, arg.col,
                    "Variable not declared");
                skip_to_newline(lexer);
                return;
            }
            const char *cwname = symbol_table_get_cname(table, mwname);
            node = create_node(NODE_WRITE, cwname, "");
            node->is_variable = 1;
            node->var_type = symbol_table_get(table, mwname);
        }
    } else {
        error_state_add(parser_errors, arg.line, arg.col,
            "Expected string, char, number, or variable in write()");
        skip_to_newline(lexer);
        return;
    }

    Token rparen = lexer_next(lexer);
    if (rparen.type != TOKEN_RPAREN) {
        error_state_add(parser_errors, arg.line, arg.col,
            "Expected ')' after write argument");
        skip_to_newline(lexer);
        return;
    }

    emit(node);
}

void parse_read(Lexer *lexer, SymbolTable *table) {
    Token read_tok = lexer_next(lexer);

    Token lparen = lexer_next(lexer);
    if (lparen.type != TOKEN_LPAREN) {
        error_state_add(parser_errors, read_tok.line, read_tok.col,
            "Expected '(' after 'read'");
        skip_to_newline(lexer);
        return;
    }

    Token arg = lexer_next(lexer);
    if (!token_is_name(arg)) {
        error_state_add(parser_errors, arg.line, arg.col,
            "Expected variable name in read()");
        skip_to_newline(lexer);
        return;
    }

    if (!symbol_table_has(table, arg.value)) {
        error_state_add(parser_errors, arg.line, arg.col,
            "Variable not declared");
        skip_to_newline(lexer);
        return;
    }

    if (symbol_table_is_const(table, arg.value)) {
        error_state_add(parser_errors, arg.line, arg.col,
            "Cannot read into constant");
        skip_to_newline(lexer);
        return;
    }

    char *mangled = mangle_name_alloc(arg.value);

    VarType vtype = symbol_table_get(table, mangled);

    /* Read into object type is not supported */
    if (vtype == TYPE_OBJECT) {
        error_state_add(parser_errors, arg.line, arg.col,
            "Cannot read into object variable");
        skip_to_newline(lexer);
        return;
    }

    /* Read sets dtype dynamically: null variable takes the type of input read.
       For null variables, read() reads text so dtype becomes STRING. */
    if (vtype == TYPE_NULL) {
        symbol_table_update(table, mangled, TYPE_STRING);
        vtype = TYPE_STRING;
    }

    Token rparen = lexer_next(lexer);
    if (rparen.type != TOKEN_RPAREN) {
        error_state_add(parser_errors, arg.line, arg.col,
            "Expected ')' after read argument");
        skip_to_newline(lexer);
        return;
    }

    ASTNode *node = create_node(NODE_READ, mangled, "");
    node->var_type = vtype;

    emit(node);
}

/* Try to parse a sole function call RHS: fname(args) with nothing after.
   On success, fills cexpr with the C call expression and returns 1 (tokens consumed).
   On failure, restores lexer and returns 0. */

/* Look up a pre-scanned signature by emitted C name. Used for calls that
   appear before the target function body has been parsed. */
FuncInfo *find_funcinfo_by_cname(const char *cname) {
    for (int i = 0; i < g_func_count; i++) {
        if (strcmp(g_funcs[i].name, cname) == 0) return &g_funcs[i];
    }
    return NULL;
}

void append_arg_expr(char **outp, int *pos, int *cap, ASTNode *a) {
    if (a->var_type == TYPE_ARRAY && a->value[0] != '\0') {
        if (a->is_variable)
            STMT_APPEND(*outp, *cap, *pos, "_ltng_ah_pack(%s_data, %s_len, %s_cap)", a->value, a->value, a->value);
        else
            STMT_APPEND(*outp, *cap, *pos, "%s", a->value);
    } else if (a->var_type == TYPE_STRING && a->value[0] == '"') {
        STMT_APPEND(*outp, *cap, *pos, "%s", a->value);
    } else if (a->is_variable && a->value[0] != '\0') {
        STMT_APPEND(*outp, *cap, *pos, "%s", a->value);
    } else if ((a->var_type == TYPE_STRING || a->var_type == TYPE_OBJECT || a->var_type == TYPE_CHAR) && a->value[0] != '\0') {
        STMT_APPEND(*outp, *cap, *pos, "%s", a->value);
    } else if (a->var_type == TYPE_BOOL && a->value[0] != '\0') {
        STMT_APPEND(*outp, *cap, *pos, "((%s)?\"true\":\"false\")", a->value);
    } else if (a->value[0] == '\0') {
        STMT_APPEND(*outp, *cap, *pos, "\"\"");
    } else {
        STMT_APPEND(*outp, *cap, *pos, "_ltng_ad((double)(%s))", a->value);
    }
}

void build_call_cexpr(ASTNode *call, char **cexpr, int *cexpr_cap) {
    int cpos = 0;
    int cline = call->line > 0 ? call->line : 1;
    int ccol = call->col > 0 ? call->col : 1;
    if ((*cexpr)[0]) (*cexpr)[0] = 0; /* reset */
    if (strcmp(call->name, "__ltng_indirect") == 0) {
        int argc = 0;
        for (ASTNode *a = call->args; a; a = a->next) argc++;
        STMT_APPEND(*cexpr, *cexpr_cap, cpos, "__ltng_call_indirect(%s, _ltng_retbuf(), %d, %d, %d", call->for_var, cline, ccol, argc);
        for (ASTNode *a = call->args; a; a = a->next) {
            STMT_APPEND(*cexpr, *cexpr_cap, cpos, ", ");
            append_arg_expr(cexpr, &cpos, cexpr_cap, a);
        }
        STMT_APPEND(*cexpr, *cexpr_cap, cpos, ")");
    } else {
        ASTNode *fn = find_function_node_by_cname(call->name);
        int variadic = fn ? fn->is_variadic : 0;
        if (!fn) {
            FuncInfo *fi = find_funcinfo_by_cname(call->name);
            if (fi) variadic = fi->is_variadic;
        }
        if (variadic) {
            int restn = 0;
            for (ASTNode *a = call->rest_args; a; a = a->next) restn++;
            STMT_APPEND(*cexpr, *cexpr_cap, cpos,
                "({ int __rn = 0; int __rc = 0; char **__rv = _ltng_arr_new(%d);",
                restn > 0 ? restn : 1);
            for (ASTNode *a = call->rest_args; a; a = a->next) {
                STMT_APPEND(*cexpr, *cexpr_cap, cpos,
                    " { char *__av = _ltng_ds_fmt(\"%%s\", ");
                append_arg_expr(cexpr, &cpos, cexpr_cap, a);
                STMT_APPEND(*cexpr, *cexpr_cap, cpos,
                    "); _ltng_arr_push(&__rv, &__rn, &__rc, __av); free(__av); }");
            }
            STMT_APPEND(*cexpr, *cexpr_cap, cpos,
                " char *__rr = %s(_ltng_retbuf(), %d, %d", call->name, cline, ccol);
            for (ASTNode *a = call->args; a; a = a->next) {
                STMT_APPEND(*cexpr, *cexpr_cap, cpos, ", ");
                append_arg_expr(cexpr, &cpos, cexpr_cap, a);
            }
            STMT_APPEND(*cexpr, *cexpr_cap, cpos,
                ", __rn, (const char**)__rv); _ltng_arr_free(__rv, __rn); __rr; })");
        } else {
            STMT_APPEND(*cexpr, *cexpr_cap, cpos, "%s(_ltng_retbuf(), %d, %d", call->name, cline, ccol);
            for (ASTNode *a = call->args; a; a = a->next) {
                STMT_APPEND(*cexpr, *cexpr_cap, cpos, ", ");
                append_arg_expr(cexpr, &cpos, cexpr_cap, a);
            }
            STMT_APPEND(*cexpr, *cexpr_cap, cpos, ")");
        }
    }
}

int append_immediate_call_chains(Lexer *lexer, SymbolTable *table, char **cexpr, int *cexpr_cap) {
    int chained = 0;
    while (1) {
        if (lexer_peek(lexer).type == TOKEN_LPAREN) {
            chained = 1;
            int cline = lexer_peek(lexer).line, ccol = lexer_peek(lexer).col;
            lexer_next(lexer); /* consume ( */
            ASTNode *args = NULL, *last = NULL;
            if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                while (1) {
                    char *arg_expr = (char*)malloc(65536); if(arg_expr) arg_expr[0]=0;
                    VarType at = collect_expression(lexer, table, &arg_expr, &(int){65536}, 1, 1, NULL, NULL, NULL, NULL);
                    ASTNode *an = create_node(NODE_PARAM, "", arg_expr);
                    an->var_type = at; an->is_expr = 1;
                    an->v_sl = g_expr_sl; an->v_sc = g_expr_sc; an->v_el = g_expr_el; an->v_ec = g_expr_ec;
                    if (!args) args = last = an; else { last->next = an; last = an; }
                    Token sep = lexer_peek(lexer);
                    if (sep.type == TOKEN_COMMA) { lexer_next(lexer); continue; }
                    break;
                }
            }
            if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                error_state_add(parser_errors, lexer_peek(lexer).line, lexer_peek(lexer).col,
                    "Expected ')' in chained call");
                return chained;
            }
            lexer_next(lexer); /* consume ) */
            int argc = 0; for (ASTNode *a = args; a; a = a->next) argc++;
            char *next_expr = (char*)malloc(65536); if(next_expr) next_expr[0]=0; int next_expr_cap = 65536; int p = 0;
            static int _chain_id = 0; int cid = _chain_id++;
            STMT_APPEND(next_expr, next_expr_cap, p,
                "({ char *__ct%d=_ltng_ds_fmt(\"%%s\",%s); char *__ctr%d=__ltng_call_indirect(__ct%d,_ltng_retbuf(),%d,%d,%d",
                cid, *cexpr, cid, cid, cline > 0 ? cline : 1, ccol > 0 ? ccol : 1, argc);
            /* p grown dynamically */
            for (ASTNode *a = args; a ; a = a->next) {
                if (a->var_type == TYPE_STRING && a->value[0] == '"')
                    STMT_APPEND(next_expr, next_expr_cap, p, ",%s", a->value);
                else if (a->is_variable && a->value[0] != '\0')
                    STMT_APPEND(next_expr, next_expr_cap, p, ",%s", a->value);
                else if ((a->var_type == TYPE_STRING || a->var_type == TYPE_OBJECT || a->var_type == TYPE_CHAR) && a->value[0] != '\0')
                    STMT_APPEND(next_expr, next_expr_cap, p, ",%s", a->value);
                else if (a->var_type == TYPE_BOOL && a->value[0] != '\0')
                    STMT_APPEND(next_expr, next_expr_cap, p, ",((%s)?\"true\":\"false\")", a->value);
                else if (a->value[0] == '\0')
                    STMT_APPEND(next_expr, next_expr_cap, p, ",\"\"");
                else
                    STMT_APPEND(next_expr, next_expr_cap, p, ",_ltng_ad((double)(%s))", a->value);
                /* p grown dynamically */
            }
            STMT_APPEND(next_expr, next_expr_cap, p, "); free(__ct%d); __ctr%d; })", cid, cid);
            if (p < next_expr_cap) next_expr[p] = 0;
            SNPRINTF_FULL(*cexpr, "%s", next_expr);
            continue;
        } else if (lexer_peek(lexer).type == TOKEN_DOT) {
            int saved_dot = lexer->current;
            lexer_next(lexer); /* consume . */
            Token mname = lexer_peek(lexer);
            /* Array method chain on expression result (e.g. getArray().sort()) */
            if ((mname.type == TOKEN_IDENT || token_is_name(mname)) &&
                (strcmp(mname.value,"push")==0||strcmp(mname.value,"pop")==0||strcmp(mname.value,"insert")==0||
                 strcmp(mname.value,"remove")==0||strcmp(mname.value,"contains")==0||strcmp(mname.value,"index_of")==0||
                 strcmp(mname.value,"sort")==0||strcmp(mname.value,"reverse")==0||strcmp(mname.value,"copy")==0||strcmp(mname.value,"slice")==0) &&
                lexer->current + 1 < lexer->count && lexer->tokens[lexer->current + 1].type == TOKEN_LPAREN) {
                lexer_next(lexer); /* consume method name */
                static int _ahc_id = 0; int aid = _ahc_id++;
                char *prefix = (char*)malloc(16384); int prefix_cap = 16384; if(!prefix){SNPRINTF_FULL(*cexpr, "%s", "");chained=1;continue;} prefix[0]=0; int pp = 0;
                STMT_APPEND(prefix,prefix_cap,pp,
                    "char *__ahsrc%d=_ltng_ds_new(%s); _ltng_ah *__ah%d=_ltng_ah_unpack(__ahsrc%d); char **__td%d=NULL; int __tl%d=0,__tc%d=0; if(__ah%d){ _ltng_arr_copy(__ah%d->d,&__td%d,&__tl%d,&__tc%d,__ah%d->len); } ",
                    aid,*cexpr,aid,aid,aid,aid,aid,aid,aid,aid,aid,aid,aid);
                char *final_expr = (char*)malloc(4096); if(!final_expr){SNPRINTF_FULL(*cexpr, "%s", "\"\"");chained=1;continue;} strcpy(final_expr,"\"\""); int chain_err=0; int has_scalar=0; int step=0;
                Token cm = mname; int has_chain=1;
                while (has_chain && !chain_err) {
                    step++;
                    if (lexer_peek(lexer).type != TOKEN_LPAREN) { chain_err=1; break; }
                    lexer_next(lexer);
                    has_scalar = 1; (void)has_scalar;
                    if (strcmp(cm.value,"copy")==0) {
                        if(lexer_peek(lexer).type!=TOKEN_RPAREN){chain_err=1;break;} lexer_next(lexer);
                        /* copy is no-op on temp (already a copy), but keep as temp */
                        SNPRINTF_FULL(final_expr,"({ char *__r=_ltng_ds_new(\"\"); for(int _i=0;_i<__tl%d;_i++){ if(_i) _ltng_ds_append(&__r,\", \"); _ltng_ds_append(&__r,__td%d[_i]?__td%d[_i]:\"\"); } __r; })",aid,aid,aid);
                        has_scalar = 1; (void)has_scalar;
                    } else if (strcmp(cm.value,"slice")==0) {
                        char *s0=(char*)malloc(65536); char *s1=(char*)malloc(65536); if(s0)s0[0]=0; if(s1)s1[0]=0;
                        collect_expression(lexer,table,&s0, &(int){sizeof(s0)},1,1,NULL,NULL,NULL,NULL);
                        if(lexer_peek(lexer).type!=TOKEN_COMMA){chain_err=1;break;} lexer_next(lexer);
                        collect_expression(lexer,table,&s1, &(int){sizeof(s1)},1,0,NULL,NULL,NULL,NULL);
                        if(lexer_peek(lexer).type!=TOKEN_RPAREN){chain_err=1;break;} lexer_next(lexer);
                        STMT_APPEND(prefix,prefix_cap,pp,"{ char **__ns=NULL; int __nl=0,__nc=0; _ltng_arr_slice(__td%d,__tl%d,(int)(%s),(int)(%s),&__ns,&__nl,&__nc); for(int _i=0;_i<__tl%d;_i++){if(__td%d[_i])free(__td%d[_i]);} free(__td%d); __td%d=__ns; __tl%d=__nl; __tc%d=__nc; } ",aid,aid,s0,s1,aid,aid,aid,aid,aid,aid,aid);
                        SNPRINTF_FULL(final_expr,"({ char *__r=_ltng_ds_new(\"\"); for(int _i=0;_i<__tl%d;_i++){ if(_i) _ltng_ds_append(&__r,\", \"); _ltng_ds_append(&__r,__td%d[_i]?__td%d[_i]:\"\"); } __r; })",aid,aid,aid);
                        has_scalar = 1; (void)has_scalar;
                    } else if (strcmp(cm.value,"push")==0) {
                        char *ae = (char*)malloc(65536); if(ae) ae[0]=0; VarType at=collect_expression(lexer,table,&ae, &(int){65536},1,0,NULL,NULL,NULL,NULL);
                        if(lexer_peek(lexer).type!=TOKEN_RPAREN){chain_err=1;break;} lexer_next(lexer);
                        const char *fmt=(at==TYPE_INT)?"%lld":(at==TYPE_FLOAT)?"%g":"%s";
                        const char *cast=(at==TYPE_INT)?"(long long)":(at==TYPE_FLOAT)?"(double)":"";
                        STMT_APPEND(prefix,prefix_cap,pp,"char *__v%d_%d = _ltng_ds_fmt(\"%s\",%s(%s)); _ltng_arr_push(&__td%d,&__tl%d,&__tc%d,__v%d_%d); ",aid,step,fmt,cast,ae,aid,aid,aid,aid,step);
                        SNPRINTF_FULL(final_expr,"({ _ltng_retbuf_fmt(\"%%s\",__v%d_%d); })",aid,step);
                    } else if (strcmp(cm.value,"pop")==0) {
                        if(lexer_peek(lexer).type==TOKEN_RPAREN){ lexer_next(lexer); STMT_APPEND(prefix,prefix_cap,pp,"char *__pv%d_%d=_ltng_arr_pop(__td%d,&__tl%d); ",aid,step,aid,aid); SNPRINTF_FULL(final_expr,"__pv%d_%d",aid,step); }
                        else { char *ie = (char*)malloc(65536); if(ie) ie[0]=0; collect_expression(lexer,table,&ie, &(int){65536},1,0,NULL,NULL,NULL,NULL); if(lexer_peek(lexer).type!=TOKEN_RPAREN){chain_err=1;break;} lexer_next(lexer); STMT_APPEND(prefix,prefix_cap,pp,"char *__pv%d_%d=_ltng_arr_pop_index(__td%d,&__tl%d,(int)(%s)); ",aid,step,aid,aid,ie); SNPRINTF_FULL(final_expr,"__pv%d_%d",aid,step); }
                    } else if (strcmp(cm.value,"insert")==0) {
                        char *ie = (char*)malloc(65536); if(ie) ie[0]=0; collect_expression(lexer,table,&ie, &(int){65536},1,1,NULL,NULL,NULL,NULL);
                        if(lexer_peek(lexer).type!=TOKEN_COMMA){chain_err=1;break;} lexer_next(lexer);
                        char *ae = (char*)malloc(65536); if(ae) ae[0]=0; VarType at=collect_expression(lexer,table,&ae, &(int){65536},1,0,NULL,NULL,NULL,NULL);
                        if(lexer_peek(lexer).type!=TOKEN_RPAREN){chain_err=1;break;} lexer_next(lexer);
                        const char *fmt=(at==TYPE_INT)?"%lld":(at==TYPE_FLOAT)?"%g":"%s";
                        const char *cast=(at==TYPE_INT)?"(long long)":(at==TYPE_FLOAT)?"(double)":"";
                        STMT_APPEND(prefix,prefix_cap,pp,"char *__iv%d_%d = _ltng_ds_fmt(\"%s\",%s(%s)); _ltng_arr_insert(&__td%d,&__tl%d,&__tc%d,(int)(%s),__iv%d_%d); ",aid,step,fmt,cast,ae,aid,aid,aid,ie,aid,step);
                        SNPRINTF_FULL(final_expr,"({ _ltng_retbuf_fmt(\"%%s\",__iv%d_%d); })",aid,step);
                    } else if (strcmp(cm.value,"remove")==0) {
                        char *ie = (char*)malloc(65536); if(ie) ie[0]=0; collect_expression(lexer,table,&ie, &(int){65536},1,0,NULL,NULL,NULL,NULL);
                        if(lexer_peek(lexer).type!=TOKEN_RPAREN){chain_err=1;break;} lexer_next(lexer);
                        STMT_APPEND(prefix,prefix_cap,pp,"char *__rv%d_%d=_ltng_arr_pop_index(__td%d,&__tl%d,(int)(%s)); ",aid,step,aid,aid,ie);
                        SNPRINTF_FULL(final_expr,"__rv%d_%d",aid,step);
                    } else if (strcmp(cm.value,"sort")==0) {
                        if(lexer_peek(lexer).type!=TOKEN_RPAREN){chain_err=1;break;} lexer_next(lexer);
                        STMT_APPEND(prefix,prefix_cap,pp,"_ltng_arr_sort(__td%d,__tl%d); ",aid,aid);
                        SNPRINTF_FULL(final_expr,"({ _ltng_retbuf_fmt(\"true\"); })");
                    } else if (strcmp(cm.value,"reverse")==0) {
                        if(lexer_peek(lexer).type!=TOKEN_RPAREN){chain_err=1;break;} lexer_next(lexer);
                        STMT_APPEND(prefix,prefix_cap,pp,"_ltng_arr_reverse(__td%d,__tl%d); ",aid,aid);
                        SNPRINTF_FULL(final_expr,"({ _ltng_retbuf_fmt(\"true\"); })");
                    } else if (strcmp(cm.value,"contains")==0) {
                        char *ae = (char*)malloc(65536); if(ae) ae[0]=0; VarType at=collect_expression(lexer,table,&ae, &(int){65536},1,0,NULL,NULL,NULL,NULL);
                        if(lexer_peek(lexer).type!=TOKEN_RPAREN){chain_err=1;break;} lexer_next(lexer);
                        const char *fmt=(at==TYPE_INT)?"%lld":(at==TYPE_FLOAT)?"%g":"%s";
                        const char *cast=(at==TYPE_INT)?"(long long)":(at==TYPE_FLOAT)?"(double)":"";
                        STMT_APPEND(prefix,prefix_cap,pp,"char *__cv%d_%d = _ltng_ds_fmt(\"%s\",%s(%s)); int __cr%d_%d=_ltng_arr_contains(__td%d,__tl%d,__cv%d_%d); ",aid,step,fmt,cast,ae,aid,step,aid,aid,aid,step);
                        SNPRINTF_FULL(final_expr,"({ _ltng_retbuf_fmt(\"%%s\",__cr%d_%d?\"true\":\"false\"); })",aid,step);
                    } else if (strcmp(cm.value,"index_of")==0) {
                        char *ae = (char*)malloc(65536); if(ae) ae[0]=0; VarType at=collect_expression(lexer,table,&ae, &(int){65536},1,0,NULL,NULL,NULL,NULL);
                        if(lexer_peek(lexer).type!=TOKEN_RPAREN){chain_err=1;break;} lexer_next(lexer);
                        const char *fmt=(at==TYPE_INT)?"%lld":(at==TYPE_FLOAT)?"%g":"%s";
                        const char *cast=(at==TYPE_INT)?"(long long)":(at==TYPE_FLOAT)?"(double)":"";
                        STMT_APPEND(prefix,prefix_cap,pp,"char *__ix%d_%d = _ltng_ds_fmt(\"%s\",%s(%s)); int __ir%d_%d=_ltng_arr_index_of(__td%d,__tl%d,__ix%d_%d); ",aid,step,fmt,cast,ae,aid,step,aid,aid,aid,step);
                        SNPRINTF_FULL(final_expr,"({ _ltng_retbuf_fmt(\"%%d\",__ir%d_%d); })",aid,step);
                    } else { chain_err=1; break; }
                    has_chain=0;
                    if(lexer_peek(lexer).type==TOKEN_DOT){ int sd=lexer->current; lexer_next(lexer); Token nm=lexer_peek(lexer); if((nm.type==TOKEN_IDENT||token_is_name(nm)) && (strcmp(nm.value,"push")==0||strcmp(nm.value,"pop")==0||strcmp(nm.value,"insert")==0||strcmp(nm.value,"remove")==0||strcmp(nm.value,"contains")==0||strcmp(nm.value,"index_of")==0||strcmp(nm.value,"sort")==0||strcmp(nm.value,"reverse")==0||strcmp(nm.value,"copy")==0||strcmp(nm.value,"slice")==0)){ lexer_next(lexer); cm=nm; has_chain=1; } else lexer->current=sd; }
                }
                if (chain_err) { SNPRINTF_FULL(*cexpr, "%s", "\"\""); free(prefix); free(final_expr); }
                else {
                    char *buf = _ltng_cfmt("({ %schar *__fres%d=(char*)(%s); for(int _i=0;_i<__tl%d;_i++){if(__td%d[_i])free(__td%d[_i]);} free(__td%d); _ltng_ah_free(__ah%d); free(__ahsrc%d); __fres%d; })",
                        prefix,aid,final_expr,aid,aid,aid,aid,aid,aid,aid);
                    if(buf){ SNPRINTF_FULL(*cexpr, "%s", buf); free(buf); }
                    else SNPRINTF_FULL(*cexpr, "%s", "");
                    free(prefix); free(final_expr);
                }
                chained = 1;
                continue;
            }
            /* UFCS method chain on expression result: expr.method(args)
               Value-consumed context: keep receiver intact, use return value. */
            if (!((mname.type == TOKEN_IDENT || token_is_name(mname)) &&
                  is_function_name_scoped(table, mname.value) &&
                  lexer->current + 1 < lexer->count &&
                  lexer->tokens[lexer->current + 1].type == TOKEN_LPAREN)) {
                lexer->current = saved_dot;
                break;
            }
            lexer_next(lexer); /* consume method name */
            lexer_next(lexer); /* consume ( */
            /* Build receiver arg from current cexpr string */
            ASTNode *recv = create_node(NODE_PARAM, "", "");
            recv->is_expr = 1;
            recv->var_type = TYPE_STRING;
            { free(recv->value); recv->value = ltng_strdup(*cexpr); }
            ASTNode *call = parse_call_node(lexer, table, mname.value, mname.line, mname.col, recv);
            if (call->is_base_assign) {
                /* Value-consumed context: keep receiver intact, use return value */
                call->is_base_assign = 0;
            }
            build_call_cexpr(call, cexpr, cexpr_cap);
            chained = 1;
            continue;
        }
        break;
    }
    return chained;
}

ASTNode *make_receiver_arg(SymbolTable *table, Token rtok) {
    ASTNode *recv = create_node(NODE_PARAM, "", "");
    char *rm = mangle_name_alloc(rtok.value);
    if (symbol_table_has(table, rm)) {
        recv->is_variable = 1;
        { free(recv->value); recv->value = ltng_strdup(rm); }
        recv->var_type = symbol_table_get(table, rm);
    } else if (symbol_table_has(table, rtok.value)) {
        recv->is_variable = 1;
        { free(recv->value); recv->value = ltng_strdup(rtok.value); }
        recv->var_type = symbol_table_get(table, rtok.value);
    } else {
        recv->is_expr = 1;
        { free(recv->value); recv->value = ltng_strdup(rtok.value); }
        recv->var_type = TYPE_FLOAT;
    }
    return recv;
}

int get_function_passthrough_param(const char *fname);

/* Get the effective result type of a call, applying pass-through specialization
   when feasible. For direct calls to pass-through functions, uses the argument
   type of the passed-through parameter. */
VarType infer_call_result_type(ASTNode *call) {
    if (!call) return TYPE_STRING;
    if (strcmp(call->name, "__ltng_indirect") == 0) return TYPE_NULL; /* dynamic/unknown: preserve legacy numeric behavior */
    /* Check for pass-through specialization */
    int pt = get_function_passthrough_param(call->name);
    if (pt >= 0) {
        /* Find the argument for parameter pt in the expanded args list */
        int idx = 0;
        for (ASTNode *a = call->args; a; a = a->next, idx++) {
            if (idx == pt) {
                VarType at = a->var_type;
                /* Treat unknown/void/null as non-specializable string pass-through */
                if (at == TYPE_INT || at == TYPE_FLOAT || at == TYPE_BOOL || at == TYPE_STRING || at == TYPE_CHAR) {
                    return at;
                }
                /* If arg is a variable, try to get its actual type? var_type should be set */
                return TYPE_STRING;
            }
        }
        /* pt might be in variadic rest args? unlikely for simple passthrough */
        return TYPE_STRING;
    }
    return infer_function_return_type(call->name);
}

int try_parse_sole_call_expr(Lexer *lexer, SymbolTable *table, char **cexpr, int *cexpr_cap, int allow_base_assign, VarType *out_type, char **out_call_cname) {
    (void)cexpr_cap; (void)allow_base_assign;
    Token first = lexer_peek(lexer);
    int saved = lexer->current;
    if (out_type) *out_type = TYPE_STRING;
    if (out_call_cname) *out_call_cname = NULL;
    ASTNode *call = NULL;
    int is_method_form = 0;

    if (first.type == TOKEN_IDENT && !is_callable_ident(table, first.value)) {
        /* method form? receiver.method( */
        lexer_next(lexer);
        if (lexer_peek(lexer).type == TOKEN_DOT) {
            lexer_next(lexer);
            Token mname = lexer_next(lexer);
            if ((mname.type == TOKEN_IDENT || token_is_name(mname)) &&
                lexer_peek(lexer).type == TOKEN_LPAREN &&
                is_function_name_scoped(table, mname.value)) {
                lexer_next(lexer);
                ASTNode *recv = make_receiver_arg(table, first);
                call = parse_call_node(lexer, table, mname.value, mname.line, mname.col, recv);
                is_method_form = 1;
            }
        }
        (void)is_method_form;
    if (!call) { lexer->current = saved; return 0; }
    } else if (first.type == TOKEN_IDENT && is_callable_ident(table, first.value)) {
        Token fname = lexer_next(lexer);
        if (lexer_peek(lexer).type != TOKEN_LPAREN) { lexer->current = saved; return 0; }
        lexer_next(lexer); /* consume ( */
        call = parse_call_node(lexer, table, fname.value, fname.line, fname.col, NULL);
    } else {
        lexer->current = saved;
        return 0;
    }

    if (call->is_base_assign) {
        /* Value-consumed sole-call context: keep receiver intact, use return value */
        call->is_base_assign = 0;
    }

    /* handle chained immediate calls: expr()(args)... */
    char *cur_expr = (char*)malloc(65536); if(cur_expr) cur_expr[0]=0; int cur_expr_cap = 65536;
    build_call_cexpr(call, &cur_expr, &cur_expr_cap);
    int chain = 0;
    while (lexer_peek(lexer).type == TOKEN_LPAREN) {
        chain = 1;
        int cline = lexer_peek(lexer).line;
        int ccol = lexer_peek(lexer).col;
        lexer_next(lexer); /* consume ( */
        /* parse args as indirect call */
        ASTNode *args = NULL, *last = NULL;
        if (lexer_peek(lexer).type != TOKEN_RPAREN) {
            while (1) {
                char *arg_expr = (char*)malloc(65536); if(arg_expr) arg_expr[0]=0;
                VarType at = collect_expression(lexer, table, &arg_expr, &(int){65536}, 1, 1, NULL, NULL, NULL, NULL);
                ASTNode *an = create_node(NODE_PARAM, "", arg_expr);
                an->var_type = at;
                an->is_expr = 1;
                an->v_sl = g_expr_sl; an->v_sc = g_expr_sc; an->v_el = g_expr_el; an->v_ec = g_expr_ec;
                if (!args) args = last = an; else { last->next = an; last = an; }
                Token sep = lexer_peek(lexer);
                if (sep.type == TOKEN_COMMA) { lexer_next(lexer); continue; }
                break;
            }
        }
        if (lexer_peek(lexer).type != TOKEN_RPAREN) { lexer->current = saved; return 0; }
        lexer_next(lexer); /* consume ) */
        int argc = 0; for (ASTNode *a = args; a; a = a->next) argc++;
        char *next_expr = (char*)malloc(65536); if(next_expr) next_expr[0]=0; int next_expr_cap = 65536;
        int p = 0;
        static int _scid = 0; int cid = _scid++;
        STMT_APPEND(next_expr, next_expr_cap, p, "({ char *__ct%d=_ltng_ds_fmt(\"%%s\",%s); char *__ctr%d=__ltng_call_indirect(__ct%d,_ltng_retbuf(),%d,%d,%d",
            cid, cur_expr, cid, cid, cline>0?cline:1, ccol>0?ccol:1, argc);
        /* p grown dynamically */
        for (ASTNode *a = args; a ; a = a->next) {
            if (a->var_type == TYPE_STRING && a->value[0] == '"')
                STMT_APPEND(next_expr, next_expr_cap, p, ",%s", a->value);
            else if (a->is_variable && a->value[0] != '\0')
                STMT_APPEND(next_expr, next_expr_cap, p, ",%s", a->value);
            else if ((a->var_type == TYPE_STRING || a->var_type == TYPE_OBJECT || a->var_type == TYPE_CHAR) && a->value[0] != '\0')
                STMT_APPEND(next_expr, next_expr_cap, p, ",%s", a->value);
            else if (a->var_type == TYPE_BOOL && a->value[0] != '\0')
                STMT_APPEND(next_expr, next_expr_cap, p, ",((%s)?\"true\":\"false\")", a->value);
            else if (a->value[0] == '\0')
                STMT_APPEND(next_expr, next_expr_cap, p, ",\"\"");
            else
                STMT_APPEND(next_expr, next_expr_cap, p, ",_ltng_ad((double)(%s))", a->value);
            /* p grown dynamically */
        }
        STMT_APPEND(next_expr, next_expr_cap, p, "); free(__ct%d); __ctr%d; })", cid, cid);
        if (p < next_expr_cap) next_expr[p] = 0;
        SNPRINTF_FULL(cur_expr, "%s", next_expr);
    }

    Token after = lexer_peek(lexer);
    if (after.type != TOKEN_NEWLINE && after.type != TOKEN_EOF && after.type != TOKEN_RBRACE) {
        lexer->current = saved; return 0;
    }
    SNPRINTF_FULL(*cexpr, "%s", cur_expr);
    if (out_type) {
        if (chain) *out_type = TYPE_STRING; /* chained indirect/dynamic result */
        else if (strcmp(call->name, "__ltng_indirect") == 0) *out_type = TYPE_STRING;
        else *out_type = infer_call_result_type(call);
    }
    if (out_call_cname && !chain) {
        *out_call_cname = ltng_strdup(call->name);
    }
    return 1;
}


int parse_object_literal(Lexer *lexer, SymbolTable *table, const char *mangled, int is_const, int is_assignment) {
    /* assumes '{' consumed. is_assignment: 1 when used in set (not a new declaration). */
    ObjSchema schema;
    memset(&schema, 0, sizeof(schema));
    schema.var = ltng_strdup(""); schema.default_val = ltng_strdup("");

    /* skip leading newlines */
    while (lexer_peek(lexer).type == TOKEN_NEWLINE) lexer_next(lexer);

    while (lexer_peek(lexer).type != TOKEN_RBRACE && lexer_peek(lexer).type != TOKEN_EOF) {
        while (lexer_peek(lexer).type == TOKEN_NEWLINE) lexer_next(lexer);
        if (lexer_peek(lexer).type == TOKEN_RBRACE) break;

        Token fname = lexer_next(lexer);
        if (!token_is_name(fname)) {
            error_state_add(parser_errors, fname.line, fname.col,
                "Expected field name in object literal");
            /* try to recover: skip token */
            if (fname.type == TOKEN_EOF) break;
            continue;
        }

        Token eq = lexer_next(lexer);
        if (eq.type != TOKEN_EQUALS) {
            error_state_add(parser_errors, eq.line, eq.col,
                "Expected '=' after object field name");
            skip_to_newline(lexer);
            while (lexer_peek(lexer).type == TOKEN_NEWLINE) lexer_next(lexer);
            continue;
        }

        Token val = lexer_peek(lexer);
        objschema_ensure(&schema, schema.count + 1);
        ObjField *f = (schema.count < schema.cap) ? &schema.fields[schema.count] : NULL;
        if (f) {
            { free(f->name); f->name = ltng_strdup(fname.value); }
            if (val.type == TOKEN_STRING) {
                lexer_next(lexer);
                f->type = TYPE_STRING;
                { free(f->value); f->value = ltng_strdup(val.value); }
            } else if (val.type == TOKEN_CHAR) {
                lexer_next(lexer);
                f->type = TYPE_STRING;
                { free(f->value); f->value = ltng_strdup(val.value); }
            } else if (val.type == TOKEN_INT_LIT) {
                lexer_next(lexer);
                f->type = TYPE_INT;
                { free(f->value); f->value = ltng_strdup(val.value); }
            } else if (val.type == TOKEN_FLOAT_LIT) {
                lexer_next(lexer);
                f->type = TYPE_FLOAT;
                { free(f->value); f->value = ltng_strdup(val.value); }
            } else if ((val.type == TOKEN_IDENT || token_is_name(val)) && is_bool_value(val.value)) {
                lexer_next(lexer);
                f->type = TYPE_BOOL;
                { const char *_bv = (strcmp(val.value,"true")==0||strcmp(val.value,"t")==0) ? "true" : "false"; free(f->value); f->value = ltng_strdup(_bv); }
            } else if (val.type == TOKEN_MINUS && (lexer->current+1 < lexer->count) &&
                       (lexer->tokens[lexer->current+1].type == TOKEN_INT_LIT || lexer->tokens[lexer->current+1].type == TOKEN_FLOAT_LIT)) {
                lexer_next(lexer); /* - */
                Token num = lexer_next(lexer);
                f->type = (num.type == TOKEN_FLOAT_LIT) ? TYPE_FLOAT : TYPE_INT;
                { char *_s = _ltng_cfmt("-%s", num.value); free(f->value); f->value = _s ? _s : ltng_strdup(""); }
            } else {
                error_state_add(parser_errors, val.line, val.col,
                    "Expected a literal value for object field");
                skip_to_newline(lexer);
                while (lexer_peek(lexer).type == TOKEN_NEWLINE) lexer_next(lexer);
                continue;
            }
            schema.count++;
        }

        /* skip optional comma and newlines */
        if (lexer_peek(lexer).type == TOKEN_COMMA) lexer_next(lexer);
        while (lexer_peek(lexer).type == TOKEN_NEWLINE) lexer_next(lexer);
    }

    if (lexer_peek(lexer).type == TOKEN_RBRACE) lexer_next(lexer);

    const char *cname = symbol_table_get_cname(table, mangled);
    { free(schema.var); schema.var = ltng_strdup(cname); }
    obj_build_all(&schema);
    obj_register(cname, &schema);

    if (!is_assignment) {
        if (is_const) {
            symbol_table_add_locked(table, mangled, TYPE_OBJECT);
            symbol_table_set_const(table, mangled, 1);
        } else {
            symbol_table_add(table, mangled, TYPE_OBJECT);
        }
        cname = symbol_table_get_cname(table, mangled);
    } else {
        symbol_table_update(table, mangled, TYPE_OBJECT);
        cname = symbol_table_get_cname(table, mangled);
    }

    ASTNode *node = create_node((!is_assignment && is_const) ? NODE_CONST : (is_assignment ? NODE_SET : NODE_LET), cname, schema.default_val);
    node->var_type = TYPE_OBJECT;
    node->is_expr = 0;
    emit(node);
    return 1;
}

/* Parse and emit chained array methods (and scoped user-defined methods) on a known array variable.
   Called after copy/slice in let/set contexts, e.g. let b = a.copy().sort().reverse()
   target_src is the original source name (for UDM receiver construction),
   target_rm is the mangled name used for array method nodes. */
void emit_chained_methods_on_array_var(Lexer *lexer, SymbolTable *table,
                                               const char *target_src, const char *target_rm) {
    while (lexer_peek(lexer).type == TOKEN_DOT) {
        int saved_chain = lexer->current;
        lexer_next(lexer); /* consume . */
        Token mname = lexer_peek(lexer);
        if (!(mname.type == TOKEN_IDENT || token_is_name(mname))) {
            lexer->current = saved_chain;
            return;
        }
        lexer_next(lexer); /* consume method name */
        if (lexer_peek(lexer).type != TOKEN_LPAREN) {
            lexer->current = saved_chain;
            return;
        }

        int is_array_method = (strcmp(mname.value, "push") == 0 ||
                               strcmp(mname.value, "pop") == 0 ||
                               strcmp(mname.value, "insert") == 0 ||
                               strcmp(mname.value, "remove") == 0 ||
                               strcmp(mname.value, "sort") == 0 ||
                               strcmp(mname.value, "reverse") == 0 ||
                               strcmp(mname.value, "contains") == 0 ||
                               strcmp(mname.value, "index_of") == 0);

        if (is_array_method) {
            lexer_next(lexer); /* consume ( */
            if (strcmp(mname.value, "push") == 0) {
                char *arg_expr = (char*)malloc(65536); if(arg_expr) arg_expr[0]=0;
                VarType arg_type = collect_expression(lexer, table, &arg_expr, &(int){65536}, 1, 0, NULL, NULL, NULL, NULL);
                if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                    error_state_add(parser_errors, lexer_peek(lexer).line, lexer_peek(lexer).col, "Expected ')' after push argument");
                    skip_to_newline(lexer); return;
                }
                lexer_next(lexer);
                {
                    VarType cur_et = symbol_table_get_array_elem_type(table, target_rm);
                    VarType new_et = arg_type;
                    if (new_et == TYPE_CHAR || new_et == TYPE_OBJECT || new_et == TYPE_ARRAY ||
                        new_et == TYPE_NULL || new_et == TYPE_VOID || new_et == TYPE_STRING)
                        new_et = TYPE_NULL;
                    if (cur_et == TYPE_NULL || new_et == TYPE_NULL) cur_et = TYPE_NULL;
                    else if (cur_et == new_et) { }
                    else if ((cur_et == TYPE_INT && new_et == TYPE_FLOAT) || (cur_et == TYPE_FLOAT && new_et == TYPE_INT)) cur_et = TYPE_FLOAT;
                    else cur_et = TYPE_NULL;
                    symbol_table_set_array_elem_type(table, target_rm, cur_et);
                }
                ASTNode *node = create_node(NODE_ARRAY_PUSH, (char *)target_rm, arg_expr);
                node->var_type = arg_type; node->is_expr = 1; emit(node);
            } else if (strcmp(mname.value, "insert") == 0) {
                char *idx_expr = (char*)malloc(65536); if(idx_expr) idx_expr[0]=0;
                collect_expression(lexer, table, &idx_expr, &(int){65536}, 1, 1, NULL, NULL, NULL, NULL);
                if (lexer_peek(lexer).type != TOKEN_COMMA) {
                    error_state_add(parser_errors, lexer_peek(lexer).line, lexer_peek(lexer).col, "Expected ',' after insert index");
                    skip_to_newline(lexer); return;
                }
                lexer_next(lexer);
                char *arg_expr = (char*)malloc(65536); if(arg_expr) arg_expr[0]=0;
                VarType arg_type = collect_expression(lexer, table, &arg_expr, &(int){65536}, 1, 0, NULL, NULL, NULL, NULL);
                if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                    error_state_add(parser_errors, lexer_peek(lexer).line, lexer_peek(lexer).col, "Expected ')' after insert argument");
                    skip_to_newline(lexer); return;
                }
                lexer_next(lexer);
                {
                    VarType cur_et = symbol_table_get_array_elem_type(table, target_rm);
                    VarType new_et = arg_type;
                    if (new_et == TYPE_CHAR || new_et == TYPE_OBJECT || new_et == TYPE_ARRAY ||
                        new_et == TYPE_NULL || new_et == TYPE_VOID || new_et == TYPE_STRING)
                        new_et = TYPE_NULL;
                    if (cur_et == TYPE_NULL || new_et == TYPE_NULL) cur_et = TYPE_NULL;
                    else if (cur_et == new_et) { }
                    else if ((cur_et == TYPE_INT && new_et == TYPE_FLOAT) || (cur_et == TYPE_FLOAT && new_et == TYPE_INT)) cur_et = TYPE_FLOAT;
                    else cur_et = TYPE_NULL;
                    symbol_table_set_array_elem_type(table, target_rm, cur_et);
                }
                ASTNode *node = create_node(NODE_ARRAY_INSERT, (char *)target_rm, idx_expr);
                { free(node->array_index); node->array_index = ltng_strdup(arg_expr); }
                node->var_type = arg_type; node->is_expr = 1; emit(node);
            } else if (strcmp(mname.value, "remove") == 0) {
                char *idx_expr = (char*)malloc(65536); if(idx_expr) idx_expr[0]=0;
                collect_expression(lexer, table, &idx_expr, &(int){65536}, 1, 0, NULL, NULL, NULL, NULL);
                if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                    error_state_add(parser_errors, lexer_peek(lexer).line, lexer_peek(lexer).col, "Expected ')' after remove index");
                    skip_to_newline(lexer); return;
                }
                lexer_next(lexer);
                ASTNode *node = create_node(NODE_ARRAY_REMOVE, (char *)target_rm, idx_expr);
                node->var_type = TYPE_STRING; node->is_expr = 1; emit(node);
            } else if (strcmp(mname.value, "sort") == 0) {
                if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                    error_state_add(parser_errors, lexer_peek(lexer).line, lexer_peek(lexer).col, "Expected ')' after sort");
                    skip_to_newline(lexer); return;
                }
                lexer_next(lexer);
                ASTNode *node = create_node(NODE_ARRAY_SORT, (char *)target_rm, "");
                node->var_type = TYPE_STRING; node->is_expr = 1; emit(node);
            } else if (strcmp(mname.value, "reverse") == 0) {
                if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                    error_state_add(parser_errors, lexer_peek(lexer).line, lexer_peek(lexer).col, "Expected ')' after reverse");
                    skip_to_newline(lexer); return;
                }
                lexer_next(lexer);
                ASTNode *node = create_node(NODE_ARRAY_REVERSE, (char *)target_rm, "");
                node->var_type = TYPE_STRING; node->is_expr = 1; emit(node);
            } else if (strcmp(mname.value, "pop") == 0) {
                if (lexer_peek(lexer).type == TOKEN_RPAREN) {
                    lexer_next(lexer);
                    ASTNode *node = create_node(NODE_ARRAY_POP, (char *)target_rm, "");
                    node->var_type = TYPE_STRING; node->is_expr = 1; emit(node);
                } else {
                    char *idx_expr = (char*)malloc(65536); if(idx_expr) idx_expr[0]=0;
                    collect_expression(lexer, table, &idx_expr, &(int){65536}, 1, 0, NULL, NULL, NULL, NULL);
                    if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                        error_state_add(parser_errors, lexer_peek(lexer).line, lexer_peek(lexer).col, "Expected ')' after pop index");
                        skip_to_newline(lexer); return;
                    }
                    lexer_next(lexer);
                    ASTNode *node = create_node(NODE_ARRAY_POP, (char *)target_rm, idx_expr);
                    node->var_type = TYPE_STRING; node->is_expr = 1; emit(node);
                }
            } else {
                /* contains / index_of as statements - result discarded */
                if (lexer_peek(lexer).type == TOKEN_RPAREN) {
                    lexer_next(lexer);
                } else {
                    char *dummy = (char*)malloc(65536); if(dummy) dummy[0]=0;
                    collect_expression(lexer, table, &dummy, &(int){65536}, 1, 0, NULL, NULL, NULL, NULL);
                    if (lexer_peek(lexer).type == TOKEN_RPAREN) lexer_next(lexer);
                }
            }
        } else if (is_function_name_scoped(table, mname.value)) {
            /* Chained user-defined method on target */
            lexer_next(lexer); /* consume ( */
            ASTNode *recv = create_node(NODE_PARAM, "", "");
            if (symbol_table_has(table, target_rm)) {
                recv->is_variable = 1;
                { free(recv->value); recv->value = ltng_strdup(target_rm); }
                recv->var_type = symbol_table_get(table, target_rm);
            } else if (symbol_table_has(table, target_src)) {
                recv->is_variable = 1;
                { free(recv->value); recv->value = ltng_strdup(target_src); }
                recv->var_type = symbol_table_get(table, target_src);
            } else {
                recv->is_expr = 1;
                { free(recv->value); recv->value = ltng_strdup(target_src); }
                recv->var_type = TYPE_FLOAT;
            }
            ASTNode *call = parse_call_node(lexer, table, mname.value, mname.line, mname.col, recv);
            emit(call);
        } else {
            lexer->current = saved_chain;
            return;
        }
    }
}


void parse_let(Lexer *lexer, SymbolTable *table) {
    Token let_tok = lexer_next(lexer);

    /* Phase 2: declarations are allowed in control-flow and nested blocks */

    Token name = lexer_next(lexer);
    if (!token_is_name(name)) {
        error_state_add(parser_errors, let_tok.line, let_tok.col + 4,
            "Expected variable name after 'let'");
        skip_to_newline(lexer);
        return;
    }

    if (is_ltng_reserved(name.value)) {
        error_state_add(parser_errors, name.line, name.col,
            "Cannot use reserved word as variable name");
        skip_to_newline(lexer);
        return;
    }

    char *mangled = mangle_name_alloc(name.value);

    if (symbol_table_has_local(table, mangled)) {
        error_state_add(parser_errors, name.line, name.col,
            "Variable already declared in this scope");
        skip_to_newline(lexer);
        return;
    }

    /* Check for bare 'let x' (no assignment) - creates a null variable */
    if (lexer_peek(lexer).type == TOKEN_NEWLINE ||
        lexer_peek(lexer).type == TOKEN_EOF ||
        lexer_peek(lexer).type == TOKEN_RBRACE) {
        symbol_table_add_locked(table, mangled, TYPE_NULL);
        const char *cname = symbol_table_get_cname(table, mangled);
        ASTNode *node = create_node(NODE_LET, cname, "");
        node->var_type = TYPE_NULL;
        node->is_expr = 0;
        emit(node);
        return;
    }

    Token equals = lexer_next(lexer);
    if (equals.type != TOKEN_EQUALS) {
        error_state_add(parser_errors, name.line, name.col,
            "Expected '=' after variable name");
        skip_to_newline(lexer);
        return;
    }

    /* anonymous function expression: let f = function(params) { ... } */
    if (lexer_peek(lexer).type == TOKEN_FUNCTION) {
        Token after_fn = lexer->tokens[lexer->current + 1];
        if (after_fn.type == TOKEN_LPAREN) {
            ASTNode *fn = parse_function(lexer, table, g_enclosing_cname);
            if (fn && g_program_ref) {
                fn->next = g_program_ref->functions;
                g_program_ref->functions = fn;
            }
            symbol_table_add(table, mangled, TYPE_STRING);
            symbol_table_set_func_ref(table, mangled, fn ? fn->name : "", -1);
            if (fn) {
                int pc = 0;
                for (ASTNode *p = fn->params; p; p = p->next) pc++;
                symbol_table_set_func_ref(table, mangled, fn->name, pc);
            }
            const char *cname = symbol_table_get_cname(table, mangled);
            char *fval = (char*)malloc(65536); if(fval) fval[0]=0;
            if (fn && fn->capture_count > 0) {
                char *expr = (char*)malloc(65536); if(expr) expr[0]=0;
                SNPRINTF_FULL(expr, "\"%s\"", fn->name);
                for (int ci = 0; ci < fn->capture_count; ci++) {
                    char *tmp = (char*)malloc(65536); if(tmp) tmp[0]=0;
                    SNPRINTF_FULL(tmp, "_ltng_add_sn(_ltng_add_sn(%s,\"\\001\"),_ltng_ptr_to_str((void*)%s))", expr, fn->captures[ci]);
                    SNPRINTF_FULL(expr, "%s", tmp);
                }
                SNPRINTF_FULL(fval, "%s", expr);
            } else {
                SNPRINTF_FULL(fval, "\"%s\"", fn ? fn->name : "");
            }
            ASTNode *node = create_node(NODE_LET, cname, fval);
            node->var_type = TYPE_STRING;
            node->is_expr = 1;
            emit(node);
            return;
        }
    }

    /* named function reference: let f = someFunc */
    if (lexer_peek(lexer).type == TOKEN_IDENT) {
        Token fnref_tok = lexer_peek(lexer);
        Token after_fnref = (lexer->current + 1 < lexer->count) ? lexer->tokens[lexer->current + 1] : fnref_tok;
        if (after_fnref.type == TOKEN_NEWLINE || after_fnref.type == TOKEN_EOF ||
            after_fnref.type == TOKEN_RBRACE) {
            char *mangled_ref = mangle_name_alloc(fnref_tok.value);
            int is_var = symbol_table_has(table, mangled_ref) || symbol_table_has(table, fnref_tok.value);
            char *ref_cname = NULL; int ref_pc = -1;
            if (!is_var && resolve_func_ref_info(table, fnref_tok.value, &ref_cname, &ref_pc)) {
                lexer_next(lexer);
                symbol_table_add(table, mangled, TYPE_STRING);
                symbol_table_set_func_ref(table, mangled, ref_cname, ref_pc);
                const char *cname = symbol_table_get_cname(table, mangled);
                char *fval = (char*)malloc(65536); if(fval) fval[0]=0;
                SNPRINTF_FULL(fval, "\"%s\"", ref_cname);
                ASTNode *node = create_node(NODE_LET, cname, fval);
                node->var_type = TYPE_STRING;
                node->is_expr = 1;
                emit(node);
                return;
            }
        }
    }

    /* object literal */
    if (lexer_peek(lexer).type == TOKEN_LBRACE) {
        lexer_next(lexer); /* consume { */
        parse_object_literal(lexer, table, mangled, 0, 0);
        return;
    }

    /* array literal: let x = [1,2,3] or [[1,2],[3,4]] */
    if (lexer_peek(lexer).type == TOKEN_LBRACKET) {
        lexer_next(lexer); /* consume [ */
        ASTNode *arr = create_node(NODE_ARRAY_LIT, mangled, "");
        if (!parse_array_literal(lexer, table, arr)) {
            error_state_add(parser_errors, let_tok.line, let_tok.col,
                "Invalid array literal");
            skip_to_newline(lexer);
            return;
        }
        symbol_table_add(table, mangled, TYPE_ARRAY);
        symbol_table_set_array_dims(table, mangled, arr->array_dim_count, arr->array_dims);
        symbol_table_set_array_elem_type(table, mangled, arr->array_elem_type);
        const char *cname = symbol_table_get_cname(table, mangled);
        ASTNode *node = create_node(NODE_LET, cname, arr->value);
        node->var_type = TYPE_ARRAY;
        node->array_dim_count = arr->array_dim_count;
        node->array_dims = malloc(arr->array_dim_count * sizeof(int));
        for (int d = 0; d < arr->array_dim_count; d++) {
            node->array_dims[d] = arr->array_dims[d];
        }
        node->array_elem_count = arr->array_elem_count;
        node->array_elem_type = arr->array_elem_type;
        node->is_expr = 0;
        emit(node);
        return;
    }

    /* let b = a        (array copy/alias)
       let b = a.copy() (array copy via method)
       let b = a.slice(s, e) (array slice via method) */
    {
        Token src_tok = lexer_peek(lexer);
        if (src_tok.type == TOKEN_IDENT && !is_type_keyword(src_tok.value) && !is_bool_value(src_tok.value)) {
            char *sm = mangle_name_alloc(src_tok.value);
            if (symbol_table_has(table, sm) && symbol_table_get(table, sm) == TYPE_ARRAY) {
                Token n2 = lexer->tokens[lexer->current + 1];
                if (n2.type == TOKEN_NEWLINE || n2.type == TOKEN_EOF || n2.type == TOKEN_RBRACE) {
                    /* let b = a - plain alias/copy */
                    lexer_next(lexer); /* consume src ident */
                    const char *scname = symbol_table_get_cname(table, sm);
                    int *src_dims = NULL; int src_dim_count = 0;
                    if (symbol_table_get_array_dims(table, sm, &src_dims, &src_dim_count))
                        symbol_table_set_array_dims(table, mangled, src_dim_count, src_dims);
                    symbol_table_add(table, mangled, TYPE_ARRAY);
                    if (src_dim_count > 0) symbol_table_set_array_dims(table, mangled, src_dim_count, src_dims);
                    symbol_table_set_array_elem_type(table, mangled, symbol_table_get_array_elem_type(table, sm));
                    const char *dcname = symbol_table_get_cname(table, mangled);
                    ASTNode *node = create_node(NODE_LET, dcname, "");
                    node->var_type = TYPE_ARRAY;
                    node->array_elem_type = symbol_table_get_array_elem_type(table, sm);
                    node->is_expr = 0;
                    emit(node);
                    /* Emit copy statement */
                    ASTNode *cp = create_node(NODE_ARRAY_COPY, dcname, (char *)scname);
                    cp->is_expr = 1;
                    emit(cp);
                    return;
                } else if (n2.type == TOKEN_DOT) {
                    Token n3 = lexer->tokens[lexer->current + 2];
                    if ((n3.type == TOKEN_IDENT || token_is_name(n3)) &&
                        strcmp(n3.value, "copy") == 0) {
                        /* let b = a.copy() */
                        lexer_next(lexer); /* consume src ident */
                        lexer_next(lexer); /* consume . */
                        lexer_next(lexer); /* consume copy */
                        if (lexer_peek(lexer).type != TOKEN_LPAREN) {
                            error_state_add(parser_errors, n3.line, n3.col,
                                "Expected '(' after 'copy'");
                            skip_to_newline(lexer);
                            return;
                        }
                        lexer_next(lexer); /* consume ( */
                        if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                            error_state_add(parser_errors, lexer_peek(lexer).line,
                                lexer_peek(lexer).col, "Expected ')' after 'copy()'");
                            skip_to_newline(lexer);
                            return;
                        }
                        lexer_next(lexer); /* consume ) */
                        const char *scname = symbol_table_get_cname(table, sm);
                        int *src_dims2 = NULL; int src_dim_count2 = 0;
                        if (symbol_table_get_array_dims(table, sm, &src_dims2, &src_dim_count2))
                            symbol_table_set_array_dims(table, mangled, src_dim_count2, src_dims2);
                        symbol_table_add(table, mangled, TYPE_ARRAY);
                        if (src_dim_count2 > 0) symbol_table_set_array_dims(table, mangled, src_dim_count2, src_dims2);
                        symbol_table_set_array_elem_type(table, mangled, symbol_table_get_array_elem_type(table, sm));
                        const char *dcname = symbol_table_get_cname(table, mangled);
                        ASTNode *node = create_node(NODE_LET, dcname, "");
                        node->var_type = TYPE_ARRAY;
                        node->array_elem_type = symbol_table_get_array_elem_type(table, sm);
                        node->is_expr = 0;
                        emit(node);
                        ASTNode *cp = create_node(NODE_ARRAY_COPY, dcname, (char *)scname);
                        cp->is_expr = 1;
                        emit(cp);
                        /* Support chaining: let b = a.copy().sort().reverse() */
                        emit_chained_methods_on_array_var(lexer, table, name.value, mangled);
                        return;
                    } else if ((n3.type == TOKEN_IDENT || token_is_name(n3)) &&
                               strcmp(n3.value, "slice") == 0) {
                        /* let b = a.slice(s, e) */
                        lexer_next(lexer); /* consume src ident */
                        lexer_next(lexer); /* consume . */
                        lexer_next(lexer); /* consume slice */
                        if (lexer_peek(lexer).type != TOKEN_LPAREN) {
                            error_state_add(parser_errors, n3.line, n3.col,
                                "Expected '(' after 'slice'");
                            skip_to_newline(lexer);
                            return;
                        }
                        lexer_next(lexer); /* consume ( */
                        char *start_expr = (char*)malloc(65536); if(start_expr) start_expr[0]=0;
                        collect_expression(lexer, table, &start_expr, &(int){65536}, 1, 1, NULL, NULL, NULL, NULL);
                        if (lexer_peek(lexer).type != TOKEN_COMMA) {
                            error_state_add(parser_errors, lexer_peek(lexer).line,
                                lexer_peek(lexer).col, "Expected ',' after slice start");
                            skip_to_newline(lexer);
                            return;
                        }
                        lexer_next(lexer); /* consume , */
                        char *end_expr = (char*)malloc(65536); if(end_expr) end_expr[0]=0;
                        collect_expression(lexer, table, &end_expr, &(int){65536}, 1, 0, NULL, NULL, NULL, NULL);
                        if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                            error_state_add(parser_errors, lexer_peek(lexer).line,
                                lexer_peek(lexer).col, "Expected ')' after slice end");
                            skip_to_newline(lexer);
                            return;
                        }
                        lexer_next(lexer); /* consume ) */
                        const char *scname = symbol_table_get_cname(table, sm);
                        symbol_table_add(table, mangled, TYPE_ARRAY);
                        symbol_table_set_array_elem_type(table, mangled, symbol_table_get_array_elem_type(table, sm));
                        const char *dcname = symbol_table_get_cname(table, mangled);
                        ASTNode *node = create_node(NODE_LET, dcname, "");
                        node->var_type = TYPE_ARRAY;
                        node->array_elem_type = symbol_table_get_array_elem_type(table, sm);
                        node->is_expr = 0;
                        emit(node);
                        /* Build slice range string "start,end" */
                        char *slrange = (char*)malloc(65536); if(slrange) slrange[0]=0;
                        SNPRINTF_FULL(slrange, "%s,%s", start_expr, end_expr);
                        ASTNode *sl = create_node(NODE_ARRAY_SLICE, dcname, (char *)scname);
                        { free(sl->array_index); sl->array_index = ltng_strdup(slrange); }
                        sl->is_expr = 1;
                        emit(sl);
                        /* Support chaining: let b = a.slice(1,3).sort() */
                        emit_chained_methods_on_array_var(lexer, table, name.value, mangled);
                        return;
                    }
                }
            }
        }
    }

    Token next = lexer_peek(lexer);
    VarType vtype;
    Token value = {0};

    Token next2 = lexer->tokens[lexer->current + 1];
    int is_type_conv_call = (next.type == TOKEN_IDENT && is_type_conversion(next.value) &&
                             next2.type == TOKEN_LPAREN);

    int dtype_decl = 0;
    if (next.type == TOKEN_IDENT && is_type_keyword(next.value) && !is_type_conv_call) {
        Token type_tok = lexer_next(lexer);
        vtype = type_keyword_to_vartype(type_tok.value);

        Token peek_val = lexer_peek(lexer);
        if (peek_val.type == TOKEN_NEWLINE || peek_val.type == TOKEN_EOF || peek_val.type == TOKEN_RBRACE) {
            /* Type declaration without value - init to default (kept locked by default) */
            dtype_decl = 1;
            switch (vtype) {
                case TYPE_INT: value.type = TOKEN_INT_LIT; free(value.value); value.value = ltng_strdup("0"); break;
                case TYPE_FLOAT: value.type = TOKEN_INT_LIT; free(value.value); value.value = ltng_strdup("0"); break;
                case TYPE_STRING: value.type = TOKEN_STRING; free(value.value); value.value = ltng_strdup(""); break;
                case TYPE_CHAR: value.type = TOKEN_STRING; free(value.value); value.value = ltng_strdup(""); break;
                case TYPE_BOOL: value.type = TOKEN_INT_LIT; free(value.value); value.value = ltng_strdup("false"); break;
                case TYPE_OBJECT: value.type = TOKEN_STRING; free(value.value); value.value = ltng_strdup(""); break;
                case TYPE_ARRAY: value.type = TOKEN_STRING; free(value.value); value.value = ltng_strdup(""); break;
                case TYPE_NULL:
                default: value.type = TOKEN_INT_LIT; free(value.value); value.value = ltng_strdup("0"); break;
            }
            value.line = let_tok.line;
            value.col = equals.col;
        } else {
            error_state_add(parser_errors, type_tok.line, type_tok.col,
                "Cannot assign a value with a type declaration");
            skip_to_newline(lexer);
            return;
        }
    } else if (is_type_conv_call) {
        /* Type conversion call: let x = int("3.14") */
        /* Detect if sole conversion call: conv(arg) followed by end-of-statement */
        int sole_conv = 0;
        {
            int i = lexer->current + 2; /* after conv and '(' */
            int depth = 1;
            while (i < lexer->count && depth > 0) {
                TokenType tt = lexer->tokens[i].type;
                if (tt == TOKEN_LPAREN) depth++;
                else if (tt == TOKEN_RPAREN) { depth--; if (depth == 0) { i++; break; } }
                else if (tt == TOKEN_NEWLINE || tt == TOKEN_EOF) { depth = -1; break; }
                i++;
            }
            if (depth == 0) {
                /* end-of-statement after conversion call means sole RHS */
                if (i >= lexer->count) sole_conv = 1;
                else {
                    TokenType tt = lexer->tokens[i].type;
                    if (tt == TOKEN_NEWLINE || tt == TOKEN_EOF || tt == TOKEN_RBRACE) sole_conv = 1;
                }
            }
        }
        if (sole_conv) {
            Token conv = lexer_next(lexer); /* conversion name */
            lexer_next(lexer); /* ( */
            /* Collect full argument for complex cases */
            int inner_raw_cap = 65536;
            char *inner_raw = (char*)calloc(1, inner_raw_cap);
            int irpos = 0, depth = 1;
            int arg_start = lexer->current;
            while (lexer->current < lexer->count && depth > 0) {
                Token itok = lexer_peek(lexer);
                if (itok.type == TOKEN_LPAREN) depth++;
                else if (itok.type == TOKEN_RPAREN) { depth--; if (depth == 0) break; }
                else if (itok.type == TOKEN_NEWLINE || itok.type == TOKEN_EOF) break;
                itok = lexer_next(lexer);
                token_append_source(&inner_raw, &irpos, &inner_raw_cap, itok);
            }
            while (irpos > 0 && (inner_raw[irpos-1] == ' ' || inner_raw[irpos-1] == '\t')) irpos--;
            inner_raw[irpos] = '\0';
            int arg_end = lexer->current;
            if (lexer->current < lexer->count && lexer_peek(lexer).type == TOKEN_RPAREN) {
                lexer_next(lexer); /* consume ) */
            }
            Token arg = lexer->tokens[arg_start];
            int arg_tok_count = arg_end - arg_start;
            char *expr = (char*)malloc(65536); if(expr) SNPRINTF_FULL(expr, "%s", "0");
            VarType expr_type = TYPE_STRING;
            int handled = 0;
            /* Simple argument: single literal or identifier */
            if (arg_tok_count == 1 && (arg.type == TOKEN_STRING || arg.type == TOKEN_INT_LIT || arg.type == TOKEN_FLOAT_LIT ||
                 arg.type == TOKEN_CHAR || arg.type == TOKEN_IDENT)) {
                handled = 1;
                if (arg.type == TOKEN_STRING && (strcmp(conv.value, "toint") == 0 || strcmp(conv.value, "int") == 0 || strcmp(conv.value, "tofloat") == 0 || strcmp(conv.value, "float") == 0)) {
                    const char *sv = arg.value; int slen = (int)strlen(sv), ok = 0;
                    if (slen > 0) { int k=0; if(sv[k]=='+'||sv[k]=='-') k++; int digits=0,dot=0,valid=1; for(;k<slen;k++){ if(sv[k]>='0'&&sv[k]<='9') digits=1; else if(sv[k]=='.'&&!dot) dot=1; else {valid=0;break;} } if(valid&&digits) ok=1; }
                    if (!ok) { char *msg = _ltng_cfmt("Invalid conversion: cannot convert \"%s\" with %s", sv, conv.value); error_state_add(parser_errors, conv.line, conv.col, msg ? msg : ""); free(msg); }
                }
                if (strcmp(conv.value, "toint") == 0 || strcmp(conv.value, "int") == 0) {
                    expr_type = TYPE_INT;
                    if (arg.type == TOKEN_STRING) SNPRINTF_FULL(expr, "(long long)atoll(\"%s\")", arg.value);
                    else if (arg.type == TOKEN_INT_LIT || arg.type == TOKEN_FLOAT_LIT) SNPRINTF_FULL(expr, "%lld", (long long)atoll(arg.value));
                    else if (arg.type == TOKEN_CHAR) SNPRINTF_FULL(expr, "%lld", (long long)(unsigned char)arg.value[0]);
                    else if (arg.type == TOKEN_IDENT && is_bool_value(arg.value)) SNPRINTF_FULL(expr, "%d", (strcmp(arg.value,"true")==0||strcmp(arg.value,"t")==0)?1:0);
                    else { char *m = mangle_name_alloc(arg.value); if(symbol_table_has(table,m)) SNPRINTF_FULL(expr,"(long long)atoll(%s)", symbol_table_get_cname(table,m)); else SNPRINTF_FULL(expr,"0"); }
                } else if (strcmp(conv.value, "tofloat") == 0 || strcmp(conv.value, "float") == 0) {
                    expr_type = TYPE_FLOAT;
                    if (arg.type == TOKEN_STRING) SNPRINTF_FULL(expr, "(double)atof(\"%s\")", arg.value);
                    else if (arg.type == TOKEN_INT_LIT || arg.type == TOKEN_FLOAT_LIT) SNPRINTF_FULL(expr, "%.17g", atof(arg.value));
                    else if (arg.type == TOKEN_IDENT && is_bool_value(arg.value)) SNPRINTF_FULL(expr, "%d.0", (strcmp(arg.value,"true")==0||strcmp(arg.value,"t")==0)?1:0);
                    else { char *m = mangle_name_alloc(arg.value); if(symbol_table_has(table,m)) SNPRINTF_FULL(expr,"(double)atof(%s)", symbol_table_get_cname(table,m)); else SNPRINTF_FULL(expr,"0.0"); }
                } else if (strcmp(conv.value, "tobool") == 0 || strcmp(conv.value, "bool") == 0) {
                    if (arg.type == TOKEN_IDENT && !is_bool_value(arg.value)) {
                        handled = 0; /* runtime variable: handle below */
                    } else {
                        expr_type = TYPE_BOOL;
                        if (arg.type == TOKEN_STRING) {
                            int is_false = (arg.value[0]=='\0' || strcmp(arg.value,"0")==0 || strcmp(arg.value,"false")==0 || strcmp(arg.value,"f")==0);
                            SNPRINTF_FULL(expr,"%s", is_false?"false":"true");
                        } else if (arg.type == TOKEN_INT_LIT || arg.type == TOKEN_FLOAT_LIT) {
                            SNPRINTF_FULL(expr,"%s", (atof(arg.value)!=0.0)?"true":"false");
                        } else if (arg.type == TOKEN_IDENT && is_bool_value(arg.value)) {
                            SNPRINTF_FULL(expr,"%s", (strcmp(arg.value,"true")==0||strcmp(arg.value,"t")==0)?"true":"false");
                        } else {
                            SNPRINTF_FULL(expr,"false");
                        }
                    }
                } else if (strcmp(conv.value, "tostring") == 0 || strcmp(conv.value, "string") == 0) {
                    expr_type = TYPE_STRING;
                    if (arg.type == TOKEN_STRING || arg.type == TOKEN_CHAR) {
                        int p = 0;
                        expr[p++] = '"';
                        for (const char *sp = arg.value; *sp && p < (int)65533; sp++) {
                            unsigned char c = (unsigned char)*sp;
                            if (c=='\\') { expr[p++]='\\'; expr[p++]='\\'; }
                            else if (c=='"') { expr[p++]='\\'; expr[p++]='"'; }
                            else if (c=='\n') { expr[p++]='\\'; expr[p++]='n'; }
                            else if (c=='\t') { expr[p++]='\\'; expr[p++]='t'; }
                            else if (c=='\r') { expr[p++]='\\'; expr[p++]='r'; }
                            else expr[p++] = c;
                        }
                        expr[p++] = '"'; expr[p] = '\0';
                    } else if (arg.type == TOKEN_INT_LIT || arg.type == TOKEN_FLOAT_LIT) SNPRINTF_FULL(expr,"\"%s\"", arg.value);
                    else if (arg.type == TOKEN_IDENT && is_bool_value(arg.value)) SNPRINTF_FULL(expr,"\"%s\"", (strcmp(arg.value,"true")==0||strcmp(arg.value,"t")==0)?"true":"false");
                    else { char *m = mangle_name_alloc(arg.value); if(symbol_table_has(table,m)) SNPRINTF_FULL(expr,"%s", symbol_table_get_cname(table,m)); else SNPRINTF_FULL(expr,"\"\""); }
                } else { /* tochar/char */
                    expr_type = TYPE_CHAR;
                    if (arg.type == TOKEN_STRING) {
                        unsigned char ch = (unsigned char)(arg.value[0]?arg.value[0]:0);
                        if (arg.value[0]=='\\' && arg.value[1]) SNPRINTF_FULL(expr,"\"\\%c\"", arg.value[1]);
                        else if (ch=='\n') SNPRINTF_FULL(expr,"\"\\n\"");
                        else if (ch=='\t') SNPRINTF_FULL(expr,"\"\\t\"");
                        else if (ch=='\r') SNPRINTF_FULL(expr,"\"\\r\"");
                        else if (ch=='\\') SNPRINTF_FULL(expr,"\"\\\\\"");
                        else if (ch=='\"') SNPRINTF_FULL(expr,"\"\\\"\"");
                        else SNPRINTF_FULL(expr,"\"%c\"", ch);
                    } else if (arg.type == TOKEN_INT_LIT) {
                        int iv = (int)atoi(arg.value);
                        if (iv=='\n') SNPRINTF_FULL(expr,"\"\\n\"");
                        else if (iv=='\t') SNPRINTF_FULL(expr,"\"\\t\"");
                        else if (iv=='\r') SNPRINTF_FULL(expr,"\"\\r\"");
                        else if (iv=='\\') SNPRINTF_FULL(expr,"\"\\\\\"");
                        else if (iv=='\"') SNPRINTF_FULL(expr,"\"\\\"\"");
                        else if (iv>=32 && iv<=126) SNPRINTF_FULL(expr,"\"%c\"", (char)iv);
                        else SNPRINTF_FULL(expr, "({ char *_tc=_ltng_ds_new(\"\"); int _cv=%d; if(_cv=='\\n') _ltng_ds_set(&_tc,\"\\n\"); else if(_cv=='\\t') _ltng_ds_set(&_tc,\"\\t\"); else if(_cv=='\\r') _ltng_ds_set(&_tc,\"\\r\"); else if(_cv=='\\\\') _ltng_ds_set(&_tc,\"\\\\\"); else if(_cv=='\\\"') _ltng_ds_set(&_tc,\"\\\"\"); else _ltng_ds_setfmt(&_tc,\"%%c\",(char)_cv); char *_r=_ltng_as(_tc); free(_tc); _r; })", iv);
                    } else if (arg.type == TOKEN_CHAR) {
                        /* token value for char literal is the char content (possibly escaped like n or \\) */
                        if (arg.value[0]=='\\' && arg.value[1]) SNPRINTF_FULL(expr,"\"\\%c\"", arg.value[1]);
                        else if (arg.value[0]=='\n') SNPRINTF_FULL(expr,"\"\\n\"");
                        else if (arg.value[0]=='\t') SNPRINTF_FULL(expr,"\"\\t\"");
                        else if (arg.value[0]=='\r') SNPRINTF_FULL(expr,"\"\\r\"");
                        else if (arg.value[0]=='\\') SNPRINTF_FULL(expr,"\"\\\\\"");
                        else if (arg.value[0]=='\"') SNPRINTF_FULL(expr,"\"\\\"\"");
                        else SNPRINTF_FULL(expr,"\"%c\"", arg.value[0]?arg.value[0]:'\0');
                    } else { char *m = mangle_name_alloc(arg.value); if(symbol_table_has(table,m)) { const char *cn=symbol_table_get_cname(table,m); SNPRINTF_FULL(expr,"(_ltng_is_num_str(%s)?((char[]){(char)atoll(%s),0}):((char[]){(%s)[0]? (%s)[0]:0,0}))",cn,cn,cn,cn);} else SNPRINTF_FULL(expr,"\"\""); }
                }
            }
            if (!handled) {
                /* Complex/runtime arg: compile conversion expression via general path, then adapt for typed storage */
                char *conv_expr_src = (char*)malloc(65536); if(conv_expr_src) conv_expr_src[0]=0;
                SNPRINTF_FULL(conv_expr_src, "%s(%s)", conv.value, inner_raw);
                int compiled_cap = 65536;
                char *compiled = (char*)malloc(compiled_cap); if(compiled) compiled[0]=0; compiled[0] = '\0';
                VarType ctype = parse_expression_from_string(conv_expr_src, table, &compiled, &compiled_cap);
                if (compiled[0] == '\0') SNPRINTF_FULL(compiled, "0");
                if (strcmp(conv.value, "toint") == 0 || strcmp(conv.value, "int") == 0) {
                    expr_type = TYPE_INT;
                    if (ctype == TYPE_INT || ctype == TYPE_BOOL) SNPRINTF_FULL(expr,"(long long)(%s)", compiled);
                    else SNPRINTF_FULL(expr,"(long long)atoll(%s)", compiled);
                } else if (strcmp(conv.value, "tofloat") == 0 || strcmp(conv.value, "float") == 0) {
                    expr_type = TYPE_FLOAT;
                    if (ctype == TYPE_FLOAT) SNPRINTF_FULL(expr,"(double)(%s)", compiled);
                    else if (ctype == TYPE_INT || ctype == TYPE_BOOL) SNPRINTF_FULL(expr,"(double)(%s)", compiled);
                    else SNPRINTF_FULL(expr,"(double)atof(%s)", compiled);
                } else if (strcmp(conv.value, "tobool") == 0 || strcmp(conv.value, "bool") == 0) {
                    expr_type = TYPE_BOOL;
                    if (ctype == TYPE_BOOL) SNPRINTF_FULL(expr,"%s", compiled);
                    else SNPRINTF_FULL(expr,"(((%s)[0]!='\\0'&&strcmp(%s,\"0\")!=0&&strcmp(%s,\"false\")!=0&&strcmp(%s,\"f\")!=0)?\"true\":\"false\")", compiled, compiled, compiled, compiled);
                } else if (strcmp(conv.value, "tochar") == 0 || strcmp(conv.value, "char") == 0) {
                    expr_type = TYPE_CHAR;
                    SNPRINTF_FULL(expr,"%s", compiled);
                } else {
                    expr_type = TYPE_STRING;
                    SNPRINTF_FULL(expr,"%s", compiled);
                }
            }
            VarType _let_vt = expr_type;
            if (strcmp(conv.value,"tobool")==0||strcmp(conv.value,"bool")==0) _let_vt = TYPE_BOOL;
            if (strcmp(conv.value,"tochar")==0||strcmp(conv.value,"char")==0) _let_vt = TYPE_CHAR;
            symbol_table_add(table, mangled, _let_vt);
            const char *cname = symbol_table_get_cname(table, mangled);
            ASTNode *node = create_node(NODE_LET, cname, expr);
            node->var_type = _let_vt;
            if (node->var_type == TYPE_INT || node->var_type == TYPE_FLOAT) node->is_expr = 1;
            else if (node->var_type == TYPE_BOOL) {
                node->is_expr = 0; /* "true"/"false" literal strings stored as bool value */
                /* if complex expr produced C ternary string expr, treat as expr copy */
                if (strchr(expr, '(') != NULL || strncmp(expr, "_ltng", 5) == 0) node->is_expr = 1;
            } else {
                /* string/char */
                node->is_expr = 1;
                if (expr[0] == '"' && strchr(expr, '(') == NULL) node->is_expr = 1; /* string literal copied via snprintf in some paths */
            }
            emit(node);
            return;
        }
        char *expr = (char*)malloc(65536); if(expr) expr[0]=0;
        VarType expr_type = collect_expression(lexer, table, &expr, &(int){65536}, 0, 0, NULL, NULL, NULL, NULL);
        symbol_table_add(table, mangled, expr_type);
        const char *cname = symbol_table_get_cname(table, mangled);
        ASTNode *node = create_node(NODE_LET, cname, expr);
        node->var_type = expr_type;
        node->is_expr = 1;
        emit(node);
        return;
    } else {
        Token peek_first = lexer_peek(lexer);
        int use_expr = has_operator_ahead(lexer) || peek_first.type == TOKEN_LPAREN ||
            peek_first.type == TOKEN_MINUS || peek_first.type == TOKEN_BANG;
        if (!use_expr && peek_first.type == TOKEN_IDENT &&
            !is_bool_value(peek_first.value) && !is_type_keyword(peek_first.value)) {
            int saved = lexer->current;
            lexer_next(lexer);
            Token peek_second = lexer_peek(lexer);
            lexer->current = saved;
            if (peek_second.type == TOKEN_NEWLINE || peek_second.type == TOKEN_EOF || peek_second.type == TOKEN_LPAREN || peek_second.type == TOKEN_DOT) {
                use_expr = 1;
            }
        }
        if (use_expr) {
            char *expr = (char*)malloc(65536); if(expr) expr[0]=0;
            /* Skip try_parse_sole_call_expr for array.push/pop - let
               collect_expression handle them as inline C expressions */
            if (!(lexer->current + 2 < lexer->count &&
                  lexer->tokens[lexer->current + 1].type == TOKEN_DOT &&
                  lexer->tokens[lexer->current + 2].type == TOKEN_IDENT &&
                  (strcmp(lexer->tokens[lexer->current + 2].value, "push") == 0 ||
                   strcmp(lexer->tokens[lexer->current + 2].value, "pop") == 0))) {
                VarType call_type = TYPE_STRING;
                char *call_cname = NULL;
                if (try_parse_sole_call_expr(lexer, table, &expr, &(int){65536}, 1, &call_type, &call_cname)) {
                    VarType store_type = (call_type == TYPE_VOID) ? TYPE_STRING : call_type;
                    ObjSchema *ret_sc = NULL;
                    if (store_type == TYPE_OBJECT && call_cname[0]) {
                        FuncInfo *cfi = find_funcinfo_by_cname(call_cname);
                        if (!cfi) {
                            for (int i = 0; i < g_func_count; i++) {
                                if (strcmp(g_funcs[i].src, call_cname) == 0 || strcmp(g_funcs[i].name, call_cname) == 0) { cfi = &g_funcs[i]; break; }
                            }
                        }
                        if (cfi && cfi->return_obj_base[0]) ret_sc = obj_lookup(cfi->return_obj_base);
                        if (!ret_sc) {
                            ASTNode *cfn = find_function_node_by_cname(call_cname);
                            if (cfn && cfn->return_obj_base[0]) ret_sc = obj_lookup(cfn->return_obj_base);
                        }
                    }
                    if (ret_sc) {
                        const char *cname = symbol_table_get_cname(table, mangled);
                        symbol_table_add(table, mangled, TYPE_OBJECT);
                        ObjSchema ds; objschema_deep_copy(&ds, ret_sc);
                        free(ds.var); ds.var = ltng_strdup(cname);
                        obj_build_all(&ds);
                        obj_register(cname, &ds);
                        objschema_free(&ds);
                        ASTNode *node = create_node(NODE_LET, cname, expr);
                        node->var_type = TYPE_OBJECT;
                        node->is_expr = 1;
                        node->is_default = 1;
                        emit(node);
                        /* copy returned object fields from function staging area */
                        for (int i = 0; i < ret_sc->count; i++) {
                            char *df = obj_resolve_field_alloc(cname, ret_sc->fields[i].name);
                            char *stage_base = _ltng_cfmt("__ltng_oret_%s", call_cname);
                            char *sf = obj_resolve_field_alloc(stage_base, ret_sc->fields[i].name);
                            ASTNode *sn = create_node(NODE_SET, df ? df : "", sf ? sf : "");
                            sn->var_type = TYPE_STRING;
                            sn->is_expr = 1;
                            sn->is_variable = 1;
                            emit(sn);
                            free(df); free(stage_base); free(sf);
                        }
                        return;
                    }
                    symbol_table_add(table, mangled, store_type);
                    const char *cname = symbol_table_get_cname(table, mangled);
                    ASTNode *node = create_node(NODE_LET, cname, expr);
                    node->var_type = store_type;
                    node->is_expr = 1;
                    node->is_default = 1; /* sole call result */
                    emit(node);
                    return;
                }
            }
            VarType expr_type = collect_expression(lexer, table, &expr, &(int){65536}, 0, 0, NULL, NULL, NULL, NULL);
            symbol_table_add(table, mangled, expr_type);
            const char *cname = symbol_table_get_cname(table, mangled);
            ASTNode *node = create_node(NODE_LET, cname, expr);
            node->var_type = expr_type;
            node->is_expr = 1;
            emit(node);
            return;
        }
        Token typeof_val = {0};
        if (try_parse_typeof(lexer, table, &typeof_val)) {
            value = typeof_val;
        } else {
            Token peek_check = lexer_peek(lexer);
            if (peek_check.type == TOKEN_IDENT && strcmp(peek_check.value, "typeof") == 0) {
                skip_to_newline(lexer);
                return;
            }
            value = lexer_next(lexer);
            if (value.type != TOKEN_STRING && value.type != TOKEN_INT_LIT &&
                value.type != TOKEN_FLOAT_LIT && value.type != TOKEN_CHAR &&
                !(value.type == TOKEN_IDENT && is_bool_value(value.value))) {
                error_state_add(parser_errors, value.line, value.col,
                    "Expected value after '='");
                skip_to_newline(lexer);
                return;
            }
        }
        if (value.type == TOKEN_CHAR) {
            vtype = TYPE_STRING;
        } else if (value.type == TOKEN_STRING) {
            vtype = TYPE_STRING;
        } else {
            vtype = detect_type(value.value);
        }
    }

    if (dtype_decl) {
        symbol_table_add_locked(table, mangled, vtype);
    } else {
        symbol_table_add(table, mangled, vtype);
    }
    const char *cname = symbol_table_get_cname(table, mangled);

    resolve_string_interpolation(&value, table, value.line, value.col);

    ASTNode *node = create_node(NODE_LET, cname, value.value);
    node->var_type = vtype;

    emit(node);
}

void parse_const(Lexer *lexer, SymbolTable *table) {
    Token const_tok = lexer_next(lexer);

    /* Phase 2: declarations are allowed in control-flow and nested blocks */

    Token name = lexer_next(lexer);
    if (!token_is_name(name)) {
        error_state_add(parser_errors, const_tok.line, const_tok.col + 6,
            "Expected variable name after 'const'");
        skip_to_newline(lexer);
        return;
    }

    if (is_ltng_reserved(name.value)) {
        error_state_add(parser_errors, name.line, name.col,
            "Cannot use reserved word as variable name");
        skip_to_newline(lexer);
        return;
    }

    char *mangled = mangle_name_alloc(name.value);

    if (symbol_table_has_local(table, mangled)) {
        error_state_add(parser_errors, name.line, name.col,
            "Variable already declared in this scope");
        skip_to_newline(lexer);
        return;
    }

    Token equals = lexer_next(lexer);
    if (equals.type != TOKEN_EQUALS) {
        error_state_add(parser_errors, name.line, name.col,
            "Expected '=' after variable name");
        skip_to_newline(lexer);
        return;
    }

    /* anonymous function expression: const f = function(params) { ... } */
    if (lexer_peek(lexer).type == TOKEN_FUNCTION) {
        Token after_fn = lexer->tokens[lexer->current + 1];
        if (after_fn.type == TOKEN_LPAREN) {
            ASTNode *fn = parse_function(lexer, table, g_enclosing_cname);
            if (fn && g_program_ref) {
                fn->next = g_program_ref->functions;
                g_program_ref->functions = fn;
            }
            symbol_table_add_locked(table, mangled, TYPE_STRING);
            symbol_table_set_func_ref(table, mangled, fn ? fn->name : "", -1);
            if (fn) {
                int pc = 0;
                for (ASTNode *p = fn->params; p; p = p->next) pc++;
                symbol_table_set_func_ref(table, mangled, fn->name, pc);
            }
            const char *cname = symbol_table_get_cname(table, mangled);
            char *fval = (char*)malloc(65536); if(fval) fval[0]=0;
            if (fn && fn->capture_count > 0) {
                char *expr = (char*)malloc(65536); if(expr) expr[0]=0;
                SNPRINTF_FULL(expr, "\"%s\"", fn->name);
                for (int ci = 0; ci < fn->capture_count; ci++) {
                    char *tmp = (char*)malloc(65536); if(tmp) tmp[0]=0;
                    SNPRINTF_FULL(tmp, "_ltng_add_sn(_ltng_add_sn(%s,\"\\001\"),_ltng_ptr_to_str((void*)%s))", expr, fn->captures[ci]);
                    SNPRINTF_FULL(expr, "%s", tmp);
                }
                SNPRINTF_FULL(fval, "%s", expr);
            } else {
                SNPRINTF_FULL(fval, "\"%s\"", fn ? fn->name : "");
            }
            ASTNode *node = create_node(NODE_CONST, cname, fval);
            node->var_type = TYPE_STRING;
            node->is_expr = 1;
            emit(node);
            return;
        }
    }

    /* named function reference: const f = someFunc */
    if (lexer_peek(lexer).type == TOKEN_IDENT) {
        Token fnref_tok = lexer_peek(lexer);
        Token after_fnref = (lexer->current + 1 < lexer->count) ? lexer->tokens[lexer->current + 1] : fnref_tok;
        if (after_fnref.type == TOKEN_NEWLINE || after_fnref.type == TOKEN_EOF ||
            after_fnref.type == TOKEN_RBRACE) {
            char *mangled_ref = mangle_name_alloc(fnref_tok.value);
            int is_var = symbol_table_has(table, mangled_ref) || symbol_table_has(table, fnref_tok.value);
            char *ref_cname = NULL; int ref_pc = -1;
            if (!is_var && resolve_func_ref_info(table, fnref_tok.value, &ref_cname, &ref_pc)) {
                lexer_next(lexer);
                symbol_table_add_locked(table, mangled, TYPE_STRING);
                symbol_table_set_func_ref(table, mangled, ref_cname, ref_pc);
                const char *cname = symbol_table_get_cname(table, mangled);
                char *fval = (char*)malloc(65536); if(fval) fval[0]=0;
                SNPRINTF_FULL(fval, "\"%s\"", ref_cname);
                ASTNode *node = create_node(NODE_CONST, cname, fval);
                node->var_type = TYPE_STRING;
                node->is_expr = 1;
                emit(node);
                return;
            }
        }
    }

    /* object literal */
    if (lexer_peek(lexer).type == TOKEN_LBRACE) {
        lexer_next(lexer); /* consume { */
        parse_object_literal(lexer, table, mangled, 1, 0);
        return;
    }

    /* array literal: const x = [1,2,3] or [[1,2],[3,4]] */
    if (lexer_peek(lexer).type == TOKEN_LBRACKET) {
        lexer_next(lexer); /* consume [ */
        ASTNode *arr = create_node(NODE_ARRAY_LIT, mangled, "");
        if (!parse_array_literal(lexer, table, arr)) {
            error_state_add(parser_errors, const_tok.line, const_tok.col,
                "Invalid array literal");
            skip_to_newline(lexer);
            return;
        }
        symbol_table_add_locked(table, mangled, TYPE_ARRAY);
        symbol_table_set_array_dims(table, mangled, arr->array_dim_count, arr->array_dims);
        symbol_table_set_array_elem_type(table, mangled, arr->array_elem_type);
        symbol_table_set_const(table, mangled, 1);
        const char *_cac = symbol_table_get_cname(table, mangled);
        ASTNode *node = create_node(NODE_CONST, _cac, arr->value);
        node->var_type = TYPE_ARRAY;
        node->array_dim_count = arr->array_dim_count;
        node->array_dims = malloc(arr->array_dim_count * sizeof(int));
        for (int d = 0; d < arr->array_dim_count; d++) {
            node->array_dims[d] = arr->array_dims[d];
        }
        node->array_elem_count = arr->array_elem_count;
        node->array_elem_type = arr->array_elem_type;
        node->is_expr = 0;
        emit(node);
        return;
    }

    /* const b = a        (array copy/alias)
       const b = a.copy() (array copy via method)
       const b = a.slice(s, e) (array slice via method) */
    {
        Token src_tok = lexer_peek(lexer);
        if (src_tok.type == TOKEN_IDENT && !is_type_keyword(src_tok.value) && !is_bool_value(src_tok.value)) {
            char *sm = mangle_name_alloc(src_tok.value);
            if (symbol_table_has(table, sm) && symbol_table_get(table, sm) == TYPE_ARRAY) {
                Token n2 = lexer->tokens[lexer->current + 1];
                if (n2.type == TOKEN_NEWLINE || n2.type == TOKEN_EOF || n2.type == TOKEN_RBRACE) {
                    /* const b = a - plain alias/copy */
                    lexer_next(lexer); /* consume src ident */
                    const char *scname = symbol_table_get_cname(table, sm);
                    int *src_dims = NULL; int src_dim_count = 0;
                    if (symbol_table_get_array_dims(table, sm, &src_dims, &src_dim_count))
                        symbol_table_set_array_dims(table, mangled, src_dim_count, src_dims);
                    symbol_table_add_locked(table, mangled, TYPE_ARRAY);
                    symbol_table_set_const(table, mangled, 1);
                    if (src_dim_count > 0) symbol_table_set_array_dims(table, mangled, src_dim_count, src_dims);
                    symbol_table_set_array_elem_type(table, mangled, symbol_table_get_array_elem_type(table, sm));
                    const char *dcname = symbol_table_get_cname(table, mangled);
                    ASTNode *node = create_node(NODE_CONST, dcname, "");
                    node->var_type = TYPE_ARRAY;
                    node->array_elem_type = symbol_table_get_array_elem_type(table, sm);
                    node->is_expr = 0;
                    emit(node);
                    ASTNode *cp = create_node(NODE_ARRAY_COPY, dcname, (char *)scname);
                    cp->is_expr = 1;
                    emit(cp);
                    return;
                } else if (n2.type == TOKEN_DOT) {
                    Token n3 = lexer->tokens[lexer->current + 2];
                    if ((n3.type == TOKEN_IDENT || token_is_name(n3)) &&
                        strcmp(n3.value, "copy") == 0) {
                        /* const b = a.copy() */
                        lexer_next(lexer); /* consume src ident */
                        lexer_next(lexer); /* consume . */
                        lexer_next(lexer); /* consume copy */
                        if (lexer_peek(lexer).type != TOKEN_LPAREN) {
                            error_state_add(parser_errors, n3.line, n3.col, "Expected '(' after 'copy'");
                            skip_to_newline(lexer); return;
                        }
                        lexer_next(lexer); /* consume ( */
                        if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                            error_state_add(parser_errors, lexer_peek(lexer).line, lexer_peek(lexer).col, "Expected ')' after 'copy()'");
                            skip_to_newline(lexer); return;
                        }
                        lexer_next(lexer); /* consume ) */
                        const char *scname = symbol_table_get_cname(table, sm);
                        int *src_dims2 = NULL; int src_dim_count2 = 0;
                        if (symbol_table_get_array_dims(table, sm, &src_dims2, &src_dim_count2))
                            symbol_table_set_array_dims(table, mangled, src_dim_count2, src_dims2);
                        symbol_table_add_locked(table, mangled, TYPE_ARRAY);
                        symbol_table_set_const(table, mangled, 1);
                        if (src_dim_count2 > 0) symbol_table_set_array_dims(table, mangled, src_dim_count2, src_dims2);
                        symbol_table_set_array_elem_type(table, mangled, symbol_table_get_array_elem_type(table, sm));
                        const char *dcname = symbol_table_get_cname(table, mangled);
                        ASTNode *node = create_node(NODE_CONST, dcname, "");
                        node->var_type = TYPE_ARRAY;
                        node->array_elem_type = symbol_table_get_array_elem_type(table, sm);
                        node->is_expr = 0;
                        emit(node);
                        ASTNode *cp = create_node(NODE_ARRAY_COPY, dcname, (char *)scname);
                        cp->is_expr = 1;
                        emit(cp);
                        /* Support chaining: const b = a.copy().sort().reverse() */
                        emit_chained_methods_on_array_var(lexer, table, name.value, mangled);
                        return;
                    } else if ((n3.type == TOKEN_IDENT || token_is_name(n3)) &&
                               strcmp(n3.value, "slice") == 0) {
                        /* const b = a.slice(s, e) */
                        lexer_next(lexer); /* consume src ident */
                        lexer_next(lexer); /* consume . */
                        lexer_next(lexer); /* consume slice */
                        if (lexer_peek(lexer).type != TOKEN_LPAREN) {
                            error_state_add(parser_errors, n3.line, n3.col, "Expected '(' after 'slice'");
                            skip_to_newline(lexer); return;
                        }
                        lexer_next(lexer); /* consume ( */
                        char *start_expr = (char*)malloc(65536); if(start_expr) start_expr[0]=0;
                        collect_expression(lexer, table, &start_expr, &(int){65536}, 1, 1, NULL, NULL, NULL, NULL);
                        if (lexer_peek(lexer).type != TOKEN_COMMA) {
                            error_state_add(parser_errors, lexer_peek(lexer).line, lexer_peek(lexer).col, "Expected ',' after slice start");
                            skip_to_newline(lexer); return;
                        }
                        lexer_next(lexer); /* consume , */
                        char *end_expr = (char*)malloc(65536); if(end_expr) end_expr[0]=0;
                        collect_expression(lexer, table, &end_expr, &(int){65536}, 1, 0, NULL, NULL, NULL, NULL);
                        if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                            error_state_add(parser_errors, lexer_peek(lexer).line, lexer_peek(lexer).col, "Expected ')' after slice end");
                            skip_to_newline(lexer); return;
                        }
                        lexer_next(lexer); /* consume ) */
                        const char *scname = symbol_table_get_cname(table, sm);
                        symbol_table_add_locked(table, mangled, TYPE_ARRAY);
                        symbol_table_set_const(table, mangled, 1);
                        symbol_table_set_array_elem_type(table, mangled, symbol_table_get_array_elem_type(table, sm));
                        const char *dcname = symbol_table_get_cname(table, mangled);
                        ASTNode *node = create_node(NODE_CONST, dcname, "");
                        node->var_type = TYPE_ARRAY;
                        node->array_elem_type = symbol_table_get_array_elem_type(table, sm);
                        node->is_expr = 0;
                        emit(node);
                        char *slrange = (char*)malloc(65536); if(slrange) slrange[0]=0;
                        SNPRINTF_FULL(slrange, "%s,%s", start_expr, end_expr);
                        ASTNode *sl = create_node(NODE_ARRAY_SLICE, dcname, (char *)scname);
                        { free(sl->array_index); sl->array_index = ltng_strdup(slrange); }
                        sl->is_expr = 1;
                        emit(sl);
                        /* Support chaining: const b = a.slice(1,3).sort() */
                        emit_chained_methods_on_array_var(lexer, table, name.value, mangled);
                        return;
                    }
                }
            }
        }
    }

    Token next = lexer_peek(lexer);
    VarType vtype;
    Token value;

    int is_type_conv_call = (next.type == TOKEN_IDENT && is_type_conversion(next.value) &&
                             lexer->current + 1 < lexer->count &&
                             lexer->tokens[lexer->current + 1].type == TOKEN_LPAREN);

    /* const requires a value assignment: const x = <value>
       Type-only declarations like 'const x = int' are not supported. */
    if (next.type == TOKEN_IDENT && is_type_keyword(next.value) && !is_type_conv_call) {
        error_state_add(parser_errors, next.line, next.col,
            "const requires a value assignment, not a type declaration");
        skip_to_newline(lexer);
        return;
    } else if (is_type_conv_call) {
        /* Type conversion call: const x = int("3.14") */
        char *expr = (char*)malloc(65536); if(expr) expr[0]=0;
        VarType expr_type = collect_expression(lexer, table, &expr, &(int){65536}, 0, 0, NULL, NULL, NULL, NULL);
        symbol_table_add_locked(table, mangled, expr_type);
        symbol_table_set_const(table, mangled, 1);
        ASTNode *node = create_node(NODE_CONST, mangled, expr);
        node->var_type = expr_type;
        node->is_expr = 1;
        emit(node);
        return;
    } else {
        Token peek_first = lexer_peek(lexer);
        int use_expr = has_operator_ahead(lexer) || peek_first.type == TOKEN_LPAREN ||
            peek_first.type == TOKEN_MINUS || peek_first.type == TOKEN_BANG;
        if (!use_expr && peek_first.type == TOKEN_IDENT &&
            !is_bool_value(peek_first.value) && !is_type_keyword(peek_first.value)) {
            int saved = lexer->current;
            lexer_next(lexer);
            Token peek_second = lexer_peek(lexer);
            lexer->current = saved;
            if (peek_second.type == TOKEN_NEWLINE || peek_second.type == TOKEN_EOF || peek_second.type == TOKEN_LPAREN || peek_second.type == TOKEN_DOT) {
                use_expr = 1;
            }
        }
        if (use_expr) {
            char *expr = (char*)malloc(65536); if(expr) expr[0]=0;
            /* Skip try_parse_sole_call_expr for array.push/pop - let
               collect_expression handle them as inline C expressions */
            if (!(lexer->current + 2 < lexer->count &&
                  lexer->tokens[lexer->current + 1].type == TOKEN_DOT &&
                  lexer->tokens[lexer->current + 2].type == TOKEN_IDENT &&
                  (strcmp(lexer->tokens[lexer->current + 2].value, "push") == 0 ||
                   strcmp(lexer->tokens[lexer->current + 2].value, "pop") == 0))) {
                VarType call_type = TYPE_STRING;
                char *call_cname = NULL;
                if (try_parse_sole_call_expr(lexer, table, &expr, &(int){65536}, 1, &call_type, &call_cname)) {
                    VarType store_type = (call_type == TYPE_VOID) ? TYPE_STRING : call_type;
                    ObjSchema *ret_sc = NULL;
                    if (store_type == TYPE_OBJECT && call_cname[0]) {
                        FuncInfo *cfi = find_funcinfo_by_cname(call_cname);
                        if (!cfi) {
                            for (int i = 0; i < g_func_count; i++) {
                                if (strcmp(g_funcs[i].src, call_cname) == 0 || strcmp(g_funcs[i].name, call_cname) == 0) { cfi = &g_funcs[i]; break; }
                            }
                        }
                        if (cfi && cfi->return_obj_base[0]) ret_sc = obj_lookup(cfi->return_obj_base);
                        if (!ret_sc) {
                            ASTNode *cfn = find_function_node_by_cname(call_cname);
                            if (cfn && cfn->return_obj_base[0]) ret_sc = obj_lookup(cfn->return_obj_base);
                        }
                    }
                    if (ret_sc) {
                        const char *dcname = symbol_table_get_cname(table, mangled);
                        symbol_table_add_locked(table, mangled, TYPE_OBJECT);
                        symbol_table_set_const(table, mangled, 1);
                        
                        ObjSchema ds; objschema_deep_copy(&ds, ret_sc);
                        free(ds.var); ds.var = ltng_strdup(dcname);
                        obj_build_all(&ds);
                        obj_register(dcname, &ds);
                        objschema_free(&ds);
                        ASTNode *node = create_node(NODE_CONST, dcname, expr);
                        node->var_type = TYPE_OBJECT;
                        node->is_expr = 1;
                        node->is_default = 1;
                        emit(node);
                        for (int i = 0; i < ret_sc->count; i++) {
                            char *df = obj_resolve_field_alloc(dcname, ret_sc->fields[i].name);
                            char *stage_base = _ltng_cfmt("__ltng_oret_%s", call_cname);
                            char *sf = obj_resolve_field_alloc(stage_base, ret_sc->fields[i].name);
                            ASTNode *sn = create_node(NODE_SET, df ? df : "", sf ? sf : "");
                            sn->var_type = TYPE_STRING;
                            sn->is_expr = 1;
                            sn->is_variable = 1;
                            emit(sn);
                            free(df); free(stage_base); free(sf);
                        }
                        return;
                    }
                    symbol_table_add_locked(table, mangled, store_type);
                    symbol_table_set_const(table, mangled, 1);
                    ASTNode *node = create_node(NODE_CONST, mangled, expr);
                    node->var_type = store_type;
                    node->is_expr = 1;
                    node->is_default = 1; /* sole call result */
                    emit(node);
                    return;
                }
            }
            VarType expr_type = collect_expression(lexer, table, &expr, &(int){65536}, 0, 0, NULL, NULL, NULL, NULL);
            symbol_table_add_locked(table, mangled, expr_type);
            symbol_table_set_const(table, mangled, 1);
            ASTNode *node = create_node(NODE_CONST, mangled, expr);
            node->var_type = expr_type;
            node->is_expr = 1;
            emit(node);
            return;
        }
        Token typeof_val = {0};
        if (try_parse_typeof(lexer, table, &typeof_val)) {
            value = typeof_val;
        } else {
            Token peek_check = lexer_peek(lexer);
            if (peek_check.type == TOKEN_IDENT && strcmp(peek_check.value, "typeof") == 0) {
                skip_to_newline(lexer);
                return;
            }
            value = lexer_next(lexer);
            if (value.type != TOKEN_STRING && value.type != TOKEN_INT_LIT &&
                value.type != TOKEN_FLOAT_LIT && value.type != TOKEN_CHAR &&
                !(value.type == TOKEN_IDENT && is_bool_value(value.value))) {
                error_state_add(parser_errors, value.line, value.col,
                    "Expected value after '='");
                skip_to_newline(lexer);
                return;
            }
        }
        if (value.type == TOKEN_CHAR) {
            vtype = TYPE_STRING;
        } else if (value.type == TOKEN_STRING) {
            vtype = TYPE_STRING;
        } else {
            vtype = detect_type(value.value);
        }
    }

    symbol_table_add_locked(table, mangled, vtype);

    symbol_table_set_const(table, mangled, 1);
    const char *cname = symbol_table_get_cname(table, mangled);

    resolve_string_interpolation(&value, table, value.line, value.col);

    ASTNode *node = create_node(NODE_CONST, cname, value.value);
    node->var_type = vtype;

    emit(node);
}


static void _tas_ensure(char **outp, int *cap, int need) {
    if (need < *cap) return;
    int nc = *cap * 2;
    if (nc < need + 1) nc = need + 1;
    char *nb = (char*)realloc(*outp, nc);
    if (!nb) return;
    *outp = nb; *cap = nc;
}
void token_append_source(char **outp, int *pos, int *cap, Token tok) {
    char *out = *outp;
    switch (tok.type) {
        case TOKEN_STRING:
            _tas_ensure(outp, cap, *pos + 2); out = *outp;
            out[(*pos)++] = '"';
            for (const char *p = tok.value; *p; p++) {
                if (*p == '\\' || *p == '"') { _tas_ensure(outp, cap, *pos + 3); out = *outp; out[(*pos)++] = '\\'; out[(*pos)++] = *p; }
                else if (*p == '\n') { _tas_ensure(outp, cap, *pos + 3); out = *outp; out[(*pos)++] = '\\'; out[(*pos)++] = 'n'; }
                else if (*p == '\t') { _tas_ensure(outp, cap, *pos + 3); out = *outp; out[(*pos)++] = '\\'; out[(*pos)++] = 't'; }
                else { _tas_ensure(outp, cap, *pos + 2); out = *outp; out[(*pos)++] = *p; }
            }
            _tas_ensure(outp, cap, *pos + 2); out = *outp;
            out[(*pos)++] = '"';
            out[*pos] = 0;
            break;
        case TOKEN_CHAR:
            STMT_APPEND(*outp, *cap, *pos, "'%s'", tok.value);
            break;
        default:
            if (tok.value[0]) {
                STMT_APPEND(*outp, *cap, *pos, "%s", tok.value);
            } else {
                const char *sym = "";
                switch (tok.type) {
                    case TOKEN_PLUS: sym="+"; break; case TOKEN_MINUS: sym="-"; break;
                    case TOKEN_STAR: sym="*"; break; case TOKEN_SLASH: sym="/"; break;
                    case TOKEN_PERCENT: sym="%"; break; case TOKEN_EQUALS: sym="="; break;
                    case TOKEN_EQ_EQ: sym="=="; break; case TOKEN_BANG_EQ: sym="!="; break;
                    case TOKEN_LT: sym="<"; break; case TOKEN_GT: sym=">"; break;
                    case TOKEN_LT_EQ: sym="<="; break; case TOKEN_GT_EQ: sym=">="; break;
                    case TOKEN_LPAREN: sym="("; break; case TOKEN_RPAREN: sym=")"; break;
                    case TOKEN_LBRACE: sym="{"; break; case TOKEN_RBRACE: sym="}"; break;
                    case TOKEN_COMMA: sym=","; break; case TOKEN_DOT: sym="."; break; case TOKEN_COLON: sym=":"; break;
                    case TOKEN_BANG: sym="!"; break;
                    case TOKEN_AMP_AMP: sym="&&"; break; case TOKEN_PIPE_PIPE: sym="||"; break;
                    case TOKEN_CARET_CARET: sym="^^"; break;
                    case TOKEN_PLUS_PLUS: sym="++"; break; case TOKEN_MINUS_MINUS: sym="--"; break;
                    case TOKEN_PLUS_EQUALS: sym="+="; break; case TOKEN_MINUS_EQUALS: sym="-="; break;
                    case TOKEN_STAR_EQUALS: sym="*="; break; case TOKEN_SLASH_EQUALS: sym="/="; break;
                    case TOKEN_PERCENT_EQUALS: sym="%="; break;
                    default: sym=""; break;
                }
                STMT_APPEND(*outp, *cap, *pos, "%s", sym);
            }
            break;
    }
}

const char *type_name_str(VarType t) {
    switch (t) {
        case TYPE_INT: return "int";
        case TYPE_FLOAT: return "float";
        case TYPE_STRING: return "string";
        case TYPE_CHAR: return "char";
        case TYPE_BOOL: return "bool";
        case TYPE_OBJECT: return "object";
        case TYPE_ARRAY: return "array";
        case TYPE_NULL: return "null";
        case TYPE_VOID: return "void";
    }
    return "unknown";
}

/* Enforce dtype immutability: a variable cannot change its declared type.
   TYPE_NULL is the exception: a null variable can be assigned any type.
   Numeric types (int/float) are compatible for assignment to preserve
   backward-compatible behavior with numeric promotion/coercion. */
int check_set_type_immutable(SymbolTable *table, const char *mangled,
                                     VarType new_type, int line, int col,
                                     Lexer *lexer) {
    VarType existing = symbol_table_get(table, mangled);
    if (existing != new_type) {
        /* Allow numeric compatibility (int <-> float) */
        if ((existing == TYPE_INT || existing == TYPE_FLOAT) &&
            (new_type == TYPE_INT || new_type == TYPE_FLOAT)) {
            return 1;
        }
        /* char and string are closely related in this runtime */
        if ((existing == TYPE_CHAR || existing == TYPE_STRING) &&
            (new_type == TYPE_CHAR || new_type == TYPE_STRING)) {
            return 1;
        }
        /* A locked variable cannot change dtype arbitrarily. */
        if (symbol_table_get_locked(table, mangled)) {
            char *msg = _ltng_cfmt("Cannot change type of locked variable '%s' from %s to %s", mangled, type_name_str(existing), type_name_str(new_type));
            error_state_add(parser_errors, line, col, msg ? msg : "");
            free(msg);
            skip_to_newline(lexer);
            return 0;
        }
        /* An unlocked variable may change its dtype freely via set. */
        return 1;
    }
    return 1;
}

void parse_set(Lexer *lexer, SymbolTable *table) {
    Token set_tok = lexer_next(lexer);

    Token name = lexer_next(lexer);
    if (!token_is_name(name)) {
        error_state_add(parser_errors, set_tok.line, set_tok.col + 4,
            "Expected variable name after 'set'");
        skip_to_newline(lexer);
        return;
    }

    if (is_ltng_reserved(name.value)) {
        error_state_add(parser_errors, name.line, name.col,
            "Cannot use reserved word as variable name");
        skip_to_newline(lexer);
        return;
    }

    char *mangled = mangle_name_alloc(name.value);

    if (!symbol_table_has(table, mangled)) {
        error_state_add(parser_errors, name.line, name.col,
            "Variable not declared");
        skip_to_newline(lexer);
        return;
    }

    if (symbol_table_is_const(table, mangled)) {
        error_state_add(parser_errors, name.line, name.col,
            "Cannot assign to constant");
        skip_to_newline(lexer);
        return;
    }

    const char *cname = symbol_table_get_cname(table, mangled);
    Token op_tok = lexer_next(lexer);
    if (op_tok.type == TOKEN_LBRACKET) {
        /* Array element assignment: set x[i] = expr, set x[i][j] = expr, etc. */
        if (symbol_table_get(table, mangled) != TYPE_ARRAY) {
            error_state_add(parser_errors, name.line, name.col,
                "Cannot index non-array variable");
            skip_to_newline(lexer);
            return;
        }
        /* Parse index expressions */
        int idx_cap_ie = 4, index_count = 0;
        char **index_exprs = (char**)malloc(idx_cap_ie * sizeof(char*));
        if (!index_exprs) { error_state_add(parser_errors, op_tok.line, op_tok.col, "Out of memory"); skip_to_newline(lexer); return; }
        /* op_tok is first '[' already consumed */
        while (1) {
            if (index_count >= idx_cap_ie) {
                int nc = idx_cap_ie * 2;
                char **nb = (char**)realloc(index_exprs, nc * sizeof(char*));
                if (!nb) { for(int _i=0;_i<index_count;_i++) free(index_exprs[_i]); free(index_exprs); skip_to_newline(lexer); return; }
                index_exprs = nb; idx_cap_ie = nc;
            }
            index_exprs[index_count] = (char*)malloc(65536);
            if (!index_exprs[index_count]) { for(int _i=0;_i<index_count;_i++) free(index_exprs[_i]); free(index_exprs); skip_to_newline(lexer); return; }
            index_exprs[index_count][0] = 0;
            collect_expression(lexer, table, &index_exprs[index_count], &(int){65536}, 1, 0, NULL, NULL, NULL, NULL);
            index_count++;
            Token rb = lexer_next(lexer);
            if (rb.type != TOKEN_RBRACKET) {
                error_state_add(parser_errors, rb.line, rb.col,
                    "Expected ']' after array index");
                skip_to_newline(lexer);
                for(int _i=0;_i<index_count;_i++) free(index_exprs[_i]);
                free(index_exprs);
                return;
            }
            Token nxt = lexer_peek(lexer);
            if (nxt.type == TOKEN_LBRACKET) {
                lexer_next(lexer); /* consume '[' */
                op_tok = nxt;
                continue;
            }
            break;
        }
        /* Expect '=' or a compound assignment operator */
        Token eq = lexer_next(lexer);
        char compound_op = 0;
        if (eq.type == TOKEN_PLUS_EQUALS || eq.type == TOKEN_MINUS_EQUALS ||
            eq.type == TOKEN_STAR_EQUALS || eq.type == TOKEN_SLASH_EQUALS ||
            eq.type == TOKEN_PERCENT_EQUALS) {
            compound_op = eq.type == TOKEN_PLUS_EQUALS ? '+' :
                          eq.type == TOKEN_MINUS_EQUALS ? '-' :
                          eq.type == TOKEN_STAR_EQUALS ? '*' :
                          eq.type == TOKEN_SLASH_EQUALS ? '/' : '%';
        } else if (eq.type != TOKEN_EQUALS) {
            error_state_add(parser_errors, eq.line, eq.col,
                "Expected '=' or compound operator after array index in set statement");
            skip_to_newline(lexer);
            for(int _ie=0;_ie<index_count;_ie++) free(index_exprs[_ie]);
            free(index_exprs);
            return;
        }
        /* Parse RHS expression */
        char *rhs_expr = (char*)malloc(65536); if(rhs_expr) rhs_expr[0]=0;
        VarType rhs_type = collect_expression(lexer, table, &rhs_expr, &(int){65536}, 0, 0, NULL, NULL, NULL, NULL);
        VarType assign_type = rhs_type;
        if (compound_op) {
            /* Expand `set arr[i] += rhs` into a normal element assignment using
               the current typed element value. */
            VarType cet = symbol_table_get_array_elem_type(table, mangled);
            int *cdims = NULL; int ccount = 0;
            int chave = symbol_table_get_array_dims(table, mangled, &cdims, &ccount);
            if (!chave || ccount <= 0) { ccount = 1; cdims = malloc(sizeof(int)); if (cdims) cdims[0] = 0; chave = 0; }
            if (ccount != index_count) {
                char *errmsg = _ltng_cfmt("Array '%s' has %d dimension(s) but %d index(es) were given", name.value, ccount, index_count);
                error_state_add(parser_errors, name.line, name.col, errmsg ? errmsg : "");
                free(errmsg);
                if (!chave) free(cdims);
                for(int _ie=0;_ie<index_count;_ie++) free(index_exprs[_ie]);
                free(index_exprs);
                return;
            }
            char *clenname = _ltng_cfmt("%s_len", mangled);
            char *carrname = _ltng_cfmt("%s_data", mangled);
            char *cidx = (char*)malloc(65536); if(cidx)cidx[0]=0; int cidx_cap = 65536;
            int cpos = 0; SNPRINTF_FULL(cidx, "({ "); cpos = (int)strlen(cidx);
            for (int d = 0; d < index_count; d++) {
                if (ccount == 1)
                    STMT_APPEND(cidx, cidx_cap, cpos,
                        "int __ci%d = (int)(%s); __ci%d = __ci%d > 0 ? __ci%d - 1 : __ci%d + (%s); if (__ci%d < 0 || __ci%d >= %s) __ci%d = %s; ",
                        d, index_exprs[d], d, d, d, d, clenname, d, d, clenname, d, clenname);
                else
                    STMT_APPEND(cidx, cidx_cap, cpos,
                        "int __ci%d = (int)(%s); __ci%d = __ci%d > 0 ? __ci%d - 1 : __ci%d + (%d); ",
                        d, index_exprs[d], d, d, d, d, cdims[d]);
            }
            STMT_APPEND(cidx, cidx_cap, cpos, "int __cidx = __ci0");
            for (int d = 1; d < index_count; d++)
                STMT_APPEND(cidx, cidx_cap, cpos, "; __cidx = __cidx * %d + __ci%d", cdims[d], d);
            STMT_APPEND(cidx, cidx_cap, cpos, "; __cidx; })");
            if (!chave) free(cdims);

            char *curexpr=(char*)malloc(65536); char *combined=(char*)malloc(65536); if(curexpr)curexpr[0]=0; if(combined)combined[0]=0;
            if (cet == TYPE_INT) {
                SNPRINTF_FULL(curexpr,
                    "({ const char *__ltng_cur = _ltng_arr_get(%s, %s, %s); atoll(__ltng_cur ? __ltng_cur : \"\"); })",
                    carrname, clenname, cidx);
            } else if (cet == TYPE_FLOAT) {
                SNPRINTF_FULL(curexpr,
                    "({ const char *__ltng_cur = _ltng_arr_get(%s, %s, %s); atof(__ltng_cur ? __ltng_cur : \"\"); })",
                    carrname, clenname, cidx);
            } else if (cet == TYPE_BOOL) {
                SNPRINTF_FULL(curexpr,
                    "({ const char *__ltng_cur = _ltng_arr_get(%s, %s, %s); (__ltng_cur && (__ltng_cur[0]=='t' || __ltng_cur[0]=='T' || __ltng_cur[0]=='1')) ? 1 : 0; })",
                    carrname, clenname, cidx);
            } else {
                SNPRINTF_FULL(curexpr, "_ltng_arr_get(%s, %s, %s)", carrname, clenname, cidx);
            }
            if (cet == TYPE_STRING && compound_op == '+') {
                SNPRINTF_FULL(combined, "_ltng_add_sn(%s, %s)", curexpr, rhs_expr);
            } else if (compound_op == '/' || compound_op == '%') {
                if (cet == TYPE_FLOAT && compound_op == '%')
                    SNPRINTF_FULL(combined,
                        "fmod(%s, (%s) == 0.0 ? 1.0 : (%s))", curexpr, rhs_expr, rhs_expr);
                else if (cet == TYPE_FLOAT)
                    SNPRINTF_FULL(combined,
                        "((%s) == 0.0 ? 0.0 : (%s) / (%s))", rhs_expr, curexpr, rhs_expr);
                else if (compound_op == '%')
                    SNPRINTF_FULL(combined,
                        "((%s) == 0 ? 0 : ((long long)(%s) %% (long long)(%s)))", rhs_expr, curexpr, rhs_expr);
                else
                    SNPRINTF_FULL(combined,
                        "((%s) == 0 ? 0 : ((long long)(%s) / (long long)(%s)))", rhs_expr, curexpr, rhs_expr);
            } else {
                SNPRINTF_FULL(combined, "(%s %c %s)", curexpr, compound_op, rhs_expr);
            }
            SNPRINTF_FULL(rhs_expr, "%s", combined);
        }
        /* Homogeneous arrays keep their inferred element type: a single element
           assignment coerces the value instead of de-typing every element. Dynamic
           (untyped) arrays keep their raw value semantics. (Step 4) */
        {
            VarType cur_et = symbol_table_get_array_elem_type(table, mangled);
            if (cur_et == TYPE_INT || cur_et == TYPE_FLOAT || cur_et == TYPE_BOOL) {
                char *coerced = (char*)malloc(65536); if(coerced) coerced[0]=0;
                int rhs_is_text = (rhs_type == TYPE_STRING || rhs_type == TYPE_CHAR ||
                                   rhs_type == TYPE_OBJECT || rhs_type == TYPE_ARRAY ||
                                   rhs_type == TYPE_NULL || rhs_type == TYPE_VOID);
                if (cur_et == TYPE_INT) {
                    if (rhs_is_text)
                        SNPRINTF_FULL(coerced,
                                 "({ const char *__ltng_rhs = (%s); atoll(__ltng_rhs ? __ltng_rhs : \"\"); })", rhs_expr);
                    else
                        SNPRINTF_FULL(coerced, "(long long)(%s)", rhs_expr);
                } else if (cur_et == TYPE_FLOAT) {
                    if (rhs_is_text)
                        SNPRINTF_FULL(coerced,
                                 "({ const char *__ltng_rhs = (%s); atof(__ltng_rhs ? __ltng_rhs : \"\"); })", rhs_expr);
                    else
                        SNPRINTF_FULL(coerced, "(double)(%s)", rhs_expr);
                } else { /* TYPE_BOOL */
                    if (rhs_is_text)
                        SNPRINTF_FULL(coerced,
                                 "({ const char *__ltng_rhs = (%s); (__ltng_rhs && (__ltng_rhs[0]=='t' || __ltng_rhs[0]=='T' || __ltng_rhs[0]=='1')) ? 1 : 0; })", rhs_expr);
                    else
                        SNPRINTF_FULL(coerced, "((%s) ? 1 : 0)", rhs_expr);
                }
                SNPRINTF_FULL(rhs_expr, "%s", coerced);
                assign_type = cur_et;
            } else {
                /* Dynamic array: preserve numeric/bool formatting when known. */
                if (rhs_type != TYPE_INT && rhs_type != TYPE_FLOAT && rhs_type != TYPE_BOOL)
                    assign_type = TYPE_STRING;
            }
        }

        /* Get array dimensions */
        int dim_count = 0;
        int *dims = NULL;
        int have_dims = symbol_table_get_array_dims(table, mangled, &dims, &dim_count);
        char *lenname = _ltng_cfmt("%s_len", mangled);
        if (!have_dims || dim_count <= 0) {
            dim_count = 1;
            dims = malloc(sizeof(int));
            if (dims) dims[0] = 0;
            have_dims = 0;
        }
        if (index_count != dim_count) {
            char *errmsg = _ltng_cfmt("Array '%s' has %d dimension(s) but %d index(es) were given", name.value, dim_count, index_count);
            error_state_add(parser_errors, name.line, name.col, errmsg ? errmsg : "");
            free(errmsg);
            if (!have_dims) free(dims);
            for(int _ie=0;_ie<index_count;_ie++) free(index_exprs[_ie]);
            free(index_exprs);
            return;
        }

        /* Build index computation C code into array_index field */
        char *idx_code = (char*)malloc(65536); if(idx_code) idx_code[0]=0; int idx_code_cap = 65536;
        int ipos = 0;
        STMT_APPEND(idx_code, idx_code_cap, ipos, "({ ");
        for (int d = 0; d < index_count; d++) {
            if (dim_count == 1) {
                STMT_APPEND(idx_code, idx_code_cap, ipos,
                    "int __i%d = (int)(%s); __i%d = __i%d > 0 ? __i%d - 1 : __i%d + (%s); if (__i%d < 0 || __i%d >= %s) __i%d = %s; ",
                    d, index_exprs[d], d, d, d, d, lenname, d, d, lenname, d, lenname);
            } else {
                STMT_APPEND(idx_code, idx_code_cap, ipos,
                    "int __i%d = (int)(%s); __i%d = __i%d > 0 ? __i%d - 1 : __i%d + (%d); ",
                    d, index_exprs[d], d, d, d, d, dims[d]);
            }
        }
        STMT_APPEND(idx_code, idx_code_cap, ipos, "int __idx = __i0");
        for (int d = 1; d < index_count; d++) {
            STMT_APPEND(idx_code, idx_code_cap, ipos,
                "; __idx = __idx * %d + __i%d", dims[d], d);
        }
        STMT_APPEND(idx_code, idx_code_cap, ipos, "; __idx; })");
        if (!have_dims) free(dims);

        ASTNode *node = create_node(NODE_ARRAY_SET, cname, rhs_expr);
        { free(node->array_index); node->array_index = ltng_strdup(idx_code); }
        node->var_type = assign_type;
        node->is_expr = 1;
        emit(node);
        for(int _ie=0;_ie<index_count;_ie++) free(index_exprs[_ie]);
        free(index_exprs);
        return;
    }
    if (op_tok.type == TOKEN_PLUS_EQUALS || op_tok.type == TOKEN_MINUS_EQUALS ||
        op_tok.type == TOKEN_STAR_EQUALS || op_tok.type == TOKEN_SLASH_EQUALS ||
        op_tok.type == TOKEN_PERCENT_EQUALS) {
        char opchar = op_tok.type == TOKEN_PLUS_EQUALS ? '+' :
                       op_tok.type == TOKEN_MINUS_EQUALS ? '-' :
                       op_tok.type == TOKEN_STAR_EQUALS ? '*' :
                       op_tok.type == TOKEN_SLASH_EQUALS ? '/' : '%';
        /* collect raw RHS source (not C-ified) to avoid double-parse issues */
        int expr_rhs_src_cap = 65536;
        char *expr_rhs_src = (char*)malloc(expr_rhs_src_cap); if(expr_rhs_src) expr_rhs_src[0]=0;
        int rpos = 0;
        while (lexer->current < lexer->count) {
            Token rt = lexer->tokens[lexer->current];
            if (rt.type == TOKEN_NEWLINE || rt.type == TOKEN_EOF) break;
            if (rpos > 0) { STMT_APPEND(expr_rhs_src, expr_rhs_src_cap, rpos, " "); }
            token_append_source(&expr_rhs_src, &rpos, &expr_rhs_src_cap, rt);
            lexer->current++;
        }
        expr_rhs_src[rpos] = '\0';
        char *full_expr = (char*)malloc(65536); if(full_expr) full_expr[0]=0;
        SNPRINTF_FULL(full_expr, "%s %c (%s)", name.value, opchar, expr_rhs_src);
        int expr_out_cap = 65536;
        char *expr_out = (char*)malloc(expr_out_cap); if(expr_out) expr_out[0]=0;
        VarType etype = parse_expression_from_string(full_expr, table, &expr_out, &expr_out_cap);
        if (!check_set_type_immutable(table, mangled, etype, set_tok.line, set_tok.col, lexer))
            return;
        symbol_table_update(table, mangled, etype);
        ASTNode *node = create_node(NODE_SET, cname, expr_out);
        node->var_type = etype;
        node->is_expr = 1;
        emit(node);
        return;
    } else if (op_tok.type != TOKEN_EQUALS) {
        error_state_add(parser_errors, name.line, name.col,
            "Expected '=' after variable name");
        skip_to_newline(lexer);
        return;
    }

    /* Array assignment/copy/slice/literal on existing array variable:
       set a = b, set a = b.copy(), set a = b.slice(s,e), set a = [1,2,3] */
    if (symbol_table_get(table, mangled) == TYPE_ARRAY) {
        if (lexer_peek(lexer).type == TOKEN_LBRACKET) {
            lexer_next(lexer); /* consume [ */
            ASTNode *arr = create_node(NODE_ARRAY_LIT, mangled, "");
            if (!parse_array_literal(lexer, table, arr)) {
                error_state_add(parser_errors, set_tok.line, set_tok.col,
                    "Invalid array literal");
                skip_to_newline(lexer);
                return;
            }
            if (!check_set_type_immutable(table, mangled, TYPE_ARRAY, set_tok.line, set_tok.col, lexer))
                return;
            symbol_table_set_array_dims(table, mangled, arr->array_dim_count, arr->array_dims);
            symbol_table_set_array_elem_type(table, mangled, arr->array_elem_type);
            ASTNode *node = create_node(NODE_SET, cname, arr->value);
            node->var_type = TYPE_ARRAY;
            node->array_elem_type = arr->array_elem_type;
            node->array_dim_count = arr->array_dim_count;
            node->array_dims = malloc(arr->array_dim_count * sizeof(int));
            for (int d = 0; d < arr->array_dim_count; d++) node->array_dims[d] = arr->array_dims[d];
            node->array_elem_count = arr->array_elem_count;
            node->is_expr = 0;
            emit(node);
            return;
        }
        Token src_tok = lexer_peek(lexer);
        if (src_tok.type == TOKEN_IDENT && !is_type_keyword(src_tok.value) && !is_bool_value(src_tok.value)) {
            char *sm = mangle_name_alloc(src_tok.value);
            if (symbol_table_has(table, sm) && symbol_table_get(table, sm) == TYPE_ARRAY) {
                Token n2 = lexer->tokens[lexer->current + 1];
                if (n2.type == TOKEN_NEWLINE || n2.type == TOKEN_EOF || n2.type == TOKEN_RBRACE) {
                    /* set a = b */
                    lexer_next(lexer); /* consume src ident */
                    const char *scname = symbol_table_get_cname(table, sm);
                    int *sd = NULL; int sdc = 0;
                    if (symbol_table_get_array_dims(table, sm, &sd, &sdc))
                        symbol_table_set_array_dims(table, mangled, sdc, sd);
                    symbol_table_set_array_elem_type(table, mangled, symbol_table_get_array_elem_type(table, sm));
                    /* Emit copy into existing array variable (dst). */
                    ASTNode *cp = create_node(NODE_ARRAY_COPY, (char *)cname, (char *)scname);
                    cp->is_expr = 1;
                    emit(cp);
                    return;
                } else if (n2.type == TOKEN_DOT) {
                    Token n3 = lexer->tokens[lexer->current + 2];
                    if ((n3.type == TOKEN_IDENT || token_is_name(n3)) &&
                        strcmp(n3.value, "copy") == 0) {
                        /* set a = b.copy() */
                        lexer_next(lexer); /* consume src ident */
                        lexer_next(lexer); /* consume . */
                        lexer_next(lexer); /* consume copy */
                        if (lexer_peek(lexer).type != TOKEN_LPAREN) {
                            error_state_add(parser_errors, n3.line, n3.col,
                                "Expected '(' after 'copy'");
                            skip_to_newline(lexer);
                            return;
                        }
                        lexer_next(lexer); /* consume ( */
                        if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                            error_state_add(parser_errors, lexer_peek(lexer).line,
                                lexer_peek(lexer).col, "Expected ')' after 'copy()'");
                            skip_to_newline(lexer);
                            return;
                        }
                        lexer_next(lexer); /* consume ) */
                        const char *scname = symbol_table_get_cname(table, sm);
                        int *sd2 = NULL; int sdc2 = 0;
                        if (symbol_table_get_array_dims(table, sm, &sd2, &sdc2))
                            symbol_table_set_array_dims(table, mangled, sdc2, sd2);
                        symbol_table_set_array_elem_type(table, mangled, symbol_table_get_array_elem_type(table, sm));
                        ASTNode *cp = create_node(NODE_ARRAY_COPY, (char *)cname, (char *)scname);
                        cp->is_expr = 1;
                        emit(cp);
                        /* Support chaining: set b = a.copy().sort() */
                        emit_chained_methods_on_array_var(lexer, table, name.value, mangled);
                        return;
                    } else if ((n3.type == TOKEN_IDENT || token_is_name(n3)) &&
                               strcmp(n3.value, "slice") == 0) {
                        /* set a = b.slice(s, e) */
                        lexer_next(lexer); /* consume src ident */
                        lexer_next(lexer); /* consume . */
                        lexer_next(lexer); /* consume slice */
                        if (lexer_peek(lexer).type != TOKEN_LPAREN) {
                            error_state_add(parser_errors, n3.line, n3.col,
                                "Expected '(' after 'slice'");
                            skip_to_newline(lexer);
                            return;
                        }
                        lexer_next(lexer); /* consume ( */
                        char *start_expr = (char*)malloc(65536); if(start_expr) start_expr[0]=0;
                        collect_expression(lexer, table, &start_expr, &(int){65536}, 1, 1, NULL, NULL, NULL, NULL);
                        if (lexer_peek(lexer).type != TOKEN_COMMA) {
                            error_state_add(parser_errors, lexer_peek(lexer).line,
                                lexer_peek(lexer).col, "Expected ',' after slice start");
                            skip_to_newline(lexer);
                            return;
                        }
                        lexer_next(lexer); /* consume , */
                        char *end_expr = (char*)malloc(65536); if(end_expr) end_expr[0]=0;
                        collect_expression(lexer, table, &end_expr, &(int){65536}, 1, 0, NULL, NULL, NULL, NULL);
                        if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                            error_state_add(parser_errors, lexer_peek(lexer).line,
                                lexer_peek(lexer).col, "Expected ')' after slice end");
                            skip_to_newline(lexer);
                            return;
                        }
                        lexer_next(lexer); /* consume ) */
                        const char *scname = symbol_table_get_cname(table, sm);
                        symbol_table_set_array_elem_type(table, mangled, symbol_table_get_array_elem_type(table, sm));
                        char *slrange = (char*)malloc(65536); if(slrange) slrange[0]=0;
                        SNPRINTF_FULL(slrange, "%s,%s", start_expr, end_expr);
                        ASTNode *sl = create_node(NODE_ARRAY_SLICE, (char *)cname, (char *)scname);
                        { free(sl->array_index); sl->array_index = ltng_strdup(slrange); }
                        sl->is_expr = 1;
                        emit(sl);
                        /* Support chaining: set b = a.slice(1,3).sort() */
                        emit_chained_methods_on_array_var(lexer, table, name.value, mangled);
                        return;
                    }
                }
            }
        }
    }

    Token next = lexer_peek(lexer);
    VarType vtype;
    Token value;

    /* anonymous function expression: set f = function(params) { ... } */
    if (next.type == TOKEN_FUNCTION) {
        Token after_fn = lexer->tokens[lexer->current + 1];
        if (after_fn.type == TOKEN_LPAREN) {
            ASTNode *fn = parse_function(lexer, table, g_enclosing_cname);
            if (fn && g_program_ref) {
                fn->next = g_program_ref->functions;
                g_program_ref->functions = fn;
            }
            if (!check_set_type_immutable(table, mangled, TYPE_STRING, set_tok.line, set_tok.col, lexer))
                return;
            symbol_table_update(table, mangled, TYPE_STRING);
            if (fn) {
                int pc = 0;
                for (ASTNode *p = fn->params; p; p = p->next) pc++;
                symbol_table_set_func_ref(table, mangled, fn->name, pc);
            } else {
                symbol_table_set_func_ref(table, mangled, "", -1);
            }
            char *fval = (char*)malloc(65536); if(fval) fval[0]=0;
            if (fn && fn->capture_count > 0) {
                char *expr = (char*)malloc(65536); if(expr) expr[0]=0;
                SNPRINTF_FULL(expr, "\"%s\"", fn->name);
                for (int ci = 0; ci < fn->capture_count; ci++) {
                    char *tmp = (char*)malloc(65536); if(tmp) tmp[0]=0;
                    SNPRINTF_FULL(tmp, "_ltng_add_sn(_ltng_add_sn(%s,\"\\001\"),_ltng_ptr_to_str((void*)%s))", expr, fn->captures[ci]);
                    SNPRINTF_FULL(expr, "%s", tmp);
                }
                SNPRINTF_FULL(fval, "%s", expr);
            } else {
                SNPRINTF_FULL(fval, "\"%s\"", fn ? fn->name : "");
            }
            ASTNode *node = create_node(NODE_SET, cname, fval);
            node->var_type = TYPE_STRING;
            node->is_expr = 1;
            emit(node);
            return;
        }
    }

    /* named function reference: set f = someFunc */
    if (next.type == TOKEN_IDENT) {
        Token after_fnref = (lexer->current + 1 < lexer->count) ? lexer->tokens[lexer->current + 1] : next;
        if (after_fnref.type == TOKEN_NEWLINE || after_fnref.type == TOKEN_EOF ||
            after_fnref.type == TOKEN_RBRACE) {
            char *mangled_ref = mangle_name_alloc(next.value);
            int is_var = symbol_table_has(table, mangled_ref) || symbol_table_has(table, next.value);
            char *ref_cname = NULL; int ref_pc = -1;
            if (!is_var && resolve_func_ref_info(table, next.value, &ref_cname, &ref_pc)) {
                lexer_next(lexer);
                if (!check_set_type_immutable(table, mangled, TYPE_STRING, set_tok.line, set_tok.col, lexer))
                    return;
                symbol_table_update(table, mangled, TYPE_STRING);
                symbol_table_set_func_ref(table, mangled, ref_cname, ref_pc);
                char *fval = (char*)malloc(65536); if(fval) fval[0]=0;
                SNPRINTF_FULL(fval, "\"%s\"", ref_cname);
                ASTNode *node = create_node(NODE_SET, cname, fval);
                node->var_type = TYPE_STRING;
                node->is_expr = 1;
                emit(node);
                return;
            }
        }
    }

    /* object literal assignment */
    if (next.type == TOKEN_LBRACE) {
        lexer_next(lexer); /* consume { */
        if (!check_set_type_immutable(table, mangled, TYPE_OBJECT, set_tok.line, set_tok.col, lexer))
            return;
        parse_object_literal(lexer, table, mangled, 0, 1);
        return;
    }

    Token next2 = lexer->tokens[lexer->current + 1];
    int is_type_conv_call = (next.type == TOKEN_IDENT && is_type_conversion(next.value) &&
                             next2.type == TOKEN_LPAREN);

    if (next.type == TOKEN_IDENT && is_type_keyword(next.value) && !is_type_conv_call) {
        Token type_tok = lexer_next(lexer);
        vtype = type_keyword_to_vartype(type_tok.value);

        Token peek_val = lexer_peek(lexer);
        if (peek_val.type == TOKEN_NEWLINE || peek_val.type == TOKEN_EOF) {
            error_state_add(parser_errors, type_tok.line, type_tok.col,
                "Expected value after type");
            skip_to_newline(lexer);
            return;
        }

        Token peek_val2 = lexer_peek(lexer);
        {
            char *expr = (char*)malloc(65536); if(expr) expr[0]=0;
            /* Skip try_parse_sole_call_expr for array.push/pop */
            if (!(lexer->current + 2 < lexer->count &&
                  lexer->tokens[lexer->current + 1].type == TOKEN_DOT &&
                  lexer->tokens[lexer->current + 2].type == TOKEN_IDENT &&
                  (strcmp(lexer->tokens[lexer->current + 2].value, "push") == 0 ||
                   strcmp(lexer->tokens[lexer->current + 2].value, "pop") == 0))) {
                VarType call_type = TYPE_STRING;
                char *call_cname = NULL;
                if (try_parse_sole_call_expr(lexer, table, &expr, &(int){65536}, 1, &call_type, &call_cname)) {
                    VarType store_type = (call_type == TYPE_VOID) ? TYPE_STRING : call_type;
                    ObjSchema *ret_sc = NULL;
                    if (store_type == TYPE_OBJECT && call_cname[0]) {
                        FuncInfo *cfi = find_funcinfo_by_cname(call_cname);
                        if (!cfi) {
                            for (int i = 0; i < g_func_count; i++) {
                                if (strcmp(g_funcs[i].src, call_cname) == 0 || strcmp(g_funcs[i].name, call_cname) == 0) { cfi = &g_funcs[i]; break; }
                            }
                        }
                        if (cfi && cfi->return_obj_base[0]) ret_sc = obj_lookup(cfi->return_obj_base);
                        if (!ret_sc) {
                            ASTNode *cfn = find_function_node_by_cname(call_cname);
                            if (cfn && cfn->return_obj_base[0]) ret_sc = obj_lookup(cfn->return_obj_base);
                        }
                    }
                    if (ret_sc) {
                        const char *dcname = symbol_table_get_cname(table, mangled);
                        if (!check_set_type_immutable(table, mangled, TYPE_OBJECT, type_tok.line, type_tok.col, lexer)) return;
                        symbol_table_update(table, mangled, TYPE_OBJECT);
                        
                        ObjSchema ds; objschema_deep_copy(&ds, ret_sc);
                        free(ds.var); ds.var = ltng_strdup(dcname);
                        obj_build_all(&ds);
                        obj_register(dcname, &ds);
                        objschema_free(&ds);
                        ASTNode *node = create_node(NODE_SET, dcname, expr);
                        node->var_type = TYPE_OBJECT;
                        node->is_expr = 1;
                        node->is_default = 1;
                        emit(node);
                        for (int i = 0; i < ret_sc->count; i++) {
                            char *df = obj_resolve_field_alloc(dcname, ret_sc->fields[i].name);
                            char *stage_base = _ltng_cfmt("__ltng_oret_%s", call_cname);
                            char *sf = obj_resolve_field_alloc(stage_base, ret_sc->fields[i].name);
                            ASTNode *sn = create_node(NODE_SET, df ? df : "", sf ? sf : "");
                            sn->var_type = TYPE_STRING;
                            sn->is_expr = 1;
                            sn->is_variable = 1;
                            emit(sn);
                            free(df); free(stage_base); free(sf);
                        }
                        return;
                    }
                    vtype = (call_type == TYPE_VOID) ? TYPE_STRING : call_type;
                    if (!check_set_type_immutable(table, mangled, vtype, type_tok.line, type_tok.col, lexer))
                        return;
                    symbol_table_update(table, mangled, vtype);
                    ASTNode *node = create_node(NODE_SET, cname, expr);
                    node->var_type = vtype;
                    node->is_expr = 1;
                    node->is_default = 1; /* sole call result */
                    emit(node);
                    return;
                }
            }
        }
        if (has_operator_ahead(lexer) || peek_val2.type == TOKEN_MINUS ||
            peek_val2.type == TOKEN_BANG || peek_val2.type == TOKEN_LPAREN) {
            char *expr = (char*)malloc(65536); if(expr) expr[0]=0;
            VarType expr_type = collect_expression(lexer, table, &expr, &(int){65536}, 0, 0, NULL, NULL, NULL, NULL);
            vtype = expr_type;
            if (!check_set_type_immutable(table, mangled, vtype, type_tok.line, type_tok.col, lexer))
                return;
            symbol_table_update(table, mangled, vtype);
            ASTNode *node = create_node(NODE_SET, cname, expr);
            node->var_type = vtype;
            node->is_expr = 1;
            emit(node);
            return;
        }

        Token typeof_val = {0};
        if (try_parse_typeof(lexer, table, &typeof_val)) {
            value = typeof_val;
        } else {
            value = lexer_next(lexer);
        }

        if (vtype == TYPE_INT && value.type != TOKEN_INT_LIT) {
            error_state_add(parser_errors, value.line, value.col,
                "Expected integer value for 'int' type");
            skip_to_newline(lexer);
            return;
        }
        if (vtype == TYPE_STRING && value.type != TOKEN_STRING) {
            error_state_add(parser_errors, value.line, value.col,
                "Expected string value for 'string' type");
            skip_to_newline(lexer);
            return;
        }
        if (vtype == TYPE_BOOL && !is_bool_value(value.value) && value.type != TOKEN_STRING) {
            error_state_add(parser_errors, value.line, value.col,
                "Expected bool value (true/t/false/f) for 'bool' type");
            skip_to_newline(lexer);
            return;
        }
    } else if (is_type_conv_call) {
        /* Type conversion call: set x = int("3.14") */
        int sole_conv = 0;
        {
            int i = lexer->current + 2; /* after conv and '(' */
            int depth = 1;
            while (i < lexer->count && depth > 0) {
                TokenType tt = lexer->tokens[i].type;
                if (tt == TOKEN_LPAREN) depth++;
                else if (tt == TOKEN_RPAREN) { depth--; if (depth == 0) { i++; break; } }
                else if (tt == TOKEN_NEWLINE || tt == TOKEN_EOF) { depth = -1; break; }
                i++;
            }
            if (depth == 0) {
                /* end-of-statement after conversion call means sole RHS */
                if (i >= lexer->count) sole_conv = 1;
                else {
                    TokenType tt = lexer->tokens[i].type;
                    if (tt == TOKEN_NEWLINE || tt == TOKEN_EOF || tt == TOKEN_RBRACE) sole_conv = 1;
                }
            }
        }
        if (sole_conv) {
            Token conv = lexer_next(lexer);
            lexer_next(lexer); /* ( */
            /* Collect full argument for complex cases */
            int inner_raw_cap = 65536;
            char *inner_raw = (char*)calloc(1,inner_raw_cap);
            int irpos = 0, depth = 1;
            int arg_start = lexer->current;
            while (lexer->current < lexer->count && depth > 0) {
                Token itok = lexer_peek(lexer);
                if (itok.type == TOKEN_LPAREN) depth++;
                else if (itok.type == TOKEN_RPAREN) { depth--; if (depth == 0) break; }
                else if (itok.type == TOKEN_NEWLINE || itok.type == TOKEN_EOF) break;
                itok = lexer_next(lexer);
                token_append_source(&inner_raw, &irpos, &inner_raw_cap, itok);
            }
            while (irpos > 0 && (inner_raw[irpos-1] == ' ' || inner_raw[irpos-1] == '\t')) irpos--;
            inner_raw[irpos] = '\0';
            int arg_end = lexer->current;
            if (lexer->current < lexer->count && lexer_peek(lexer).type == TOKEN_RPAREN) {
                lexer_next(lexer); /* consume ) */
            }
            Token arg = lexer->tokens[arg_start];
            int arg_tok_count = arg_end - arg_start;
            char *expr = (char*)malloc(65536); if(expr) SNPRINTF_FULL(expr, "%s", "0");
            VarType expr_type = TYPE_STRING;
            int handled = 0;
            if (arg_tok_count == 1 && (arg.type == TOKEN_STRING || arg.type == TOKEN_INT_LIT || arg.type == TOKEN_FLOAT_LIT ||
                 arg.type == TOKEN_CHAR || arg.type == TOKEN_IDENT)) {
                handled = 1;
                if (arg.type == TOKEN_STRING && (strcmp(conv.value, "toint") == 0 || strcmp(conv.value, "int") == 0 || strcmp(conv.value, "tofloat") == 0 || strcmp(conv.value, "float") == 0)) {
                    const char *sv = arg.value; int slen = (int)strlen(sv), ok = 0;
                    if (slen > 0) { int k=0; if(sv[k]=='+'||sv[k]=='-') k++; int digits=0,dot=0,valid=1; for(;k<slen;k++){ if(sv[k]>='0'&&sv[k]<='9') digits=1; else if(sv[k]=='.'&&!dot) dot=1; else {valid=0;break;} } if(valid&&digits) ok=1; }
                    if (!ok) { char *msg = _ltng_cfmt("Invalid conversion: cannot convert \"%s\" with %s", sv, conv.value); error_state_add(parser_errors, conv.line, conv.col, msg ? msg : ""); free(msg); }
                }
                if (strcmp(conv.value, "toint") == 0 || strcmp(conv.value, "int") == 0) {
                    expr_type = TYPE_INT;
                    if (arg.type == TOKEN_STRING) SNPRINTF_FULL(expr, "(long long)atoll(\"%s\")", arg.value);
                    else if (arg.type == TOKEN_INT_LIT || arg.type == TOKEN_FLOAT_LIT) SNPRINTF_FULL(expr, "%lld", (long long)atoll(arg.value));
                    else if (arg.type == TOKEN_CHAR) SNPRINTF_FULL(expr, "%lld", (long long)(unsigned char)arg.value[0]);
                    else if (arg.type == TOKEN_IDENT && is_bool_value(arg.value)) SNPRINTF_FULL(expr, "%d", (strcmp(arg.value,"true")==0||strcmp(arg.value,"t")==0)?1:0);
                    else { char *m = mangle_name_alloc(arg.value); if(symbol_table_has(table,m)) SNPRINTF_FULL(expr,"(long long)atoll(%s)", symbol_table_get_cname(table,m)); else SNPRINTF_FULL(expr,"0"); }
                } else if (strcmp(conv.value, "tofloat") == 0 || strcmp(conv.value, "float") == 0) {
                    expr_type = TYPE_FLOAT;
                    if (arg.type == TOKEN_STRING) SNPRINTF_FULL(expr, "(double)atof(\"%s\")", arg.value);
                    else if (arg.type == TOKEN_INT_LIT || arg.type == TOKEN_FLOAT_LIT) SNPRINTF_FULL(expr, "%.17g", atof(arg.value));
                    else if (arg.type == TOKEN_IDENT && is_bool_value(arg.value)) SNPRINTF_FULL(expr, "%d.0", (strcmp(arg.value,"true")==0||strcmp(arg.value,"t")==0)?1:0);
                    else { char *m = mangle_name_alloc(arg.value); if(symbol_table_has(table,m)) SNPRINTF_FULL(expr,"(double)atof(%s)", symbol_table_get_cname(table,m)); else SNPRINTF_FULL(expr,"0.0"); }
                } else if (strcmp(conv.value, "tobool") == 0 || strcmp(conv.value, "bool") == 0) {
                    if (arg.type == TOKEN_IDENT && !is_bool_value(arg.value)) {
                        handled = 0;
                    } else {
                        expr_type = TYPE_BOOL;
                        if (arg.type == TOKEN_STRING) {
                            int is_false = (arg.value[0]=='\0' || strcmp(arg.value,"0")==0 || strcmp(arg.value,"false")==0 || strcmp(arg.value,"f")==0);
                            SNPRINTF_FULL(expr,"%s", is_false?"false":"true");
                        } else if (arg.type == TOKEN_INT_LIT || arg.type == TOKEN_FLOAT_LIT) {
                            SNPRINTF_FULL(expr,"%s", (atof(arg.value)!=0.0)?"true":"false");
                        } else if (arg.type == TOKEN_IDENT && is_bool_value(arg.value)) {
                            SNPRINTF_FULL(expr,"%s", (strcmp(arg.value,"true")==0||strcmp(arg.value,"t")==0)?"true":"false");
                        } else {
                            SNPRINTF_FULL(expr,"false");
                        }
                    }
                } else if (strcmp(conv.value, "tostring") == 0 || strcmp(conv.value, "string") == 0) {
                    expr_type = TYPE_STRING;
                    if (arg.type == TOKEN_STRING || arg.type == TOKEN_CHAR) {
                        int p = 0;
                        expr[p++] = '"';
                        for (const char *sp = arg.value; *sp && p < (int)65533; sp++) {
                            unsigned char c = (unsigned char)*sp;
                            if (c=='\\') { expr[p++]='\\'; expr[p++]='\\'; }
                            else if (c=='"') { expr[p++]='\\'; expr[p++]='"'; }
                            else if (c=='\n') { expr[p++]='\\'; expr[p++]='n'; }
                            else if (c=='\t') { expr[p++]='\\'; expr[p++]='t'; }
                            else if (c=='\r') { expr[p++]='\\'; expr[p++]='r'; }
                            else expr[p++] = c;
                        }
                        expr[p++] = '"'; expr[p] = '\0';
                    } else if (arg.type == TOKEN_INT_LIT || arg.type == TOKEN_FLOAT_LIT) SNPRINTF_FULL(expr,"\"%s\"", arg.value);
                    else if (arg.type == TOKEN_IDENT && is_bool_value(arg.value)) SNPRINTF_FULL(expr,"\"%s\"", (strcmp(arg.value,"true")==0||strcmp(arg.value,"t")==0)?"true":"false");
                    else { char *m = mangle_name_alloc(arg.value); if(symbol_table_has(table,m)) SNPRINTF_FULL(expr,"%s", symbol_table_get_cname(table,m)); else SNPRINTF_FULL(expr,"\"\""); }
                } else { /* tochar/char */
                    expr_type = TYPE_CHAR;
                    if (arg.type == TOKEN_STRING) {
                        unsigned char ch = (unsigned char)(arg.value[0]?arg.value[0]:0);
                        if (arg.value[0]=='\\' && arg.value[1]) SNPRINTF_FULL(expr,"\"\\%c\"", arg.value[1]);
                        else if (ch=='\n') SNPRINTF_FULL(expr,"\"\\n\"");
                        else if (ch=='\t') SNPRINTF_FULL(expr,"\"\\t\"");
                        else if (ch=='\r') SNPRINTF_FULL(expr,"\"\\r\"");
                        else if (ch=='\\') SNPRINTF_FULL(expr,"\"\\\\\"");
                        else if (ch=='\"') SNPRINTF_FULL(expr,"\"\\\"\"");
                        else SNPRINTF_FULL(expr,"\"%c\"", ch);
                    } else if (arg.type == TOKEN_INT_LIT) {
                        int iv = (int)atoi(arg.value);
                        if (iv=='\n') SNPRINTF_FULL(expr,"\"\\n\"");
                        else if (iv=='\t') SNPRINTF_FULL(expr,"\"\\t\"");
                        else if (iv=='\r') SNPRINTF_FULL(expr,"\"\\r\"");
                        else if (iv=='\\') SNPRINTF_FULL(expr,"\"\\\\\"");
                        else if (iv=='\"') SNPRINTF_FULL(expr,"\"\\\"\"");
                        else if (iv>=32 && iv<=126) SNPRINTF_FULL(expr,"\"%c\"", (char)iv);
                        else SNPRINTF_FULL(expr, "({ char *_tc=_ltng_ds_new(\"\"); int _cv=%d; if(_cv=='\\n') _ltng_ds_set(&_tc,\"\\n\"); else if(_cv=='\\t') _ltng_ds_set(&_tc,\"\\t\"); else if(_cv=='\\r') _ltng_ds_set(&_tc,\"\\r\"); else if(_cv=='\\\\') _ltng_ds_set(&_tc,\"\\\\\"); else if(_cv=='\\\"') _ltng_ds_set(&_tc,\"\\\"\"); else _ltng_ds_setfmt(&_tc,\"%%c\",(char)_cv); char *_r=_ltng_as(_tc); free(_tc); _r; })", iv);
                    } else if (arg.type == TOKEN_CHAR) {
                        /* token value for char literal is the char content (possibly escaped like n or \\) */
                        if (arg.value[0]=='\\' && arg.value[1]) SNPRINTF_FULL(expr,"\"\\%c\"", arg.value[1]);
                        else if (arg.value[0]=='\n') SNPRINTF_FULL(expr,"\"\\n\"");
                        else if (arg.value[0]=='\t') SNPRINTF_FULL(expr,"\"\\t\"");
                        else if (arg.value[0]=='\r') SNPRINTF_FULL(expr,"\"\\r\"");
                        else if (arg.value[0]=='\\') SNPRINTF_FULL(expr,"\"\\\\\"");
                        else if (arg.value[0]=='\"') SNPRINTF_FULL(expr,"\"\\\"\"");
                        else SNPRINTF_FULL(expr,"\"%c\"", arg.value[0]?arg.value[0]:'\0');
                    } else { char *m = mangle_name_alloc(arg.value); if(symbol_table_has(table,m)) { const char *cn=symbol_table_get_cname(table,m); SNPRINTF_FULL(expr,"(_ltng_is_num_str(%s)?((char[]){(char)atoll(%s),0}):((char[]){(%s)[0]? (%s)[0]:0,0}))",cn,cn,cn,cn);} else SNPRINTF_FULL(expr,"\"\""); }
                }
            }
            if (!handled) {
                char *conv_expr_src = (char*)malloc(65536); if(conv_expr_src) conv_expr_src[0]=0;
                SNPRINTF_FULL(conv_expr_src, "%s(%s)", conv.value, inner_raw);
                int compiled_cap = 65536;
                char *compiled = (char*)malloc(compiled_cap); if(compiled) compiled[0]=0; compiled[0] = '\0';
                VarType ctype = parse_expression_from_string(conv_expr_src, table, &compiled, &compiled_cap);
                if (compiled[0] == '\0') SNPRINTF_FULL(compiled, "0");
                if (strcmp(conv.value, "toint") == 0 || strcmp(conv.value, "int") == 0) {
                    expr_type = TYPE_INT;
                    if (ctype == TYPE_INT || ctype == TYPE_BOOL) SNPRINTF_FULL(expr,"(long long)(%s)", compiled);
                    else SNPRINTF_FULL(expr,"(long long)atoll(%s)", compiled);
                } else if (strcmp(conv.value, "tofloat") == 0 || strcmp(conv.value, "float") == 0) {
                    expr_type = TYPE_FLOAT;
                    if (ctype == TYPE_FLOAT) SNPRINTF_FULL(expr,"(double)(%s)", compiled);
                    else if (ctype == TYPE_INT || ctype == TYPE_BOOL) SNPRINTF_FULL(expr,"(double)(%s)", compiled);
                    else SNPRINTF_FULL(expr,"(double)atof(%s)", compiled);
                } else if (strcmp(conv.value, "tobool") == 0 || strcmp(conv.value, "bool") == 0) {
                    expr_type = TYPE_BOOL;
                    if (ctype == TYPE_BOOL) SNPRINTF_FULL(expr,"%s", compiled);
                    else SNPRINTF_FULL(expr,"(((%s)[0]!='\\0'&&strcmp(%s,\"0\")!=0&&strcmp(%s,\"false\")!=0&&strcmp(%s,\"f\")!=0)?\"true\":\"false\")", compiled, compiled, compiled, compiled);
                } else if (strcmp(conv.value, "tochar") == 0 || strcmp(conv.value, "char") == 0) {
                    expr_type = TYPE_CHAR;
                    SNPRINTF_FULL(expr,"%s", compiled);
                } else {
                    expr_type = TYPE_STRING;
                    SNPRINTF_FULL(expr,"%s", compiled);
                }
            }
            VarType _set_vt = expr_type;
            if (strcmp(conv.value,"tobool")==0||strcmp(conv.value,"bool")==0) _set_vt = TYPE_BOOL;
            if (strcmp(conv.value,"tochar")==0||strcmp(conv.value,"char")==0) _set_vt = TYPE_CHAR;
            if (!check_set_type_immutable(table, mangled, _set_vt, set_tok.line, set_tok.col, lexer))
                return;
            symbol_table_update(table, mangled, _set_vt);
            ASTNode *node = create_node(NODE_SET, cname, expr);
            node->var_type = _set_vt;
            if (node->var_type == TYPE_INT || node->var_type == TYPE_FLOAT) node->is_expr = 1;
            else if (node->var_type == TYPE_BOOL) {
                node->is_expr = 0;
                if (strchr(expr, '(') != NULL || strncmp(expr, "_ltng", 5) == 0) node->is_expr = 1;
            } else {
                node->is_expr = 1;
            }
            emit(node);
            return;
        }
        char *expr = (char*)malloc(65536); if(expr) expr[0]=0;
        VarType expr_type = collect_expression(lexer, table, &expr, &(int){65536}, 0, 0, NULL, NULL, NULL, NULL);
        if (!check_set_type_immutable(table, mangled, expr_type, set_tok.line, set_tok.col, lexer))
            return;
        symbol_table_update(table, mangled, expr_type);
        ASTNode *node = create_node(NODE_SET, cname, expr);
        node->var_type = expr_type;
        node->is_expr = 1;
        emit(node);
        return;
    } else {
        Token peek_first = lexer_peek(lexer);
        int use_expr = has_operator_ahead(lexer) || peek_first.type == TOKEN_LPAREN ||
            peek_first.type == TOKEN_MINUS || peek_first.type == TOKEN_BANG;
        if (!use_expr && peek_first.type == TOKEN_IDENT &&
            !is_bool_value(peek_first.value) && !is_type_keyword(peek_first.value)) {
            int saved = lexer->current;
            lexer_next(lexer);
            Token peek_second = lexer_peek(lexer);
            lexer->current = saved;
            if (peek_second.type == TOKEN_NEWLINE || peek_second.type == TOKEN_EOF || peek_second.type == TOKEN_LPAREN || peek_second.type == TOKEN_DOT) {
                use_expr = 1;
            }
        }
        if (use_expr) {
            char *expr = (char*)malloc(65536); if(expr) expr[0]=0;
            /* Skip try_parse_sole_call_expr for array.push/pop - let
               collect_expression handle them as inline C expressions */
            if (!(lexer->current + 2 < lexer->count &&
                  lexer->tokens[lexer->current + 1].type == TOKEN_DOT &&
                  lexer->tokens[lexer->current + 2].type == TOKEN_IDENT &&
                  (strcmp(lexer->tokens[lexer->current + 2].value, "push") == 0 ||
                   strcmp(lexer->tokens[lexer->current + 2].value, "pop") == 0))) {
                VarType call_type = TYPE_STRING;
                char *call_cname = NULL;
                if (try_parse_sole_call_expr(lexer, table, &expr, &(int){65536}, 1, &call_type, &call_cname)) {
                    VarType store_type = (call_type == TYPE_VOID) ? TYPE_STRING : call_type;
                    ObjSchema *ret_sc = NULL;
                    if (store_type == TYPE_OBJECT && call_cname[0]) {
                        FuncInfo *cfi = find_funcinfo_by_cname(call_cname);
                        if (!cfi) {
                            for (int i = 0; i < g_func_count; i++) {
                                if (strcmp(g_funcs[i].src, call_cname) == 0 || strcmp(g_funcs[i].name, call_cname) == 0) { cfi = &g_funcs[i]; break; }
                            }
                        }
                        if (cfi && cfi->return_obj_base[0]) ret_sc = obj_lookup(cfi->return_obj_base);
                        if (!ret_sc) {
                            ASTNode *cfn = find_function_node_by_cname(call_cname);
                            if (cfn && cfn->return_obj_base[0]) ret_sc = obj_lookup(cfn->return_obj_base);
                        }
                    }
                    if (ret_sc) {
                        const char *dcname = symbol_table_get_cname(table, mangled);
                        if (!check_set_type_immutable(table, mangled, TYPE_OBJECT, set_tok.line, set_tok.col, lexer)) return;
                        symbol_table_update(table, mangled, TYPE_OBJECT);
                        
                        ObjSchema ds; objschema_deep_copy(&ds, ret_sc);
                        free(ds.var); ds.var = ltng_strdup(dcname);
                        obj_build_all(&ds);
                        obj_register(dcname, &ds);
                        objschema_free(&ds);
                        ASTNode *node = create_node(NODE_SET, dcname, expr);
                        node->var_type = TYPE_OBJECT;
                        node->is_expr = 1;
                        node->is_default = 1;
                        emit(node);
                        for (int i = 0; i < ret_sc->count; i++) {
                            char *df = obj_resolve_field_alloc(dcname, ret_sc->fields[i].name);
                            char *stage_base = _ltng_cfmt("__ltng_oret_%s", call_cname);
                            char *sf = obj_resolve_field_alloc(stage_base, ret_sc->fields[i].name);
                            ASTNode *sn = create_node(NODE_SET, df ? df : "", sf ? sf : "");
                            sn->var_type = TYPE_STRING;
                            sn->is_expr = 1;
                            sn->is_variable = 1;
                            emit(sn);
                            free(df); free(stage_base); free(sf);
                        }
                        return;
                    }
                    vtype = (call_type == TYPE_VOID) ? TYPE_STRING : call_type;
                    if (!check_set_type_immutable(table, mangled, vtype, set_tok.line, set_tok.col, lexer))
                        return;
                    symbol_table_update(table, mangled, vtype);
                    ASTNode *node = create_node(NODE_SET, cname, expr);
                    node->var_type = vtype;
                    node->is_expr = 1;
                    node->is_default = 1; /* sole call result */
                    emit(node);
                    return;
                }
            }
            VarType expr_type = collect_expression(lexer, table, &expr, &(int){65536}, 0, 0, NULL, NULL, NULL, NULL);
            vtype = expr_type;
            if (!check_set_type_immutable(table, mangled, vtype, set_tok.line, set_tok.col, lexer))
                return;
            symbol_table_update(table, mangled, vtype);
            ASTNode *node = create_node(NODE_SET, cname, expr);
            node->var_type = vtype;
            node->is_expr = 1;
            emit(node);
            return;
        }
        Token typeof_val = {0};
        if (try_parse_typeof(lexer, table, &typeof_val)) {
            value = typeof_val;
        } else {
            Token peek_check = lexer_peek(lexer);
            if (peek_check.type == TOKEN_IDENT && strcmp(peek_check.value, "typeof") == 0) {
                skip_to_newline(lexer);
                return;
            }
            value = lexer_next(lexer);
            if (value.type != TOKEN_STRING && value.type != TOKEN_INT_LIT &&
                value.type != TOKEN_FLOAT_LIT && value.type != TOKEN_CHAR &&
                !(value.type == TOKEN_IDENT && is_bool_value(value.value))) {
                error_state_add(parser_errors, value.line, value.col,
                    "Expected value after '='");
                skip_to_newline(lexer);
                return;
            }
        }
        if (value.type == TOKEN_CHAR) {
            vtype = TYPE_STRING;
        } else if (value.type == TOKEN_STRING) {
            vtype = TYPE_STRING;
        } else {
            vtype = detect_type(value.value);
        }
    }

    if (!check_set_type_immutable(table, mangled, vtype, set_tok.line, set_tok.col, lexer))
        return;
    symbol_table_update(table, mangled, vtype);

    resolve_string_interpolation(&value, table, value.line, value.col);

    ASTNode *node = create_node(NODE_SET, cname, value.value);
    node->var_type = vtype;

    emit(node);
}

ASTNode *parse_switch(Lexer *lexer, SymbolTable *table) {
    Token kw = lexer_next(lexer);

    Token lp = lexer_next(lexer);
    if (lp.type != TOKEN_LPAREN) {
        error_state_add(parser_errors, lp.line, lp.col,
            "Expected '(' after switch");
        skip_to_newline(lexer);
        return NULL;
    }

    char *cond = (char*)malloc(65536); if(cond) cond[0]=0;
    int csl=0,csc=0,cel=0,cec=0;
    VarType ctype = collect_expression(lexer, table, &cond, &(int){65536}, 1, 0, &csl,&csc,&cel,&cec);

    Token rp = lexer_next(lexer);
    if (rp.type != TOKEN_RPAREN) {
        error_state_add(parser_errors, rp.line, rp.col,
            "Expected ')' after switch condition");
        skip_to_newline(lexer);
        return NULL;
    }

    Token lb = lexer_next(lexer);
    while (lb.type == TOKEN_NEWLINE) lb = lexer_next(lexer);
    if (lb.type != TOKEN_LBRACE) {
        error_state_add(parser_errors, lb.line, lb.col,
            "Expected '{' after switch condition");
        skip_to_newline(lexer);
        return NULL;
    }

    ASTNode *sw = create_node(NODE_SWITCH, "", cond);
    sw->line = kw.line; sw->col = kw.col;
    sw->v_sl=csl; sw->v_sc=csc; sw->v_el=cel; sw->v_ec=cec;
    sw->var_type = ctype;
    sw->is_expr = 1;
    ASTNode *last_case = NULL;

    while (1) {
        Token t = lexer_peek(lexer);
        if (t.type == TOKEN_EOF) {
            error_state_add(parser_errors, t.line, t.col,
                "Expected '}' to close switch");
            break;
        }
        if (t.type == TOKEN_RBRACE) {
            lexer_next(lexer);
            break;
        }
        if (t.type == TOKEN_NEWLINE) {
            lexer_next(lexer);
            continue;
        }

                if (t.type == TOKEN_CASE) {
            lexer_next(lexer);
            char *cexpr = (char*)calloc(1,65536);
            int cexpr_cap = 65536;
            int pos = 0;
            int depth_paren = 0, depth_bracket = 0, depth_brace = 0;
            while (1) {
                Token ct = lexer_peek(lexer);
                if (ct.type == TOKEN_EOF) break;
                if (ct.type == TOKEN_COLON && depth_paren == 0 && depth_bracket == 0 && depth_brace == 0) break;
                if (ct.type == TOKEN_LPAREN) depth_paren++;
                else if (ct.type == TOKEN_RPAREN) { if (depth_paren > 0) depth_paren--; }
                else if (ct.type == TOKEN_LBRACE) depth_brace++;
                else if (ct.type == TOKEN_RBRACE) { if (depth_brace > 0) depth_brace--; }
                ct = lexer_next(lexer);
                if (ct.type == TOKEN_NEWLINE) continue;
                if (ct.type == TOKEN_STRING) {
                    STMT_APPEND(cexpr, cexpr_cap, pos, "\"%s\"", ct.value);
                } else if (ct.type == TOKEN_CHAR) {
                    if (ct.value[0] != '\0') {
                        if (ctype == TYPE_STRING)
                            STMT_APPEND(cexpr, cexpr_cap, pos, "\"%s\"", ct.value);
                        else
                            STMT_APPEND(cexpr, cexpr_cap, pos, "'%s'", ct.value);
                    } else {
                        if (ctype == TYPE_STRING)
                            STMT_APPEND(cexpr, cexpr_cap, pos, "\"\"");
                        else
                            STMT_APPEND(cexpr, cexpr_cap, pos, "'\\0'");
                    }
                } else if (ct.type == TOKEN_IDENT) {
                    if (is_bool_value(ct.value)) {
                        STMT_APPEND(cexpr, cexpr_cap, pos, "%s",
                            (strcmp(ct.value, "true") == 0 || strcmp(ct.value, "t") == 0) ? "true" : "false");
                    } else {
                        char *mangled = mangle_name_alloc(ct.value);
                        if (symbol_table_has(table, mangled)) {
                            VarType vt = symbol_table_get(table, mangled);
                            if (vt == TYPE_STRING) STMT_APPEND(cexpr, cexpr_cap, pos, "%s", mangled);
                            else if (vt == TYPE_BOOL) STMT_APPEND(cexpr, cexpr_cap, pos, "(%s[0]=='t'||%s[0]=='T')?1:0", mangled, mangled);
                            else if (vt == TYPE_INT) STMT_APPEND(cexpr, cexpr_cap, pos, "atoll(%s)", mangled);
                            else if (vt == TYPE_FLOAT) STMT_APPEND(cexpr, cexpr_cap, pos, "atof(%s)", mangled);
                            else STMT_APPEND(cexpr, cexpr_cap, pos, "atoll(%s)", mangled);
                        } else if (is_c_reserved(ct.value)) {
                            STMT_APPEND(cexpr, cexpr_cap, pos, "%s", ct.value);
                        } else {
                            STMT_APPEND(cexpr, cexpr_cap, pos, "%s", ct.value);
                        }
                    }
                } else if (ct.type == TOKEN_INT_LIT || ct.type == TOKEN_FLOAT_LIT) {
                    STMT_APPEND(cexpr, cexpr_cap, pos, "%s", ct.value);
                }                 else if (ct.type == TOKEN_PLUS) STMT_APPEND(cexpr, cexpr_cap, pos, "+");
                else if (ct.type == TOKEN_MINUS) STMT_APPEND(cexpr, cexpr_cap, pos, "-");
                else if (ct.type == TOKEN_STAR) STMT_APPEND(cexpr, cexpr_cap, pos, "*");
                else if (ct.type == TOKEN_SLASH) STMT_APPEND(cexpr, cexpr_cap, pos, "/");
                else if (ct.type == TOKEN_PERCENT) STMT_APPEND(cexpr, cexpr_cap, pos, "%%");
                else if (ct.type == TOKEN_EQ_EQ) STMT_APPEND(cexpr, cexpr_cap, pos, "==");
                else if (ct.type == TOKEN_BANG_EQ) STMT_APPEND(cexpr, cexpr_cap, pos, "!=");
                else if (ct.type == TOKEN_LT_EQ) STMT_APPEND(cexpr, cexpr_cap, pos, "<=");
                else if (ct.type == TOKEN_GT_EQ) STMT_APPEND(cexpr, cexpr_cap, pos, ">=");
                else if (ct.type == TOKEN_LT) STMT_APPEND(cexpr, cexpr_cap, pos, "<");
                else if (ct.type == TOKEN_GT) STMT_APPEND(cexpr, cexpr_cap, pos, ">");
                else if (ct.type == TOKEN_AMP_AMP) STMT_APPEND(cexpr, cexpr_cap, pos, "&&");
                else if (ct.type == TOKEN_PIPE_PIPE) STMT_APPEND(cexpr, cexpr_cap, pos, "||");
                else if (ct.type == TOKEN_BANG) STMT_APPEND(cexpr, cexpr_cap, pos, "!");
                else if (ct.type == TOKEN_LPAREN) STMT_APPEND(cexpr, cexpr_cap, pos, "(");
                else if (ct.type == TOKEN_RPAREN) STMT_APPEND(cexpr, cexpr_cap, pos, ")");

                else if (ct.type == TOKEN_EQUALS) STMT_APPEND(cexpr, cexpr_cap, pos, "=");
                else STMT_APPEND(cexpr, cexpr_cap, pos, "%s", ct.value);
            }
            Token colon = lexer_next(lexer);
            if (colon.type != TOKEN_COLON) {
                error_state_add(parser_errors, colon.line, colon.col,
                    "Expected ':' after case value");
                skip_to_newline(lexer);
                continue;
            }
            /* Normalize string expressions in case values (e.g. "a"+"b", s*2) */
            if (ctype == TYPE_STRING || strchr(cexpr, '"') != NULL) {
                int _hp=0,_hs=0,_in=0; char _qc=0;
                for (const char *_p = cexpr; *_p; _p++) {
                    if (_in) { if (*_p == '\\' && _p[1]) { _p++; continue; } if (*_p == _qc) _in = 0; continue; }
                    if (*_p == '"' || *_p == '\'') { _in = 1; _qc = *_p; continue; }
                    if (*_p == '+') _hp = 1;
                    if (*_p == '*') _hs = 1;
                }
                if (_hp || _hs) normalize_string_expr(&cexpr, &cexpr_cap, 0, table);
            }
ASTNode *cnode = create_node(NODE_CASE, "", cexpr);
            cnode->is_default = 0;
            cnode->var_type = ctype;

            Token lb2 = lexer_next(lexer);
            while (lb2.type == TOKEN_NEWLINE) lb2 = lexer_next(lexer);
            if (lb2.type != TOKEN_LBRACE) {
                error_state_add(parser_errors, lb2.line, lb2.col,
                    "Expected '{' after case");
                skip_to_newline(lexer);
                continue;
            }
            cnode->body = parse_block(lexer, table, 1);

            if (last_case) last_case->next = cnode;
            else sw->cases = cnode;
            last_case = cnode;
            continue;
        } else if (t.type == TOKEN_DEFAULT) {
            lexer_next(lexer);
            Token colon = lexer_next(lexer);
            if (colon.type != TOKEN_COLON) {
                error_state_add(parser_errors, colon.line, colon.col,
                    "Expected ':' after default");
                skip_to_newline(lexer);
                continue;
            }

            ASTNode *dnode = create_node(NODE_CASE, "", "");
            dnode->is_default = 1;
            dnode->var_type = ctype;

            Token lb2 = lexer_next(lexer);
            while (lb2.type == TOKEN_NEWLINE) lb2 = lexer_next(lexer);
            if (lb2.type != TOKEN_LBRACE) {
                error_state_add(parser_errors, lb2.line, lb2.col,
                    "Expected '{' after default");
                skip_to_newline(lexer);
                continue;
            }
            dnode->body = parse_block(lexer, table, 1);

            if (last_case) last_case->next = dnode;
            else sw->cases = dnode;
            last_case = dnode;
            continue;
        } else {
            error_state_add(parser_errors, t.line, t.col,
                "Unexpected statement in switch (expected case or default)");
            lexer_next(lexer);
            continue;
        }
    }

    return sw;
}



ASTNode *parse_switch(Lexer *lexer, SymbolTable *table);

ASTNode *parse_if_core(Lexer *lexer, SymbolTable *table);
ASTNode *parse_switch(Lexer *lexer, SymbolTable *table);


void transform_inc_dec_stmt(Lexer *lexer, SymbolTable *table) {
    Token t = lexer_peek(lexer);
    if (t.type == TOKEN_PLUS_PLUS) {
        lexer_next(lexer);
        Token name = lexer_next(lexer);
        if (!token_is_name(name)) { skip_to_newline(lexer); return; }
        /* try to be permissive */
        char *mangled = mangle_name_alloc(name.value);
        if (symbol_table_is_const(table, mangled)) { skip_to_newline(lexer); return; }
        if (!symbol_table_has(table, mangled)) {
            /* add if missing? maybe not - but try to proceed */
        }
        char *full_expr = (char*)malloc(65536); if(full_expr) full_expr[0]=0;
        SNPRINTF_FULL(full_expr, "%s + 1", name.value);
        int expr_out_cap = 65536;
        char *expr_out = (char*)malloc(expr_out_cap); if(expr_out) expr_out[0]=0;
        VarType etype = parse_expression_from_string(full_expr, table, &expr_out, &expr_out_cap);
        if (!symbol_table_has(table, mangled)) {
            symbol_table_update(table, mangled, etype);
        } else {
            symbol_table_update(table, mangled, etype);
        }
        const char *cname = symbol_table_get_cname(table, mangled);
        ASTNode *node = create_node(NODE_SET, cname, expr_out);
        node->var_type = etype;
        node->is_expr = 1;
        emit(node);
        return;
    } else if (t.type == TOKEN_MINUS_MINUS) {
        lexer_next(lexer);
        Token name = lexer_next(lexer);
        if (!token_is_name(name)) { skip_to_newline(lexer); return; }
        char *mangled = mangle_name_alloc(name.value);
        if (symbol_table_is_const(table, mangled)) { skip_to_newline(lexer); return; }
        char *full_expr = (char*)malloc(65536); if(full_expr) full_expr[0]=0;
        SNPRINTF_FULL(full_expr, "%s - 1", name.value);
        int expr_out_cap = 65536;
        char *expr_out = (char*)malloc(expr_out_cap); if(expr_out) expr_out[0]=0;
        VarType etype = parse_expression_from_string(full_expr, table, &expr_out, &expr_out_cap);
        if (!symbol_table_has(table, mangled)) {
            symbol_table_update(table, mangled, etype);
        } else {
            symbol_table_update(table, mangled, etype);
        }
        const char *cname = symbol_table_get_cname(table, mangled);
        ASTNode *node = create_node(NODE_SET, cname, expr_out);
        node->var_type = etype;
        node->is_expr = 1;
        emit(node);
        return;
    } else if (t.type == TOKEN_IDENT) {
        int saved = lexer->current;
        lexer_next(lexer);
        Token t2 = lexer_peek(lexer);
        if (t2.type == TOKEN_PLUS_PLUS) {
            Token name = t;
            lexer_next(lexer);
            if (is_ltng_reserved(name.value)) { lexer->current = saved; return; }
            char *mangled = mangle_name_alloc(name.value);
            if (!symbol_table_has(table, mangled) || symbol_table_is_const(table, mangled)) { lexer->current = saved; return; }
            char *full_expr = (char*)malloc(65536); if(full_expr) full_expr[0]=0;
            SNPRINTF_FULL(full_expr, "%s + 1", name.value);
            int expr_out_cap = 65536;
            char *expr_out = (char*)malloc(expr_out_cap); if(expr_out) expr_out[0]=0;
            VarType etype = parse_expression_from_string(full_expr, table, &expr_out, &expr_out_cap);
            symbol_table_update(table, mangled, etype);
            const char *cname = symbol_table_get_cname(table, mangled);
            ASTNode *node = create_node(NODE_SET, cname, expr_out);
            node->var_type = etype;
            node->is_expr = 1;
            emit(node);
            return;
        } else if (t2.type == TOKEN_MINUS_MINUS) {
            Token name = t;
            lexer_next(lexer);
            if (is_ltng_reserved(name.value)) { lexer->current = saved; return; }
            char *mangled = mangle_name_alloc(name.value);
            if (!symbol_table_has(table, mangled) || symbol_table_is_const(table, mangled)) { lexer->current = saved; return; }
            char *full_expr = (char*)malloc(65536); if(full_expr) full_expr[0]=0;
            SNPRINTF_FULL(full_expr, "%s - 1", name.value);
            int expr_out_cap = 65536;
            char *expr_out = (char*)malloc(expr_out_cap); if(expr_out) expr_out[0]=0;
            VarType etype = parse_expression_from_string(full_expr, table, &expr_out, &expr_out_cap);
            symbol_table_update(table, mangled, etype);
            const char *cname = symbol_table_get_cname(table, mangled);
            ASTNode *node = create_node(NODE_SET, cname, expr_out);
            node->var_type = etype;
            node->is_expr = 1;
            emit(node);
            return;
        }
        lexer->current = saved;
    }
}

void parse_statement(Lexer *lexer, SymbolTable *table);


ASTNode *parse_while(Lexer *lexer, SymbolTable *table) {
    Token kw = lexer_next(lexer);

    Token lp = lexer_next(lexer);
    if (lp.type != TOKEN_LPAREN) {
        error_state_add(parser_errors, lp.line, lp.col,
            "Expected '(' after while");
        skip_to_newline(lexer);
        return NULL;
    }

    char *cond = (char*)malloc(65536); if(cond) cond[0]=0;
    int csl=0,csc=0,cel=0,cec=0;
    cond[0] = '\0';
    VarType ctype = TYPE_INT;
    if (lexer->current + 1 < lexer->count) {
        Token _ct = lexer->tokens[lexer->current];
        Token _nt = lexer->tokens[lexer->current + 1];
        if ((_ct.type == TOKEN_IDENT || token_is_name(_ct)) && _nt.type == TOKEN_RPAREN &&
            !is_bool_value(_ct.value) && strcmp(_ct.value, "typeof") != 0) {
            char *_cm = mangle_name_alloc(_ct.value);
            if (symbol_table_has(table, _cm)) {
                VarType _cvt = symbol_table_get(table, _cm);
                int _dyn = 0;
                if (_cvt == TYPE_NULL || _cvt == TYPE_STRING || _cvt == TYPE_CHAR || _cvt == TYPE_OBJECT) _dyn = 1;
                if (symbol_table_is_param(table, _cm) && !symbol_table_get_locked(table, _cm)) {
                    VarType _pdt = effective_param_default_type(_ct.value, _cvt);
                    if (_pdt == _cvt) _dyn = 1;
                }
                if (_dyn) {
                    lexer_next(lexer);
                    const char *_cn = symbol_table_get_cname(table, _cm);
                    SNPRINTF_FULL(cond, "_ltng_str_truthy(%s)", _cn ? _cn : _cm);
                    ctype = TYPE_STRING;
                }
            }
        }
    }
    if (cond[0] == '\0') {
        ctype = collect_expression(lexer, table, &cond, &(int){65536}, 1, 0, &csl,&csc,&cel,&cec);
        if (ctype == TYPE_STRING || ctype == TYPE_CHAR || ctype == TYPE_OBJECT || ctype == TYPE_NULL) {
            char *_tc = _ltng_cfmt("_ltng_str_truthy(%s)", cond);
            if(_tc){ free(cond); cond = _tc; }
        }
    }

    Token rp = lexer_next(lexer);
    if (rp.type != TOKEN_RPAREN) {
        error_state_add(parser_errors, rp.line, rp.col,
            "Expected ')' after while condition");
        skip_to_newline(lexer);
        return NULL;
    }

    if (cond[0] == '\0') {
        error_state_add(parser_errors, kw.line, kw.col,
            "Expected condition inside while()");
    }

    Token lb = lexer_next(lexer);
    while (lb.type == TOKEN_NEWLINE) lb = lexer_next(lexer);
    if (lb.type != TOKEN_LBRACE) {
        error_state_add(parser_errors, lb.line, lb.col,
            "Expected '{' after while condition");
        skip_to_newline(lexer);
        return NULL;
    }

    ASTNode *node = create_node(NODE_WHILE, "", cond);
    node->line = kw.line; node->col = kw.col;
    node->v_sl=csl; node->v_sc=csc; node->v_el=cel; node->v_ec=cec;
    node->var_type = ctype;
    node->is_expr = 1;
    g_break_depth++;
    node->body = parse_block(lexer, table, 1);
    g_break_depth--;
    (void)kw;
    return node;
}

ASTNode *parse_do_while(Lexer *lexer, SymbolTable *table) {
    Token kw = lexer_next(lexer);

    Token lb = lexer_next(lexer);
    while (lb.type == TOKEN_NEWLINE) lb = lexer_next(lexer);
    if (lb.type != TOKEN_LBRACE) {
        error_state_add(parser_errors, lb.line, lb.col,
            "Expected '{' after do");
        skip_to_newline(lexer);
        return NULL;
    }

    ASTNode *node = create_node(NODE_DO_WHILE, "", "");
    node->line = kw.line; node->col = kw.col;
    g_break_depth++;
    node->body = parse_block(lexer, table, 1);
    g_break_depth--;

    Token wh = lexer_peek(lexer);
    while (wh.type == TOKEN_NEWLINE) {
        lexer_next(lexer);
        wh = lexer_peek(lexer);
    }
    if (wh.type == TOKEN_WHILE) {
        lexer_next(lexer);
    } else {
        error_state_add(parser_errors, wh.line, wh.col,
            "Expected 'while' after do block");
        skip_to_newline(lexer);
        return node;
    }

    Token lp = lexer_next(lexer);
    if (lp.type != TOKEN_LPAREN) {
        error_state_add(parser_errors, lp.line, lp.col,
            "Expected '(' after while");
        skip_to_newline(lexer);
        return node;
    }

    char *cond = (char*)malloc(65536); if(cond) cond[0]=0;
    int csl=0,csc=0,cel=0,cec=0;
    cond[0] = '\0';
    VarType ctype = TYPE_INT;
    if (lexer->current + 1 < lexer->count) {
        Token _ct = lexer->tokens[lexer->current];
        Token _nt = lexer->tokens[lexer->current + 1];
        if ((_ct.type == TOKEN_IDENT || token_is_name(_ct)) && _nt.type == TOKEN_RPAREN &&
            !is_bool_value(_ct.value) && strcmp(_ct.value, "typeof") != 0) {
            char *_cm = mangle_name_alloc(_ct.value);
            if (symbol_table_has(table, _cm)) {
                VarType _cvt = symbol_table_get(table, _cm);
                int _dyn = 0;
                if (_cvt == TYPE_NULL || _cvt == TYPE_STRING || _cvt == TYPE_CHAR || _cvt == TYPE_OBJECT) _dyn = 1;
                if (symbol_table_is_param(table, _cm) && !symbol_table_get_locked(table, _cm)) {
                    VarType _pdt = effective_param_default_type(_ct.value, _cvt);
                    if (_pdt == _cvt) _dyn = 1;
                }
                if (_dyn) {
                    lexer_next(lexer);
                    const char *_cn = symbol_table_get_cname(table, _cm);
                    SNPRINTF_FULL(cond, "_ltng_str_truthy(%s)", _cn ? _cn : _cm);
                    ctype = TYPE_STRING;
                }
            }
        }
    }
    if (cond[0] == '\0') {
        ctype = collect_expression(lexer, table, &cond, &(int){65536}, 1, 0, &csl,&csc,&cel,&cec);
        if (ctype == TYPE_STRING || ctype == TYPE_CHAR || ctype == TYPE_OBJECT || ctype == TYPE_NULL) {
            char *_tc = _ltng_cfmt("_ltng_str_truthy(%s)", cond);
            if(_tc){ free(cond); cond = _tc; }
        }
    }
    { free(node->value); node->value = ltng_strdup(cond); }
    node->v_sl=csl; node->v_sc=csc; node->v_el=cel; node->v_ec=cec;
    node->var_type = ctype;
    node->is_expr = 1;

    Token rp = lexer_next(lexer);
    if (rp.type != TOKEN_RPAREN) {
        error_state_add(parser_errors, rp.line, rp.col,
            "Expected ')' after while condition");
        skip_to_newline(lexer);
        return node;
    }
    (void)kw;
    return node;
}


ASTNode *parse_for(Lexer *lexer, SymbolTable *table) {
    Token kw = lexer_next(lexer);

    Token lp = lexer_next(lexer);
    if (lp.type != TOKEN_LPAREN) {
        error_state_add(parser_errors, lp.line, lp.col,
            "Expected '(' after for");
        skip_to_newline(lexer);
        return NULL;
    }

    /* Iterator variable */
    Token var_tok = lexer_next(lexer);
    if (var_tok.type != TOKEN_IDENT && !token_is_name(var_tok)) {
        error_state_add(parser_errors, var_tok.line, var_tok.col,
            "Expected loop variable name after 'for ('");
        skip_to_newline(lexer);
        return NULL;
    }
    if (is_ltng_reserved(var_tok.value) && strcmp(var_tok.value, "in") != 0) {
        error_state_add(parser_errors, var_tok.line, var_tok.col,
            "Cannot use reserved word as loop variable");
        skip_to_newline(lexer);
        return NULL;
    }

    Token next_tok = lexer_peek(lexer);
    if (next_tok.type == TOKEN_IN) {
        /* for-each: for (x in arr) { ... } */
        lexer_next(lexer); /* consume 'in' */
        Token arr_tok = lexer_next(lexer);
        if (arr_tok.type != TOKEN_IDENT && !token_is_name(arr_tok)) {
            error_state_add(parser_errors, arr_tok.line, arr_tok.col,
                "Expected array variable name after 'in'");
            skip_to_newline(lexer);
            return NULL;
        }
        char *am = mangle_name_alloc(arr_tok.value);
        if (!symbol_table_has(table, am) || symbol_table_get(table, am) != TYPE_ARRAY) {
            error_state_add(parser_errors, arr_tok.line, arr_tok.col,
                "for-in expects an array variable");
            skip_to_newline(lexer);
            return NULL;
        }
        Token rp = lexer_next(lexer);
        if (rp.type != TOKEN_RPAREN) {
            error_state_add(parser_errors, rp.line, rp.col,
                "Expected ')' after for-in header");
            skip_to_newline(lexer);
            return NULL;
        }
        Token lb = lexer_next(lexer);
        while (lb.type == TOKEN_NEWLINE) lb = lexer_next(lexer);
        if (lb.type != TOKEN_LBRACE) {
            error_state_add(parser_errors, lb.line, lb.col,
                "Expected '{' after for-in header");
            skip_to_newline(lexer);
            return NULL;
        }
        char *mangled = mangle_name_alloc(var_tok.value);
        VarType elem_vt = symbol_table_get_array_elem_type(table, am);
        if (elem_vt == TYPE_NULL) elem_vt = TYPE_STRING;
        Scope *loop_scope = (Scope *)malloc(sizeof(Scope));
        if (!loop_scope) {
            error_state_add(parser_errors, var_tok.line, var_tok.col, "Memory allocation failed");
            return NULL;
        }
        scope_init(loop_scope, table, 0);
        if (elem_vt != TYPE_STRING && elem_vt != TYPE_NULL)
            symbol_table_add(loop_scope, mangled, elem_vt);
        else
            symbol_table_add_local(loop_scope, mangled, elem_vt);
        const char *cname = symbol_table_get_cname(loop_scope, mangled);
        const char *acname = symbol_table_get_cname(table, am);
        ASTNode *node = create_node(NODE_FOR, cname, "");
    node->line = kw.line; node->col = kw.col;
        node->var_type = elem_vt;
        { free(node->for_var); node->for_var = ltng_strdup(var_tok.value); }
        { free(node->for_start); node->for_start = ltng_strdup(acname); }
        node->for_step_op = 'E';
        g_break_depth++;
        node->body = parse_block(lexer, loop_scope, 1);
        g_break_depth--;
        free(loop_scope);
        (void)kw;
        return node;
    }

    if (next_tok.type != TOKEN_FROM) {
        error_state_add(parser_errors, next_tok.line, next_tok.col,
            "Expected 'from' or 'in' in for loop");
        skip_to_newline(lexer);
        return NULL;
    }
    lexer_next(lexer); /* consume 'from' */

    /* Start expression */
    char *start_expr = (char*)malloc(65536); if(start_expr) start_expr[0]=0;
    int ssl=0,ssc=0,sel=0,sec=0;
    VarType start_type = collect_expression(lexer, table, &start_expr, &(int){65536}, 1, 0, &ssl,&ssc,&sel,&sec);

    Token to_tok = lexer_next(lexer);
    if (to_tok.type != TOKEN_TO) {
        error_state_add(parser_errors, to_tok.line, to_tok.col,
            "Expected 'to' in for loop");
        skip_to_newline(lexer);
        return NULL;
    }

    /* End expression - stop at step or ) */
    char *end_expr = (char*)malloc(65536); if(end_expr) end_expr[0]=0;
    int esl=0,esc=0,eel=0,eec=0;
    VarType end_type = collect_expression(lexer, table, &end_expr, &(int){65536}, 1, 0, &esl,&esc,&eel,&eec);

    /* Optional step */
    char *step_expr = (char*)malloc(65536); if(step_expr) SNPRINTF_FULL(step_expr, "%s", "1");
    int stsl=0,stsc=0,stel=0,stec=0;
    char step_op = '+';
    Token next = lexer_peek(lexer);
    if (next.type == TOKEN_STEP) {
        lexer_next(lexer);
        Token sop = lexer_peek(lexer);
        if (sop.type == TOKEN_PLUS) {
            step_op = '+'; lexer_next(lexer);
        } else if (sop.type == TOKEN_MINUS) {
            step_op = '-'; lexer_next(lexer);
        } else if (sop.type == TOKEN_STAR) {
            step_op = '*'; lexer_next(lexer);
        } else if (sop.type == TOKEN_SLASH) {
            step_op = '/'; lexer_next(lexer);
        } else if (sop.type == TOKEN_PERCENT) {
            error_state_add(parser_errors, sop.line, sop.col,
                "'%' is not allowed in for loop step");
            lexer_next(lexer);
            step_op = '+';
        } else {
            /* plain number/expression means additive */
            step_op = '+';
        }
        collect_expression(lexer, table, &step_expr, &(int){65536}, 1, 0, &stsl,&stsc,&stel,&stec);
        if (step_expr[0] == '\0') {
            strcpy(step_expr, "1");
        }
    }

    Token rp = lexer_next(lexer);
    if (rp.type != TOKEN_RPAREN) {
        error_state_add(parser_errors, rp.line, rp.col,
            "Expected ')' after for loop header");
        skip_to_newline(lexer);
        return NULL;
    }

    Token lb = lexer_next(lexer);
    while (lb.type == TOKEN_NEWLINE) lb = lexer_next(lexer);
    if (lb.type != TOKEN_LBRACE) {
        error_state_add(parser_errors, lb.line, lb.col,
            "Expected '{' after for loop header");
        skip_to_newline(lexer);
        return NULL;
    }

    /* Add loop variable to loop scope */
    char *mangled = mangle_name_alloc(var_tok.value);
    /* determine type - prefer int if both int, else float */
    VarType var_type = (start_type == TYPE_FLOAT || end_type == TYPE_FLOAT) ? TYPE_FLOAT : TYPE_INT;

    Scope *loop_scope = (Scope *)malloc(sizeof(Scope));
    if (!loop_scope) {
        error_state_add(parser_errors, var_tok.line, var_tok.col, "Memory allocation failed");
        return NULL;
    }
    scope_init(loop_scope, table, 0);
    symbol_table_add_local(loop_scope, mangled, var_type);
    const char *cname = symbol_table_get_cname(loop_scope, mangled);

    ASTNode *node = create_node(NODE_FOR, cname, "");
    node->line = kw.line; node->col = kw.col;
    node->fs_sl=ssl; node->fs_sc=ssc; node->fs_el=sel; node->fs_ec=sec;
    node->fe_sl=esl; node->fe_sc=esc; node->fe_el=eel; node->fe_ec=eec;
    node->fst_sl=stsl; node->fst_sc=stsc; node->fst_el=stel; node->fst_ec=stec;
    node->var_type = var_type;
    { free(node->for_var); node->for_var = ltng_strdup(var_tok.value); }
    { free(node->for_start); node->for_start = ltng_strdup(start_expr); }
    { free(node->for_end); node->for_end = ltng_strdup(end_expr); }
    { free(node->for_step); node->for_step = ltng_strdup(step_expr); }
    node->for_step_op = step_op;
    g_break_depth++;
    node->body = parse_block(lexer, loop_scope, 1);
    g_break_depth--;
    free(loop_scope);
    (void)kw;
    return node;
}

ASTNode *parse_else_chain(Lexer *lexer, SymbolTable *table) {
    int saved = lexer->current;
    while (lexer_peek(lexer).type == TOKEN_NEWLINE) lexer_next(lexer);
    Token t = lexer_peek(lexer);

    if (t.type == TOKEN_ELSE) {
        lexer_next(lexer);
        if (lexer_peek(lexer).type == TOKEN_IF) {
            return parse_if_core(lexer, table);
        }
        Token lb = lexer_next(lexer);
        while (lb.type == TOKEN_NEWLINE) lb = lexer_next(lexer);
        if (lb.type != TOKEN_LBRACE) {
            error_state_add(parser_errors, lb.line, lb.col,
                "Expected '{' after 'else'");
            return NULL;
        }
        ASTNode *else_node = create_node(NODE_ELSE, "", "");
        else_node->line = t.line; else_node->col = t.col;
        else_node->body = parse_block(lexer, table, 1);
        return else_node;
    }

    lexer->current = saved;
    return NULL;
}

ASTNode *parse_if_core(Lexer *lexer, SymbolTable *table) {
    Token kw = lexer_next(lexer);

    Token lp = lexer_next(lexer);
    if (lp.type != TOKEN_LPAREN) {
        error_state_add(parser_errors, lp.line, lp.col,
            "Expected '(' after if");
        skip_to_newline(lexer);
        return NULL;
    }

    char *cond = (char*)malloc(65536); if(cond) cond[0]=0;
    int csl=0,csc=0,cel=0,cec=0;
    cond[0] = '\0';
    VarType ctype = TYPE_INT;
    if (lexer->current + 1 < lexer->count) {
        Token _ct = lexer->tokens[lexer->current];
        Token _nt = lexer->tokens[lexer->current + 1];
        if ((_ct.type == TOKEN_IDENT || token_is_name(_ct)) && _nt.type == TOKEN_RPAREN &&
            !is_bool_value(_ct.value) && strcmp(_ct.value, "typeof") != 0) {
            char *_cm = mangle_name_alloc(_ct.value);
            if (symbol_table_has(table, _cm)) {
                VarType _cvt = symbol_table_get(table, _cm);
                int _dyn = 0;
                if (_cvt == TYPE_NULL || _cvt == TYPE_STRING || _cvt == TYPE_CHAR || _cvt == TYPE_OBJECT) _dyn = 1;
                if (symbol_table_is_param(table, _cm) && !symbol_table_get_locked(table, _cm)) {
                    VarType _pdt = effective_param_default_type(_ct.value, _cvt);
                    if (_pdt == _cvt) _dyn = 1;
                }
                if (_dyn) {
                    lexer_next(lexer);
                    const char *_cn = symbol_table_get_cname(table, _cm);
                    SNPRINTF_FULL(cond, "_ltng_str_truthy(%s)", _cn ? _cn : _cm);
                    ctype = TYPE_STRING;
                }
            }
        }
    }
    if (cond[0] == '\0') {
        ctype = collect_expression(lexer, table, &cond, &(int){65536}, 1, 0, &csl,&csc,&cel,&cec);
        if (ctype == TYPE_STRING || ctype == TYPE_CHAR || ctype == TYPE_OBJECT || ctype == TYPE_NULL) {
            char *_tc = _ltng_cfmt("_ltng_str_truthy(%s)", cond);
            if(_tc){ free(cond); cond = _tc; }
        }
    }

    Token rp = lexer_next(lexer);
    if (rp.type != TOKEN_RPAREN) {
        error_state_add(parser_errors, rp.line, rp.col,
            "Expected ')' after if condition");
        skip_to_newline(lexer);
        return NULL;
    }

    if (cond[0] == '\0') {
        error_state_add(parser_errors, kw.line, kw.col,
            "Expected condition inside if()");
    }

    Token lb = lexer_next(lexer);
    while (lb.type == TOKEN_NEWLINE) lb = lexer_next(lexer);
    if (lb.type != TOKEN_LBRACE) {
        error_state_add(parser_errors, lb.line, lb.col,
            "Expected '{' after if condition");
        skip_to_newline(lexer);
        return NULL;
    }

    ASTNode *node = create_node(NODE_IF, "", cond);
    node->line = kw.line; node->col = kw.col;
    node->v_sl=csl; node->v_sc=csc; node->v_el=cel; node->v_ec=cec;
    node->var_type = ctype;
    node->is_expr = 1;
    node->body = parse_block(lexer, table, 1);
    node->orelse = parse_else_chain(lexer, table);
    (void)kw;
    return node;
}

ASTNode *parse_block(Lexer *lexer, SymbolTable *table, int is_control) {
    ASTNode *saved = g_list;
    g_list = NULL;
    Scope *child = (Scope *)malloc(sizeof(Scope));
    if (!child) {
        error_state_add(parser_errors, 0, 0, "Memory allocation failed");
        return NULL;
    }
    scope_init(child, table, is_control);

    while (1) {
        Token t = lexer_peek(lexer);
        if (t.type == TOKEN_EOF) {
            error_state_add(parser_errors, t.line, t.col,
                "Expected '}' to close block");
            break;
        }
        if (t.type == TOKEN_RBRACE) {
            lexer_next(lexer);
            break;
        }
        if (t.type == TOKEN_CASE || t.type == TOKEN_DEFAULT) {
            break;
        }
        if (t.type == TOKEN_NEWLINE) {
            lexer_next(lexer);
            continue;
        }
        parse_statement(lexer, child);
        if (lexer_peek(lexer).type == TOKEN_NEWLINE) lexer_next(lexer);
    }

    ASTNode *result = g_list;
    g_list = saved;
    free(child);
    return reverse_list(result);
}

/* -------- Phase 4: structured exception handling -------- */

void skip_newlines(Lexer *lexer) {
    while (lexer_peek(lexer).type == TOKEN_NEWLINE) lexer_next(lexer);
}


void objschema_ensure(ObjSchema *s, int n) {
    if (!s || n <= s->cap) return;
    int nc = s->cap ? s->cap * 2 : 8;
    while (nc < n) nc *= 2;
    ObjField *nf = (ObjField*)realloc(s->fields, nc * sizeof(ObjField));
    if (!nf) return;
    for (int i = s->cap; i < nc; i++) { nf[i].name = ltng_strdup(""); nf[i].value = ltng_strdup(""); nf[i].type = TYPE_NULL; }
    s->fields = nf; s->cap = nc;
}
void build_catch_schema(const char *cname) {
    ObjSchema schema;
    memset(&schema, 0, sizeof(schema));
    schema.var = ltng_strdup("");
    schema.default_val = ltng_strdup("");
    static const char *field_names[] = {
        "type", "message", "trace", "line", "column", "function", "value"
    };
    static const VarType field_types[] = {
        TYPE_STRING, TYPE_STRING, TYPE_STRING, TYPE_INT, TYPE_INT, TYPE_STRING, TYPE_STRING
    };
    int n = (int)(sizeof(field_names) / sizeof(field_names[0]));
    objschema_ensure(&schema, n);
    for (int i = 0; i < n; i++) {
        free(schema.fields[i].name); schema.fields[i].name = ltng_strdup(field_names[i]);
        schema.fields[i].type = field_types[i];
        free(schema.fields[i].value); schema.fields[i].value = ltng_strdup("");
    }
    schema.count = n;
    free(schema.var); schema.var = ltng_strdup(cname);
    obj_build_all(&schema);
    obj_register(cname, &schema);
}

ASTNode *parse_try(Lexer *lexer, SymbolTable *table) {
    Token kw = lexer_next(lexer); /* try */

    skip_newlines(lexer);
    Token lb = lexer_next(lexer);
    while (lb.type == TOKEN_NEWLINE) lb = lexer_next(lexer);
    if (lb.type != TOKEN_LBRACE) {
        error_state_add(parser_errors, lb.line, lb.col,
            "Expected '{' after 'try'");
        skip_to_newline(lexer);
        return NULL;
    }

    ASTNode *node = create_node(NODE_TRY, "", "");
    node->line = kw.line; node->col = kw.col;
    node->body = parse_block(lexer, table, 0);

    int have_catch = 0;
    int have_else = 0;
    int have_finally = 0;

    /* Optional catch clause. Newlines between '}' and 'catch' are allowed. */
    skip_newlines(lexer);
    if (lexer_peek(lexer).type == TOKEN_CATCH) {
        lexer_next(lexer); /* catch */
        have_catch = 1;

        char *catch_var_src = NULL;
        int has_var = 0;

        Token t = lexer_peek(lexer);
        if (t.type == TOKEN_LPAREN) {
            lexer_next(lexer); /* ( */
            Token var = lexer_next(lexer);
            if (!token_is_name(var) || is_ltng_reserved(var.value)) {
                error_state_add(parser_errors, var.line, var.col,
                    "Expected catch variable name");
            } else {
                free(catch_var_src); catch_var_src = ltng_strdup(var.value);
                has_var = 1;
            }
            Token rp = lexer_next(lexer);
            if (rp.type != TOKEN_RPAREN) {
                error_state_add(parser_errors, rp.line, rp.col,
                    "Expected ')' after catch variable");
            }
        } else if (token_is_name(t) && t.type != TOKEN_LBRACE) {
            /* catch e { ... } */
            Token var = lexer_next(lexer);
            if (is_ltng_reserved(var.value)) {
                error_state_add(parser_errors, var.line, var.col,
                    "Expected catch variable name");
            } else {
                free(catch_var_src); catch_var_src = ltng_strdup(var.value);
                has_var = 1;
            }
        }

        skip_newlines(lexer);
        Token clb = lexer_next(lexer);
        while (clb.type == TOKEN_NEWLINE) clb = lexer_next(lexer);
        if (clb.type != TOKEN_LBRACE) {
            error_state_add(parser_errors, clb.line, clb.col,
                "Expected '{' after 'catch'");
            skip_to_newline(lexer);
            node->has_catch = 1;
            return node;
        }

        Scope *catch_scope = (Scope *)malloc(sizeof(Scope));
        if (!catch_scope) {
            error_state_add(parser_errors, kw.line, kw.col, "Memory allocation failed");
            return node;
        }
        scope_init(catch_scope, table, 0);

        if (has_var) {
            char *mangled = mangle_name_alloc(catch_var_src);
            /* Catch variables always receive a unique emitted C name so nested
               catches with the same source name cannot collide. */
            symbol_table_add_in_ex(catch_scope, mangled, TYPE_OBJECT, 1);
            const char *cname = symbol_table_get_cname(catch_scope, mangled);
            { free(node->name); node->name = ltng_strdup(cname); }
            { free(node->for_var); node->for_var = ltng_strdup(catch_var_src); }
            build_catch_schema(cname);
        }

        g_in_catch++;
        node->orelse = parse_block(lexer, catch_scope, 0);
        g_in_catch--;
        free(catch_scope);

        skip_newlines(lexer);
    }

    /* Optional else (success) clause. Runs only when the try body completes
       without throwing. Requires a catch clause, and must precede finally. */
    if (lexer_peek(lexer).type == TOKEN_ELSE) {
        Token else_tok = lexer_next(lexer); /* else */
        have_else = 1;
        if (!have_catch) {
            error_state_add(parser_errors, else_tok.line, else_tok.col,
                "'else' in a try construct requires a preceding 'catch' clause");
        }
        skip_newlines(lexer);
        Token elb = lexer_next(lexer);
        while (elb.type == TOKEN_NEWLINE) elb = lexer_next(lexer);
        if (elb.type != TOKEN_LBRACE) {
            error_state_add(parser_errors, elb.line, elb.col,
                "Expected '{' after 'else'");
            skip_to_newline(lexer);
        } else {
            node->else_body = parse_block(lexer, table, 0);
        }
        skip_newlines(lexer);
    }

    if (lexer_peek(lexer).type == TOKEN_FINALLY) {
        lexer_next(lexer); /* finally */
        have_finally = 1;
        skip_newlines(lexer);
        Token flb = lexer_next(lexer);
        while (flb.type == TOKEN_NEWLINE) flb = lexer_next(lexer);
        if (flb.type != TOKEN_LBRACE) {
            error_state_add(parser_errors, flb.line, flb.col,
                "Expected '{' after 'finally'");
            skip_to_newline(lexer);
            node->has_catch = have_catch;
            node->has_else = have_else;
            node->has_finally = have_finally;
            return node;
        }
        node->cases = parse_block(lexer, table, 0);
    }

    if (!have_catch && !have_finally) {
        error_state_add(parser_errors, kw.line, kw.col,
            "'try' requires a 'catch' or 'finally' clause");
    }

    node->has_catch = have_catch;
    node->has_else = have_else;
    node->has_finally = have_finally;
    return node;
}

int ltng_ident_ends_with(const char *s, const char *suffix) {
    size_t ls = strlen(s), lf = strlen(suffix);
    if (lf > ls) return 0;
    return strcmp(s + (ls - lf), suffix) == 0;
}

void parse_throw(Lexer *lexer, SymbolTable *table) {
    Token kw = lexer_next(lexer); /* throw */

    Token next = lexer_peek(lexer);
    if (next.type == TOKEN_NEWLINE || next.type == TOKEN_EOF ||
        next.type == TOKEN_RBRACE) {
        if (g_in_catch <= 0) {
            error_state_add(parser_errors, kw.line, kw.col,
                "bare 'throw' can only be used inside a catch block");
            return;
        }
        ASTNode *node = create_node(NODE_THROW, "", "");
        node->line = kw.line; node->col = kw.col;
        node->is_expr = 0; /* rethrow active exception */
        emit(node);
        return;
    }

    char *type_expr = (char*)malloc(65536); if(type_expr) type_expr[0]=0;
    char *msg_expr = (char*)malloc(65536); if(msg_expr) msg_expr[0]=0;
    SNPRINTF_FULL(type_expr, "\"Error\"");
    msg_expr[0] = '\0';
    VarType mtype = TYPE_STRING;
    int handled = 0;

    if (next.type == TOKEN_IDENT &&
        lexer->current + 1 < lexer->count &&
        lexer->tokens[lexer->current + 1].type == TOKEN_LPAREN) {
        int is_error_fn = (strcmp(next.value, "error") == 0) &&
                          !is_function_name_scoped(table, next.value);
        int is_custom = ltng_ident_ends_with(next.value, "Error") &&
                        !is_function_name_scoped(table, next.value);

        if (is_error_fn || is_custom) {
            lexer_next(lexer); /* name */
            lexer_next(lexer); /* ( */

            char *first = (char*)malloc(65536); if(first) first[0]=0;
            VarType ftype = collect_expression(lexer, table, &first, &(int){65536}, 1, 1,
                                               NULL, NULL, NULL, NULL);

            if (is_error_fn) {
                Token comma = lexer_next(lexer);
                if (comma.type != TOKEN_COMMA) {
                    error_state_add(parser_errors, comma.line, comma.col,
                        "Expected ',' after error type in error(type, message)");
                    skip_to_newline(lexer);
                    return;
                }
                char *second = (char*)malloc(65536); if(second) second[0]=0;
                VarType stype = collect_expression(lexer, table, &second, &(int){65536}, 1, 0,
                                                   NULL, NULL, NULL, NULL);
                if (ftype != TYPE_STRING && ftype != TYPE_CHAR && ftype != TYPE_OBJECT) {
                    error_state_add(parser_errors, kw.line, kw.col,
                        "error(type, message) requires a string error type");
                }
                SNPRINTF_FULL(type_expr, "%s", first[0] ? first : "\"Error\"");
                SNPRINTF_FULL(msg_expr, "%s", second[0] ? second : "\"\"");
                mtype = stype;
            } else {
                SNPRINTF_FULL(type_expr, "\"%s\"", next.value);
                SNPRINTF_FULL(msg_expr, "%s", first[0] ? first : "\"\"");
                mtype = ftype;
                /* Tolerate and ignore additional custom-error arguments. */
                while (lexer_peek(lexer).type != TOKEN_RPAREN &&
                       lexer_peek(lexer).type != TOKEN_NEWLINE &&
                       lexer_peek(lexer).type != TOKEN_EOF) {
                    lexer_next(lexer);
                }
            }

            Token rp = lexer_next(lexer);
            if (rp.type != TOKEN_RPAREN) {
                error_state_add(parser_errors, rp.line, rp.col,
                    "Expected ')' after throw expression");
                skip_to_newline(lexer);
                return;
            }
            handled = 1;
        }
    }

    /* Preserve boolean literals as "true"/"false" instead of their internal
       numeric representation when they are the entire thrown expression. */
    if (!handled &&
        next.type == TOKEN_IDENT &&
        (strcmp(next.value, "true") == 0 || strcmp(next.value, "false") == 0) &&
        lexer->current + 1 < lexer->count &&
        (lexer->tokens[lexer->current + 1].type == TOKEN_NEWLINE ||
         lexer->tokens[lexer->current + 1].type == TOKEN_EOF ||
         lexer->tokens[lexer->current + 1].type == TOKEN_RBRACE)) {
        lexer_next(lexer);
        SNPRINTF_FULL(msg_expr, "\"%s\"", next.value);
        mtype = TYPE_STRING;
        handled = 1;
    }

    if (!handled) {
        mtype = collect_expression(lexer, table, &msg_expr, &(int){65536}, 0, 0,
                                   NULL, NULL, NULL, NULL);
    }

    if (msg_expr[0] == '\0') {
        error_state_add(parser_errors, kw.line, kw.col,
            "Expected expression after 'throw'");
        skip_to_newline(lexer);
        return;
    }

    ASTNode *node = create_node(NODE_THROW, type_expr, msg_expr);
    node->line = kw.line; node->col = kw.col;
    node->is_expr = 1;
    node->var_type = mtype;
    emit(node);
}




/* Try to parse a function call as a complete argument (boundary: comma or rparen at depth 0).
   Returns 1 on success (cexpr filled with raw "F(_ltng_retbuf(),...)" call), lexer consumed past call.
   Returns 0 if not a sole call arg (lexer restored). Sets *out_type to call result type when non-NULL. */
int try_parse_call_arg(Lexer *lexer, SymbolTable *table, char **cexpr, int *cexpr_cap, VarType *out_type) {
    Token first = lexer_peek(lexer);
    if (first.type != TOKEN_IDENT || !is_callable_ident(table, first.value)) return 0;
    int saved = lexer->current;
    Token fname = lexer_next(lexer);
    if (lexer_peek(lexer).type != TOKEN_LPAREN) { lexer->current = saved; return 0; }
    lexer_next(lexer); /* consume ( */
    ASTNode *call = parse_call_node(lexer, table, fname.value, fname.line, fname.col, NULL);
    if (call->is_base_assign) {
        /* Value-consumed call-arg context: keep receiver intact, use return value */
        call->is_base_assign = 0;
    }
    Token after = lexer_peek(lexer);
    if (after.type != TOKEN_COMMA && after.type != TOKEN_RPAREN) {
        lexer->current = saved;
        return 0;
    }
    build_call_cexpr(call, cexpr, cexpr_cap);
    if (out_type) *out_type = infer_call_result_type(call);
    return 1;
}

/* Find a lifted function node by its C (mangled) name. */
ASTNode *find_function_node_by_cname(const char *cname) {
    if (!g_program_ref) return NULL;
    for (ASTNode *fn = g_program_ref->functions; fn; fn = fn->next) {
        if (strcmp(fn->name, cname) == 0) return fn;
    }
    return NULL;
}

/* Return 1 if the function's first parameter is named "base".
   Such functions are "receiver" functions: called via UFCS as
   receiver.method(args), and the return value is written back to receiver. */
int function_first_param_is_base(const char *cname) {
    ASTNode *fn = find_function_node_by_cname(cname);
    if (!fn || !fn->params) return 0;
    return strcmp(fn->params->for_var, "base") == 0;
}

/* Find a function AST node by its short source name (fn->for_var) or mangled C name. */
ASTNode *find_function_ast_by_name(const char *name) {
    if (!g_program_ref) return NULL;
    for (ASTNode *fn = g_program_ref->functions; fn; fn = fn->next) {
        if (strcmp(fn->for_var, name) == 0 || strcmp(fn->name, name) == 0) return fn;
    }
    return NULL;
}

/* Resolve named function reference info (cname and param count). Falls back to
   scope/global function registration when AST node is not yet linked (e.g.
   self-reference inside function body during parsing). Returns 1 on success. */
int resolve_func_ref_info(SymbolTable *table, const char *name, char **out_cname, int *out_pc) {
    ASTNode *fndef = find_function_ast_by_name(name);
    if (fndef) {
        if (out_cname) *out_cname = ltng_strdup(fndef->name);
        if (out_pc) {
            int pc = 0;
            for (ASTNode *p = fndef->params; p; p = p->next) pc++;
            *out_pc = pc;
        }
        return 1;
    }
    char *cname = resolve_func_name(table, name);
    if (!cname) return 0;
    if (out_pc) {
        int pc = -1;
        for (int i = 0; i < g_func_count; i++) {
            if (strcmp(g_funcs[i].name, cname) == 0) { pc = g_funcs[i].param_count; break; }
        }
        *out_pc = pc;
    }
    if (out_cname) *out_cname = cname;
    else free(cname);
    return 1;
}

int is_string_literal_return(ASTNode *ret) {
    if (!ret || ret->var_type != TYPE_STRING) return 0;
    const char *v = ret->value;
    while (*v == ' ' || *v == '\t') v++;
    return v[0] == '"';
}

int var_types_return_compatible_nodes(ASTNode *a, ASTNode *b) {
    if (!a || !b) return 1;
    VarType ta = a->var_type, tb = b->var_type;
    if (ta == tb) return 1;
    if ((ta == TYPE_INT || ta == TYPE_FLOAT) && (tb == TYPE_INT || tb == TYPE_FLOAT)) return 1;
    /* Variable pass-through returns are represented as TYPE_STRING in current implementation;
       treat them as compatible unless clearly conflicting string literals. */
    int a_lit = is_string_literal_return(a);
    int b_lit = is_string_literal_return(b);
    if (ta == TYPE_STRING && !a_lit) return 1;
    if (tb == TYPE_STRING && !b_lit) return 1;
    /* both string literals or string-literal vs non-string - incompatible */
    return 0;
}

void collect_return_nodes(ASTNode *node, ASTNode **nodes, int *count, int max) {
    if (!node || *count >= max) return;
    if (node->type == NODE_RETURN) {
        nodes[(*count)++] = node;
        return;
    }
    collect_return_nodes(node->body, nodes, count, max);
    collect_return_nodes(node->orelse, nodes, count, max);
    collect_return_nodes(node->else_body, nodes, count, max);
    collect_return_nodes(node->cases, nodes, count, max);
    collect_return_nodes(node->next, nodes, count, max);
}

/* Conservative control-flow check: does this statement list always return
   (or throw) before falling through? Loops are treated as possibly not
   executing, which is the safe answer for diagnostics. */
int stmt_always_exits(ASTNode *node);

int block_always_exits(ASTNode *block) {
    for (ASTNode *s = block; s; s = s->next) {
        if (stmt_always_exits(s)) return 1;
    }
    return 0;
}

int stmt_always_exits(ASTNode *node) {
    if (!node) return 0;
    switch (node->type) {
        case NODE_RETURN:
        case NODE_THROW:
            return 1;
        case NODE_BLOCK:
            return block_always_exits(node->body);
        case NODE_IF:
            if (!node->orelse) return 0;
            if (!block_always_exits(node->body)) return 0;
            if (node->orelse->type == NODE_IF) return stmt_always_exits(node->orelse);
            return block_always_exits(node->orelse->body);
        case NODE_SWITCH: {
            /* A switch only guarantees exit when it has a default case. */
            int has_default = 0;
            for (ASTNode *c = node->cases; c; c = c->next) {
                if (c->is_default) { has_default = 1; break; }
            }
            if (!has_default) return 0;
            for (ASTNode *c = node->cases; c; c = c->next) {
                if (!block_always_exits(c->body)) return 0;
            }
            return 1;
        }
        case NODE_TRY:
            /* finally with guaranteed exit overrides everything */
            if (node->has_finally && node->cases && block_always_exits(node->cases))
                return 1;
            /* try always exits and catch (if present) always exits => always exits.
               Normal completion cannot occur if try always exits, so else is irrelevant. */
            if (block_always_exits(node->body)) {
                if (!node->has_catch) return 1;
                if (node->orelse && block_always_exits(node->orelse)) return 1;
            }
            return 0;
        default:
            return 0;
    }
}

int return_type_matches_annotation(ASTNode *ret, VarType want) {    if (!ret) return 1;
    if (ret->value[0] == '\0' && !ret->is_expr) return want == TYPE_VOID;
    if (want == TYPE_VOID) return 0;
    if (ret->fs_sl >= 1000) {
        VarType ct = (VarType)(ret->fs_sl - 1000);
        if (ct == want) return 1;
        if ((ct == TYPE_INT || ct == TYPE_FLOAT) && (want == TYPE_INT || want == TYPE_FLOAT)) return 1;
        if (want == TYPE_STRING && (ct == TYPE_CHAR || ct == TYPE_STRING || ct == TYPE_BOOL || ct == TYPE_INT || ct == TYPE_FLOAT)) return 1;
        if (want == TYPE_CHAR && (ct == TYPE_CHAR || ct == TYPE_STRING)) return 1;
        if (want == TYPE_BOOL && (ct == TYPE_BOOL || ct == TYPE_INT)) return 1;
    }
    /* Pass-through variables and call results are represented as strings at
       runtime; only reject clearly conflicting literal returns. */
    if (ret->is_variable) return 1;
    if (ret->value[0] == '(' || strstr(ret->value, "_ltng_") != NULL) {
        if (ret->var_type == want) return 1;
        if ((ret->var_type == TYPE_INT || ret->var_type == TYPE_FLOAT) && (want == TYPE_INT || want == TYPE_FLOAT)) return 1;
        if (want == TYPE_STRING && (ret->var_type == TYPE_CHAR || ret->var_type == TYPE_STRING || ret->var_type == TYPE_BOOL || ret->var_type == TYPE_INT || ret->var_type == TYPE_FLOAT)) return 1;
        if (want == TYPE_BOOL && (ret->var_type == TYPE_BOOL || ret->var_type == TYPE_INT)) return 1;
        if (want == TYPE_CHAR && (ret->var_type == TYPE_CHAR || ret->var_type == TYPE_STRING)) return 1;
        return 1;
    }

    const char *v = ret->value;
    while (*v == ' ' || *v == '\t') v++;
    if (*v == '\0') return want == TYPE_VOID;

    VarType got;
    if (*v == '"') {
        got = TYPE_STRING;
    } else if (*v == '\'') {
        got = TYPE_CHAR;
    } else if (strcmp(v, "true") == 0 || strcmp(v, "false") == 0 ||
               strcmp(v, "t") == 0 || strcmp(v, "f") == 0) {
        got = TYPE_BOOL;
    } else {
        int dot = 0, numeric = 1;
        const char *p = v;
        if (*p == '-' || *p == '+') p++;
        if (!*p) numeric = 0;
        for (; *p; p++) {
            if (*p == '.') { dot = 1; continue; }
            if (*p < '0' || *p > '9') { numeric = 0; break; }
        }
        if (!numeric) {
            /* Non-literal expressions: trust semantic type when available */
            if (ret->var_type == want) return 1;
            if ((ret->var_type == TYPE_INT || ret->var_type == TYPE_FLOAT) && (want == TYPE_INT || want == TYPE_FLOAT)) return 1;
            if (want == TYPE_STRING && (ret->var_type == TYPE_CHAR || ret->var_type == TYPE_STRING || ret->var_type == TYPE_BOOL)) return 1;
            if (want == TYPE_BOOL && (ret->var_type == TYPE_BOOL || ret->var_type == TYPE_INT)) return 1;
            return 1;
        }
        got = dot ? TYPE_FLOAT : TYPE_INT;
    }

    /* Prefer semantic expression type when it is specific (e.g. toint/tofloat conversions emit quoted strings) */
    if (ret->var_type == want) return 1;
    if ((ret->var_type == TYPE_INT || ret->var_type == TYPE_FLOAT) && (want == TYPE_INT || want == TYPE_FLOAT)) return 1;
    if (want == TYPE_BOOL && ret->var_type == TYPE_BOOL) return 1;
    if (want == TYPE_CHAR && ret->var_type == TYPE_CHAR) return 1;
    if (ret->var_type == TYPE_BOOL && (want == TYPE_INT || want == TYPE_FLOAT)) return 0;

    if (got == want) return 1;
    if ((got == TYPE_INT || got == TYPE_FLOAT) &&
        (want == TYPE_INT || want == TYPE_FLOAT)) return 1;
    if (want == TYPE_CHAR && (got == TYPE_STRING || got == TYPE_CHAR)) return 1;
    if (want == TYPE_STRING && got == TYPE_CHAR) return 1;
    if (want == TYPE_BOOL && (got == TYPE_INT || got == TYPE_BOOL)) return 1;
    /* Reject clear mismatches like returning string literal from int function */
    if ((want == TYPE_INT || want == TYPE_FLOAT) && got == TYPE_STRING && ret->var_type == TYPE_STRING) return 0;
    if (want == TYPE_BOOL && got == TYPE_STRING && ret->var_type == TYPE_STRING) return 0;
    return 0;
}

void validate_function_return_types(ASTNode *fn) {
    if (!fn) return;
    ASTNode *nodes[64];
    int count = 0;
    collect_return_nodes(fn->body, nodes, &count, 64);

    if (fn->has_return_type) {
        if (fn->return_type == TYPE_VOID) {
            for (int i = 0; i < count; i++) {
                if (!(nodes[i]->value[0] == '\0' && !nodes[i]->is_expr)) {
                    char *msg = _ltng_cfmt("Void function '%s' cannot return a value", fn->for_var[0] ? fn->for_var : fn->name);
                    error_state_add(parser_errors, nodes[i]->line, nodes[i]->col, msg ? msg : "");
                    free(msg);
                    return;
                }
            }
            return;
        }
        if (count == 0) {
            char *msg = _ltng_cfmt("Function '%s' is declared to return '%s' but has no return statement", fn->for_var[0] ? fn->for_var : fn->name, type_name_str(fn->return_type));
            error_state_add(parser_errors, fn->line, fn->col, msg ? msg : "");
            free(msg);
            return;
        }
        for (int i = 0; i < count; i++) {
            if (!return_type_matches_annotation(nodes[i], fn->return_type)) {
                char *msg = _ltng_cfmt("Return type mismatch in function '%s': expected '%s'", fn->for_var[0] ? fn->for_var : fn->name, type_name_str(fn->return_type));
                error_state_add(parser_errors, nodes[i]->line, nodes[i]->col, msg ? msg : "");
                free(msg);
                return;
            }
        }
        if (!block_always_exits(fn->body)) {
            char *msg = _ltng_cfmt("Function '%s' may not return a value on all control paths", fn->for_var[0] ? fn->for_var : fn->name);
            error_state_add(parser_errors, fn->line, fn->col, msg ? msg : "");
            free(msg);
        }
        return;
    }

    if (count <= 1) return;
    for (int i = 1; i < count; i++) {
        if (!var_types_return_compatible_nodes(nodes[0], nodes[i])) {
            char *msg = _ltng_cfmt("Inconsistent return types in function '%s'", fn->for_var[0] ? fn->for_var : fn->name);
            error_state_add(parser_errors, fn->line, fn->col, msg ? msg : "");
            free(msg);
            return;
        }
        /* also ensure pairwise consistency against accumulated base? first vs all is enough if compat is transitive in this model */
    }
}

/* Recursively scan an AST subtree for NODE_RETURN nodes and determine the
   dominant return type. Sets *has_return when any return statement is found. */
VarType analyze_function_returns(ASTNode *node, int *has_return) {
    if (!node) return TYPE_VOID;

    if (node->type == NODE_RETURN) {
        *has_return = 1;
        return node->var_type;
    }

    VarType result = TYPE_VOID;
    int have_type = 0;
    VarType child;

    ASTNode *kids[5] = { node->body, node->orelse, node->cases, node->else_body, node->next };
    for (int k = 0; k < 5; k++) {
        int child_has_return = 0;
        child = analyze_function_returns(kids[k], &child_has_return);
        if (child_has_return) {
            *has_return = 1;
            if (!have_type) { result = child; have_type = 1; }
            else if (result == child) { /* same */ }
            else if ((result == TYPE_INT || result == TYPE_FLOAT) &&
                     (child == TYPE_INT || child == TYPE_FLOAT)) {
                result = (result == TYPE_FLOAT || child == TYPE_FLOAT) ? TYPE_FLOAT : TYPE_INT;
            } else if ((result == TYPE_STRING || result == TYPE_CHAR) &&
                       (child == TYPE_STRING || child == TYPE_CHAR)) {
                result = TYPE_STRING;
            } else {
                /* Mixed/incompatible (e.g. value vs bare/void, or object vs int). */
                result = TYPE_STRING;
            }
        }
    }
    return result;
}

/* Check if a function body is a simple pass-through of a single parameter.
   Returns the parameter index (>=0) if all return statements in the body
   pass through the same parameter with no other value returns; returns -1 otherwise.
   Bare returns (no value) or mixed returns disqualify. */
int analyze_function_passthrough(ASTNode *node, int *pt_param, int *saw_return, int *disqualified) {
    if (!node || *disqualified) return *pt_param;
    if (node->type == NODE_RETURN) {
        *saw_return = 1;
        if (node->passthrough_param >= 0) {
            if (*pt_param == -2) *pt_param = node->passthrough_param; /* first */
            else if (*pt_param != node->passthrough_param) *disqualified = 1;
        } else {
            /* value return that is not a simple param pass-through, or bare return */
            *disqualified = 1;
        }
        return *pt_param;
    }
    ASTNode *kids[5] = { node->body, node->orelse, node->cases, node->else_body, node->next };
    for (int k = 0; k < 5; k++) {
        analyze_function_passthrough(kids[k], pt_param, saw_return, disqualified);
        if (*disqualified) return -1;
    }
    return *pt_param;
}

int get_function_passthrough_param(const char *fname) {
    /* Check FuncInfo cache first */
    FuncInfo *fi = find_funcinfo_by_cname(fname);
    if (!fi) {
        for (int i = 0; i < g_func_count; i++) {
            if (strcmp(g_funcs[i].src, fname) == 0) { fi = &g_funcs[i]; break; }
        }
    }
    /* -1 means unknown/not-passthrough in cache; we need to distinguish "not checked"
       from "checked, not passthrough". Use -2 in a local check? Easier to just compute
       when we have the function node. But cache positive results in FuncInfo. */
    if (fi && fi->passthrough_param >= 0) return fi->passthrough_param;

    ASTNode *fn = find_function_node_by_cname(fname);
    if (!fn) fn = find_function_ast_by_name(fname);
    if (!fn) return -1;
    if (fn->has_return_type) return -1; /* explicit annotation takes precedence */

    int pt = -2; /* sentinel: no returns seen yet */
    int saw = 0, disq = 0;
    analyze_function_passthrough(fn->body, &pt, &saw, &disq);
    if (disq || !saw || pt < 0) return -1;
    if (fi) fi->passthrough_param = pt;
    /* also cache on src-name match */
    if (!fi) {
        for (int i = 0; i < g_func_count; i++) {
            if (strcmp(g_funcs[i].src, fname) == 0) { g_funcs[i].passthrough_param = pt; break; }
        }
    }
    return pt;
}

/* Infer the effective return type of a user-defined function. For annotated
   functions returns the annotation; otherwise analyzes return statements.
   If no return statements are present, returns TYPE_VOID.
   Returns TYPE_NULL when the function/return type is unknown (e.g. self-recursive
   call during body parsing before the node is linked) so callers can apply
   legacy numeric behavior instead of treating it as a string return. */
VarType infer_function_return_type(const char *fname) {
    ASTNode *fn = find_function_node_by_cname(fname);
    if (!fn) fn = find_function_ast_by_name(fname);
    if (fn) {
        if (fn->has_return_type) return fn->return_type;
        int has_return = 0;
        VarType rt = analyze_function_returns(fn->body, &has_return);
        if (!has_return) return TYPE_VOID;
        return rt;
    }
    /* Fall back to prescanned signatures */
    FuncInfo *fi = find_funcinfo_by_cname(fname);
    if (!fi) {
        for (int i = 0; i < g_func_count; i++) {
            if (strcmp(g_funcs[i].src, fname) == 0) { fi = &g_funcs[i]; break; }
        }
    }
    if (fi && fi->has_return_type) return fi->return_type;
    /* Unknown target: preserve legacy numeric behavior */
    return TYPE_NULL;
}

int function_returns_string(const char *fname) {
    ASTNode *fn = find_function_node_by_cname(fname);
    if (!fn) fn = find_function_ast_by_name(fname);
    if (fn) {
        if (fn->has_return_type) return fn->return_type == TYPE_STRING;
        int has_return = 0;
        VarType rt = analyze_function_returns(fn->body, &has_return);
        return rt == TYPE_STRING;
    }
    /* Fall back to the prescanned signatures (for forward references whose
       bodies have not been parsed yet). */
    FuncInfo *fi = find_funcinfo_by_cname(fname);
    if (!fi) {
        for (int i = 0; i < g_func_count; i++) {
            if (strcmp(g_funcs[i].src, fname) == 0) { fi = &g_funcs[i]; break; }
        }
    }
    if (fi && fi->has_return_type) return fi->return_type == TYPE_STRING;
    /* Unknown target: preserve legacy numeric behavior. */
    return 0;
}

/* Find a function binding in a non-root (local/nested) scope. Top-level
   functions are resolved through the global signature registry so overloads
   can be selected by arity. */
int find_local_func_binding(SymbolTable *table, const char *src, char **out) {
    for (Scope *s = table; s && s->parent; s = s->parent) {
        for (int i = 0; i < s->func_count; i++) {
            if (strcmp(s->funcs[i].src, src) == 0) {
                if (out) *out = ltng_strdup(s->funcs[i].cname);
                return 1;
            }
        }
    }
    return 0;
}

/* Re-evaluate a default expression captured by the signature pre-scan. Used
   when a call appears before the function body has been parsed. Errors are
   suppressed; on failure the expression falls back to an empty string. */
void eval_default_tokens(Lexer *lexer, SymbolTable *table, int start, int end,
                                char **out, int *out_cap, VarType *out_type) {
    if (*out) (*out)[0] = '\0';
    if (out_type) *out_type = TYPE_STRING;
    if (start < 0 || end <= start || end > lexer->count) {
        { if(*out_cap<3){char*_n=realloc(*out,3);if(_n){*out=_n;*out_cap=3;}} (*out)[0]='"';(*out)[1]='"';(*out)[2]=0; return; }
    }
    int n = end - start;
    Lexer tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.token_capacity = n + 8;
    tmp.tokens = (Token *)calloc((size_t)tmp.token_capacity, sizeof(Token));
    if (!tmp.tokens) { if(*out_cap<3){char*_n=realloc(*out,3);if(_n){*out=_n;*out_cap=3;}} (*out)[0]='"';(*out)[1]='"';(*out)[2]=0; return; }
    for (int i = 0; i < n; i++) tmp.tokens[i] = lexer->tokens[start + i];
    for (int i = n; i < tmp.token_capacity; i++) {
        tmp.tokens[i].type = TOKEN_EOF;
        { free(tmp.tokens[i].value); tmp.tokens[i].value = ltng_strdup(""); }
        tmp.tokens[i].line = n > 0 ? tmp.tokens[n - 1].line : 1;
        tmp.tokens[i].col = 1;
    }
    tmp.count = tmp.token_capacity;
    tmp.current = 0;
    ErrorState scratch;
    memset(&scratch, 0, sizeof(scratch));
    ErrorState *saved = parser_errors;
    parser_errors = &scratch;
    VarType t = collect_expression(&tmp, table, out, out_cap, 1, 1, NULL, NULL, NULL, NULL);
    parser_errors = saved;
    if (scratch.count > 0) {
        { if(*out_cap<3){char*_n=realloc(*out,3);if(_n){*out=_n;*out_cap=3;}} (*out)[0]='"';(*out)[1]='"';(*out)[2]=0; t = TYPE_STRING; }
    }
    if (out_type) *out_type = t;
    free(tmp.tokens);
}

/* Select the overload of 'src' that best matches the supplied arguments.
   Returns NULL when no candidate matches. *group_count receives the number of
   top-level candidates with this name. */
FuncInfo *resolve_overload(const char *src, ASTNode *args, int line, int col,
                                  int *group_count) {
    int positional = 0;
    for (ASTNode *a = args; a; a = a->next) {
        if (!a->for_var[0]) positional++;
    }
    *group_count = 0;
    FuncInfo *best = NULL;
    int best_variadic = 2, best_defaults = 1 << 30, best_exact = -1;
    int ambiguous = 0;
    for (int i = 0; i < g_func_count; i++) {
        FuncInfo *fi = &g_funcs[i];
        if (!fi->top_level || strcmp(fi->src, src) != 0) continue;
        (*group_count)++;

        int pcnt = fi->param_count > 0 ? fi->param_count : 0;
        int *provided = pcnt ? (int*)calloc((size_t)pcnt, sizeof(int)) : NULL;
        int ok = 1;
        for (ASTNode *a = args; a && ok; a = a->next) {
            if (!a->for_var[0]) continue;
            int found = -1;
            for (int p = 0; p < pcnt; p++) {
                if (fi->param_names[p] && strcmp(fi->param_names[p], a->for_var) == 0) { found = p; break; }
            }
            if (found < 0) { ok = 0; break; }
            if (provided[found]) { ok = 0; break; }
            provided[found] = 1;
        }
        if (!ok) { free(provided); continue; }

        int pos = positional;
        int used_defaults = 0;
        for (int p = 0; p < pcnt; p++) {
            if (provided[p]) continue;
            if (pos > 0) { pos--; provided[p] = 1; }
            else if (fi->has_default && fi->has_default[p]) { used_defaults++; }
            else { ok = 0; break; }
        }
        if (!ok) { free(provided); continue; }
        int rest_used = 0;
        if (pos > 0) {
            if (fi->is_variadic) rest_used = 1;
            else { ok = 0; }
        }
        if (!ok) { free(provided); continue; }

        int sv = fi->is_variadic ? 1 : 0;
        int exact = (used_defaults == 0 && !rest_used) ? 1 : 0;
        if (!best ||
            sv < best_variadic ||
            (sv == best_variadic && used_defaults < best_defaults) ||
            (sv == best_variadic && used_defaults == best_defaults && exact > best_exact)) {
            best = fi;
            best_variadic = sv;
            best_defaults = used_defaults;
            best_exact = exact;
            ambiguous = 0;
        } else if (sv == best_variadic && used_defaults == best_defaults && exact == best_exact) {
            if (best && !(fi->param_count == best->param_count &&
                          fi->required_count == best->required_count &&
                          fi->is_variadic == best->is_variadic)) {
                ambiguous = 1;
            }
        }
        free(provided);
    }
    if (best && ambiguous) {
        char *msg = _ltng_cfmt("Ambiguous call to overloaded function '%s'", src);
        error_state_add(parser_errors, line, col, msg ? msg : "");
        free(msg);
    }
    return best;
}

/* Expand supplied arguments to a full positional list for the chosen function,
   filling defaults and splitting extra positional arguments into the variadic
   rest list. Works with a parsed AST (fn_def) or a pre-scanned signature (fi). */
ASTNode *expand_call_args(Lexer *lexer, SymbolTable *table, FuncInfo *fi,
                                 ASTNode *fn_def, ASTNode *args, const char *fname,
                                 int line, int col, ASTNode **out_rest) {
    *out_rest = NULL;
    int pc = 0;
    int is_variadic = 0;
    if (fn_def) {
        for (ASTNode *p = fn_def->params; p; p = p->next) pc++;
        is_variadic = fn_def->is_variadic;
    } else if (fi) {
        pc = fi->param_count;
        is_variadic = fi->is_variadic;
    }
    if (pc < 0) pc = 0;

    ASTNode **param_node = pc ? (ASTNode**)calloc((size_t)pc, sizeof(ASTNode*)) : NULL;
    ASTNode **chosen = pc ? (ASTNode**)calloc((size_t)pc, sizeof(ASTNode*)) : NULL;
    int *provided = pc ? (int*)calloc((size_t)pc, sizeof(int)) : NULL;
    ASTNode *pn = fn_def ? fn_def->params : NULL;
    for (int p = 0; p < pc; p++) {
        param_node[p] = pn;
        if (pn) pn = pn->next;
    }

    for (ASTNode *a = args; a; a = a->next) {
        if (!a->for_var[0]) continue;
        int found = -1;
        for (int p = 0; p < pc; p++) {
            const char *pname = param_node[p] ? param_node[p]->for_var :
                                (fi && fi->param_names && p < fi->param_count && fi->param_names[p] ? fi->param_names[p] : "");
            if (pname && pname[0] && strcmp(pname, a->for_var) == 0) { found = p; break; }
        }
        if (found < 0) {
            char *msg = _ltng_cfmt("Unknown argument '%s' in call to '%s'", a->for_var, fname);
            error_state_add(parser_errors, line, col, msg ? msg : "");
            free(msg);
        } else if (provided[found]) {
            char *msg = _ltng_cfmt("Duplicate argument '%s' in call to '%s'", a->for_var, fname);
            error_state_add(parser_errors, line, col, msg ? msg : "");
            free(msg);
        } else {
            provided[found] = 1;
            chosen[found] = a;
        }
    }

    int pos_n = 0;
    for (ASTNode *a = args; a; a = a->next) if (!a->for_var[0]) pos_n++;
    ASTNode **pos = pos_n ? (ASTNode**)malloc((size_t)pos_n * sizeof(ASTNode*)) : NULL;
    int pos_i = 0;
    for (ASTNode *a = args; a; a = a->next) {
        if (a->for_var[0]) continue;
        if (pos_i < pos_n) pos[pos_i++] = a;
    }
    pos_i = 0;

    ASTNode *eff = NULL, *efflast = NULL;
    for (int p = 0; p < pc; p++) {
        ASTNode *outn = chosen[p];
        if (!outn) {
            if (pos_i < pos_n) {
                outn = pos[pos_i++];
            } else {
                int hasdef = param_node[p] ? param_node[p]->has_default :
                             (fi && fi->has_default && p < fi->param_count ? fi->has_default[p] : 0);
                if (hasdef) {
                    outn = create_node(NODE_PARAM, "", "");
                    outn->is_expr = 1;
                    outn->has_default = 1;
                    if (param_node[p]) {
                        { char *_s = _ltng_cfmt("%s", param_node[p]->value); free(outn->value); outn->value = _s ? _s : ltng_strdup(""); }
                        outn->var_type = param_node[p]->var_type;
                    } else {
                        char *defexpr = (char*)malloc(65536); if(defexpr) defexpr[0]=0;
                        int defexpr_cap = 65536;
                        VarType dvt = TYPE_STRING;
                        eval_default_tokens(lexer, table, fi->def_start[p], fi->def_end[p],
                                            &defexpr, &defexpr_cap, &dvt);
                        { char *_s = _ltng_cfmt("%s", defexpr); free(outn->value); outn->value = _s ? _s : ltng_strdup(""); }
                        outn->var_type = dvt;
                    }
                } else {
                    const char *pname = param_node[p] ? param_node[p]->for_var : (fi && fi->param_names && p < fi->param_count && fi->param_names[p] ? fi->param_names[p] : "?");
                    char *msg = _ltng_cfmt("Missing required argument '%s' in call to '%s'", pname, fname);
                    error_state_add(parser_errors, line, col, msg ? msg : "");
                    free(msg);
                    outn = create_node(NODE_PARAM, "", "\"\"");
                    outn->var_type = TYPE_STRING;
                    outn->is_expr = 1;
                }
            }
        }
        outn->next = NULL;
        if (!eff) eff = efflast = outn;
        else { efflast->next = outn; efflast = outn; }
    }

    if (pos_i < pos_n) {
        if (is_variadic) {
            ASTNode *rest = NULL, *rlast = NULL;
            for (; pos_i < pos_n; pos_i++) {
                ASTNode *rn = pos[pos_i];
                rn->next = NULL;
                if (!rest) rest = rlast = rn;
                else { rlast->next = rn; rlast = rn; }
            }
            *out_rest = rest;
        } else {
            char *msg = _ltng_cfmt("Too many arguments in call to '%s'", fname);
            error_state_add(parser_errors, line, col, msg ? msg : "");
            free(msg);
        }
    }
    free(param_node);
    free(chosen);
    free(provided);
    free(pos);
    return eff;
}

ASTNode *parse_call_node(Lexer *lexer, SymbolTable *table, const char *fname_tok_val, int line, int col, ASTNode *first_arg) {
    /* assumes fname IDENT consumed, '(' consumed */
    ASTNode *call = create_node(NODE_CALL, "", "");
    call->line = line;
    call->col = col;
    char *fm = ltng_strdup("");
    int indirect = 0;
    int indirect_dynamic = 0;
    int direct_overload = 0;
    int fr_pc = -1;
    char *fr_cname = NULL;
    char *local_cname = NULL;
    int has_local = find_local_func_binding(table, fname_tok_val, &local_cname);
    int scoped = is_function_name_scoped(table, fname_tok_val);
    /* indirect call through tracked function-reference variable? */
    if (!scoped &&
        symbol_table_get_func_ref(table, fname_tok_val, &fr_cname, &fr_pc)) {
        indirect = 1;
        { free(fm); fm = ltng_strdup(fr_cname); }
    } else if (!scoped) {
        /* dynamic indirect call through plain variable holding function name */
        char *mangled = mangle_name_alloc(fname_tok_val);
        if (symbol_table_has(table, mangled) || symbol_table_has(table, fname_tok_val)) {
            indirect_dynamic = 1;
            const char *vcname = symbol_table_get_cname(table, mangled);
            if (!vcname || !vcname[0]) vcname = symbol_table_get_cname(table, fname_tok_val);
            { free(call->for_var); call->for_var = ltng_strdup(vcname ? vcname : mangled); }
            { free(fm); fm = ltng_strdup("__ltng_indirect"); }
        } else {
            { char *_rn = resolve_func_name(table, fname_tok_val);
              if (_rn) { free(fm); fm = _rn; }
              else { char *_mn = mangle_name_alloc(fname_tok_val); free(fm); fm = _mn; } }
        }
    } else if (has_local) {
        { free(fm); fm = ltng_strdup(local_cname); }
    } else {
        /* top-level call: overload is selected after the arguments are parsed */
        direct_overload = 1;
        { char *_mn = mangle_name_alloc(fname_tok_val); free(fm); fm = _mn; }
    }
    /* If calling a tracked function-ref that is actually a closure (has captures),
       we must dispatch through the variable's closure value (encoded). */
    if (indirect && !indirect_dynamic) {
        ASTNode *tfn = find_function_ast_by_name(fm);
        if (!tfn) tfn = find_function_ast_by_name(fr_cname);
        if (tfn && tfn->capture_count > 0) {
            char *mangled_v = mangle_name_alloc(fname_tok_val);
            const char *vcname = symbol_table_get_cname(table, mangled_v);
            if (!vcname || !vcname[0]) vcname = symbol_table_get_cname(table, fname_tok_val);
            if (!vcname || !vcname[0]) vcname = mangled_v;
            { free(call->for_var); call->for_var = ltng_strdup(vcname); }
            { free(fm); fm = ltng_strdup("__ltng_indirect"); }
            indirect_dynamic = 1;
        }
    }
    if (!direct_overload) {
        { free(call->name); call->name = ltng_strdup(fm); }
        if (!indirect_dynamic && function_first_param_is_base(fm)) {
            call->is_base_assign = 1;
        }
    }
    ASTNode *args = NULL, *alast = NULL;
    int has_named_arg = 0;
    if (first_arg) { args = alast = first_arg; first_arg->next = NULL; }
    if (lexer_peek(lexer).type != TOKEN_RPAREN) {
        while (1) {
            Token at = lexer_peek(lexer);
            int named_arg = 0;
            char *named_key = NULL;
            if (at.type == TOKEN_IDENT) {
                int saved = lexer->current;
                Token idtok = lexer_next(lexer);
                Token eq = lexer_peek(lexer);
                if (eq.type == TOKEN_EQUALS) {
                    lexer_next(lexer); /* consume '=' */
                    named_arg = 1;
                    free(named_key); named_key = ltng_strdup(idtok.value);
                } else {
                    lexer->current = saved;
                }
            }
            /* String literals passed directly */
            if (at.type == TOKEN_STRING && !named_arg) {
                lexer_next(lexer);
                ASTNode *an = create_node(NODE_PARAM, "", "");
                { char *_s = _ltng_cfmt("\"%s\"", at.value); free(an->value); an->value = _s ? _s : ltng_strdup(""); }
                an->var_type = TYPE_STRING;
                an->is_expr = 1;
                if (!args) args = alast = an; else { alast->next = an; alast = an; }
            } else if (at.type == TOKEN_FUNCTION && !named_arg) {
                Token after_fn = lexer->tokens[lexer->current + 1];
                if (after_fn.type == TOKEN_LPAREN) {
                    ASTNode *fn = parse_function(lexer, table, g_enclosing_cname);
                    if (fn && g_program_ref) {
                        fn->next = g_program_ref->functions;
                        g_program_ref->functions = fn;
                    }
                    ASTNode *an = create_node(NODE_PARAM, "", "");
                    { char *_s = _ltng_cfmt("\"%s\"", fn ? fn->name : ""); free(an->value); an->value = _s ? _s : ltng_strdup(""); }
                    an->var_type = TYPE_STRING;
                    an->is_expr = 1;
                    if (!args) args = alast = an; else { alast->next = an; alast = an; }
                } else {
                    /* named function ref not supported as expression here; fall through */
                    char *aexpr = (char*)malloc(65536); if(aexpr) aexpr[0]=0;
                    VarType avt = collect_expression(lexer, table, &aexpr, &(int){65536}, 1, 1, NULL, NULL, NULL, NULL);
                    ASTNode *an = create_node(NODE_PARAM, "", aexpr);
                    an->var_type = avt;
                    an->is_expr = 1;
                    if (!args) args = alast = an; else { alast->next = an; alast = an; }
                }
            } else if (at.type == TOKEN_CHAR && !named_arg) {
                lexer_next(lexer);
                ASTNode *an = create_node(NODE_PARAM, "", "");
                if (at.value[0]) { char *_s = _ltng_cfmt("'%s'", at.value); free(an->value); an->value = _s ? _s : ltng_strdup(""); }
                else { free(an->value); an->value = ltng_strdup("'\\0'"); }
                an->var_type = TYPE_STRING;
                an->is_expr = 1;
                if (!args) args = alast = an; else { alast->next = an; alast = an; }
            } else if (!named_arg && at.type == TOKEN_IDENT && is_callable_ident(table, at.value)) {
                /* function call argument - pass result as string buffer */
                char *cexpr = (char*)malloc(65536); if(cexpr) cexpr[0]=0;
                VarType carg_type = TYPE_STRING;
                if (try_parse_call_arg(lexer, table, &cexpr, &(int){65536}, &carg_type)) {
                    ASTNode *an = create_node(NODE_PARAM, "", cexpr);
                    an->var_type = carg_type;
                    an->is_expr = 1;
                    an->is_variable = 1; /* pass pointer directly (call returns char* buffer) */
                    if (!args) args = alast = an; else { alast->next = an; alast = an; }
                } else {
                    /* complex expression involving call, or plain variable (since is_callable_ident now includes vars) */
                    VarType avt = collect_expression(lexer, table, &cexpr, &(int){65536}, 1, 1, NULL, NULL, NULL, NULL);
                    ASTNode *an = create_node(NODE_PARAM, "", cexpr);
                    an->var_type = avt;
                    an->is_expr = 1;
                    if ((avt == TYPE_STRING || avt == TYPE_OBJECT) && cexpr[0] != '"' && cexpr[0] != '\0') {
                        int simple = 1;
                        for (char *p = cexpr; *p; p++) {
                            if (*p==' '||*p=='\t'||*p=='('||*p==')'||*p=='+'||*p=='*'||*p=='-'||*p=='/'||*p=='%'||*p=='='||*p=='!'||*p=='<'||*p=='>'||*p=='&'||*p=='|'||*p=='?') { simple = 0; break; }
                        }
                        if (simple) an->is_variable = 1;
                    }
                    if (named_arg) {
                        { free(an->for_var); an->for_var = ltng_strdup(named_key); }
                        has_named_arg = 1; (void)has_named_arg;
                    }
                    if (!args) args = alast = an; else { alast->next = an; alast = an; }
                }
            } else {
                /* Parse as expression (or named arg value) */
                char *aexpr = (char*)malloc(65536); if(aexpr) aexpr[0]=0;
                VarType avt = collect_expression(lexer, table, &aexpr, &(int){65536}, 1, 1, NULL, NULL, NULL, NULL);
                ASTNode *an = create_node(NODE_PARAM, "", aexpr);
                an->var_type = avt;
                an->is_expr = 1;
                if ((avt == TYPE_STRING || avt == TYPE_OBJECT) && aexpr[0] != '"' && aexpr[0] != '\0') {
                    int simple = 1;
                    for (char *p = aexpr; *p; p++) {
                        if (*p==' '||*p=='\t'||*p=='('||*p==')'||*p=='+'||*p=='*'||*p=='-'||*p=='/'||*p=='%'||*p=='='||*p=='!'||*p=='<'||*p=='>'||*p=='&'||*p=='|'||*p=='?') { simple = 0; break; }
                    }
                    if (simple) an->is_variable = 1;
                }
                if (named_arg) {
                    { free(an->for_var); an->for_var = ltng_strdup(named_key); }
                    has_named_arg = 1;
                }
                if (!args) args = alast = an; else { alast->next = an; alast = an; }
            }
            if (lexer_peek(lexer).type == TOKEN_COMMA) { lexer_next(lexer); continue; }
            break;
        }
    }
    if (lexer_peek(lexer).type == TOKEN_RPAREN) lexer_next(lexer);
    else { error_state_add(parser_errors, line, col, "Expected ')' after function arguments"); }

    /* Resolve the target and expand defaults / variadic arguments. */
    ASTNode *fn_def = NULL;
    FuncInfo *fi = NULL;
    if (direct_overload) {
        int group = 0;
        fi = resolve_overload(fname_tok_val, args, line, col, &group);
        int no_match = 0;
        if (!fi) {
            if (group > 1) {
                int supplied = 0;
                for (ASTNode *a = args; a; a = a->next) supplied++;
                char *msg = _ltng_cfmt("No matching overload for '%s' with %d argument(s)", fname_tok_val, supplied);
                error_state_add(parser_errors, line, col, msg ? msg : "");
                free(msg);
            }
            no_match = 1;
            (void)no_match;
            /* Pick the first candidate so codegen can continue after an error. */
            for (int i = 0; i < g_func_count; i++) {
                if (g_funcs[i].top_level && strcmp(g_funcs[i].src, fname_tok_val) == 0) {
                    fi = &g_funcs[i];
                    break;
                }
            }
        }
        if (fi) {
            { free(fm); fm = ltng_strdup(fi->name); }
            fn_def = find_function_node_by_cname(fi->name);
        }
        { free(call->name); call->name = ltng_strdup(fm); }
        if (!indirect_dynamic && function_first_param_is_base(fm)) {
            call->is_base_assign = 1;
        }
    } else if (!indirect_dynamic) {
        fn_def = find_function_node_by_cname(fm);
        if (!fn_def) fn_def = find_function_ast_by_name(fm);
    }

    if (!indirect_dynamic && (fi || fn_def)) {
        ASTNode *rest = NULL;
        ASTNode *eff = expand_call_args(lexer, table, fi, fn_def, args,
                                        fname_tok_val, line, col, &rest);
        call->rest_args = rest;
        call->args = eff;
    } else {
        call->args = args;
    }
    return call;
}

void record_return_obj_schema(const char *obj_base) {
    if (!obj_base || !obj_base[0] || !(g_enclosing_cname ? g_enclosing_cname[0] : 0)) return;
    if (!obj_lookup(obj_base)) return;
    FuncInfo *fi = find_funcinfo_by_cname(g_enclosing_cname);
    if (fi && fi->return_obj_base[0] == '\0') {
        { free(fi->return_obj_base); fi->return_obj_base = ltng_strdup(obj_base); }
    }
    ASTNode *fn = find_function_node_by_cname(g_enclosing_cname);
    if (fn && fn->return_obj_base[0] == '\0') {
        { free(fn->return_obj_base); fn->return_obj_base = ltng_strdup(obj_base); }
    }
}

void parse_return(Lexer *lexer, SymbolTable *table) {
    Token kw = lexer_next(lexer);
    if (!g_in_function) {
        error_state_add(parser_errors, kw.line, kw.col,
            "'return' can only be used inside a function");
        skip_to_newline(lexer);
        return;
    }
    char *expr = (char*)malloc(65536); if(expr) expr[0]=0;
    expr[0] = '\0';
    VarType etype = TYPE_INT;
    Token next = lexer_peek(lexer);
    if (next.type != TOKEN_NEWLINE && next.type != TOKEN_EOF && next.type != TOKEN_RBRACE) {
        /* anonymous function expression: return function(params) { ... } */
        if (next.type == TOKEN_FUNCTION) {
            Token after_fn = lexer->tokens[lexer->current + 1];
            if (after_fn.type == TOKEN_LPAREN) {
                ASTNode *fn = parse_function(lexer, table, g_enclosing_cname);
                if (fn && g_program_ref) { fn->next = g_program_ref->functions; g_program_ref->functions = fn; }
                char *fval = (char*)malloc(65536); if(fval) fval[0]=0;
                if (fn && fn->capture_count > 0) {
                    char *expr = (char*)malloc(65536); if(expr) expr[0]=0;
                    SNPRINTF_FULL(expr, "\"%s\"", fn->name);
                    for (int ci = 0; ci < fn->capture_count; ci++) {
                        char *tmp = (char*)malloc(65536); if(tmp) tmp[0]=0;
                        SNPRINTF_FULL(tmp, "_ltng_add_sn(_ltng_add_sn(%s,\"\\001\"),_ltng_ptr_to_str((void*)%s))", expr, fn->captures[ci]);
                        SNPRINTF_FULL(expr, "%s", tmp);
                    }
                    SNPRINTF_FULL(fval, "%s", expr);
                } else {
                    SNPRINTF_FULL(fval, "\"%s\"", fn ? fn->name : "");
                }
                ASTNode *node = create_node(NODE_RETURN, "", fval);
                node->line = kw.line; node->col = kw.col;
                node->var_type = TYPE_STRING;
                node->is_expr = 1;
                emit(node);
                return;
            }
        }
        /* named function reference: return someFunc */
        if (next.type == TOKEN_IDENT) {
            Token after_fnref = (lexer->current + 1 < lexer->count) ? lexer->tokens[lexer->current + 1] : next;
            if (after_fnref.type == TOKEN_NEWLINE || after_fnref.type == TOKEN_EOF || after_fnref.type == TOKEN_RBRACE) {
                char *mangled_ref = mangle_name_alloc(next.value);
                int is_var = symbol_table_has(table, mangled_ref) || symbol_table_has(table, next.value);
                char *ref_cname = NULL; int ref_pc = -1;
                if (!is_var && resolve_func_ref_info(table, next.value, &ref_cname, &ref_pc)) {
                    lexer_next(lexer);
                    char *fval = (char*)malloc(65536); if(fval) fval[0]=0; SNPRINTF_FULL(fval, "\"%s\"", ref_cname);
                    ASTNode *node = create_node(NODE_RETURN, "", fval);
                    node->line = kw.line; node->col = kw.col;
                    node->var_type = TYPE_STRING;
                    node->is_expr = 1;
                    emit(node);
                    return;
                }
            }
        }
        /* sole string literal (only if nothing follows - otherwise full expression) */
        if (next.type == TOKEN_STRING) {
            int saved = lexer->current;
            lexer_next(lexer);
            Token after = lexer_peek(lexer);
            lexer->current = saved;
            if (after.type == TOKEN_NEWLINE || after.type == TOKEN_EOF || after.type == TOKEN_RBRACE) {
                lexer_next(lexer);
                SNPRINTF_FULL(expr, "\"%s\"", next.value);
                etype = TYPE_STRING;
                ASTNode *node = create_node(NODE_RETURN, "", expr);
    node->line = kw.line; node->col = kw.col;
                node->var_type = etype;
                node->is_expr = 1;
                emit(node);
                return;
            }
            /* fall through to full expression parsing */
        }
        /* sole bool literal */
        if (next.type == TOKEN_IDENT && is_bool_value(next.value)) {
            int saved = lexer->current;
            Token btok = lexer_next(lexer);
            Token after = lexer_peek(lexer);
            lexer->current = saved;
            if (after.type == TOKEN_NEWLINE || after.type == TOKEN_EOF || after.type == TOKEN_RBRACE) {
                lexer_next(lexer);
                SNPRINTF_FULL(expr, "%s", (strcmp(btok.value, "true") == 0 || strcmp(btok.value, "t") == 0) ? "1" : "0");
                etype = TYPE_BOOL;
                ASTNode *node = create_node(NODE_RETURN, "", expr);
                node->line = kw.line; node->col = kw.col;
                node->var_type = etype;
                node->is_expr = 1;
                emit(node);
                return;
            }
            /* fall through to full expression parsing */
        }
        /* sole variable - pass through while preserving its type when known */
        if (next.type == TOKEN_IDENT && !is_bool_value(next.value) && !is_type_keyword(next.value) && !is_function_name_scoped(table, next.value)) {
            int saved = lexer->current;
            Token id = lexer_next(lexer);
            Token after = lexer_peek(lexer);
            lexer->current = saved;
            if (after.type == TOKEN_NEWLINE || after.type == TOKEN_EOF || after.type == TOKEN_RBRACE) {
                lexer_next(lexer);
                char *m = mangle_name_alloc(id.value);
                const char *cname = symbol_table_get_cname(table, m);
                SNPRINTF_FULL(expr, "%s", cname);
                if (expr[0]) {} /* growable */
                VarType vtype2 = TYPE_STRING;
                if (symbol_table_has(table, m)) vtype2 = symbol_table_get(table, m);
                /* Preserve legacy pass-through for dynamic/string/object values, but keep
                   typed numeric/bool returns for known-typed variables. Value-initialized
                   locals are dtype-unlocked by default, so infer from current known type
                   (excluding untyped/dynamic parameters). For parameters with default
                   values of known types, prefer that type for inference/return. */
                if (vtype2 == TYPE_ARRAY) {
                    etype = TYPE_ARRAY;
                    SNPRINTF_FULL(expr, "_ltng_ah_pack(%s_data, %s_len, %s_cap)", cname, cname, cname);
                    if ((g_enclosing_cname ? g_enclosing_cname[0] : 0)) {
                        FuncInfo *efi = find_funcinfo_by_cname(g_enclosing_cname);
                        if (efi) { efi->return_type = TYPE_ARRAY; efi->has_return_type = 1; }
                    }
                } else if (vtype2 == TYPE_OBJECT) {
                    etype = TYPE_OBJECT;
                } else if ((vtype2 == TYPE_INT || vtype2 == TYPE_FLOAT || vtype2 == TYPE_BOOL) &&
                           (symbol_table_get_locked(table, m) || !symbol_table_is_param(table, m))) {
                    etype = vtype2;
                    if (vtype2 == TYPE_INT) SNPRINTF_FULL(expr, "atoll(%s)", cname);
                    else if (vtype2 == TYPE_FLOAT) SNPRINTF_FULL(expr, "atof(%s)", cname);
                    else SNPRINTF_FULL(expr, "(%s[0]=='t'?1:0)", cname);
                } else {
                    /* Try param default type */
                    VarType pdt = TYPE_NULL;
                    if ((g_enclosing_cname ? g_enclosing_cname[0] : 0)) {
                        FuncInfo *efi = find_funcinfo_by_cname(g_enclosing_cname);
                        if (efi) {
                            for (int pi = 0; pi < efi->param_count; pi++) {
                                if (strcmp(efi->param_names[pi], id.value) == 0) {
                                    if (efi->has_default[pi]) pdt = efi->def_type[pi];
                                    break;
                                }
                            }
                        }
                    }
                    if (pdt == TYPE_INT || pdt == TYPE_FLOAT || pdt == TYPE_BOOL) {
                        etype = pdt;
                        if (pdt == TYPE_INT) SNPRINTF_FULL(expr, "atoll(%s)", cname);
                        else if (pdt == TYPE_FLOAT) SNPRINTF_FULL(expr, "atof(%s)", cname);
                        else SNPRINTF_FULL(expr, "(%s[0]=='t'?1:0)", cname);
                    } else {
                        etype = TYPE_STRING;
                    }
                }
                ASTNode *node = create_node(NODE_RETURN, "", expr);
                node->var_type = etype;
                node->is_expr = 1;
                node->is_variable = (etype == TYPE_OBJECT || etype == TYPE_STRING) ? 1 : 0;
                /* Mark pure pass-through of an untyped parameter (no default type coercion)
                   so call sites can specialize return type when feasible. */
                if (etype == TYPE_STRING && symbol_table_is_param(table, m) && (g_enclosing_cname ? g_enclosing_cname[0] : 0)) {
                    int _is_pt = 1;
                    /* exclude if param has a concrete default type (we'd have taken pdt branch above) */
                    VarType _pdt2 = TYPE_NULL;
                    FuncInfo *_efi2 = find_funcinfo_by_cname(g_enclosing_cname);
                    if (_efi2) {
                        for (int _pi = 0; _pi < _efi2->param_count; _pi++) {
                            if (strcmp(_efi2->param_names[_pi], id.value) == 0) {
                                if (_efi2->has_default[_pi] && _efi2->def_type[_pi] != TYPE_NULL &&
                                    (_efi2->def_type[_pi] == TYPE_INT || _efi2->def_type[_pi] == TYPE_FLOAT || _efi2->def_type[_pi] == TYPE_BOOL))
                                    _is_pt = 0;
                                if (_is_pt) node->passthrough_param = _pi;
                                break;
                            }
                        }
                    }
                    (void)_pdt2;
                }
                if (etype == TYPE_OBJECT) record_return_obj_schema(cname);
                emit(node);
                return;
            }
        }
        /* sole function call */
        /* Skip try_parse_sole_call_expr for array.push/pop */
        if (!(lexer->current + 2 < lexer->count &&
              lexer->tokens[lexer->current + 1].type == TOKEN_DOT &&
              lexer->tokens[lexer->current + 2].type == TOKEN_IDENT &&
              (strcmp(lexer->tokens[lexer->current + 2].value, "push") == 0 ||
               strcmp(lexer->tokens[lexer->current + 2].value, "pop") == 0))) {
            char *sole_call_cname = NULL;
            VarType sole_call_type = TYPE_STRING;
            if (try_parse_sole_call_expr(lexer, table, &expr, &(int){65536}, 1, &sole_call_type, &sole_call_cname)) {
                etype = sole_call_type;
                if (etype == TYPE_NULL || etype == TYPE_VOID) etype = TYPE_STRING;
                if (etype == TYPE_ARRAY && (g_enclosing_cname ? g_enclosing_cname[0] : 0)) {
                    FuncInfo *efi = find_funcinfo_by_cname(g_enclosing_cname);
                    if (efi) { efi->return_type = TYPE_ARRAY; efi->has_return_type = 1; }
                }
                ASTNode *node = create_node(NODE_RETURN, "", expr);
                node->var_type = etype;
                node->is_expr = 1;
                node->fs_sc = 1; /* marker: value is a bare call returning char* buffer */
                emit(node);
                return;
            }
        }
        /* object literal return: return { default = "x" v = 1 } */
        if (next.type == TOKEN_LBRACE) {
            lexer_next(lexer); /* consume { */
            char *temp_mangled = _ltng_cfmt("__ret_obj_%d", g_ret_obj_count++);
            symbol_table_add(table, temp_mangled, TYPE_OBJECT);
            parse_object_literal(lexer, table, temp_mangled, 0, 0);
            /* parse_object_literal emitted a NODE_LET to init fields; now emit return */
            ASTNode *node = create_node(NODE_RETURN, "", temp_mangled);
            node->var_type = TYPE_OBJECT;
            node->is_expr = 1;
            node->is_variable = 1;
            record_return_obj_schema(temp_mangled);
            emit(node);
            return;
        }
        /* Detect sole conversion call for return type inference */
        VarType conv_override = TYPE_NULL;
        {
            Token ct = lexer_peek(lexer);
            if (ct.type == TOKEN_IDENT && is_type_conversion(ct.value) &&
                lexer->current + 1 < lexer->count && lexer->tokens[lexer->current + 1].type == TOKEN_LPAREN) {
                /* check if sole: conv(...) followed by end-of-statement */
                int i = lexer->current + 2, depth = 1, sole = 0;
                while (i < lexer->count && depth > 0) {
                    TokenType tt = lexer->tokens[i].type;
                    if (tt == TOKEN_LPAREN) depth++;
                    else if (tt == TOKEN_RPAREN) { depth--; if (depth == 0) { i++; break; } }
                    else if (tt == TOKEN_NEWLINE || tt == TOKEN_EOF) { depth = -1; break; }
                    i++;
                }
                if (depth == 0) {
                    if (i >= lexer->count) sole = 1;
                    else { TokenType tt = lexer->tokens[i].type; if (tt == TOKEN_NEWLINE || tt == TOKEN_EOF || tt == TOKEN_RBRACE) sole = 1; }
                }
                if (sole) {
                    if (strcmp(ct.value, "toint") == 0 || strcmp(ct.value, "int") == 0) conv_override = TYPE_INT;
                    else if (strcmp(ct.value, "tofloat") == 0 || strcmp(ct.value, "float") == 0) conv_override = TYPE_FLOAT;
                    else if (strcmp(ct.value, "tobool") == 0 || strcmp(ct.value, "bool") == 0) conv_override = TYPE_BOOL;
                    else if (strcmp(ct.value, "tochar") == 0 || strcmp(ct.value, "char") == 0) conv_override = TYPE_CHAR;
                    else if (strcmp(ct.value, "tostring") == 0 || strcmp(ct.value, "string") == 0) conv_override = TYPE_STRING;
                }
            }
        }
        int _esl=0,_esc=0,_eel=0,_eec=0;
        etype = collect_expression(lexer, table, &expr, &(int){65536}, 0, 0, NULL, NULL, NULL, NULL);
        _esl=g_expr_sl; _esc=g_expr_sc; _eel=g_expr_el; _eec=g_expr_ec;
        ASTNode *node = create_node(NODE_RETURN, "", expr);
        node->v_sl=_esl; node->v_sc=_esc; node->v_el=_eel; node->v_ec=_eec;
        node->line = kw.line; node->col = kw.col;
        node->var_type = etype;
        node->is_expr = 1;
        if (conv_override != TYPE_NULL) node->fs_sl = 1000 + (int)conv_override; /* marker: sole conversion return */
        emit(node);
        return;
    }
    /* bare return (no value) */
    ASTNode *node = create_node(NODE_RETURN, "", expr);
    node->line = kw.line; node->col = kw.col;
    node->var_type = TYPE_VOID;
    node->is_expr = 0;
    emit(node);
}

void parse_exit(Lexer *lexer, SymbolTable *table) {
    Token kw = lexer_next(lexer);
    char *expr = (char*)malloc(65536); if(expr) expr[0]=0;
    expr[0] = '\0';
    Token next = lexer_peek(lexer);
    if (next.type == TOKEN_NEWLINE || next.type == TOKEN_EOF || next.type == TOKEN_RBRACE) {
        error_state_add(parser_errors, kw.line, kw.col,
            "Expected integer expression after 'exit'");
        skip_to_newline(lexer);
        return;
    }
    VarType etype = collect_expression(lexer, table, &expr, &(int){65536}, 0, 0, NULL, NULL, NULL, NULL);
    int esl=g_expr_sl,esc=g_expr_sc,eel=g_expr_el,eec=g_expr_ec;
    if (etype != TYPE_INT && etype != TYPE_FLOAT) {
        error_state_add(parser_errors, kw.line, kw.col,
            "Expected integer expression after 'exit'");
        skip_to_newline(lexer);
        return;
    }
    ASTNode *node = create_node(NODE_EXIT, "", expr);
    node->line = kw.line; node->col = kw.col;
    node->v_sl=esl; node->v_sc=esc; node->v_el=eel; node->v_ec=eec;
    node->var_type = etype;
    node->is_expr = 1;
    emit(node);
}


int is_ident_start_char(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
int is_ident_char(char c) { return is_ident_start_char(c) || (c >= '0' && c <= '9'); }

void add_capture_name(char ***names, int *count, int *cap, const char *name) {
    if (!names || !count || !cap || !name || !name[0]) return;
    if (strncmp(name, "_ltng", 5) == 0) return;
    for (int i = 0; i < *count; i++) if ((*names)[i] && strcmp((*names)[i], name) == 0) return;
    if (*count >= *cap) {
        int nc = *cap ? (*cap * 2) : 32;
        char **nn = (char**)realloc(*names, (size_t)nc * sizeof(char*));
        if (!nn) return;
        *names = nn; *cap = nc;
    }
    (*names)[*count] = ltng_strdup(name);
    (*count)++;
}

void collect_idents_from_str(const char *s, char ***names, int *count, int *cap) {
    if (!s) return;
    for (int i = 0; s[i]; ) {
        if (is_ident_start_char(s[i])) {
            int start = i;
            while (s[i] && is_ident_char(s[i])) i++;
            int len = i - start;
            char *buf = (char*)malloc((size_t)len + 1);
            if (!buf) return;
            memcpy(buf, s + start, (size_t)len);
            buf[len] = 0;
            add_capture_name(names, count, cap, buf);
            free(buf);
        } else if (s[i] == '"') {
            i++; while (s[i] && s[i] != '"') { if (s[i] == '\\' && s[i+1]) i += 2; else i++; }
            if (s[i] == '"') i++;
        } else if (s[i] == '\'') {
            i++; while (s[i] && s[i] != '\'') { if (s[i] == '\\' && s[i+1]) i += 2; else i++; }
            if (s[i] == '\'') i++;
        } else {
            i++;
        }
    }
}

void collect_declared_locals(ASTNode *node, char ***names, int *count, int *cap, int depth) {
    if (!node || depth > 128) return;
    if (node->type == NODE_LET || node->type == NODE_CONST) {
        if (node->name[0]) add_capture_name(names, count, cap, node->name);
    }
    if (node->type == NODE_FUNCTION) return;
    collect_declared_locals(node->body, names, count, cap, depth + 1);
    collect_declared_locals(node->orelse, names, count, cap, depth + 1);
    collect_declared_locals(node->else_body, names, count, cap, depth + 1);
    collect_declared_locals(node->cases, names, count, cap, depth + 1);
    collect_declared_locals(node->next, names, count, cap, depth + 1);
}

void collect_used_idents(ASTNode *node, char ***names, int *count, int *cap, int depth) {
    if (!node || depth > 128) return;
    if (node->type == NODE_FUNCTION) { collect_used_idents(node->next, names, count, cap, depth); return; }
    if (node->type == NODE_READ && node->name[0]) add_capture_name(names, count, cap, node->name);
    if (node->type == NODE_CALL) {
        if (strcmp(node->name, "__ltng_indirect") == 0 && node->for_var[0])
            add_capture_name(names, count, cap, node->for_var);
    }
    collect_idents_from_str(node->value, names, count, cap);
    collect_idents_from_str(node->for_start, names, count, cap);
    collect_idents_from_str(node->for_end, names, count, cap);
    collect_idents_from_str(node->for_step, names, count, cap);
    collect_idents_from_str(node->array_index, names, count, cap);
    collect_used_idents(node->body, names, count, cap, depth + 1);
    collect_used_idents(node->orelse, names, count, cap, depth + 1);
    collect_used_idents(node->else_body, names, count, cap, depth + 1);
    collect_used_idents(node->cases, names, count, cap, depth + 1);
    collect_used_idents(node->args, names, count, cap, depth + 1);
    collect_used_idents(node->params, names, count, cap, depth + 1);
    collect_used_idents(node->next, names, count, cap, depth + 1);
}

int name_in_list(char **names, int count, const char *name) {
    for (int i = 0; i < count; i++) if (names[i] && strcmp(names[i], name) == 0) return 1;
    return 0;
}

ASTNode *parse_function(Lexer *lexer, SymbolTable *table, const char *enclosing_cname) {
    Token kw = lexer_next(lexer); /* 'function' */
    int is_anon = 0;
    Token name = {0};
    if (lexer_peek(lexer).type == TOKEN_LPAREN) {
        /* anonymous function: function(params) { ... } */
        is_anon = 1;
        name.type = TOKEN_IDENT;
        name.line = kw.line;
        name.col = kw.col;
        { char *_s = _ltng_cfmt("__anon_%d", g_anon_count++); free(name.value); name.value = _s ? _s : ltng_strdup(""); }
    } else {
        name = lexer_next(lexer);
        if (!token_is_name(name)) {
            error_state_add(parser_errors, name.line, name.col,
                "Expected function name or '(' after 'function'");
            skip_to_newline(lexer);
            return NULL;
        }
        if (is_ltng_reserved(name.value) || is_ltng_keyword(name.value)) {
            error_state_add(parser_errors, name.line, name.col,
                "Cannot use reserved word as function name");
            skip_to_newline(lexer);
            return NULL;
        }
    }

    char *fname_short = mangle_name_alloc(name.value);
    char *cname = NULL;
    int top_level_named = (!is_anon && !(enclosing_cname && enclosing_cname[0]));
    FuncInfo *sig = NULL;
    if (top_level_named) {
        for (int i = 0; i < g_func_count; i++) {
            if (g_funcs[i].line == name.line && g_funcs[i].col == name.col &&
                strcmp(g_funcs[i].src, name.value) == 0) {
                sig = &g_funcs[i];
                break;
            }
        }
    }
    if (sig) {
        cname = ltng_strdup(sig->name);
    } else if (enclosing_cname && enclosing_cname[0]) {
        cname = _ltng_cfmt("%s__%s", enclosing_cname, fname_short);
    } else {
        cname = ltng_strdup(fname_short);
    }
    /* reject duplicate function definition in same scope (named functions) */
    if (!is_anon && g_program_ref) {
        for (ASTNode *ex = g_program_ref->functions; ex; ex = ex->next) {
            if (strcmp(ex->name, cname) == 0) {
                char *msg = _ltng_cfmt("Function '%s' already defined", name.value);
                error_state_add(parser_errors, name.line, name.col, msg ? msg : "");
                free(msg);
                skip_to_newline(lexer);
                return NULL;
            }
        }
    }
    /* bind short name in enclosing scope (so calls inside it resolve) - named only */
    if (!is_anon && table) symbol_table_add_func(table, name.value, cname, 0);
    /* register lifted C name globally for codegen/prototypes (nested/anon only;
       top-level signatures were already registered by prescan_functions). */
    if (!sig) {
        ensure_func_cap(); if (g_func_count < g_func_cap) {
            FuncInfo *nf = &g_funcs[g_func_count++];
            funcinfo_init(nf);
            free(nf->name); nf->name = ltng_strdup(cname);
            free(nf->src); nf->src = ltng_strdup(name.value);
            nf->line = name.line;
            nf->col = name.col;
        }
    }

    Token lp = lexer_next(lexer);
    if (lp.type != TOKEN_LPAREN) {
        error_state_add(parser_errors, lp.line, lp.col,
            is_anon ? "Expected '(' after 'function'" : "Expected '(' after function name");
        skip_to_newline(lexer);
        return NULL;
    }

    /* Parse parameters */
    ASTNode *params = NULL;
    ASTNode *plast = NULL;
    int pcount = 0;
    int variadic = 0;
    char *variadic_name = NULL;
    /* function body scope: parent is enclosing function scope
       (so sibling nested functions are visible to each other) */
    Scope *func_scope = (Scope *)malloc(sizeof(Scope));
    if (!func_scope) {
        error_state_add(parser_errors, kw.line, kw.col, "Memory allocation failed");
        return NULL;
    }
    scope_init(func_scope, table, 0);

    if (lexer_peek(lexer).type != TOKEN_RPAREN) {
        while (1) {
            Token p = lexer_next(lexer);
            if (p.type == TOKEN_ELLIPSIS) {
                Token rn = lexer_next(lexer);
                if (!token_is_name(rn) || is_ltng_reserved(rn.value)) {
                    error_state_add(parser_errors, rn.line, rn.col,
                        "Expected a valid name for the variadic parameter");
                    skip_to_newline(lexer);
                    free(func_scope);
                    return NULL;
                }
                variadic = 1;
                { free(variadic_name); variadic_name = ltng_strdup(rn.value); }
                char *rm = mangle_name_alloc(rn.value);
                symbol_table_add_param(func_scope, rm, TYPE_ARRAY);
                int zero_dim = 0;
                symbol_table_set_array_dims(func_scope, rm, 1, &zero_dim);
                if (lexer_peek(lexer).type == TOKEN_COMMA) {
                    error_state_add(parser_errors, rn.line, rn.col,
                        "Variadic parameter must be the last parameter");
                }
                break;
            }
            if (!token_is_name(p)) {
                error_state_add(parser_errors, p.line, p.col,
                    "Expected parameter name");
                skip_to_newline(lexer);
                free(func_scope);
                return NULL;
            }
            if (is_ltng_reserved(p.value)) {
                error_state_add(parser_errors, p.line, p.col,
                    "Cannot use reserved word as parameter name");
                skip_to_newline(lexer);
                free(func_scope);
                return NULL;
            }
            char *pm = mangle_name_alloc(p.value);
            symbol_table_add_param(func_scope, pm, TYPE_FLOAT);
            ASTNode *pn = create_node(NODE_PARAM, pm, "");
            pn->var_type = TYPE_FLOAT;
            { free(pn->for_var); pn->for_var = ltng_strdup(p.value); }
            pn->has_default = 0;
            { free(pn->value); pn->value = ltng_strdup(""); }
            if (lexer_peek(lexer).type == TOKEN_EQUALS) {
                /* Default parameter value: name = expr */
                lexer_next(lexer); /* = */
                char *defexpr = (char*)malloc(65536); if(defexpr) defexpr[0]=0;
                VarType dvt = collect_expression(lexer, table, &defexpr, &(int){65536}, 1, 1, NULL, NULL, NULL, NULL);
                pn->has_default = 1;
                pn->var_type = dvt;
                { char *_s = _ltng_cfmt("%s", defexpr); free(pn->value); pn->value = _s ? _s : ltng_strdup(""); }
            }
            if (!params) params = plast = pn; else { plast->next = pn; plast = pn; }
            pcount++;
            if (lexer_peek(lexer).type == TOKEN_COMMA) { lexer_next(lexer); continue; }
            break;
        }
    }
    /* update metadata on the signature/registration and local binding */
    {
        FuncInfo *fi = sig;
        if (!fi) {
            for (int i = 0; i < g_func_count; i++) {
                if (strcmp(g_funcs[i].name, cname) == 0) { fi = &g_funcs[i]; break; }
            }
        }
        if (fi) {
            fi->param_count = pcount;
            int req = 0;
            int pi = 0;
            for (ASTNode *pp = params; pp; pp = pp->next) {
                if (!pp->has_default) req++;
                {
                    funcinfo_ensure_params(fi, pi + 1);
                    free(fi->param_names[pi]); fi->param_names[pi] = ltng_strdup(pp->for_var);
                    fi->has_default[pi] = pp->has_default;
                    fi->def_type[pi] = pp->has_default ? pp->var_type : TYPE_NULL;
                    pi++;
                }
            }
            fi->required_count = req;
            fi->is_variadic = variadic;
            fi->defined = 1;
        }
    }
    if (!is_anon && table) symbol_table_add_func(table, name.value, cname, pcount);

    Token rp = lexer_next(lexer);
    if (rp.type != TOKEN_RPAREN) {
        error_state_add(parser_errors, rp.line, rp.col,
            "Expected ')' after function parameters");
        skip_to_newline(lexer);
        free(func_scope);
        return NULL;
    }

    /* Optional return type annotation: function f(...) -> int { ... } */
    int has_return_type = 0;
    VarType return_type = TYPE_STRING;
    while (lexer_peek(lexer).type == TOKEN_NEWLINE) lexer_next(lexer);
    if (lexer_peek(lexer).type == TOKEN_ARROW) {
        Token arrow = lexer_next(lexer);
        Token rt = lexer_next(lexer);
        if (rt.type == TOKEN_IDENT && is_return_type_keyword(rt.value)) {
            has_return_type = 1;
            return_type = type_keyword_to_vartype(rt.value);
        } else {
            error_state_add(parser_errors, arrow.line, arrow.col,
                "Expected a return type after '->'");
            skip_to_newline(lexer);
            free(func_scope);
            return NULL;
        }
        while (lexer_peek(lexer).type == TOKEN_NEWLINE) lexer_next(lexer);
    }

    Token lb = lexer_next(lexer);
    while (lb.type == TOKEN_NEWLINE) lb = lexer_next(lexer);
    if (lb.type != TOKEN_LBRACE) {
        error_state_add(parser_errors, lb.line, lb.col,
            "Expected '{' after function header");
        skip_to_newline(lexer);
        free(func_scope);
        return NULL;
    }

    ASTNode *fn = create_node(NODE_FUNCTION, cname, "");
    fn->line = kw.line; fn->col = kw.col;
    if (!is_anon) { free(fn->for_var); fn->for_var = ltng_strdup(name.value); }
    else { free(fn->for_var); fn->for_var = ltng_strdup(""); }
    fn->params = params;
    fn->has_return_type = has_return_type;
    fn->return_type = return_type;
    fn->is_variadic = variadic;
    if (variadic) {
        { free(fn->variadic_name); fn->variadic_name = variadic_name ? ltng_strdup(variadic_name) : ltng_strdup(""); }
    }
    if (sig) {
        sig->has_return_type = has_return_type;
        sig->return_type = return_type;
    }
    char *saved_enc = ltng_strdup(g_enclosing_cname ? g_enclosing_cname : "");
    { free(g_enclosing_cname); g_enclosing_cname = ltng_strdup(cname ? cname : ""); }
    /* Detect params used as arrays in function body (including base). */
    {
        ASTNode *pp_scan = params;
        while (pp_scan) {
            if (pp_scan->for_var[0]) {
                int scan = lexer->current;
                int depth = 1;
                int param_as_array = 0;
                while (scan < lexer->count && depth > 0) {
                    Token t = lexer->tokens[scan];
                    if (t.type == TOKEN_LBRACE) { depth++; scan++; continue; }
                    if (t.type == TOKEN_RBRACE) { depth--; if (depth == 0) break; scan++; continue; }
                    if ((t.type == TOKEN_IDENT || token_is_name(t)) && strcmp(t.value, pp_scan->for_var) == 0) {
                        Token n1 = (scan + 1 < lexer->count) ? lexer->tokens[scan + 1] : t;
                        Token p1 = (scan - 1 >= 0) ? lexer->tokens[scan - 1] : t;
                        if (n1.type == TOKEN_DOT) {
                            Token n2 = (scan + 2 < lexer->count) ? lexer->tokens[scan + 2] : t;
                            if ((n2.type == TOKEN_IDENT || token_is_name(n2)) &&
                                (strcmp(n2.value,"push")==0||strcmp(n2.value,"pop")==0||strcmp(n2.value,"insert")==0||
                                 strcmp(n2.value,"remove")==0||strcmp(n2.value,"contains")==0||strcmp(n2.value,"index_of")==0||
                                 strcmp(n2.value,"sort")==0||strcmp(n2.value,"reverse")==0||strcmp(n2.value,"copy")==0||
                                 strcmp(n2.value,"slice")==0||strcmp(n2.value,"length")==0)) {
                                param_as_array = 1; break;
                            }
                        } else if (n1.type == TOKEN_LBRACKET) {
                            param_as_array = 1; break;
                        } else if (p1.type == TOKEN_IN) {
                            param_as_array = 1; break;
                        }
                    }
                    scan++;
                }
                if (param_as_array) {
                    pp_scan->var_type = TYPE_ARRAY;
                    symbol_table_update(func_scope, pp_scan->name, TYPE_ARRAY);
                    symbol_table_set_array_elem_type(func_scope, pp_scan->name, TYPE_NULL);
                    if (pp_scan == params && strcmp(pp_scan->for_var, "base") == 0)
                        fn->base_is_array = 1;
                }
            }
            pp_scan = pp_scan->next;
        }
    }
    g_in_function++;
    fn->body = parse_block(lexer, func_scope, 0);
    g_in_function--;
    { free(g_enclosing_cname); g_enclosing_cname = saved_enc; }

    /* Compute captures for nested/anonymous functions */
    fn->capture_count = 0;
    if (is_anon || (enclosing_cname && enclosing_cname[0])) {
        char **used = NULL, **locals = NULL;
        int used_n = 0, locals_n = 0, used_cap = 0, locals_cap = 0;
        for (ASTNode *p = fn->params; p; p = p->next) {
            if (p->for_var[0]) add_capture_name(&locals, &locals_n, &locals_cap, p->for_var);
            if (p->name[0]) add_capture_name(&locals, &locals_n, &locals_cap, p->name);
        }
        if (fn->is_variadic && fn->variadic_name && fn->variadic_name[0])
            add_capture_name(&locals, &locals_n, &locals_cap, fn->variadic_name);
        collect_declared_locals(fn->body, &locals, &locals_n, &locals_cap, 0);
        collect_used_idents(fn->body, &used, &used_n, &used_cap, 0);
        for (int i = 0; i < used_n; i++) {
            const char *u = used[i];
            if (!u || name_in_list(locals, locals_n, u)) continue;
            if (strcmp(u,"true")==0||strcmp(u,"false")==0||strcmp(u,"t")==0||strcmp(u,"f")==0) continue;
            if (strcmp(u,"null")==0||strcmp(u,"write")==0||strcmp(u,"read")==0) continue;
            if (strcmp(u,"int")==0||strcmp(u,"float")==0||strcmp(u,"string")==0||strcmp(u,"char")==0||strcmp(u,"bool")==0) continue;
            if (strcmp(u,"if")==0||strcmp(u,"else")==0||strcmp(u,"for")==0||strcmp(u,"while")==0||strcmp(u,"return")==0) continue;
            if (strcmp(u,"let")==0||strcmp(u,"const")==0||strcmp(u,"set")==0||strcmp(u,"function")==0) continue;
            if (strcmp(u,"try")==0||strcmp(u,"catch")==0||strcmp(u,"finally")==0||strcmp(u,"throw")==0) continue;
            if (strcmp(u,"break")==0||strcmp(u,"continue")==0||strcmp(u,"switch")==0||strcmp(u,"case")==0||strcmp(u,"default")==0) continue;
            if (strcmp(u,"and")==0||strcmp(u,"or")==0||strcmp(u,"not")==0||strcmp(u,"in")==0||strcmp(u,"from")==0||strcmp(u,"to")==0||strcmp(u,"step")==0||strcmp(u,"by")==0) continue;
            if (strcmp(u,"exit")==0||strcmp(u,"include")==0) continue;
            if (is_function_name(u)) continue;
            if (u[0] >= '0' && u[0] <= '9') continue;
            char *mangled_u = mangle_name_alloc(u);
            int in_enclosing = 0;
            const char *cap_cname = NULL;
            if (table) {
                for (Scope *s = table; s && !in_enclosing; s = s->parent) {
                    for (int vi = 0; vi < s->count; vi++) {
                        VarEntry *ve = &s->vars[vi];
                        if (strcmp(ve->name, u) == 0 || strcmp(ve->name, mangled_u) == 0 ||
                            strcmp(ve->cname, u) == 0 || strcmp(ve->cname, mangled_u) == 0) {
                            in_enclosing = 1;
                            cap_cname = ve->cname[0] ? ve->cname : ve->name;
                            break;
                        }
                    }
                }
            }
            free(mangled_u);
            if (!in_enclosing || !cap_cname) continue;
            int dup = 0;
            for (int c = 0; c < fn->capture_count; c++) {
                if (fn->captures && fn->captures[c] && strcmp(fn->captures[c], cap_cname) == 0) { dup = 1; break; }
            }
            if (dup) continue;
            char **ncaps = (char**)realloc(fn->captures, (size_t)(fn->capture_count + 1) * sizeof(char*));
            if (!ncaps) continue;
            fn->captures = ncaps;
            fn->captures[fn->capture_count] = ltng_strdup(cap_cname);
            fn->capture_count++;
        }
        for (int i = 0; i < used_n; i++) free(used[i]);
        for (int i = 0; i < locals_n; i++) free(locals[i]);
        free(used); free(locals);
    }

    free(func_scope);
    validate_function_return_types(fn);
    /* Propagate inferred return type and object-return schema for unannotated functions. */
    {
        FuncInfo *fi2 = NULL;
        if (sig) fi2 = sig;
        else {
            for (int i = 0; i < g_func_count; i++) {
                if (strcmp(g_funcs[i].name, cname) == 0) { fi2 = &g_funcs[i]; break; }
            }
        }
        if (!fn->has_return_type) {
            int has_ret = 0;
            VarType irt = analyze_function_returns(fn->body, &has_ret);
            if (!has_ret) irt = TYPE_VOID;
            fn->return_type = irt;
            if (fi2) {
                if (!fi2->has_return_type) fi2->return_type = irt;
            }
        }
        if (fi2 && fn->return_obj_base[0] == '\0' && fi2->return_obj_base[0] != '\0') {
            { free(fn->return_obj_base); fn->return_obj_base = ltng_strdup(fi2->return_obj_base); }
        }
        if (fi2 && fi2->return_obj_base[0] == '\0' && fn->return_obj_base[0] != '\0') {
            { free(fi2->return_obj_base); fi2->return_obj_base = ltng_strdup(fn->return_obj_base); }
        }
    }
    (void)kw;
    return fn;
}

void parse_statement(Lexer *lexer, SymbolTable *table) {
    Token token = lexer_peek(lexer);
    g_stmt_line = token.line;
    g_stmt_col = token.col;
    if (token.type == TOKEN_LBRACE) {
        /* Bare block: { ... } introduces a nested scope. */
        lexer_next(lexer); /* consume { */
        ASTNode *node = create_node(NODE_BLOCK, "", "");
        node->line = token.line; node->col = token.col;
        node->body = parse_block(lexer, table, 0);
        emit(node);
        return;
    }
    if (token.type == TOKEN_PLUS_PLUS || token.type == TOKEN_MINUS_MINUS) {
        transform_inc_dec_stmt(lexer, table);
        return;
    }
    if (token.type == TOKEN_IDENT) {
        int saved = lexer->current;
        Token t = lexer_peek(lexer);
        lexer_next(lexer);
        Token t2 = lexer_peek(lexer);
        if (t2.type == TOKEN_PLUS_PLUS || t2.type == TOKEN_MINUS_MINUS) {
            lexer->current = saved;
            transform_inc_dec_stmt(lexer, table);
            return;
        }
        if (t2.type == TOKEN_LPAREN && is_callable_ident(table, t.value)) {
            /* function call statement: name(args) */
            char *fm = ltng_strdup("");
            int fr_pc = -1;
            if (!is_function_name_scoped(table, t.value) &&
                symbol_table_get_func_ref(table, t.value, &fm, &fr_pc)) {
            } else {
                { char *_rn = resolve_func_name(table, t.value);
                  if (_rn) { free(fm); fm = _rn; }
                  else { char *_mn = mangle_name_alloc(t.value); free(fm); fm = _mn; } }
            }
            if (function_first_param_is_base(fm)) {
                error_state_add(parser_errors, t.line, t.col,
                    "function with 'base' parameter must be called with method syntax (receiver.method(args))");
                skip_to_newline(lexer);
                return;
            }
            lexer->current = saved;
            Token fname = lexer_next(lexer); /* consume fname */
            lexer_next(lexer); /* consume ( */
            ASTNode *call = parse_call_node(lexer, table, fname.value, fname.line, fname.col, NULL);
            /* handle chained immediate calls: f()()... */
            if (lexer_peek(lexer).type == TOKEN_LPAREN) {
                char *cexpr = (char*)malloc(65536); if(cexpr) cexpr[0]=0; int cexpr_cap = 65536;
                build_call_cexpr(call, &cexpr, &cexpr_cap);
                while (lexer_peek(lexer).type == TOKEN_LPAREN) {
                    int cline = lexer_peek(lexer).line, ccol = lexer_peek(lexer).col;
                    lexer_next(lexer);
                    ASTNode *args = NULL, *last = NULL;
                    if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                        while (1) {
                            char *arg_expr = (char*)malloc(65536); if(arg_expr) arg_expr[0]=0;
                            VarType at = collect_expression(lexer, table, &arg_expr, &(int){65536}, 1, 1, NULL, NULL, NULL, NULL);
                            ASTNode *an = create_node(NODE_PARAM, "", arg_expr);
                            an->var_type = at; an->is_expr = 1;
                            an->v_sl=g_expr_sl; an->v_sc=g_expr_sc; an->v_el=g_expr_el; an->v_ec=g_expr_ec;
                            if (!args) args = last = an; else { last->next = an; last = an; }
                            Token sep = lexer_peek(lexer);
                            if (sep.type == TOKEN_COMMA) { lexer_next(lexer); continue; }
                            break;
                        }
                    }
                    if (lexer_peek(lexer).type != TOKEN_RPAREN) { error_state_add(parser_errors, lexer_peek(lexer).line, lexer_peek(lexer).col, "Expected ')' in chained call"); skip_to_newline(lexer); return; }
                    lexer_next(lexer);
                    int argc = 0; for (ASTNode *a = args; a; a = a->next) argc++;
                    char *next_expr = (char*)malloc(65536); if(next_expr) next_expr[0]=0; int next_expr_cap = 65536; int p = 0;
                    static int _stcid = 0; int cid = _stcid++;
                    STMT_APPEND(next_expr, next_expr_cap, p, "({ char *__ct%d=_ltng_ds_fmt(\"%%s\",%s); char *__ctr%d=__ltng_call_indirect(__ct%d,_ltng_retbuf(),%d,%d,%d",
                        cid,cexpr,cid,cid,cline>0?cline:1,ccol>0?ccol:1,argc);
                    /* p grown dynamically */
                    for (ASTNode *a = args; a ; a = a->next) {
                        if (a->var_type == TYPE_STRING && a->value[0] == '"') STMT_APPEND(next_expr, next_expr_cap, p, ",%s", a->value);
                        else if (a->is_variable && a->value[0] != '\0') STMT_APPEND(next_expr, next_expr_cap, p, ",%s", a->value);
                        else if ((a->var_type == TYPE_STRING || a->var_type == TYPE_OBJECT) && a->value[0] != '\0') STMT_APPEND(next_expr, next_expr_cap, p, ",%s", a->value);
                        else if (a->value[0] == '\0') STMT_APPEND(next_expr, next_expr_cap, p, ",\"\"");
                        else STMT_APPEND(next_expr, next_expr_cap, p, ",_ltng_ad((double)(%s))", a->value);
                        /* p grown dynamically */
                    }
                    STMT_APPEND(next_expr, next_expr_cap, p, "); free(__ct%d); __ctr%d; })", cid, cid);
                    if (p < next_expr_cap) next_expr[p] = 0;
                    SNPRINTF_FULL(cexpr, "%s", next_expr);
                }
                ASTNode *stmt = create_node(NODE_CALL, "", cexpr);
                stmt->is_expr = 1;
                emit(stmt);
                return;
            }
            emit(call);
            return;
        }
        if (t2.type == TOKEN_DOT) {
            /* Special: p.lock() / p.unlock() — dtype lock operations.
               These are compile-time only: no runtime code is generated.
               The variable's dtype must not be null. */
            lexer_next(lexer); /* consume DOT */
            Token mname = lexer_next(lexer);
            if ((mname.type == TOKEN_IDENT || token_is_name(mname)) &&
                (strcmp(mname.value, "lock") == 0 ||
                 strcmp(mname.value, "unlock") == 0)) {
                if (lexer_peek(lexer).type != TOKEN_LPAREN) {
                    char *errmsg = _ltng_cfmt("Expected '(' after '%s'", mname.value);
                    error_state_add(parser_errors, mname.line, mname.col, errmsg ? errmsg : "");
                    free(errmsg);
                    skip_to_newline(lexer);
                    return;
                }
                lexer_next(lexer); /* consume ( */
                if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                    char *errmsg = _ltng_cfmt("Expected ')' with no arguments for '%s()'", mname.value);
                    error_state_add(parser_errors, lexer_peek(lexer).line, lexer_peek(lexer).col, errmsg ? errmsg : "");
                    free(errmsg);
                    skip_to_newline(lexer);
                    return;
                }
                lexer_next(lexer); /* consume ) */

                char *rm = mangle_name_alloc(t.value);
                if (!symbol_table_has(table, rm)) {
                    char *errmsg = _ltng_cfmt("Variable '%s' not declared", t.value);
                    error_state_add(parser_errors, t.line, t.col, errmsg ? errmsg : "");
                    free(errmsg);
                    return;
                }
                VarType vt = symbol_table_get(table, rm);
                if (vt == TYPE_NULL) {
                    char *errmsg = _ltng_cfmt("Cannot lock/unlock variable '%s' with null dtype", t.value);
                    error_state_add(parser_errors, t.line, t.col, errmsg ? errmsg : "");
                    free(errmsg);
                    return;
                }
                int want_locked = (strcmp(mname.value, "lock") == 0);
                int already = symbol_table_get_locked(table, rm);
                if (already == want_locked) {
                    fprintf(stderr, "warning: variable '%s' is already %s\n",
                        t.value, want_locked ? "locked" : "unlocked");
                }
                symbol_table_set_locked(table, rm, want_locked);
                return;
            }
            /* Special: x.push(y) / x.pop() / x.pop(i) / x.insert(i,x) / x.remove(i)
               / x.sort() / x.reverse() / x.contains(x) / x.index_of(x) - array methods.
               These generate AST nodes handled by codegen. Supports method chaining
               on the same array variable as a statement (e.g. a.sort().reverse()). */
            if ((mname.type == TOKEN_IDENT || token_is_name(mname)) &&
                (strcmp(mname.value, "push") == 0 ||
                 strcmp(mname.value, "pop") == 0 ||
                 strcmp(mname.value, "insert") == 0 ||
                 strcmp(mname.value, "remove") == 0 ||
                 strcmp(mname.value, "sort") == 0 ||
                 strcmp(mname.value, "reverse") == 0 ||
                 strcmp(mname.value, "contains") == 0 ||
                 strcmp(mname.value, "index_of") == 0)) {

                char *rm = mangle_name_alloc(t.value);
                if (!symbol_table_has(table, rm)) {
                    char *errmsg = _ltng_cfmt("Variable '%s' not declared", t.value);
                    error_state_add(parser_errors, t.line, t.col, errmsg ? errmsg : "");
                    free(errmsg);
                    skip_to_newline(lexer);
                    return;
                }
                VarType vt = symbol_table_get(table, rm);
                if (vt != TYPE_ARRAY) {
                    char *errmsg = _ltng_cfmt("Variable '%s' is not an array", t.value);
                    error_state_add(parser_errors, t.line, t.col, errmsg ? errmsg : "");
                    free(errmsg);
                    skip_to_newline(lexer);
                    return;
                }

                /* Chain loop: handle chained methods on same base variable */
                int need_consume_lparen = 1;
                for (;;) {
                    if (need_consume_lparen) {
                        lexer_next(lexer); /* consume ( */
                    }
                    need_consume_lparen = 1; /* default for array methods */

                    int is_array_method = (strcmp(mname.value, "push") == 0 ||
                                           strcmp(mname.value, "pop") == 0 ||
                                           strcmp(mname.value, "insert") == 0 ||
                                           strcmp(mname.value, "remove") == 0 ||
                                           strcmp(mname.value, "sort") == 0 ||
                                           strcmp(mname.value, "reverse") == 0 ||
                                           strcmp(mname.value, "contains") == 0 ||
                                           strcmp(mname.value, "index_of") == 0);

                    if (is_array_method) {
                        if (strcmp(mname.value, "push") == 0) {
                            /* push(value) */
                            char *arg_expr = (char*)malloc(65536); if(arg_expr) arg_expr[0]=0;
                            VarType arg_type = collect_expression(lexer, table, &arg_expr, &(int){65536}, 1, 0, NULL, NULL, NULL, NULL);
                            if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                                error_state_add(parser_errors, lexer_peek(lexer).line,
                                    lexer_peek(lexer).col, "Expected ')' after push argument");
                                skip_to_newline(lexer);
                                return;
                            }
                            lexer_next(lexer); /* consume ) */
                            /* Update inferred array element type */
                            {
                                VarType cur_et = symbol_table_get_array_elem_type(table, rm);
                                VarType new_et = arg_type;
                                if (new_et == TYPE_CHAR || new_et == TYPE_OBJECT || new_et == TYPE_ARRAY ||
                                    new_et == TYPE_NULL || new_et == TYPE_VOID || new_et == TYPE_STRING)
                                    new_et = TYPE_NULL;
                                if (cur_et == TYPE_NULL || new_et == TYPE_NULL) cur_et = TYPE_NULL;
                                else if (cur_et == new_et) { /* same */ }
                                else if ((cur_et == TYPE_INT && new_et == TYPE_FLOAT) || (cur_et == TYPE_FLOAT && new_et == TYPE_INT)) cur_et = TYPE_FLOAT;
                                else cur_et = TYPE_NULL;
                                symbol_table_set_array_elem_type(table, rm, cur_et);
                            }
                            ASTNode *node = create_node(NODE_ARRAY_PUSH, rm, arg_expr);
                            node->var_type = arg_type;
                            node->is_expr = 1;
                            emit(node);
                        } else if (strcmp(mname.value, "insert") == 0) {
                            /* insert(index, value) */
                            char *idx_expr = (char*)malloc(65536); if(idx_expr) idx_expr[0]=0;
                            collect_expression(lexer, table, &idx_expr, &(int){65536}, 1, 1, NULL, NULL, NULL, NULL);
                            if (lexer_peek(lexer).type != TOKEN_COMMA) {
                                error_state_add(parser_errors, lexer_peek(lexer).line,
                                    lexer_peek(lexer).col, "Expected ',' after insert index");
                                skip_to_newline(lexer);
                                return;
                            }
                            lexer_next(lexer); /* consume , */
                            char *arg_expr = (char*)malloc(65536); if(arg_expr) arg_expr[0]=0;
                            VarType arg_type = collect_expression(lexer, table, &arg_expr, &(int){65536}, 1, 0, NULL, NULL, NULL, NULL);
                            if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                                error_state_add(parser_errors, lexer_peek(lexer).line,
                                    lexer_peek(lexer).col, "Expected ')' after insert argument");
                                skip_to_newline(lexer);
                                return;
                            }
                            lexer_next(lexer); /* consume ) */
                            /* Update inferred array element type */
                            {
                                VarType cur_et = symbol_table_get_array_elem_type(table, rm);
                                VarType new_et = arg_type;
                                if (new_et == TYPE_CHAR || new_et == TYPE_OBJECT || new_et == TYPE_ARRAY ||
                                    new_et == TYPE_NULL || new_et == TYPE_VOID || new_et == TYPE_STRING)
                                    new_et = TYPE_NULL;
                                if (cur_et == TYPE_NULL || new_et == TYPE_NULL) cur_et = TYPE_NULL;
                                else if (cur_et == new_et) { /* same */ }
                                else if ((cur_et == TYPE_INT && new_et == TYPE_FLOAT) || (cur_et == TYPE_FLOAT && new_et == TYPE_INT)) cur_et = TYPE_FLOAT;
                                else cur_et = TYPE_NULL;
                                symbol_table_set_array_elem_type(table, rm, cur_et);
                            }
                            ASTNode *node = create_node(NODE_ARRAY_INSERT, rm, idx_expr);
                            { free(node->array_index); node->array_index = ltng_strdup(arg_expr); }
                            node->var_type = arg_type;
                            node->is_expr = 1;
                            emit(node);
                        } else if (strcmp(mname.value, "remove") == 0) {
                            /* remove(index) */
                            char *idx_expr = (char*)malloc(65536); if(idx_expr) idx_expr[0]=0;
                            collect_expression(lexer, table, &idx_expr, &(int){65536}, 1, 0, NULL, NULL, NULL, NULL);
                            if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                                error_state_add(parser_errors, lexer_peek(lexer).line,
                                    lexer_peek(lexer).col, "Expected ')' after remove index");
                                skip_to_newline(lexer);
                                return;
                            }
                            lexer_next(lexer); /* consume ) */
                            ASTNode *node = create_node(NODE_ARRAY_REMOVE, rm, idx_expr);
                            node->var_type = TYPE_STRING;
                            node->is_expr = 1;
                            emit(node);
                        } else if (strcmp(mname.value, "sort") == 0) {
                            /* sort() */
                            if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                                error_state_add(parser_errors, lexer_peek(lexer).line,
                                    lexer_peek(lexer).col, "Expected ')' after sort");
                                skip_to_newline(lexer);
                                return;
                            }
                            lexer_next(lexer); /* consume ) */
                            ASTNode *node = create_node(NODE_ARRAY_SORT, rm, "");
                            node->var_type = TYPE_STRING;
                            node->is_expr = 1;
                            emit(node);
                        } else if (strcmp(mname.value, "reverse") == 0) {
                            /* reverse() */
                            if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                                error_state_add(parser_errors, lexer_peek(lexer).line,
                                    lexer_peek(lexer).col, "Expected ')' after reverse");
                                skip_to_newline(lexer);
                                return;
                            }
                            lexer_next(lexer); /* consume ) */
                            ASTNode *node = create_node(NODE_ARRAY_REVERSE, rm, "");
                            node->var_type = TYPE_STRING;
                            node->is_expr = 1;
                            emit(node);
                        } else if (strcmp(mname.value, "pop") == 0) {
                            /* pop() or pop(index) */
                            if (lexer_peek(lexer).type == TOKEN_RPAREN) {
                                /* pop() - no args */
                                lexer_next(lexer); /* consume ) */
                                ASTNode *node = create_node(NODE_ARRAY_POP, rm, "");
                                node->var_type = TYPE_STRING;
                                node->is_expr = 1;
                                emit(node);
                            } else {
                                /* pop(index) */
                                char *idx_expr = (char*)malloc(65536); if(idx_expr) idx_expr[0]=0;
                                collect_expression(lexer, table, &idx_expr, &(int){65536}, 1, 0, NULL, NULL, NULL, NULL);
                                if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                                    error_state_add(parser_errors, lexer_peek(lexer).line,
                                        lexer_peek(lexer).col, "Expected ')' after pop index");
                                    skip_to_newline(lexer);
                                    return;
                                }
                                lexer_next(lexer); /* consume ) */
                                ASTNode *node = create_node(NODE_ARRAY_POP, rm, idx_expr);
                                node->var_type = TYPE_STRING;
                                node->is_expr = 1;
                                emit(node);
                            }
                        } else {
                            /* contains / index_of used as statements - result discarded */
                            if (lexer_peek(lexer).type == TOKEN_RPAREN) {
                                lexer_next(lexer); /* consume ) */
                            } else {
                                char *dummy = (char*)malloc(65536); if(dummy) dummy[0]=0;
                                collect_expression(lexer, table, &dummy, &(int){65536}, 1, 0, NULL, NULL, NULL, NULL);
                                if (lexer_peek(lexer).type == TOKEN_RPAREN) lexer_next(lexer);
                            }
                        }
                    } else if (is_function_name_scoped(table, mname.value)) {
                        /* User-defined method call on same base receiver: at this point '(' already consumed */
                        ASTNode *recv = create_node(NODE_PARAM, "", "");
                        char *rm2 = mangle_name_alloc(t.value);
                        if (symbol_table_has(table, rm2)) {
                            recv->is_variable = 1;
                            { free(recv->value); recv->value = ltng_strdup(rm2); }
                            recv->var_type = symbol_table_get(table, rm2);
                        } else if (symbol_table_has(table, t.value)) {
                            recv->is_variable = 1;
                            { free(recv->value); recv->value = ltng_strdup(t.value); }
                            recv->var_type = symbol_table_get(table, t.value);
                        } else {
                            recv->is_expr = 1;
                            { free(recv->value); recv->value = ltng_strdup(t.value); }
                            recv->var_type = TYPE_FLOAT;
                        }
                        ASTNode *call = parse_call_node(lexer, table, mname.value, mname.line, mname.col, recv);
                        emit(call);
                        /* parse_call_node consumed closing ')' */
                        need_consume_lparen = 0;
                    } else {
                        error_state_add(parser_errors, mname.line, mname.col, "Unexpected token");
                        skip_to_newline(lexer);
                        return;
                    }

                    /* Method chaining on same base */
                    if (lexer_peek(lexer).type == TOKEN_DOT) {
                        int saved_chain = lexer->current;
                        lexer_next(lexer); /* consume . */
                        Token next_m = lexer_peek(lexer);
                        if (next_m.type == TOKEN_IDENT || token_is_name(next_m)) {
                            lexer_next(lexer); /* consume method name */
                            if (lexer_peek(lexer).type == TOKEN_LPAREN) {
                                int next_is_array = (strcmp(next_m.value, "push") == 0 ||
                                                     strcmp(next_m.value, "pop") == 0 ||
                                                     strcmp(next_m.value, "insert") == 0 ||
                                                     strcmp(next_m.value, "remove") == 0 ||
                                                     strcmp(next_m.value, "sort") == 0 ||
                                                     strcmp(next_m.value, "reverse") == 0 ||
                                                     strcmp(next_m.value, "contains") == 0 ||
                                                     strcmp(next_m.value, "index_of") == 0);
                                if (next_is_array || is_function_name_scoped(table, next_m.value)) {
                                    mname = next_m;
                                    /* For array methods, '(' not yet consumed; for user methods, also not yet consumed here */
                                    need_consume_lparen = 1;
                                    continue;
                                }
                            }
                        }
                        lexer->current = saved_chain;
                    }
                    return;
                }
            }
            if ((mname.type == TOKEN_IDENT || token_is_name(mname)) &&
                lexer_peek(lexer).type == TOKEN_LPAREN &&
                is_function_name_scoped(table, mname.value)) {
                /* User-defined method call(s) with chaining support on same base */
                for (;;) {
                    lexer_next(lexer); /* consume ( */
                    /* build receiver arg */
                    ASTNode *recv = create_node(NODE_PARAM, "", "");
                    char *rm = mangle_name_alloc(t.value);
                    if (symbol_table_has(table, rm)) {
                        recv->is_variable = 1;
                        { free(recv->value); recv->value = ltng_strdup(rm); }
                        recv->var_type = symbol_table_get(table, rm);
                    } else if (symbol_table_has(table, t.value)) {
                        recv->is_variable = 1;
                        { free(recv->value); recv->value = ltng_strdup(t.value); }
                        recv->var_type = symbol_table_get(table, t.value);
                    } else {
                        /* pass as expression/value - best effort */
                        recv->is_expr = 1;
                        { free(recv->value); recv->value = ltng_strdup(t.value); }
                        recv->var_type = TYPE_FLOAT;
                    }
                    ASTNode *call = parse_call_node(lexer, table, mname.value, mname.line, mname.col, recv);
                    emit(call);

                    /* Chain: .method(...) on same base receiver */
                    if (lexer_peek(lexer).type == TOKEN_DOT) {
                        int saved_chain = lexer->current;
                        lexer_next(lexer); /* consume . */
                        Token next_m = lexer_peek(lexer);
                        if ((next_m.type == TOKEN_IDENT || token_is_name(next_m))) {
                            lexer_next(lexer); /* consume method name */
                            if (lexer_peek(lexer).type == TOKEN_LPAREN) {
                                /* Array method? */
                                if (strcmp(next_m.value, "push") == 0 ||
                                    strcmp(next_m.value, "pop") == 0 ||
                                    strcmp(next_m.value, "insert") == 0 ||
                                    strcmp(next_m.value, "remove") == 0 ||
                                    strcmp(next_m.value, "sort") == 0 ||
                                    strcmp(next_m.value, "reverse") == 0 ||
                                    strcmp(next_m.value, "contains") == 0 ||
                                    strcmp(next_m.value, "index_of") == 0) {
                                    /* Defer to array-method handling is complex here; only support if base is array */
                                    char *rm3 = mangle_name_alloc(t.value);
                                    if (symbol_table_has(table, rm3) && symbol_table_get(table, rm3) == TYPE_ARRAY) {
                                        /* Handle array method inline (simplified: emit via existing logic by reusing loop not easy).
                                           Fallback: not chaining into array method from user-method here except simple cases. */
                                    }
                                }
                                if (is_function_name_scoped(table, next_m.value)) {
                                    mname = next_m;
                                    continue;
                                }
                            }
                        }
                        lexer->current = saved_chain;
                    }
                    return;
                }
            }
        }
        lexer->current = saved;
    }
    if (token.type == TOKEN_WRITE) {
        parse_write(lexer, table);
    } else if (token.type == TOKEN_READ) {
        parse_read(lexer, table);
    } else if (token.type == TOKEN_LET) {
        parse_let(lexer, table);
    } else if (token.type == TOKEN_CONST) {
        parse_const(lexer, table);
    } else if (token.type == TOKEN_SET) {
        parse_set(lexer, table);
    } else if (token.type == TOKEN_EXIT) {
        parse_exit(lexer, table);
    } else if (token.type == TOKEN_RETURN) {
        parse_return(lexer, table);
    } else if (token.type == TOKEN_FUNCTION) {
        /* nested function definition - lifted to top level (allowed in blocks; scope rules apply) */
        if (!g_in_function) {
            /* shouldn't happen from parse_statement at top level, but be safe */
            ASTNode *fn = parse_function(lexer, table, g_enclosing_cname);
            if (fn && g_program_ref) { fn->next = g_program_ref->functions; g_program_ref->functions = fn; }
            return;
        }
        {
            ASTNode *fn = parse_function(lexer, table, g_enclosing_cname);
            if (fn && g_program_ref) { fn->next = g_program_ref->functions; g_program_ref->functions = fn; }
        }
        return;
    } else if (token.type == TOKEN_IF) {
        emit(parse_if_core(lexer, table));
    } else if (token.type == TOKEN_WHILE) {
        ASTNode *node = parse_while(lexer, table);
        if (node) emit(node);
    } else if (token.type == TOKEN_DO) {
        ASTNode *node = parse_do_while(lexer, table);
        if (node) emit(node);
    } else if (token.type == TOKEN_FOR) {
        ASTNode *node = parse_for(lexer, table);
        if (node) emit(node);
    } else if (token.type == TOKEN_SWITCH) {
        ASTNode *node = parse_switch(lexer, table);
        if (node) emit(node);
    } else if (token.type == TOKEN_TRY) {
        ASTNode *node = parse_try(lexer, table);
        if (node) emit(node);
    } else if (token.type == TOKEN_THROW) {
        parse_throw(lexer, table);
    } else if (token.type == TOKEN_BREAK) {
        lexer_next(lexer);
        if (g_break_depth <= 0) {
            error_state_add(parser_errors, token.line, token.col,
                "break statement not within a loop");
        } else {
            ASTNode *node = create_node(NODE_BREAK, "", "");
            node->line = token.line; node->col = token.col;
            emit(node);
        }
    } else if (token.type == TOKEN_CONTINUE) {
        lexer_next(lexer);
        if (g_break_depth <= 0) {
            error_state_add(parser_errors, token.line, token.col,
                "continue statement not within a loop");
        } else {
            ASTNode *node = create_node(NODE_CONTINUE, "", "");
            node->line = token.line; node->col = token.col;
            emit(node);
        }
    } else {
        error_state_add(parser_errors, token.line, token.col,
            "Unexpected token");
        skip_to_newline(lexer);
    }
}

/* Skip a default-value expression token range until ',' or ')' at depth 0.
   Returns the index of the separator token (or EOF). */
int skip_default_expr(Lexer *lexer, int start) {
    int i = start, paren = 0, brack = 0, brace = 0;
    while (i < lexer->count) {
        TokenType tt = lexer->tokens[i].type;
        if (tt == TOKEN_NEWLINE) { i++; continue; }
        if (tt == TOKEN_LPAREN) paren++;
        else if (tt == TOKEN_RPAREN) {
            if (paren == 0 && brack == 0 && brace == 0) return i;
            if (paren > 0) paren--;
        } else if (tt == TOKEN_LBRACKET) brack++;
        else if (tt == TOKEN_RBRACKET) { if (brack > 0) brack--; }
        else if (tt == TOKEN_LBRACE) brace++;
        else if (tt == TOKEN_RBRACE) { if (brace > 0) brace--; }
        else if (tt == TOKEN_COMMA && paren == 0 && brack == 0 && brace == 0) return i;
        else if (tt == TOKEN_EOF) return i;
        i++;
    }
    return i;
}

/* Scan the token stream for top-level function signatures. This makes forward
   references aware of defaults, variadic parameters, return annotations, and
   overload arities before any function body is parsed. */
void prescan_functions(Lexer *lexer) {
    int depth = 0;
    for (int i = 0; i < lexer->count; i++) {
        TokenType tt = lexer->tokens[i].type;
        if (tt == TOKEN_LBRACE) { depth++; continue; }
        if (tt == TOKEN_RBRACE) { if (depth > 0) depth--; continue; }
        if (tt != TOKEN_FUNCTION || depth != 0) continue;
        if (i + 2 >= lexer->count) continue;
        Token nm = lexer->tokens[i + 1];
        if (nm.type != TOKEN_IDENT) continue;
        if (lexer->tokens[i + 2].type != TOKEN_LPAREN) continue;
        ensure_func_cap(); if (g_func_count >= g_func_cap) break;

        FuncInfo *fi = &g_funcs[g_func_count];
        funcinfo_init(fi);
        fi->top_level = 1;
        free(fi->src); fi->src = ltng_strdup(nm.value);
        fi->line = nm.line;
        fi->col = nm.col;

        int k = i + 3;
        int pc = 0, req = 0, variadic = 0;
        while (k < lexer->count && lexer->tokens[k].type != TOKEN_RPAREN) {
            if (lexer->tokens[k].type == TOKEN_NEWLINE) { k++; continue; }
            if (lexer->tokens[k].type == TOKEN_ELLIPSIS) {
                variadic = 1;
                k++;
                if (k < lexer->count && lexer->tokens[k].type == TOKEN_IDENT) k++;
                while (k < lexer->count && lexer->tokens[k].type == TOKEN_NEWLINE) k++;
                break;
            }
            if (lexer->tokens[k].type != TOKEN_IDENT) break;
            {
                funcinfo_ensure_params(fi, pc+1);
                int hasdef = 0, ds = -1, de = -1;
                free(fi->param_names[pc]); fi->param_names[pc] = ltng_strdup(lexer->tokens[k].value);
                k++;
                if (k < lexer->count && lexer->tokens[k].type == TOKEN_EQUALS) {
                    hasdef = 1;
                    k++;
                    ds = k;
                    de = skip_default_expr(lexer, k);
                    k = de;
                }
                fi->has_default[pc] = hasdef;
                fi->def_start[pc] = ds;
                fi->def_end[pc] = de;
                fi->def_type[pc] = TYPE_NULL;
                pc++;
                if (!hasdef) req = pc;
            }
            while (k < lexer->count && lexer->tokens[k].type == TOKEN_NEWLINE) k++;
            if (k < lexer->count && lexer->tokens[k].type == TOKEN_COMMA) { k++; continue; }
            break;
        }
        fi->param_count = pc;
        fi->required_count = req;
        fi->is_variadic = variadic;

        while (k < lexer->count && lexer->tokens[k].type == TOKEN_NEWLINE) k++;
        if (k < lexer->count && lexer->tokens[k].type == TOKEN_RPAREN) k++;
        while (k < lexer->count && lexer->tokens[k].type == TOKEN_NEWLINE) k++;
        if (k < lexer->count && lexer->tokens[k].type == TOKEN_ARROW) {
            k++;
            while (k < lexer->count && lexer->tokens[k].type == TOKEN_NEWLINE) k++;
            if (k < lexer->count && lexer->tokens[k].type == TOKEN_IDENT &&
                is_return_type_keyword(lexer->tokens[k].value)) {
                fi->has_return_type = 1;
                fi->return_type = type_keyword_to_vartype(lexer->tokens[k].value);
                k++;
            }
        }
        g_func_count++;
        i = k - 1;
    }

    /* Assign unique emitted C names. Non-overloaded functions keep their
       historical mangled name so existing IR/C output is unchanged. */
    for (int a = 0; a < g_func_count; a++) {
        int group = 0, idx = 0;
        for (int b = 0; b < g_func_count; b++) {
            if (strcmp(g_funcs[a].src, g_funcs[b].src) != 0) continue;
            group++;
            if (b < a) idx++;
        }
        char *mangled = mangle_name_alloc(g_funcs[a].src);
        if (group <= 1) {
            { free(g_funcs[a].name); g_funcs[a].name = ltng_strdup(mangled); }
        } else {
            { char *_s = _ltng_cfmt("%s__ov%d", mangled, idx); free(g_funcs[a].name); g_funcs[a].name = _s ? _s : ltng_strdup(""); }
        }
    }

    /* Reject duplicate overload signatures (same fixed arity and variadic-ness). */
    for (int a = 0; a < g_func_count; a++) {
        for (int b = 0; b < a; b++) {
            if (strcmp(g_funcs[a].src, g_funcs[b].src) != 0) continue;
            if (g_funcs[a].param_count == g_funcs[b].param_count &&
                g_funcs[a].required_count == g_funcs[b].required_count &&
                g_funcs[a].is_variadic == g_funcs[b].is_variadic) {
                char *msg = _ltng_cfmt("Function '%s' with the same parameter list is already defined", g_funcs[a].src);
                error_state_add(parser_errors, g_funcs[a].line, g_funcs[a].col, msg ? msg : "");
                free(msg);
            }
        }
    }
}

Program *parse(Lexer *lexer, ErrorState *errors) {
    parser_errors = errors;
    g_list = NULL;
    g_func_count = 0;
    g_obj_count = 0;
    g_in_function = 0;
    g_in_catch = 0;
    { free(g_enclosing_cname); g_enclosing_cname = ltng_strdup(""); }
    Program *program = (Program *)malloc(sizeof(Program));
    if (!program) {
        fprintf(stderr, "Error: Memory allocation failed\n");
        exit(1);
    }
    program->head = NULL;
    program->functions = NULL;
    program->count = 0;
    scope_init(&program->symbol_table, NULL, 0);
    g_program_ref = program;

    /* Pre-scan top-level function signatures for forward references,
       defaults, variadic parameters, return annotations, and overloading. */
    prescan_functions(lexer);

    while (lexer->current < lexer->count) {
        Token token = lexer_peek(lexer);
        if (token.type == TOKEN_EOF) break;

        if (token.type == TOKEN_NEWLINE) {
            lexer_next(lexer);
            continue;
        }

        if (token.type == TOKEN_RBRACE) {
            error_state_add(parser_errors, token.line, token.col,
                "Unexpected '}'");
            lexer_next(lexer);
            continue;
        }

        if (token.type == TOKEN_FUNCTION) {
            ASTNode *fn = parse_function(lexer, &program->symbol_table, "");
            if (fn) {
                fn->next = program->functions;
                program->functions = fn;
            }
            continue;
        }

        parse_statement(lexer, &program->symbol_table);
    }

    program->head = reverse_list(g_list);
    g_list = NULL;
    parser_errors = NULL;
    return program;
}

VarType parse_expression_from_string(const char *expr, SymbolTable *table,
                                     char **out, int *out_cap) {
    char *buf = (char*)malloc(65536); if(buf) buf[0]=0;
    SNPRINTF_FULL(buf, "%s\n", expr);
    int cap = (out_cap && *out_cap > 0) ? *out_cap : 65536;
    if (!out || !*out) { cap = 65536; if (out) *out = (char*)malloc(cap); }
    if (!out || !*out) { free(buf); return TYPE_INT; }
    (*out)[0] = '\0';
    for (int attempt = 0; attempt < 8; attempt++) {
        Lexer *tmp_lexer = (Lexer *)malloc(sizeof(Lexer));
        if (!tmp_lexer) { (*out)[0] = '\0'; free(buf); return TYPE_INT; }
        lexer_init(tmp_lexer, buf);
        ErrorState tmp_errors;
        memset(&tmp_errors, 0, sizeof(tmp_errors));
        tmp_errors.filename = ltng_strdup("<expr>");
        lexer_tokenize(tmp_lexer, &tmp_errors);
        ErrorState *target = codegen_get_errors();
        if (!target) target = parser_errors;
        if (!target) target = &tmp_errors;
        ErrorState *saved = parser_errors;
        parser_errors = target;
        VarType result = collect_expression(tmp_lexer, table, out, &cap, 0, 0, NULL, NULL, NULL, NULL);
        parser_errors = saved;
        free(tmp_lexer);
        free(tmp_errors.filename);
        if ((int)strlen(*out) < cap - 1) { if (out_cap) *out_cap = cap; free(buf); return result; }
        cap *= 2;
        char *nb = (char*)realloc(*out, cap);
        if (!nb) { free(buf); return result; }
        *out = nb;
        (*out)[0] = '\0';
    }
    if (out_cap) *out_cap = cap;
    free(buf);
    return TYPE_INT;
}

void free_node_list(ASTNode *node) {
    while (node) {
        ASTNode *next = node->next;
        free_node_list(node->body);
        free_node_list(node->orelse);
        free_node_list(node->cases);
        free_node_list(node->params);
        free_node_list(node->args);
        free(node);
        node = next;
    }
}

void codegen_free(Program *program) {
    if (!program) return;
    free_node_list(program->head);
    free_node_list(program->functions);
    free(program);
}
