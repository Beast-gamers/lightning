#include "ltng.h"

static ErrorState *codegen_errors = NULL;
static SymbolTable *codegen_table = NULL;

ErrorState *codegen_get_errors(void) {
    return codegen_errors;
}

static void pad(FILE *out, int indent) {
    for (int i = 0; i < indent; i++) fputs("    ", out);
}

static void write_c_string(FILE *out, const char *str) {
    for (const char *p = str; *p; p++) {
        switch (*p) {
            case '\\': fputc('\\', out); fputc('\\', out); break;
            case '"': fputc('\\', out); fputc('"', out); break;
            case '\n': fputc('\\', out); fputc('n', out); break;
            case '\t': fputc('\\', out); fputc('t', out); break;
            case '%': fputc('%', out); fputc('%', out); break;
            default: fputc(*p, out); break;
        }
    }
}

static const char *bool_to_str(const char *val) {
    if (strcmp(val, "true") == 0 || strcmp(val, "t") == 0) return "true";
    return "false";
}

static const char *default_for(VarType t) {
    switch (t) {
        case TYPE_INT: return "0";
        case TYPE_FLOAT: return "0";
        case TYPE_STRING: return "";
        case TYPE_CHAR: return "";
        case TYPE_BOOL: return "false";
        case TYPE_OBJECT: return "";
        case TYPE_NULL: return "";
    }
    return "";
}

static int is_simple_var_or_keyword(const char *name, SymbolTable *table) {
    if (strcmp(name, "true") == 0 || strcmp(name, "t") == 0 ||
        strcmp(name, "false") == 0 || strcmp(name, "f") == 0) return 1;
    if (strcmp(name, "int") == 0 || strcmp(name, "string") == 0 ||
        strcmp(name, "bool") == 0 || strcmp(name, "obj") == 0 ||
        strcmp(name, "typeof") == 0 ||
        strcmp(name, "let") == 0 || strcmp(name, "const") == 0 ||
        strcmp(name, "set") == 0 || strcmp(name, "write") == 0 ||
        strcmp(name, "read") == 0) return 1;
    char mvname[MAX_IDENTIFIER_LEN];
    mangle_name(name, mvname, MAX_IDENTIFIER_LEN);
    if (symbol_table_has(table, mvname)) return 1;
    return 0;
}

static void codegen_assign_interpolated(FILE *out, const char *varname, const char *str, int indent) {
    pad(out, indent);
    fprintf(out, "snprintf(%s, sizeof(%s), \"", varname, varname);
    int arg_count = 0;
    const char *p;
    for (p = str; *p; ) {
        if (*p == '<') {
            p++;
            while (*p && *p != '>') {
                char tmp[2] = {*p, '\0'};
                write_c_string(out, tmp);
                p++;
            }
            if (*p == '>') p++;
        } else if (*p == '$' && p[1] == '[') {
            p += 2;
            const char *start = p;
            int ibd = 0;
            while (*p) {
                if (*p == '[') ibd++;
                else if (*p == ']') { if (ibd == 0) break; ibd--; }
                p++;
            }
            if (*p == ']') {
                int len = (int)(p - start);
                char vname[MAX_IDENTIFIER_LEN];
                if (len >= MAX_IDENTIFIER_LEN) len = MAX_IDENTIFIER_LEN - 1;
                memcpy(vname, start, len);
                vname[len] = '\0';
                if (is_simple_var_or_keyword(vname, codegen_table)) {
                    fprintf(out, "%%s");
                } else {
                    char expr_out[MAX_STRING_LEN];
                    VarType etype = parse_expression_from_string(vname, codegen_table,
                        expr_out, sizeof(expr_out));
                    if (etype == TYPE_FLOAT) {
                        fprintf(out, "%%g");
                    } else if (etype == TYPE_STRING) {
                        fprintf(out, "%%s");
                    } else {
                        fprintf(out, "%%lld");
                    }
                }
                arg_count++;
                p++;
            } else {
                write_c_string(out, "$[");
            }
        } else if (*p == '%') {
            fprintf(out, "%%%%");
            p++;
        } else {
            char tmp[2] = {*p, '\0'};
            write_c_string(out, tmp);
            p++;
        }
    }
    fprintf(out, "\"");
    if (arg_count > 0) {
        for (p = str; *p; ) {
            if (*p == '<') {
                p++;
                while (*p && *p != '>') p++;
                if (*p == '>') p++;
            } else if (*p == '$' && p[1] == '[') {
                p += 2;
                const char *start = p;
                int ibd = 0;
                while (*p) {
                    if (*p == '[') ibd++;
                    else if (*p == ']') { if (ibd == 0) break; ibd--; }
                    p++;
                }
                if (*p == ']') {
                    int len = (int)(p - start);
                    char vname[MAX_IDENTIFIER_LEN];
                    if (len >= MAX_IDENTIFIER_LEN) len = MAX_IDENTIFIER_LEN - 1;
                    memcpy(vname, start, len);
                    vname[len] = '\0';
                    if (strcmp(vname, "true") == 0 || strcmp(vname, "t") == 0) {
                        fprintf(out, ", \"true\"");
                    } else if (strcmp(vname, "false") == 0 || strcmp(vname, "f") == 0) {
                        fprintf(out, ", \"false\"");
                    } else if (is_simple_var_or_keyword(vname, codegen_table)) {
                        char mvname[MAX_IDENTIFIER_LEN];
                        mangle_name(vname, mvname, MAX_IDENTIFIER_LEN);
                        if (symbol_table_has(codegen_table, mvname)) {
                            fprintf(out, ", %s", mvname);
                        } else {
                            fprintf(out, ", \"%s\"", vname);
                        }
                    } else {
                        char expr_out[MAX_STRING_LEN];
                        VarType etype = parse_expression_from_string(vname, codegen_table,
                            expr_out, sizeof(expr_out));
                        if (etype == TYPE_FLOAT) {
                            fprintf(out, ", (double)(%s)", expr_out);
                        } else if (etype == TYPE_STRING) {
                            fprintf(out, ", %s", expr_out);
                        } else {
                            fprintf(out, ", (long long)(%s)", expr_out);
                        }
                    }
                    p++;
                }
            } else {
                p++;
            }
        }
    }
    fprintf(out, ");\n");
}

static void codegen_write_interpolated(FILE *out, const char *str, int indent) {
    pad(out, indent);
    fprintf(out, "printf(\"");
    int arg_count = 0;
    const char *p;
    for (p = str; *p; ) {
        if (*p == '<') {
            p++;
            while (*p && *p != '>') {
                char tmp[2] = {*p, '\0'};
                write_c_string(out, tmp);
                p++;
            }
            if (*p == '>') p++;
        } else if (*p == '$' && p[1] == '[') {
            p += 2;
            const char *start = p;
            int ibd = 0;
            while (*p) {
                if (*p == '[') ibd++;
                else if (*p == ']') { if (ibd == 0) break; ibd--; }
                p++;
            }
            if (*p == ']') {
                int len = (int)(p - start);
                char vname[MAX_IDENTIFIER_LEN];
                if (len >= MAX_IDENTIFIER_LEN) len = MAX_IDENTIFIER_LEN - 1;
                memcpy(vname, start, len);
                vname[len] = '\0';
                if (is_simple_var_or_keyword(vname, codegen_table)) {
                    fprintf(out, "%%s");
                } else {
                    char expr_out[MAX_STRING_LEN];
                    VarType etype = parse_expression_from_string(vname, codegen_table,
                        expr_out, sizeof(expr_out));
                    if (etype == TYPE_FLOAT) {
                        fprintf(out, "%%g");
                    } else if (etype == TYPE_STRING) {
                        fprintf(out, "%%s");
                    } else {
                        fprintf(out, "%%lld");
                    }
                }
                arg_count++;
                p++;
            } else {
                write_c_string(out, "$[");
            }
        } else if (*p == '%') {
            fprintf(out, "%%%%");
            p++;
        } else {
            char tmp[2] = {*p, '\0'};
            write_c_string(out, tmp);
            p++;
        }
    }
    fprintf(out, "\\n\"");
    if (arg_count > 0) {
        for (p = str; *p; ) {
            if (*p == '<') {
                p++;
                while (*p && *p != '>') p++;
                if (*p == '>') p++;
            } else if (*p == '$' && p[1] == '[') {
                p += 2;
                const char *start = p;
                int ibd = 0;
                while (*p) {
                    if (*p == '[') ibd++;
                    else if (*p == ']') { if (ibd == 0) break; ibd--; }
                    p++;
                }
                if (*p == ']') {
                    int len = (int)(p - start);
                    char vname[MAX_IDENTIFIER_LEN];
                    if (len >= MAX_IDENTIFIER_LEN) len = MAX_IDENTIFIER_LEN - 1;
                    memcpy(vname, start, len);
                    vname[len] = '\0';
                    if (strcmp(vname, "true") == 0 || strcmp(vname, "t") == 0) {
                        fprintf(out, ", \"true\"");
                    } else if (strcmp(vname, "false") == 0 || strcmp(vname, "f") == 0) {
                        fprintf(out, ", \"false\"");
                    } else if (is_simple_var_or_keyword(vname, codegen_table)) {
                        char mvname[MAX_IDENTIFIER_LEN];
                        mangle_name(vname, mvname, MAX_IDENTIFIER_LEN);
                        if (symbol_table_has(codegen_table, mvname)) {
                            fprintf(out, ", %s", mvname);
                        } else {
                            fprintf(out, ", \"%s\"", vname);
                        }
                    } else {
                        char expr_out[MAX_STRING_LEN];
                        VarType etype = parse_expression_from_string(vname, codegen_table,
                            expr_out, sizeof(expr_out));
                        if (etype == TYPE_FLOAT) {
                            fprintf(out, ", (double)(%s)", expr_out);
                        } else if (etype == TYPE_STRING) {
                            fprintf(out, ", %s", expr_out);
                        } else {
                            fprintf(out, ", (long long)(%s)", expr_out);
                        }
                    }
                    p++;
                }
            } else {
                p++;
            }
        }
    }
    fprintf(out, ");\n");
}

static int has_interpolation(const char *str) {
    for (const char *p = str; *p; p++) {
        if (*p == '$' && p[1] == '[') return 1;
    }
    return 0;
}

static void codegen_assign(FILE *out, ASTNode *node, int indent) {
        if (node->var_type == TYPE_OBJECT) {
        ObjSchema *sc = obj_lookup(node->name);
        if (sc) {
            pad(out, indent);
            fprintf(out, "snprintf(%s, sizeof(%s), \"%%s\", \"", node->name, node->name);
            { const char *v=sc->default_val; for(;*v;v++){ if(*v=='\\'||*v=='\"') fputc('\\',out); if(*v=='\n'){fputs("\\n",out);} else if(*v=='\t'){fputs("\\t",out);} else fputc(*v,out);} }
            fprintf(out, "\");\n");
            for (int i = 0; i < sc->count; i++) {
                char fv[MAX_IDENTIFIER_LEN*2+8];
                obj_resolve_field(node->name, sc->fields[i].name, fv, sizeof(fv));
                pad(out, indent);
                fprintf(out, "snprintf(%s, sizeof(%s), \"%%s\", \"", fv, fv);
                { const char *v=sc->fields[i].value; for(;*v;v++){ if(*v=='\\'||*v=='\"') fputc('\\',out); if(*v=='\n'){fputs("\\n",out);} else if(*v=='\t'){fputs("\\t",out);} else fputc(*v,out);} }
                fprintf(out, "\");\n");
            }
        } else {
            pad(out, indent);
            fprintf(out, "snprintf(%s, sizeof(%s), \"%%s\", \"\");\n", node->name, node->name);
        }
        return;
    }
    if (node->var_type == TYPE_ARRAY) {
        /* Generate C array declaration for array literals:
           char name_data[N][256] = {{"v1"},{"v2"},...};
           int name_len = N; */
        char arrname[MAX_IDENTIFIER_LEN];
        snprintf(arrname, sizeof(arrname), "%s_data", node->name);
        /* Allocate the rectangular shape (product of dimensions), not just the
           number of written elements, so ragged literals leave empty slots
           instead of reading out of bounds. */
        int slots = node->array_elem_count;
        if (node->array_dim_count >= 1 && node->array_dims != NULL) {
            int product = 1;
            for (int d = 0; d < node->array_dim_count; d++) {
                if (node->array_dims[d] <= 0) { product = 0; break; }
                product *= node->array_dims[d];
            }
            if (product > slots) slots = product;
        }
        /* Add reserve capacity for push() operations */
        slots += ARRAY_PUSH_RESERVE;
        if (slots < 1) slots = 1;
        pad(out, indent);
        fprintf(out, "char %s[%d][256] = {", arrname, slots);
        /* Parse pipe-separated values from node->value */
        const char *val = node->value;
        int elem_idx = 0;
        while (*val && elem_idx < node->array_elem_count) {
            /* Skip leading | */
            if (*val == '|') val++;
            /* Find end of value */
            const char *end = val;
            while (*end && *end != '|') end++;
            int vlen = (int)(end - val);
            /* Emit string literal */
            fprintf(out, "\"");
            for (int i = 0; i < vlen; i++) {
                if (val[i] == '"') fputc('\\', out);
                if (val[i] == '\\') fputc('\\', out);
                fputc(val[i], out);
            }
            fprintf(out, "\"");
            val = end;
            elem_idx++;
            if (elem_idx < node->array_elem_count) fprintf(out, ",");
        }
        fprintf(out, "};\n");
        pad(out, indent);
        fprintf(out, "int %s_len = %d;\n", node->name, node->array_elem_count);
        return;
    }
    if (node->is_expr) {
        int is_add_sn = strncmp(node->value, "_ltng_add_sn", 12) == 0;
        if (node->var_type == TYPE_STRING || node->var_type == TYPE_OBJECT ||
                   is_add_sn) {
            pad(out, indent);
            fprintf(out, "snprintf(%s, sizeof(%s), \"%%s\", %s);\n",
                node->name, node->name, node->value);
        } else {
            pad(out, indent);
            if (strchr(node->value, '.')) {
                fprintf(out, "{ double _e = (%s); snprintf(%s, sizeof(%s), \"%%g\", _e); }\n",
                    node->value, node->name, node->name);
            } else {
                fprintf(out, "{ long long _e = (%s); snprintf(%s, sizeof(%s), \"%%lld\", _e); }\n",
                    node->value, node->name, node->name);
            }
        }
    } else if (node->var_type == TYPE_STRING && has_interpolation(node->value)) {
        codegen_assign_interpolated(out, node->name, node->value, indent);
    } else if ((node->var_type == TYPE_CHAR || node->var_type == TYPE_STRING) &&
               node->value[0] == '\0') {
        pad(out, indent);
        fprintf(out, "%s[0] = '\\0';\n", node->name);
    } else {
        pad(out, indent);
        fprintf(out, "snprintf(%s, sizeof(%s), \"", node->name, node->name);
        if (node->var_type == TYPE_BOOL) {
            write_c_string(out, bool_to_str(node->value));
        } else {
            /* INT/FLOAT literals are stored as-is, matching `set` (e.g. 3.14 stays "3.14"). */
            write_c_string(out, node->value);
        }
        fprintf(out, "\");\n");
    }
}

static void resolve_typeof_str(char *out, const char *in, SymbolTable *table) {
    char *w = out;
    const char *p = in;
    while (*p) {
        if (*p == '<') {
            *w++ = '<';
            p++;
            while (*p && *p != '>') {
                *w++ = *p++;
            }
            if (*p == '>') {
                *w++ = '>';
                p++;
            }
        } else if (*p == '$' && p[1] == '[') {
            const char *start = p + 2;
            const char *q = start;
            while (*q && *q != ']') q++;
            if (*q == ']' && strncmp(start, "typeof(", 7) == 0) {
                const char *arg_start = start + 7;
                const char *arg_end = q - 1;
                int arg_len = (int)(arg_end - arg_start);
                char argname[256];
                if (arg_len >= 256) arg_len = 255;
                memcpy(argname, arg_start, arg_len);
                argname[arg_len] = '\0';
                char margname[256];
                mangle_name(argname, margname, sizeof(margname));
                const char *type_str = "unknown";
                int found = 0;

                for (int i = 0; i < table->count && !found; i++) {
                    char mv[MAX_IDENTIFIER_LEN];
                    mangle_name(table->vars[i].name, mv, MAX_IDENTIFIER_LEN);
                    if (strcmp(mv, margname) == 0 || strcmp(table->vars[i].name, argname) == 0) {
                        switch (table->vars[i].type) {
                            case TYPE_INT: type_str = "int"; break;
                            case TYPE_FLOAT: type_str = "int"; break; /* float merged into int */
                            case TYPE_STRING: type_str = "string"; break;
                            case TYPE_CHAR: type_str = "string"; break; /* char merged into string */
                            case TYPE_BOOL: type_str = "bool"; break;
                            case TYPE_OBJECT: type_str = "object"; break;
                            case TYPE_NULL: type_str = "null"; break;
                            default: type_str = "unknown"; break;
                        }
                        found = 1;
                    }
                }
                const char *ts = type_str;
                while (*ts) *w++ = *ts++;
                p = q + 1;
            } else {
                *w++ = '$'; *w++ = '[';
                p = start;
            }
        } else {
            *w++ = *p++;
        }
    }
    *w = '\0';
}

static void codegen_write(FILE *out, ASTNode *node, SymbolTable *table, int indent) {
    (void)table;
    if (node->is_expr) {
        /* expression result (string or computed) */
        if (node->var_type == TYPE_STRING || strstr(node->value, "_ltng_add_sn") == node->value) {
            pad(out, indent);
            fprintf(out, "printf(\"%%s\\n\", %s);\n", node->value);
        } else if (node->var_type == TYPE_INT || node->var_type == TYPE_FLOAT) {
            pad(out, indent);
            if (strchr(node->value, '.')) {
                fprintf(out, "printf(\"%%g\\n\", (double)(%s));\n", node->value);
            } else {
                fprintf(out, "printf(\"%%lld\\n\", (long long)(%s));\n", node->value);
            }
        } else {
            pad(out, indent);
            fprintf(out, "printf(\"%%s\\n\", %s);\n", node->value);
        }
        return;
    }
    if (node->is_variable) {
        (void)table;
        pad(out, indent);
        fprintf(out, "printf(\"%%s\\n\", %s);\n", node->name);
        return;
    }
    /* literal */
    if (has_interpolation(node->value)) {
        codegen_write_interpolated(out, node->value, indent);
    } else {
        pad(out, indent);
        fprintf(out, "printf(\"%%s\\n\", \"");
        write_c_string(out, node->value);
        fprintf(out, "\");\n");
    }
}

static void codegen_read(FILE *out, ASTNode *node, int indent) {
    pad(out, indent);
    if (node->var_type == TYPE_INT || node->var_type == TYPE_FLOAT) {
        fprintf(out, "{ char _rb[256]; if (fgets(_rb, sizeof(_rb), stdin)) { size_t _l=strlen(_rb); if (_l && _rb[_l-1]=='\\n') _rb[_l-1]='\\0'; if(!_ltng_is_num_str(_rb)){ fprintf(stderr, \"Error: expected integer input for '%s'\\n\", %s); exit(1); } snprintf(%s, sizeof(%s), \"%%lld\", (long long)atoi(_rb)); %s[255]='\\0'; } }\n",
            node->name, node->name, node->name, node->name, node->name);
    } else if (node->var_type == TYPE_BOOL) {
        fprintf(out, "{ char _rb[256]; if (fgets(_rb, sizeof(_rb), stdin)) { size_t _l=strlen(_rb); if (_l && _rb[_l-1]=='\\n') _rb[_l-1]='\\0'; snprintf(%s, sizeof(%s), \"%%s\", (strcmp(_rb,\"true\")==0||strcmp(_rb,\"t\")==0)?\"true\":\"false\"); %s[255]='\\0'; } }\n",
            node->name, node->name, node->name);
    } else {
        /* TYPE_STRING, TYPE_CHAR: store raw input */
        fprintf(out, "{ char _rb[256]; if (fgets(_rb, sizeof(_rb), stdin)) { size_t _l=strlen(_rb); if (_l && _rb[_l-1]=='\\n') _rb[_l-1]='\\0'; strncpy(%s, _rb, 255); %s[255]='\\0'; } }\n",
            node->name, node->name);
    }
}

static void codegen_set(FILE *out, ASTNode *node, int indent) {
    if (node->is_expr) {
        int is_add_sn = strncmp(node->value, "_ltng_add_sn", 12) == 0;
        if (node->var_type == TYPE_STRING || node->var_type == TYPE_OBJECT ||
                   is_add_sn) {
            pad(out, indent);
            fprintf(out, "snprintf(%s, sizeof(%s), \"%%s\", %s);\n",
                node->name, node->name, node->value);
        } else {
            pad(out, indent);
            if (strchr(node->value, '.')) {
                fprintf(out, "{ double _e = (%s); snprintf(%s, sizeof(%s), \"%%g\", _e); }\n",
                    node->value, node->name, node->name);
            } else {
                fprintf(out, "{ long long _e = (%s); snprintf(%s, sizeof(%s), \"%%lld\", _e); }\n",
                    node->value, node->name, node->name);
            }
        }
    } else if (node->var_type == TYPE_STRING && has_interpolation(node->value)) {
        codegen_assign_interpolated(out, node->name, node->value, indent);
    } else {
        pad(out, indent);
        fprintf(out, "snprintf(%s, sizeof(%s), \"", node->name, node->name);
        if (node->var_type == TYPE_FLOAT) {
            char buf[64];
            snprintf(buf, sizeof(buf), "%g", atof(node->value));
            write_c_string(out, buf);
        } else if (node->var_type == TYPE_BOOL) {
            write_c_string(out, bool_to_str(node->value));
        } else {
            write_c_string(out, node->value);
        }
        fprintf(out, "\");\n");
    }
}

static void codegen_statement(FILE *out, ASTNode *node, SymbolTable *table, int indent);
static void emit_block(FILE *out, ASTNode *node, SymbolTable *table, int indent);
static void codegen_assign(FILE *out, ASTNode *node, int indent);
static void codegen_set(FILE *out, ASTNode *node, int indent);
static void codegen_write(FILE *out, ASTNode *node, SymbolTable *table, int indent);
static void codegen_read(FILE *out, ASTNode *node, int indent);
static void codegen_if(FILE *out, ASTNode *node, SymbolTable *table, int indent, int is_first);
static void codegen_while(FILE *out, ASTNode *node, SymbolTable *table, int indent);
static void codegen_do_while(FILE *out, ASTNode *node, SymbolTable *table, int indent);
static void codegen_for(FILE *out, ASTNode *node, SymbolTable *table, int indent);
static void codegen_switch(FILE *out, ASTNode *node, SymbolTable *table, int indent);

static void codegen_if(FILE *out, ASTNode *node, SymbolTable *table, int indent, int is_first) {
    (void)table;
    if (is_first) {
        pad(out, indent);
        fprintf(out, "if (%s) {\n", node->value);
    } else {
        /* chained as else-if in NODE_IF or else body */
        pad(out, indent);
        fprintf(out, "if (%s) {\n", node->value);
    }
    if (node->body) emit_block(out, node->body, table, indent + 1);
    pad(out, indent);
    fprintf(out, "}");
    if (node->orelse) {
        if (node->orelse->type == NODE_IF) {
            fprintf(out, " else ");
            /* emit chained if on same conceptual level: write 'if (...) {' */
            fprintf(out, "if (%s) {\n", node->orelse->value);
            if (node->orelse->body) emit_block(out, node->orelse->body, table, indent + 1);
            pad(out, indent);
            fprintf(out, "}");
            /* recurse for further else chain */
            ASTNode *cur = node->orelse;
            while (cur->orelse) {
                if (cur->orelse->type == NODE_IF) {
                    cur = cur->orelse;
                    fprintf(out, " else if (%s) {\n", cur->value);
                    if (cur->body) emit_block(out, cur->body, table, indent + 1);
                    pad(out, indent);
                    fprintf(out, "}");
                } else {
                    fprintf(out, " else {\n");
                    if (cur->orelse->body) emit_block(out, cur->orelse->body, table, indent + 1);
                    pad(out, indent);
                    fprintf(out, "}");
                    break;
                }
            }
            fprintf(out, "\n");
            return;
        } else {
            fprintf(out, " else {\n");
            if (node->orelse->body) emit_block(out, node->orelse->body, table, indent + 1);
            pad(out, indent);
            fprintf(out, "}\n");
            return;
        }
    }
    fprintf(out, "\n");
}

static void codegen_while(FILE *out, ASTNode *node, SymbolTable *table, int indent) {
    pad(out, indent);
    fprintf(out, "while (%s) {\n", node->value);
    if (node->body) emit_block(out, node->body, table, indent + 1);
    pad(out, indent);
    fprintf(out, "}\n");
}

static void codegen_do_while(FILE *out, ASTNode *node, SymbolTable *table, int indent) {
    pad(out, indent);
    fprintf(out, "do {\n");
    if (node->body) emit_block(out, node->body, table, indent + 1);
    pad(out, indent);
    fprintf(out, "} while (%s);\n", node->value);
}

static void codegen_for(FILE *out, ASTNode *node, SymbolTable *table, int indent) {
    (void)table;
    const char *var = node->name;
    const char *start = node->for_start;
    const char *end = node->for_end;
    const char *step = node->for_step;
    char op = node->for_step_op ? node->for_step_op : '+';
    /* Use a C for loop so that 'continue' jumps to the step expression */
    pad(out, indent);
    fprintf(out, "{\n");
    pad(out, indent + 1);
    fprintf(out, "char %s[256] = \"0\";\n", var);
    pad(out, indent + 1);
    fprintf(out, "snprintf(%s, sizeof(%s), \"%%lld\", (long long)(%s));\n", var, var, start);
    pad(out, indent + 1);
    fprintf(out, "for (; ");
    /* Division and subtraction step downward; others step upward */
    if (op == '-' || op == '/') {
        fprintf(out, "(long long)atoll(%s) >= (long long)(%s)", var, end);
    } else {
        fprintf(out, "(long long)atoll(%s) <= (long long)(%s)", var, end);
    }
    fprintf(out, "; ");
    if (op == '+') {
        fprintf(out, "snprintf(%s, sizeof(%s), \"%%lld\", (long long)atoll(%s) + (long long)(%s))", var, var, var, step);
    } else if (op == '-') {
        fprintf(out, "snprintf(%s, sizeof(%s), \"%%lld\", (long long)atoll(%s) - (long long)(%s))", var, var, var, step);
    } else if (op == '*') {
        fprintf(out, "snprintf(%s, sizeof(%s), \"%%lld\", (long long)atoll(%s) * (long long)(%s))", var, var, var, step);
    } else if (op == '/') {
        fprintf(out, "snprintf(%s, sizeof(%s), \"%%lld\", (long long)(%s) != 0 ? (long long)atoll(%s) / (long long)(%s) : 0)", var, var, var, var, step);
    } else {
        fprintf(out, "snprintf(%s, sizeof(%s), \"%%lld\", (long long)atoll(%s) + (long long)(%s))", var, var, var, step);
    }
    fprintf(out, ") {\n");
    if (node->body) emit_block(out, node->body, table, indent + 2);
    pad(out, indent + 1);
    fprintf(out, "}\n");
    pad(out, indent);
    fprintf(out, "}\n");
}

static void codegen_switch(FILE *out, ASTNode *node, SymbolTable *table, int indent) {
    /* switch implemented as a plain if / else-if chain (no do/while wrapper, no break inside cases) */
    int first = 1;
    for (ASTNode *c = node->cases; c; c = c->next) {
        if (c->is_default) {
            if (first) {
                pad(out, indent);
                fprintf(out, "{\n");
            } else {
                fprintf(out, " else {\n");
            }
            if (c->body) emit_block(out, c->body, table, indent + 1);
            pad(out, indent);
            fprintf(out, "}");
            first = 0;
            break;
        } else {
            pad(out, indent);
            if (first) fprintf(out, "if (");
            else fprintf(out, " else if (");
            /* Use strcmp for string comparison, == for numeric/bool */
            if (node->var_type == TYPE_STRING) {
                fprintf(out, "strcmp(%s, %s) == 0) {\n", node->value, c->value);
            } else {
                fprintf(out, "(%s) == (%s)) {\n", node->value, c->value);
            }
            if (c->body) emit_block(out, c->body, table, indent + 1);
            pad(out, indent);
            fprintf(out, "}");
            first = 0;
        }
    }
    if (!first) fprintf(out, "\n");
}

static void codegen_statement(FILE *out, ASTNode *node, SymbolTable *table, int indent) {
    if (!node) return;
    switch (node->type) {
        case NODE_LET:
        case NODE_CONST:
            codegen_assign(out, node, indent);
            break;
        case NODE_SET:
            codegen_set(out, node, indent);
            break;
        case NODE_WRITE:
            codegen_write(out, node, table, indent);
            break;
        case NODE_ARRAY_PUSH:
            pad(out, indent);
            if (node->var_type == TYPE_STRING) {
                fprintf(out, "snprintf(%s_data[%s_len], 256, \"%%s\", %s); %s_len++;\n",
                    node->name, node->name, node->value, node->name);
            } else if (node->var_type == TYPE_INT) {
                fprintf(out, "snprintf(%s_data[%s_len], 256, \"%%lld\", (long long)(%s)); %s_len++;\n",
                    node->name, node->name, node->value, node->name);
            } else if (node->var_type == TYPE_FLOAT) {
                fprintf(out, "snprintf(%s_data[%s_len], 256, \"%%g\", (double)(%s)); %s_len++;\n",
                    node->name, node->name, node->value, node->name);
            } else {
                fprintf(out, "snprintf(%s_data[%s_len], 256, \"%%s\", %s); %s_len++;\n",
                    node->name, node->name, node->value, node->name);
            }
            break;
        case NODE_ARRAY_POP:
            pad(out, indent);
            if (node->value[0] == '\0') {
                /* pop() - no args */
                fprintf(out, "%s_len--;\n", node->name);
            } else {
                /* pop(index) */
                fprintf(out, "({ int __pop_idx = (int)(%s); __pop_idx = __pop_idx > 0 ? __pop_idx - 1 : __pop_idx + (%s_len); char __pop_val[256]; snprintf(__pop_val, 256, \"%%s\", %s_data[__pop_idx]); memmove(&%s_data[__pop_idx], &%s_data[__pop_idx + 1], (%s_len - __pop_idx - 1) * 256); %s_len--; __pop_val; })",
                    node->value, node->name, node->name, node->name, node->name, node->name, node->name);
            }
            break;
        case NODE_READ:
            codegen_read(out, node, indent);
            break;
        case NODE_IF:
            codegen_if(out, node, table, indent, 1);
            break;
        case NODE_WHILE:
            codegen_while(out, node, table, indent);
            break;
        case NODE_DO_WHILE:
            codegen_do_while(out, node, table, indent);
            break;
        case NODE_FOR:
            codegen_for(out, node, table, indent);
            break;
        case NODE_SWITCH:
            codegen_switch(out, node, table, indent);
            break;
        case NODE_BREAK:
            pad(out, indent); fprintf(out, "break;\n");
            break;
        case NODE_CONTINUE:
            pad(out, indent); fprintf(out, "continue;\n");
            break;
        case NODE_RETURN: {
            if (node->value[0] == '\0' && node->is_expr == 0) {
                pad(out, indent); fprintf(out, "return _ret;\n");
            } else if (node->var_type == TYPE_STRING || node->var_type == TYPE_OBJECT ||
                       node->var_type == TYPE_NULL ||
                       strstr(node->value, "_ltng_add_sn") == node->value) {
                pad(out, indent); fprintf(out, "snprintf(_ret, 256, \"%%s\", %s); return _ret;\n", node->value[0] ? node->value : "\"\"");
            } else if (node->var_type == TYPE_FLOAT) {
                pad(out, indent); fprintf(out, "snprintf(_ret, 256, \"%%g\", (double)(%s)); return _ret;\n", node->value[0] ? node->value : "0");
            } else {
                pad(out, indent);
                if (strchr(node->value, '.')) {
                    fprintf(out, "snprintf(_ret, 256, \"%%g\", (double)(%s)); return _ret;\n", node->value[0] ? node->value : "0");
                } else {
                    fprintf(out, "snprintf(_ret, 256, \"%%lld\", (long long)(%s)); return _ret;\n", node->value[0] ? node->value : "0");
                }
            }
            break;
        }
        case NODE_EXIT: {
            pad(out, indent);
            if (node->var_type == TYPE_FLOAT) {
                fprintf(out, "exit((int)(%s));\n", node->value[0] ? node->value : "0");
            } else {
                fprintf(out, "exit((long long)(%s));\n", node->value[0] ? node->value : "0");
            }
            break;
        }
        case NODE_CALL: {
            /* base-assign call statement: receiver = func(receiver, args...)
               Used when the function's first parameter is named "base" and it
               is called via UFCS (receiver.method(args)). The return value is
               written back to the receiver variable. */
            if (node->is_base_assign && node->args && node->args->value[0] != '\0') {
                const char *recv = node->args->value;
                pad(out, indent);
                fprintf(out, "snprintf(%s, sizeof(%s), \"%%s\", %s(_ltng_retbuf()", recv, recv, node->name);
                for (ASTNode *a = node->args; a; a = a->next) {
                    if (a->var_type == TYPE_STRING && a->value[0] == '"') {
                        fprintf(out, ", %s", a->value);
                    } else if (a->is_variable && a->value[0] != '\0') {
                        fprintf(out, ", %s", a->value);
                    } else if ((a->var_type == TYPE_STRING || a->var_type == TYPE_OBJECT) && a->value[0] != '\0') {
                        fprintf(out, ", %s", a->value);
                    } else if (a->value[0] == '\0') {
                        fprintf(out, ", \"\"");
                    } else {
                        fprintf(out, ", _ltng_ad((double)(%s))", a->value);
                    }
                }
                fprintf(out, "));\n");
                break;
            }
            /* call statement - invoke and discard result */
            pad(out, indent);
            fprintf(out, "%s(_ltng_retbuf()", node->name);
            for (ASTNode *a = node->args; a; a = a->next) {
                if (a->var_type == TYPE_STRING && a->value[0] == '"') {
                    fprintf(out, ", %s", a->value);
                } else if (a->is_variable && a->value[0] != '\0') {
                    fprintf(out, ", %s", a->value);
                } else if ((a->var_type == TYPE_STRING || a->var_type == TYPE_OBJECT) && a->value[0] != '\0') {
                    fprintf(out, ", %s", a->value);
                } else if (a->value[0] == '\0') {
                    fprintf(out, ", \"\"");
                } else {
                    fprintf(out, ", _ltng_ad((double)(%s))", a->value);
                }
            }
            fprintf(out, ");\n");
            break;
        }
        default:
            break;
    }
}

static void emit_block(FILE *out, ASTNode *node, SymbolTable *table, int indent) {
    for (ASTNode *cur = node; cur; cur = cur->next) {
        codegen_statement(out, cur, table, indent);
    }
}

static ASTNode *find_function_by_cname(ASTNode *fns, const char *cname) {
    for (ASTNode *fn = fns; fn; fn = fn->next) {
        if (strcmp(fn->name, cname) == 0) return fn;
    }
    return NULL;
}

#define MAX_PARAM_SCHEMAS 256
typedef struct {
    char fn_name[MAX_IDENTIFIER_LEN];
    int param_index;
    char arg_var[MAX_IDENTIFIER_LEN];
} ParamSchema;
static ParamSchema g_param_schemas[MAX_PARAM_SCHEMAS];
static int g_param_schema_count = 0;

/* Record that an object variable is passed to a function parameter at a call site. */
static void record_param_schema(ASTNode *call, ASTNode *fns) {
    ASTNode *fn = find_function_by_cname(fns, call->name);
    if (!fn) return;
    int pidx = 0;
    for (ASTNode *p = fn->params, *a = call->args; p && a; p = p->next, a = a->next, pidx++) {
        if (a->is_variable && a->value[0] != '\0' && obj_lookup(a->value)) {
            /* avoid duplicates */
            int dup = 0;
            for (int i = 0; i < g_param_schema_count; i++) {
                if (strcmp(g_param_schemas[i].fn_name, fn->name) == 0 &&
                    g_param_schemas[i].param_index == pidx &&
                    strcmp(g_param_schemas[i].arg_var, a->value) == 0) { dup = 1; break; }
            }
            if (!dup && g_param_schema_count < MAX_PARAM_SCHEMAS) {
                ParamSchema *e = &g_param_schemas[g_param_schema_count++];
                strncpy(e->fn_name, fn->name, MAX_IDENTIFIER_LEN - 1);
                e->fn_name[MAX_IDENTIFIER_LEN - 1] = '\0';
                e->param_index = pidx;
                strncpy(e->arg_var, a->value, MAX_IDENTIFIER_LEN - 1);
                e->arg_var[MAX_IDENTIFIER_LEN - 1] = '\0';
            }
        }
    }
}

static void traverse_record_param_schemas(ASTNode *node, ASTNode *fns) {
    if (!node) return;
    if (node->type == NODE_CALL) {
        record_param_schema(node, fns);
    }
    traverse_record_param_schemas(node->body, fns);
    traverse_record_param_schemas(node->orelse, fns);
    traverse_record_param_schemas(node->cases, fns);
    traverse_record_param_schemas(node->args, fns);
    traverse_record_param_schemas(node->next, fns);
}

/* Merge all object schemas recorded for a function parameter into a combined schema.
   Returns the number of fields in the combined schema. */
static int merge_param_schemas(ObjSchema *combined, const char *fn_name, int param_index) {
    memset(combined, 0, sizeof(ObjSchema));
    int field_count = 0;
    for (int i = 0; i < g_param_schema_count; i++) {
        if (strcmp(g_param_schemas[i].fn_name, fn_name) != 0)
            continue;
        if (g_param_schemas[i].param_index != param_index)
            continue;
        ObjSchema *sc = obj_lookup(g_param_schemas[i].arg_var);
        if (!sc) continue;
        for (int fi = 0; fi < sc->count && field_count < MAX_OBJ_FIELDS; fi++) {
            /* check if field already in combined */
            int dup = 0;
            for (int k = 0; k < field_count; k++) {
                if (strcmp(combined->fields[k].name, sc->fields[fi].name) == 0) { dup = 1; break; }
            }
            if (!dup) {
                combined->fields[field_count] = sc->fields[fi];
                field_count++;
            }
        }
    }
    combined->count = field_count;
    strncpy(combined->var, fn_name, MAX_IDENTIFIER_LEN - 1);
    return field_count;
}

void codegen(Program *program, SymbolTable *table, FILE *out, ErrorState *errors) {
    codegen_errors = errors;
    codegen_table = table;

    /* Pre-pass: record object schemas passed to function parameters at all call sites */
    if (program->functions) {
        traverse_record_param_schemas(program->head, program->functions);
        for (ASTNode *fn = program->functions; fn; fn = fn->next) {
            traverse_record_param_schemas(fn->body, program->functions);
        }
    }

    fprintf(out, "#include <stdio.h>\n");
    fprintf(out, "#include <string.h>\n");
    fprintf(out, "#include <stdlib.h>\n");
    fprintf(out, "#include <stdarg.h>\n");
    fprintf(out, "#include <ctype.h>\n");
    fprintf(out, "static char *_ltng_concat(char *buf, size_t sz, int n, ...) { va_list ap; va_start(ap,n); buf[0]=0; for(int i=0;i<n;i++){ const char *s=va_arg(ap,const char*); strncat(buf,s,sz-strlen(buf)-1); } va_end(ap); return buf; }\n");
    fprintf(out, "static char *_ltng_repeat(char *buf, size_t sz, const char *s, long long n) { buf[0]=0; if (n < 0) n = 0; for (long long i=0; i<n && strlen(buf) < sz-1; i++) { strncat(buf, s, sz-strlen(buf)-1); } return buf; }\n");
    fprintf(out, "static char *_ltng_arr_get(char data[][256], int len, int idx) { if (idx >= 0 && idx < len) return data[idx]; if (idx < 0 && len + idx >= 0) return data[len + idx]; return \"\"; }\n");
    fprintf(out, "static int _ltng_idx2(int i0, int i1, int d1) { return i0 * d1 + i1; }\n");
    fprintf(out, "static int _ltng_idx3(int i0, int i1, int i2, int d1, int d2) { return (i0 * d1 + i1) * d2 + i2; }\n");
    fprintf(out, "static char *_ltng_retbuf(void){ static char b[32][256]; static int i; i=(i+1)%%32; b[i][0]=0; return b[i]; }\n");
    fprintf(out, "static char *_ltng_ad(double v){ char *b=_ltng_retbuf(); snprintf(b,256,\"%%g\",v); return b; }\n");
    fprintf(out, "static char *_ltng_as(const char *s){ char *b=_ltng_retbuf(); if(s){ strncpy(b,s,255); b[255]=0; } return b; }\n");
    fprintf(out, "static char *_ltng_from_int(char *buf, long long v){ snprintf(buf,256,\"%%lld\",v); return buf; }\n");
    fprintf(out, "static char *_ltng_from_double(char *buf, double v){ snprintf(buf,256,\"%%g\",v); return buf; }\n");
    fprintf(out, "static int _ltng_is_num_str(const char *s){ if(!s||!*s) return 0; const char *p=s; if(*p=='-'||*p=='+') p++; int h=0; for(;*p;p++){ if(isdigit(*p)) h=1; else if(*p=='.'){} else return 0; } return h; }\n");
    fprintf(out, "static char *_ltng_add_sn(const char *a, const char *b){ static char buf[32][256]; static int _ltng_asn_idx=0; _ltng_asn_idx=(_ltng_asn_idx+1)%%32; char *r=buf[_ltng_asn_idx]; r[0]=0; if(_ltng_is_num_str(a)&&_ltng_is_num_str(b)){ double na=atof(a),nb=atof(b); snprintf(r,256,\"%%g\",na+nb); } else { snprintf(r,256,\"%%s%%s\",a,b); } return r; }\n");
    fprintf(out, "\n");

    /* Emit function prototypes and definitions */
    if (program->functions) {
        ASTNode *fns[256]; int fncount = 0;
        for (ASTNode *fn = program->functions; fn && fncount < 256; fn = fn->next) fns[fncount++] = fn;
        /* prototypes */
        for (int i = fncount - 1; i >= 0; i--) {
            ASTNode *fn = fns[i];
            fprintf(out, "static char *%s(char *_ret", fn->name);
            for (ASTNode *p = fn->params; p; p = p->next)
                fprintf(out, ", const char *_a_%s", p->name);
            fprintf(out, ");\n");
        }
        fprintf(out, "\n");
        for (int i = fncount - 1; i >= 0; i--) {
            ASTNode *fn = fns[i];
            fprintf(out, "static char *%s(char *_ret", fn->name);
            for (ASTNode *p = fn->params; p; p = p->next)
                fprintf(out, ", const char *_a_%s", p->name);
            fprintf(out, ") {\n");
            fprintf(out, "    if(!_ret) _ret=_ltng_retbuf(); _ret[0]=0;\n");
            for (ASTNode *p = fn->params; p; p = p->next) {
                fprintf(out, "    char %s[256] = \"\"; if(_a_%s) { strncpy(%s, _a_%s, 255); %s[255]=0; }\n",
                    p->name, p->name, p->name, p->name, p->name);
            }
            /* declare object field variables for object-typed parameters */
            for (ASTNode *p = fn->params; p; p = p->next) {
                /* find the parameter index */
                int pidx = 0;
                for (ASTNode *pp = fn->params; pp && pp != p; pp = pp->next) pidx++;
                /* Merge schemas from all call sites that pass an object to this parameter */
                ObjSchema combined;
                int fcount = merge_param_schemas(&combined, fn->name, pidx);
                if (fcount > 0) {
                    for (int fi = 0; fi < combined.count; fi++) {
                        char fv[MAX_IDENTIFIER_LEN*2+8];
                        obj_resolve_field(p->name, combined.fields[fi].name, fv, sizeof(fv));
                        int bsz = (strcmp(combined.fields[fi].name, "obj") == 0) ? 512 : 256;
                        fprintf(out, "    char %s[%d] = \"", fv, bsz);
                        { const char *v=combined.fields[fi].value; for(;*v;v++){ if(*v=='\\'||*v=='\"') fputc('\\',out); if(*v=='\\n'){fputs("\\n",out);} else if(*v=='\\t'){fputs("\\t",out);} else fputc(*v,out);} }
                        fprintf(out, "\";\n");
                    }
                }
            }
            /* declare locals from body */
            {
                ASTNode *q[4096]; int qs=0, qe=0;
                for (ASTNode *n = fn->body; n; n = n->next) if (qe < 4096) q[qe++] = n;
                char decls[512][MAX_IDENTIFIER_LEN*2+8]; int dcount = 0;
                while (qs < qe) {
                    ASTNode *n = q[qs++];
                    if (!n) continue;
                    if (n->type == NODE_LET || n->type == NODE_CONST) {
                        int dup = 0;
                        for (ASTNode *p = fn->params; p; p = p->next) if (strcmp(p->name, n->name)==0) dup=1;
                        for (int k=0;k<dcount;k++) if (strcmp(decls[k], n->name)==0) dup=1;
                        if (!dup && dcount < 512) { strncpy(decls[dcount], n->name, MAX_IDENTIFIER_LEN*2+7); decls[dcount][MAX_IDENTIFIER_LEN*2+7]=0; dcount++; }
                        if (n->var_type == TYPE_OBJECT) {
                            ObjSchema *sc = obj_lookup(n->name);
                            if (sc) {
                                for (int fi = 0; fi < sc->count && dcount < 512; fi++) {
                                    char fv[MAX_IDENTIFIER_LEN*2+8];
                                    obj_resolve_field(n->name, sc->fields[fi].name, fv, sizeof(fv));
                                    int d2 = 0;
                                    for (int k=0;k<dcount;k++) if (strcmp(decls[k], fv)==0) d2=1;
                                    if (!d2) { strncpy(decls[dcount], fv, MAX_IDENTIFIER_LEN*2+7); decls[dcount][MAX_IDENTIFIER_LEN*2+7]=0; dcount++; }
                                }
                            }
                        }
                    }
                    for (ASTNode *c = n->body; c; c = c->next) if (qe < 4096) q[qe++] = c;
                    for (ASTNode *c = n->orelse; c; c = c->next) if (qe < 4096) q[qe++] = c;
                    for (ASTNode *c = n->cases; c; c = c->next) if (qe < 4096) q[qe++] = c;
                }
                for (int k=0;k<dcount;k++)
                    fprintf(out, "    char %s[256] = \"\";\n", decls[k]);
            }
            if (fn->body) emit_block(out, fn->body, table, 1);
            fprintf(out, "    return _ret;\n");
            fprintf(out, "}\n\n");
        }
    }

    fprintf(out, "int main(void) {\n");

    for (int i = 0; i < table->count; i++) {
        if (table->vars[i].type == TYPE_ARRAY) {
            /* Array variables are declared in codegen_assign with their full
               array declaration. Skip here to avoid duplicate declaration. */
            continue;
        }
        if (table->vars[i].type == TYPE_OBJECT) {
            ObjSchema *sc = obj_lookup(table->vars[i].name);
            const char *dv = sc ? sc->default_val : "";
            fprintf(out, "    char %s[256] = \"", table->vars[i].name);
            write_c_string(out, dv);
            fprintf(out, "\";\n");
            if (sc) {
                for (int fi = 0; fi < sc->count; fi++) {
                    char fv[MAX_IDENTIFIER_LEN*2+8];
                    obj_resolve_field(table->vars[i].name, sc->fields[fi].name, fv, sizeof(fv));
                    int bsz = (strcmp(sc->fields[fi].name, "obj") == 0) ? 512 : 256;
                    fprintf(out, "    char %s[%d] = \"", fv, bsz);
                    write_c_string(out, sc->fields[fi].value);
                    fprintf(out, "\";\n");
                }
            }
        } else {
            fprintf(out, "    char %s[256] = \"", table->vars[i].name);
            write_c_string(out, default_for(table->vars[i].type));
            fprintf(out, "\";\n");
        }
    }
    if (table->count > 0) fprintf(out, "\n");

    emit_block(out, program->head, table, 1);

    fprintf(out, "    return 0;\n");
    fprintf(out, "}\n");
}
