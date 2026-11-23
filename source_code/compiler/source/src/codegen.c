#include "ltng.h"

static ErrorState *codegen_errors = NULL;
static SymbolTable *codegen_table = NULL;
static CodegenLog *codegen_log = NULL;

/* -------- Phase 4 exception-codegen bookkeeping -------- */
#define CG_MAX_CLEANUPS 128
#define CG_MAX_LOOPS 64
typedef struct {
    char *top_expr;        /* assignment such as _ltng_tf0.prev */
    ASTNode *finally_body;    /* NULL when the try has no finally */
} CgCleanup;

static CgCleanup g_cleanups[CG_MAX_CLEANUPS];
static int g_cleanup_count = 0;
static int g_loop_bases[CG_MAX_LOOPS];
static int g_loop_depth = 0;
static int g_try_uid = 0;
static int g_codegen_in_function = 0;
static ASTNode *g_codegen_current_fn = NULL;
static ASTNode *g_codegen_all_fns = NULL;
static ASTNode *g_codegen_program_fns = NULL;

static void pad(FILE *out, int indent);

static ObjSchema *fn_return_obj_schema(ASTNode *fn) {
    if (!fn || !fn->return_obj_base[0]) return NULL;
    if (fn->return_type != TYPE_OBJECT) return NULL;
    return obj_lookup(fn->return_obj_base);
}

static void emit_obj_return_staging(FILE *out, ASTNode *fn, ASTNode *ret_node, int indent) {
    ObjSchema *sc = fn_return_obj_schema(fn);
    if (!sc || !ret_node) return;
    if (ret_node->var_type != TYPE_OBJECT || !ret_node->value[0]) return;
    /* only for variable/base returns (object value in a named object) */
    if (ret_node->value[0] == '"' || ret_node->value[0] == '(') return;
    char stage_base[MAX_IDENTIFIER_LEN*2+16];
    snprintf(stage_base, sizeof(stage_base), "__ltng_oret_%s", fn->name);
    for (int i = 0; i < sc->count; i++) {
        char src_f[MAX_IDENTIFIER_LEN*2+8], dst_f[MAX_IDENTIFIER_LEN*2+8];
        obj_resolve_field(ret_node->value, sc->fields[i].name, src_f, sizeof(src_f));
        obj_resolve_field(stage_base, sc->fields[i].name, dst_f, sizeof(dst_f));
        int bsz = (strcmp(sc->fields[i].name, "obj") == 0) ? 512 : 256;
        pad(out, indent);
        fprintf(out, "_ltng_ds_set(&%s, %s);\n", dst_f, src_f);
    }
    pad(out, indent);
    fprintf(out, "_ltng_ds_set(&%s, %s);\n", stage_base, ret_node->value);
}

void codegen_set_log(CodegenLog *log) {
    codegen_log = log;
}

ErrorState *codegen_get_errors(void) {
    return codegen_errors;
}

static const char *node_type_name_cg(NodeType t) {
    switch (t) {
        case NODE_WRITE: return "WRITE";
        case NODE_READ: return "READ";
        case NODE_LET: return "LET";
        case NODE_CONST: return "CONST";
        case NODE_SET: return "SET";
        case NODE_IF: return "IF";
        case NODE_ELSE: return "ELSE";
        case NODE_SWITCH: return "SWITCH";
        case NODE_CASE: return "CASE";
        case NODE_BREAK: return "BREAK";
        case NODE_CONTINUE: return "CONTINUE";
        case NODE_WHILE: return "WHILE";
        case NODE_DO_WHILE: return "DO_WHILE";
        case NODE_FOR: return "FOR";
        case NODE_FUNCTION: return "FUNCTION";
        case NODE_RETURN: return "RETURN";
        case NODE_EXIT: return "EXIT";
        case NODE_CALL: return "CALL";
        case NODE_PARAM: return "PARAM";
        case NODE_PROGRAM: return "PROGRAM";
        case NODE_ARRAY_LIT: return "ARRAY_LIT";
        case NODE_ARRAY_ACCESS: return "ARRAY_ACCESS";
        case NODE_ARRAY_PUSH: return "ARRAY_PUSH";
        case NODE_ARRAY_POP: return "ARRAY_POP";
        case NODE_ARRAY_SET: return "ARRAY_SET";
        case NODE_ARRAY_INSERT: return "ARRAY_INSERT";
        case NODE_ARRAY_REMOVE: return "ARRAY_REMOVE";
        case NODE_ARRAY_COPY: return "ARRAY_COPY";
        case NODE_ARRAY_SLICE: return "ARRAY_SLICE";
        case NODE_ARRAY_SORT: return "ARRAY_SORT";
        case NODE_ARRAY_REVERSE: return "ARRAY_REVERSE";
        case NODE_BLOCK: return "BLOCK";
        case NODE_TRY: return "TRY";
        case NODE_THROW: return "THROW";
        default: return "NODE";
    }
}

static void log_span(FILE *f, const char *src_file, const char *label, int sl,int sc,int el,int ec, const char *c_expr) {
    if (!f) return;
    if (!c_expr || !c_expr[0]) return;
    if (sl > 0) {
        fprintf(f, "  [expr:%s] ltng %s:%d:%d-%d:%d\n", label, src_file ? src_file : "<input>", sl, sc, el, ec);
    } else {
        fprintf(f, "  [expr:%s] ltng-span: unknown\n", label);
    }
    fprintf(f, "    C: %s\n", c_expr);
}

static void log_emitted_node(const char *src_file, ASTNode *node, const char *c_code) {
    if (!codegen_log || !codegen_log->enabled || !codegen_log->file || !node) return;
    fprintf(codegen_log->file, "LTNG %s:%d:%d | %s", src_file ? src_file : "<input>", node->line, node->col, node_type_name_cg(node->type));
    if (node->name && node->name[0]) fprintf(codegen_log->file, " (%s)", node->name);
    fprintf(codegen_log->file, "\n");

    /* expression/codegen-level mappings */
    if (node->type == NODE_IF || node->type == NODE_WHILE || node->type == NODE_DO_WHILE || node->type == NODE_SWITCH) {
        log_span(codegen_log->file, src_file, "condition", node->v_sl,node->v_sc,node->v_el,node->v_ec, node->value);
    } else if (node->type == NODE_SET || node->type == NODE_LET || node->type == NODE_CONST) {
        log_span(codegen_log->file, src_file, "rhs", node->v_sl,node->v_sc,node->v_el,node->v_ec, node->value);
    } else if (node->type == NODE_RETURN || node->type == NODE_EXIT) {
        log_span(codegen_log->file, src_file, "expr", node->v_sl,node->v_sc,node->v_el,node->v_ec, node->value);
    } else if (node->type == NODE_ARRAY_PUSH || node->type == NODE_ARRAY_POP || node->type == NODE_ARRAY_SET ||
               node->type == NODE_ARRAY_INSERT || node->type == NODE_ARRAY_REMOVE || node->type == NODE_ARRAY_COPY ||
               node->type == NODE_ARRAY_SLICE) {
        if (node->value[0]) log_span(codegen_log->file, src_file, "value_or_index", node->v_sl,node->v_sc,node->v_el,node->v_ec, node->value);
        if (node->array_index[0]) log_span(codegen_log->file, src_file, "array_index_or_arg", node->idx_sl,node->idx_sc,node->idx_el,node->idx_ec, node->array_index);
    } else if (node->type == NODE_FOR) {
        log_span(codegen_log->file, src_file, "for_start", node->fs_sl,node->fs_sc,node->fs_el,node->fs_ec, node->for_start);
        log_span(codegen_log->file, src_file, "for_end", node->fe_sl,node->fe_sc,node->fe_el,node->fe_ec, node->for_end);
        if (node->for_step[0]) log_span(codegen_log->file, src_file, "for_step", node->fst_sl,node->fst_sc,node->fst_el,node->fst_ec, node->for_step);
    } else if (node->type == NODE_WRITE) {
        if (node->value[0] || node->is_expr) log_span(codegen_log->file, src_file, "write_value", node->v_sl,node->v_sc,node->v_el,node->v_ec, node->value);
    } else if (node->type == NODE_CASE) {
        if (!node->is_default && node->value[0]) log_span(codegen_log->file, src_file, "case_expr", node->v_sl,node->v_sc,node->v_el,node->v_ec, node->value);
    } else {
        if (node->value[0]) log_span(codegen_log->file, src_file, "value", node->v_sl,node->v_sc,node->v_el,node->v_ec, node->value);
    }

    /* argument-level mappings for calls/writes/other arg-bearing nodes */
    if (node->args) {
        int ai = 0;
        for (ASTNode *a = node->args; a; a = a->next) {
            char *label = (char*)malloc(128);
            if(!label) label=(char*)calloc(1,1);
            snprintf(label, 128, "arg[%d]", ai++);
            if (a->value[0] || a->is_expr || a->is_variable) {
                if (a->v_sl > 0) {
                    fprintf(codegen_log->file, "  [expr:%s] ltng %s:%d:%d-%d:%d\n", label, src_file ? src_file : "<input>", a->v_sl,a->v_sc,a->v_el,a->v_ec);
                } else {
                    fprintf(codegen_log->file, "  [expr:%s] ltng-span: unknown\n", label);
                }
                fprintf(codegen_log->file, "    C: %s%s\n", a->value[0]?a->value:"", (a->is_variable && a->value[0])?" (var)":"");
            }
        }
    }

    fprintf(codegen_log->file, "---- C ----\n");
    if (c_code && c_code[0]) {
        fputs(c_code, codegen_log->file);
        if (c_code[strlen(c_code)-1] != '\n') fputc('\n', codegen_log->file);
    }
    fprintf(codegen_log->file, "-----------\n\n");
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

/* Emit a C string literal body without doubling percent signs (used for
   constants that are never interpreted as printf format strings). */
static void write_c_string_raw(FILE *out, const char *str) {
    for (const char *p = str; *p; p++) {
        switch (*p) {
            case '\\': fputs("\\\\", out); break;
            case '"': fputs("\\\"", out); break;
            case '\n': fputs("\\n", out); break;
            case '\r': fputs("\\r", out); break;
            case '\t': fputs("\\t", out); break;
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
        case TYPE_VOID: return "";
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
    char *mvname = mangle_name_alloc(name);
    int _has = symbol_table_has(table, mvname);
    free(mvname);
    if (_has) return 1;
    return 0;
}

static void codegen_assign_interpolated(FILE *out, const char *varname, const char *str, int indent) {
    pad(out, indent);
    fprintf(out, "_ltng_ds_setfmt(&%s, \"", varname);
    int arg_count = 0;
    const char *p;
    for (p = str; *p; ) {
        if (*p == '<') {
            p++;
            while (*p && *p != '>') {
                char *tmp_s = (char*)malloc(2); if(tmp_s){tmp_s[0]=*p; tmp_s[1]=0; write_c_string(out, tmp_s); free(tmp_s);}
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
                char *vname = (char*)malloc((size_t)len + 1);
                if (!vname) { p++; continue; }
                memcpy(vname, start, (size_t)len);
                vname[len] = '\0';
                if (is_simple_var_or_keyword(vname, codegen_table)) {
                    fprintf(out, "%%s");
                } else {
                    char expr_out[MAX_STRING_LEN];
                    VarType etype = parse_expression_from_string(vname, codegen_table,
                        expr_out, sizeof(expr_out));
                    if (etype == TYPE_FLOAT) {
                        fprintf(out, "%%g");
                    } else if (etype == TYPE_STRING || etype == TYPE_CHAR || etype == TYPE_OBJECT) {
                        fprintf(out, "%%s");
                    } else if (etype == TYPE_BOOL) {
                        fprintf(out, "%%s");
                    } else {
                        fprintf(out, "%%lld");
                    }
                }
                free(vname);
                arg_count++;
                p++;
            } else {
                write_c_string(out, "$[");
            }
        } else if (*p == '%') {
            fprintf(out, "%%%%");
            p++;
        } else {
            char *tmp_s = (char*)malloc(2);
            if(tmp_s){ tmp_s[0]=*p; tmp_s[1]=0; write_c_string(out, tmp_s); free(tmp_s); }
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
                        char *mvname = mangle_name_alloc(vname);
                        if (symbol_table_has(codegen_table, mvname)) {
                            fprintf(out, ", %s", mvname);
                        } else {
                            fprintf(out, ", \"%s\"", vname);
                        }
                        free(mvname);
                    } else {
                        char expr_out[MAX_STRING_LEN];
                        VarType etype = parse_expression_from_string(vname, codegen_table,
                            expr_out, sizeof(expr_out));
                        if (etype == TYPE_FLOAT) {
                            fprintf(out, ", (double)(%s)", expr_out);
                        } else if (etype == TYPE_STRING || etype == TYPE_CHAR || etype == TYPE_OBJECT) {
                            fprintf(out, ", %s", expr_out);
                        } else if (etype == TYPE_BOOL) {
                            fprintf(out, ", ((%s)?\"true\":\"false\")", expr_out);
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
                char *tmp_s = (char*)malloc(2); if(tmp_s){tmp_s[0]=*p; tmp_s[1]=0; write_c_string(out, tmp_s); free(tmp_s);}
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
                char *vname = (char*)malloc((size_t)len + 1);
                if (!vname) { p++; continue; }
                memcpy(vname, start, (size_t)len);
                vname[len] = '\0';
                if (is_simple_var_or_keyword(vname, codegen_table)) {
                    fprintf(out, "%%s");
                } else {
                    char expr_out[MAX_STRING_LEN];
                    VarType etype = parse_expression_from_string(vname, codegen_table,
                        expr_out, sizeof(expr_out));
                    if (etype == TYPE_FLOAT) {
                        fprintf(out, "%%g");
                    } else if (etype == TYPE_STRING || etype == TYPE_CHAR || etype == TYPE_OBJECT) {
                        fprintf(out, "%%s");
                    } else if (etype == TYPE_BOOL) {
                        fprintf(out, "%%s");
                    } else {
                        fprintf(out, "%%lld");
                    }
                }
                free(vname);
                arg_count++;
                p++;
            } else {
                write_c_string(out, "$[");
            }
        } else if (*p == '%') {
            fprintf(out, "%%%%");
            p++;
        } else {
            char *tmp_s = (char*)malloc(2);
            if(tmp_s){ tmp_s[0]=*p; tmp_s[1]=0; write_c_string(out, tmp_s); free(tmp_s); }
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
                    char *vname = (char*)malloc((size_t)len + 1);
                    if (!vname) { p++; continue; }
                    memcpy(vname, start, (size_t)len);
                    vname[len] = '\0';
                    if (strcmp(vname, "true") == 0 || strcmp(vname, "t") == 0) {
                        fprintf(out, ", \"true\"");
                    } else if (strcmp(vname, "false") == 0 || strcmp(vname, "f") == 0) {
                        fprintf(out, ", \"false\"");
                    } else if (is_simple_var_or_keyword(vname, codegen_table)) {
                        char *mvname = mangle_name_alloc(vname);
                        if (symbol_table_has(codegen_table, mvname)) {
                            fprintf(out, ", %s", mvname);
                        } else {
                            fprintf(out, ", \"%s\"", vname);
                        }
                        free(mvname);
                    } else {
                        char expr_out[MAX_STRING_LEN];
                        VarType etype = parse_expression_from_string(vname, codegen_table,
                            expr_out, sizeof(expr_out));
                        if (etype == TYPE_FLOAT) {
                            fprintf(out, ", (double)(%s)", expr_out);
                        } else if (etype == TYPE_STRING || etype == TYPE_CHAR || etype == TYPE_OBJECT) {
                            fprintf(out, ", %s", expr_out);
                        } else if (etype == TYPE_BOOL) {
                            fprintf(out, ", ((%s)?\"true\":\"false\")", expr_out);
                        } else {
                            fprintf(out, ", (long long)(%s)", expr_out);
                        }
                    }
                    free(vname);
                    arg_count++;
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

static int is_var_captured_by_nested(ASTNode *fn, ASTNode *all_fns, const char *var_cname) {
    if (!fn || !all_fns || !var_cname || !var_cname[0]) return 0;
    char prefix[MAX_IDENTIFIER_LEN + 4];
    snprintf(prefix, sizeof(prefix), "%s__", fn->name);
    size_t plen = strlen(prefix);
    for (ASTNode *nf = all_fns; nf; nf = nf->next) {
        if (strncmp(nf->name, prefix, plen) != 0) continue;
        /* nf is nested inside fn (directly or transitively - but captures list has cnames) */
        for (int i = 0; i < nf->capture_count; i++) {
            if (strcmp(nf->captures[i], var_cname) == 0) return 1;
        }
    }
    return 0;
}


static int cg_find_top_binop(const char *expr, char op, int *out_pos) {
    int depth = 0, in_str = 0, in_chr = 0, esc = 0;
    int last = -1;
    for (int i = 0; expr[i]; i++) {
        char c = expr[i];
        if (in_str) { if (esc) esc = 0; else if (c == '\\') esc = 1; else if (c == '"') in_str = 0; continue; }
        if (in_chr) { if (esc) esc = 0; else if (c == '\\') esc = 1; else if (c == '\'') in_chr = 0; continue; }
        if (c == '"') { in_str = 1; continue; }
        if (c == '\'') { in_chr = 1; continue; }
        if (c == '(' || c == '[' || c == '{') { depth++; continue; }
        if (c == ')' || c == ']' || c == '}') { if (depth > 0) depth--; continue; }
        if (depth == 0 && c == op) {
            last = i;
        }
    }
    if (last >= 0) { *out_pos = last; return 1; }
    return 0;
}

/* Rewrite / and % into safe runtime helpers. Mode: 0=int, 1=float, 2=generic (uses double-safe div, int-safe mod via fmod when needed? prefer int mod only if clearly int).
   For broad coverage we apply a conservative best-effort transform on top-level binary ops within each scanned segment. */
static int cg_expr_has_divmod(const char *expr) {
    int depth = 0, in_str = 0, in_chr = 0, esc = 0;
    for (int i = 0; expr[i]; i++) {
        char c = expr[i];
        if (in_str) { if (esc) esc = 0; else if (c == '\\') esc = 1; else if (c == '"') in_str = 0; continue; }
        if (in_chr) { if (esc) esc = 0; else if (c == '\\') esc = 1; else if (c == '\'') in_chr = 0; continue; }
        if (c == '"') { in_str = 1; continue; }
        if (c == '\'') { in_chr = 1; continue; }
        if (c == '(' || c == '[' || c == '{') { depth++; continue; }
        if (c == ')' || c == ']' || c == '}') { if (depth > 0) depth--; continue; }
        if ((c == '/' || c == '%') && depth == 0) return 1;
    }
    return 0;
}

static int cg_rewrite_divmod(char *out, size_t outsz, const char *expr, int is_float, int line, int col) {
    if (!expr || !expr[0]) return 0;
    if (!strchr(expr, '/') && !strchr(expr, '%')) return 0;

    size_t expr_len = strlen(expr);
    char *tmp = (char*)malloc(expr_len + 1);
    if (!tmp) return 0;
    memcpy(tmp, expr, expr_len + 1);

    /* trim */
    char *t = tmp; while (*t==' '||*t=='\t'||*t=='\n'||*t=='\r') t++;
    size_t L = strlen(t); while (L>0 && (t[L-1]==' '||t[L-1]=='\t'||t[L-1]=='\n'||t[L-1]=='\r')) { t[--L]=0; }

    /* Unwrap one layer of outer parens that wrap the whole expression (repeat a few times) */
    for (int pass = 0; pass < 4; pass++) {
        L = strlen(t);
        if (L>=2 && t[0]=='(' && t[L-1]==')') {
            int d=0,in_s=0,in_c=0,esc=0,ok=1;
            for (size_t i=0;i<L;i++){ char c=t[i];
                if(in_s){ if(esc)esc=0; else if(c=='\\')esc=1; else if(c=='"')in_s=0; continue; }
                if(in_c){ if(esc)esc=0; else if(c=='\\')esc=1; else if(c=='\'')in_c=0; continue; }
                if(c=='"'){in_s=1;continue;} if(c=='\''){in_c=1;continue;}
                if(c=='(')d++; else if(c==')'){d--; if(d==0 && i+1<L){ok=0;break;}}
            }
            if(ok && d==0){ t[L-1]=0; t++; while(*t==' '||*t=='\t') t++; continue; }
        }
        break;
    }

    /* Only rewrite if there's a div/mod at top level in this (possibly unwrapped) expr. */
    if (!cg_expr_has_divmod(t)) { free(tmp); return 0; }

    int pdiv=-1, pmod=-1;
    cg_find_top_binop(t, '/', &pdiv);
    cg_find_top_binop(t, '%', &pmod);
    int pos = -1; char op = 0;
    if (pmod > pdiv) { pos = pmod; op = '%'; } else if (pdiv >= 0) { pos = pdiv; op = '/'; }
    if (pos < 0) { free(tmp); return 0; }

    size_t tl = strlen(t);
    char *lhs_raw = (char*)malloc(tl + 1);
    char *rhs_raw = (char*)malloc(tl + 1);
    if(!lhs_raw||!rhs_raw){ free(lhs_raw); free(rhs_raw); free(tmp); return 0; }
    int lp = pos; if (lp < 0) lp = 0;
    memcpy(lhs_raw, t, (size_t)lp); lhs_raw[lp]=0;
    strncpy(rhs_raw, t+pos+1, tl); rhs_raw[tl]=0;

    /* trim lhs/rhs */
    char *a=lhs_raw; while(*a==' '||*a=='\t') a++; size_t la=strlen(a); while(la>0&&(a[la-1]==' '||a[la-1]=='\t')) a[--la]=0;
    char *b=rhs_raw; while(*b==' '||*b=='\t') b++; size_t lb=strlen(b); while(lb>0&&(b[lb-1]==' '||b[lb-1]=='\t')) b[--lb]=0;
    if (!*a || !*b) { free(lhs_raw); free(rhs_raw); free(tmp); return 0; }

    /* Recursively rewrite lhs to protect inner left-associative ops (e.g. a/b/c). */
    char *lhs_rw = (char*)malloc(outsz > 0 ? outsz : 8192);
    if(!lhs_rw){ free(lhs_raw); free(rhs_raw); free(tmp); return 0; }
    int lhs_changed = cg_rewrite_divmod(lhs_rw, outsz > 0 ? outsz : 8192, a, is_float, line, col);
    const char *lhs_final = lhs_changed ? lhs_rw : a;

    if (is_float) {
        if (op == '/') snprintf(out,outsz,"_LTNG_DIV_D(%s,%s,%d,%d)", lhs_final,b,line,col);
        else snprintf(out,outsz,"_LTNG_FMOD_D(%s,%s,%d,%d)", lhs_final,b,line,col);
    } else {
        if (op == '/') snprintf(out,outsz,"_LTNG_DIV_LL(%s,%s,%d,%d)", lhs_final,b,line,col);
        else snprintf(out,outsz,"_LTNG_MOD_LL(%s,%s,%d,%d)", lhs_final,b,line,col);
    }
    free(lhs_rw); free(lhs_raw); free(rhs_raw); free(tmp);
    return 1;
}

/* Try float-mode rewrite first (safer for mixed/unknown numeric exprs in conditions/returns), then int-mode. */
static int cg_rewrite_divmod_auto(char *out, size_t outsz, const char *expr, int prefer_float, int line, int col) {
    if (!expr || !cg_expr_has_divmod(expr)) return 0;
    if (prefer_float) return cg_rewrite_divmod(out, outsz, expr, 1, line, col);
    /* heuristic: if expr contains atof or '.' outside strings, treat as float */
    int in_s=0,in_c=0,esc=0,has_dot=0;
    for (const char *p=expr; *p; p++) {
        char c=*p;
        if(in_s){ if(esc)esc=0; else if(c=='\\')esc=1; else if(c=='"')in_s=0; continue; }
        if(in_c){ if(esc)esc=0; else if(c=='\\')esc=1; else if(c=='\'')in_c=0; continue; }
        if(c=='"'){in_s=1;continue;} if(c=='\''){in_c=1;continue;}
        if(c=='.') has_dot=1;
    }
    if (has_dot || strstr(expr, "atof(") || strstr(expr, "(double)")) return cg_rewrite_divmod(out, outsz, expr, 1, line, col);
    return cg_rewrite_divmod(out, outsz, expr, 0, line, col);
}

static void codegen_assign(FILE *out, ASTNode *node, int indent) {
    int declare_here = 1;
    if (codegen_table && symbol_table_has_local(codegen_table, node->name)) {
        /* Global-scope variables are pre-declared in main preamble */
        declare_here = 0;
    }
        if (node->var_type == TYPE_OBJECT) {
        if (declare_here) {
            pad(out, indent);
            fprintf(out, "char *%s = _ltng_ds_new(\"\");\n", node->name);
        }
        ObjSchema *sc = obj_lookup(node->name);
        if (sc) {
            pad(out, indent);
            fprintf(out, "_ltng_ds_set(&%s, \"", node->name);
            { const char *v=sc->default_val; for(;*v;v++){ if(*v=='\\'||*v=='\"') fputc('\\',out); if(*v=='\n'){fputs("\\n",out);} else if(*v=='\t'){fputs("\\t",out);} else fputc(*v,out);} }
            fprintf(out, "\");\n");
            for (int i = 0; i < sc->count; i++) {
                char fv[MAX_IDENTIFIER_LEN*2+8];
                obj_resolve_field(node->name, sc->fields[i].name, fv, sizeof(fv));
                if (declare_here) {
                    pad(out, indent);
                    fprintf(out, "char *%s = _ltng_ds_new(\"\");\n", fv);
                }
                pad(out, indent);
                fprintf(out, "_ltng_ds_set(&%s, \"", fv);
                { const char *v=sc->fields[i].value; for(;*v;v++){ if(*v=='\\'||*v=='\"') fputc('\\',out); if(*v=='\n'){fputs("\\n",out);} else if(*v=='\t'){fputs("\\t",out);} else fputc(*v,out);} }
                fprintf(out, "\");\n");
            }
        } else {
            pad(out, indent);
            fprintf(out, "_ltng_ds_set(&%s, \"\");\n", node->name);
        }
        if (node->is_expr && node->value[0]) {
            pad(out, indent);
            fprintf(out, "_ltng_ds_set(&%s, %s);\n", node->name, node->value);
        }
        return;
    }
    if (node->var_type == TYPE_ARRAY) {
        char arrname[MAX_IDENTIFIER_LEN];
        snprintf(arrname, sizeof(arrname), "%s_data", node->name);
        /* Array from expression returning an _ltng_ah handle (e.g. function call). */
        if (node->is_expr && node->array_elem_count == 0) {
            pad(out, indent);
            fprintf(out, "int %s_len = 0;\n", node->name);
            pad(out, indent);
            fprintf(out, "int %s_cap = 0;\n", node->name);
            pad(out, indent);
            fprintf(out, "char **%s = NULL;\n", arrname);
            pad(out, indent);
            fprintf(out, "{ char *__ahv = _ltng_ds_new(%s); _ltng_ah *__ah = _ltng_ah_unpack(__ahv); if (__ah) { _ltng_arr_copy(__ah->d, &%s, &%s_len, &%s_cap, __ah->len); _ltng_ah_free(__ah); } free(__ahv); }\n",
                node->value, arrname, node->name, node->name);
            return;
        }
        /* Generate dynamic array declaration:
           int name_len = 0;
           int name_cap = 0;
           char **name_data = _ltng_arr_new(cap);
           _ltng_arr_push(name_data, &name_len, &name_cap, "v1");
           ...
        */
        /* Initial capacity: product of dimensions (rectangular layout) or
           element count, whichever is larger; array grows on demand. */
        int slots = node->array_elem_count;
        if (node->array_dim_count >= 1 && node->array_dims != NULL) {
            int product = 1;
            for (int d = 0; d < node->array_dim_count; d++) {
                if (node->array_dims[d] <= 0) { product = 0; break; }
                product *= node->array_dims[d];
            }
            if (product > slots) slots = product;
        }
        if (slots < 1) slots = 1;
        pad(out, indent);
        fprintf(out, "int %s_len = 0;\n", node->name);
        pad(out, indent);
        fprintf(out, "int %s_cap = 0;\n", node->name);
        pad(out, indent);
        fprintf(out, "char **%s = _ltng_arr_new(%d);\n", arrname, slots);
        /* Push each literal element (pipe-separated raw token text). */
        const char *val = node->value;
        int elem_idx = 0;
        while (*val && elem_idx < node->array_elem_count) {
            if (*val == '|') val++;
            const char *end = val;
            while (*end && *end != '|') end++;
            int vlen = (int)(end - val);
            pad(out, indent);
            if (vlen > 0 && val[0] == '\x01') {
                /* Variable/C-expression element - push its value directly */
                fprintf(out, "_ltng_arr_push(&%s, &%s_len, &%s_cap, %.*s);\n",
                    arrname, node->name, node->name, vlen - 1, val + 1);
            } else {
                fprintf(out, "_ltng_arr_push(&%s, &%s_len, &%s_cap, \"",
                    arrname, node->name, node->name);
                for (int i = 0; i < vlen; i++) {
                    if (val[i] == '"' || val[i] == '\\') fputc('\\', out);
                    fputc(val[i], out);
                }
                fprintf(out, "\");\n");
            }
            val = end;
            elem_idx++;
        }
        return;
    }
    int _is_captured = (g_codegen_current_fn && g_codegen_all_fns &&
                        is_var_captured_by_nested(g_codegen_current_fn, g_codegen_all_fns, node->name));
    if (node->is_expr) {
        if (declare_here) {
            pad(out, indent);
            if (_is_captured) fprintf(out, "char *%s = _ltng_ds_new(\"\");\n", node->name);
            else fprintf(out, "char *%s = _ltng_ds_new(\"\");\n", node->name);
        }
        int is_add_sn = strncmp(node->value, "_ltng_add_sn", 12) == 0;
        if (node->var_type == TYPE_STRING || node->var_type == TYPE_CHAR || node->var_type == TYPE_OBJECT ||
                   is_add_sn) {
            pad(out, indent);
            {
                size_t _nlen = strlen(node->name);
                int _nbuf = (_nlen >= 5 && strcmp(node->name + _nlen - 5, "__obj") == 0) ? 512 : 256;
                fprintf(out, "_ltng_ds_set(&%s, %s);\n", node->name, node->value);
            }
        } else {
            pad(out, indent);
            int _is_call_res = node->is_default || (strncmp(node->value, "({ char __ct", 12) == 0);
            if (node->var_type == TYPE_FLOAT) {
                if (_is_call_res)
                    fprintf(out, "_ltng_ds_setfmt(&%s, \"%%g\", (double)atof(%s));\n", node->name, node->value);
                else {
                    char *_rex = (char*)malloc(4096);
                    if (cg_rewrite_divmod(_rex, 4096, node->value, 1, node->line, node->col))
                        fprintf(out, "_ltng_ds_setfmt(&%s, \"%%g\", (double)(%s));\n", node->name, _rex);
                    else
                        fprintf(out, "_ltng_ds_setfmt(&%s, \"%%g\", (double)(%s));\n", node->name, node->value);
                    free(_rex);
                }
            } else if (strchr(node->value, '.') && !_is_call_res) {
                {
                    char *_rex = (char*)malloc(4096);
                    if (cg_rewrite_divmod(_rex, 4096, node->value, 1, node->line, node->col))
                        fprintf(out, "_ltng_ds_setfmt(&%s, \"%%g\", (double)(%s));\n", node->name, _rex);
                    else
                        fprintf(out, "_ltng_ds_setfmt(&%s, \"%%g\", (double)(%s));\n", node->name, node->value);
                    free(_rex);
                }
            } else if (node->var_type == TYPE_BOOL) {
                if (_is_call_res)
                    fprintf(out, "_ltng_ds_set(&%s, %s);\n", node->name, node->value);
                else
                    fprintf(out, "_ltng_ds_set(&%s, (%s) ? \"true\" : \"false\");\n", node->name, node->value);
            } else {
                if (_is_call_res)
                    fprintf(out, "_ltng_ds_setfmt(&%s, \"%%lld\", (long long)atoll(%s));\n", node->name, node->value);
                else {
                    char *_rex = (char*)malloc(4096);
                    if (cg_rewrite_divmod(_rex, 4096, node->value, 0, node->line, node->col))
                        fprintf(out, "_ltng_ds_setfmt(&%s, \"%%lld\", (long long)(%s));\n", node->name, _rex);
                    else
                        fprintf(out, "_ltng_ds_setfmt(&%s, \"%%lld\", (long long)(%s));\n", node->name, node->value);
                    free(_rex);
                }
            }
        }
    } else if (node->var_type == TYPE_STRING && has_interpolation(node->value)) {
        if (declare_here) {
            pad(out, indent);
            if (_is_captured) fprintf(out, "char *%s = _ltng_ds_new(\"\");\n", node->name);
            else fprintf(out, "char *%s = _ltng_ds_new(\"\");\n", node->name);
        }
        codegen_assign_interpolated(out, node->name, node->value, indent);
    } else if ((node->var_type == TYPE_CHAR || node->var_type == TYPE_STRING) &&
               node->value[0] == '\0') {
        if (declare_here) {
            pad(out, indent);
            if (_is_captured) fprintf(out, "char *%s = _ltng_ds_new(\"\");\n", node->name);
            else fprintf(out, "char *%s = _ltng_ds_new(\"\");\n", node->name);
        }
        pad(out, indent);
        fprintf(out, "%s[0] = '\\0';\n", node->name);
    } else {
        if (declare_here) {
            pad(out, indent);
            if (_is_captured) fprintf(out, "char *%s = _ltng_ds_new(\"\");\n", node->name);
            else fprintf(out, "char *%s = _ltng_ds_new(\"\");\n", node->name);
        }
        pad(out, indent);
        fprintf(out, "_ltng_ds_set(&%s, \"", node->name);
        if (node->var_type == TYPE_BOOL) {
            const char *bv = node->value;
            if (strcmp(bv, "1") == 0) write_c_string(out, "true");
            else if (strcmp(bv, "0") == 0) write_c_string(out, "false");
            else write_c_string(out, bool_to_str(bv));
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
                char *argname = (char*)malloc((size_t)arg_len + 1);
                if (!argname) { p = q + 1; continue; }
                memcpy(argname, arg_start, arg_len);
                argname[arg_len] = '\0';
                char *margname = (char*)malloc((size_t)arg_len + 16);
                if (!margname) { free(argname); p = q + 1; continue; }
                mangle_name(argname, margname, (size_t)arg_len + 16);
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
                free(argname); free(margname);
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
    if (node->is_expr) {
        /* expression result (string or computed) */
        if (node->var_type == TYPE_STRING || node->var_type == TYPE_OBJECT || node->var_type == TYPE_CHAR ||
            strstr(node->value, "_ltng_add_sn") == node->value) {
            pad(out, indent);
            fprintf(out, "printf(\"%%s\\n\", %s);\n", node->value);
        } else if (node->var_type == TYPE_INT || node->var_type == TYPE_FLOAT) {
            pad(out, indent);
            if (node->var_type == TYPE_FLOAT) {
                char *_rex = (char*)malloc(4096);
                if (cg_rewrite_divmod(_rex, 4096, node->value, 1, node->line, node->col))
                    fprintf(out, "printf(\"%%g\\n\", (double)(%s));\n", _rex);
                else
                    fprintf(out, "printf(\"%%g\\n\", (double)(%s));\n", node->value);
                free(_rex);
            } else {
                char *_rex = (char*)malloc(4096);
                if (cg_rewrite_divmod(_rex, 4096, node->value, 0, node->line, node->col))
                    fprintf(out, "printf(\"%%lld\\n\", (long long)(%s));\n", _rex);
                else
                    fprintf(out, "printf(\"%%lld\\n\", (long long)(%s));\n", node->value);
                free(_rex);
            }
        } else if (node->var_type == TYPE_BOOL) {
            pad(out, indent);
            fprintf(out, "printf(\"%%s\\n\", (%s) ? \"true\" : \"false\");\n", node->value);
        } else {
            pad(out, indent);
            fprintf(out, "printf(\"%%s\\n\", %s);\n", node->value);
        }
        return;
    }
    if (node->is_variable) {
        /* Check if variable is an array - print elements joined by ", " */
        if (table) {
            for (int i = 0; i < table->count; i++) {
                if (strcmp(table->vars[i].name, node->name) == 0 && table->vars[i].type == TYPE_ARRAY) {
                    pad(out, indent);
                    fprintf(out, "{ for (int __wi = 0; __wi < %s_len; __wi++) { if (__wi) printf(\", \"); printf(\"%%s\", %s_data[__wi]); } printf(\"\\n\"); }\n",
                        node->name, node->name);
                    return;
                }
            }
        }
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
    fprintf(out, "{ char *_rb=NULL; size_t _rc=0; ssize_t _rn=getline(&_rb,&_rc,stdin); if(_rn>=0){ while(_rn>0 && (_rb[_rn-1]=='\\n' || _rb[_rn-1]=='\\r')) _rb[--_rn]='\\0'; ");
    if (node->var_type == TYPE_INT || node->var_type == TYPE_FLOAT) {
        fprintf(out, "if(!_ltng_is_num_str(_rb)){ fprintf(stderr,\"Error: expected integer input for '%s'\\\\n\",_rb); free(_rb); exit(1);} _ltng_ds_setfmt(&%s,\"%%lld\",(long long)atoll(_rb)); ",
            node->name, node->name);
    } else if (node->var_type == TYPE_BOOL) {
        fprintf(out, "_ltng_ds_set(&%s,(strcmp(_rb,\"true\")==0||strcmp(_rb,\"t\")==0)?\"true\":\"false\"); ",
            node->name);
    } else {
        fprintf(out, "_ltng_ds_set(&%s,_rb); ", node->name);
    }
    fprintf(out, "} if(_rb) free(_rb); }\n");
}

static void codegen_set(FILE *out, ASTNode *node, int indent) {
    if (node->var_type == TYPE_ARRAY) {
        char arrname[MAX_IDENTIFIER_LEN];
        snprintf(arrname, sizeof(arrname), "%s_data", node->name);
        if (node->is_expr && node->array_elem_count == 0) {
            /* Reassign from array-returning expression (ah handle). */
            pad(out, indent);
            fprintf(out, "for (int _i = 0; _i < %s_len; _i++) { if (%s[_i]) free(%s[_i]); %s[_i] = NULL; }\n",
                node->name, arrname, arrname, arrname);
            pad(out, indent);
            fprintf(out, "%s_len = 0;\n", node->name);
            pad(out, indent);
            fprintf(out, "{ char *__ahv = _ltng_ds_new(%s); _ltng_ah *__ah = _ltng_ah_unpack(__ahv); if (__ah) { _ltng_arr_copy(__ah->d, &%s, &%s_len, &%s_cap, __ah->len); _ltng_ah_free(__ah); } free(__ahv); }\n",
                node->value, arrname, node->name, node->name);
            return;
        }
        if (!node->is_expr) {
            /* Reassign array literal: clear existing elements and push new ones */
            pad(out, indent);
            fprintf(out, "for (int _i = 0; _i < %s_len; _i++) { if (%s[_i]) free(%s[_i]); %s[_i] = NULL; }\n",
                node->name, arrname, arrname, arrname);
            pad(out, indent);
            fprintf(out, "%s_len = 0;\n", node->name);
            const char *val = node->value;
            int elem_idx = 0;
            while (*val && elem_idx < node->array_elem_count) {
                if (*val == '|') val++;
                const char *end = val;
                while (*end && *end != '|') end++;
                int vlen = (int)(end - val);
                pad(out, indent);
                if (vlen > 0 && val[0] == '\x01') {
                    fprintf(out, "_ltng_arr_push(&%s, &%s_len, &%s_cap, %.*s);\n",
                        arrname, node->name, node->name, vlen - 1, val + 1);
                } else {
                    fprintf(out, "_ltng_arr_push(&%s, &%s_len, &%s_cap, \"",
                        arrname, node->name, node->name);
                    for (int i = 0; i < vlen; i++) {
                        if (val[i] == '"' || val[i] == '\\') fputc('\\', out);
                        fputc(val[i], out);
                    }
                    fprintf(out, "\");\n");
                }
                val = end;
                elem_idx++;
            }
            return;
        }
    }
    if (node->var_type == TYPE_OBJECT && !node->is_expr) {
        ObjSchema *sc = obj_lookup(node->name);
        if (sc) {
            pad(out, indent);
            fprintf(out, "_ltng_ds_set(&%s, \"", node->name);
            { const char *v=sc->default_val; for(;*v;v++){ if(*v=='\\'||*v=='\"') fputc('\\',out); if(*v=='\n'){fputs("\\n",out);} else if(*v=='\t'){fputs("\\t",out);} else fputc(*v,out);} }
            fprintf(out, "\");\n");
            for (int i = 0; i < sc->count; i++) {
                char fv[MAX_IDENTIFIER_LEN*2+8];
                obj_resolve_field(node->name, sc->fields[i].name, fv, sizeof(fv));
                pad(out, indent);
                fprintf(out, "_ltng_ds_set(&%s, \"", fv);
                { const char *v=sc->fields[i].value; for(;*v;v++){ if(*v=='\\'||*v=='\"') fputc('\\',out); if(*v=='\n'){fputs("\\n",out);} else if(*v=='\t'){fputs("\\t",out);} else fputc(*v,out);} }
                fprintf(out, "\");\n");
            }
        } else {
            pad(out, indent);
            fprintf(out, "_ltng_ds_set(&%s, \"\");\n", node->name);
        }
        return;
    }
    if (node->is_expr) {
        int is_add_sn = strncmp(node->value, "_ltng_add_sn", 12) == 0;
        if (node->var_type == TYPE_STRING || node->var_type == TYPE_CHAR || node->var_type == TYPE_OBJECT ||
                   is_add_sn) {
            pad(out, indent);
            fprintf(out, "_ltng_ds_set(&%s, %s);\n",
                node->name, node->value);
        } else {
            pad(out, indent);
            int _is_call_res = node->is_default || (strncmp(node->value, "({ char __ct", 12) == 0);
            if (node->var_type == TYPE_FLOAT) {
                if (_is_call_res)
                    fprintf(out, "_ltng_ds_setfmt(&%s, \"%%g\", (double)atof(%s));\n", node->name, node->value);
                else {
                    char *_rex = (char*)malloc(4096);
                    if (cg_rewrite_divmod(_rex, 4096, node->value, 1, node->line, node->col))
                        fprintf(out, "_ltng_ds_setfmt(&%s, \"%%g\", (double)(%s));\n", node->name, _rex);
                    else
                        fprintf(out, "_ltng_ds_setfmt(&%s, \"%%g\", (double)(%s));\n", node->name, node->value);
                    free(_rex);
                }
            } else if (strchr(node->value, '.') && !_is_call_res) {
                {
                    char *_rex = (char*)malloc(4096);
                    if (cg_rewrite_divmod(_rex, 4096, node->value, 1, node->line, node->col))
                        fprintf(out, "_ltng_ds_setfmt(&%s, \"%%g\", (double)(%s));\n", node->name, _rex);
                    else
                        fprintf(out, "_ltng_ds_setfmt(&%s, \"%%g\", (double)(%s));\n", node->name, node->value);
                    free(_rex);
                }
            } else if (node->var_type == TYPE_BOOL) {
                if (_is_call_res)
                    fprintf(out, "_ltng_ds_set(&%s, %s);\n", node->name, node->value);
                else
                    fprintf(out, "_ltng_ds_set(&%s, (%s) ? \"true\" : \"false\");\n", node->name, node->value);
            } else {
                if (_is_call_res)
                    fprintf(out, "_ltng_ds_setfmt(&%s, \"%%lld\", (long long)atoll(%s));\n", node->name, node->value);
                else {
                    char *_rex = (char*)malloc(4096);
                    if (cg_rewrite_divmod(_rex, 4096, node->value, 0, node->line, node->col))
                        fprintf(out, "_ltng_ds_setfmt(&%s, \"%%lld\", (long long)(%s));\n", node->name, _rex);
                    else
                        fprintf(out, "_ltng_ds_setfmt(&%s, \"%%lld\", (long long)(%s));\n", node->name, node->value);
                    free(_rex);
                }
            }
        }
    } else if ((node->var_type == TYPE_STRING || node->var_type == TYPE_CHAR) && has_interpolation(node->value)) {
        codegen_assign_interpolated(out, node->name, node->value, indent);
    } else {
        pad(out, indent);
        fprintf(out, "_ltng_ds_set(&%s, \"", node->name);
        if (node->var_type == TYPE_FLOAT) {
            char *buf = (char*)malloc(64);
            if(buf){ snprintf(buf, 64, "%g", atof(node->value)); write_c_string(out, buf); free(buf); }
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

/* Find a function definition by emitted C name among all program functions. */
static ASTNode *cg_find_fn_by_cname(const char *cname) {
    for (ASTNode *fn = g_codegen_program_fns; fn; fn = fn->next) {
        if (strcmp(fn->name, cname) == 0) return fn;
    }
    return NULL;
}

/* Emit the C string expression for a single call argument. */
static void emit_arg_expr(FILE *out, ASTNode *a) {
    if (a->var_type == TYPE_ARRAY && a->value[0] != '\0') {
        if (a->is_variable)
            fprintf(out, "_ltng_ah_pack(%s_data, %s_len, %s_cap)", a->value, a->value, a->value);
        else
            fputs(a->value, out);
    } else if (a->var_type == TYPE_STRING && a->value[0] == '"') {
        fputs(a->value, out);
    } else if (a->is_variable && a->value[0] != '\0') {
        fputs(a->value, out);
    } else if ((a->var_type == TYPE_STRING || a->var_type == TYPE_OBJECT || a->var_type == TYPE_CHAR) && a->value[0] != '\0') {
        fputs(a->value, out);
    } else if (a->var_type == TYPE_BOOL && a->value[0] != '\0') {
        fprintf(out, "((%s)?\"true\":\"false\")", a->value);
    } else if (a->value[0] == '\0') {
        fputs("\"\"", out);
    } else {
        fprintf(out, "_ltng_ad((double)(%s))", a->value);
    }
}

/* Emit ", <arg>" for every argument in the chain. */
static void emit_arg_list(FILE *out, ASTNode *args) {
    for (ASTNode *a = args; a; a = a->next) {
        fputs(", ", out);
        emit_arg_expr(out, a);
    }
}

/* Emit a complete call expression, adding the hidden variadic rest
   count/array arguments for direct calls to variadic functions. */
static void emit_call_expr(FILE *out, ASTNode *call) {
    int cline = call->line > 0 ? call->line : 1;
    int ccol = call->col > 0 ? call->col : 1;
    if (strcmp(call->name, "__ltng_indirect") == 0) {
        int argc = 0;
        for (ASTNode *a = call->args; a; a = a->next) argc++;
        fprintf(out, "__ltng_call_indirect(%s, _ltng_retbuf(), %d, %d, %d",
            call->for_var, cline, ccol, argc);
        emit_arg_list(out, call->args);
        fputs(")", out);
        return;
    }
    ASTNode *fn = cg_find_fn_by_cname(call->name);
    if (fn && fn->is_variadic) {
        int restn = 0;
        for (ASTNode *a = call->rest_args; a; a = a->next) restn++;
        fprintf(out, "({ int __rn = 0; int __rc = 0; char **__rv = _ltng_arr_new(%d);",
            restn > 0 ? restn : 1);
        for (ASTNode *a = call->rest_args; a; a = a->next) {
            fputs(" { char *__av = _ltng_ds_fmt(\"%s\", ", out);
            emit_arg_expr(out, a);
            fputs("); _ltng_arr_push(&__rv, &__rn, &__rc, __av); free(__av); }", out);
        }
        fprintf(out, " char *__rr = %s(_ltng_retbuf(), %d, %d", call->name, cline, ccol);
        emit_arg_list(out, call->args);
        fputs(", __rn, (const char**)__rv); _ltng_arr_free(__rv, __rn); __rr; })", out);
        return;
    }
    fprintf(out, "%s(_ltng_retbuf(), %d, %d", call->name, cline, ccol);
    emit_arg_list(out, call->args);
    fputs(")", out);
}

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
    int base = g_cleanup_count;
    if (g_loop_depth < CG_MAX_LOOPS) g_loop_bases[g_loop_depth++] = base;
    pad(out, indent);
    fprintf(out, "while (%s) {\n", node->value);
    if (node->body) emit_block(out, node->body, table, indent + 1);
    pad(out, indent);
    fprintf(out, "}\n");
    if (g_loop_depth > 0) g_loop_depth--;
}

static void codegen_do_while(FILE *out, ASTNode *node, SymbolTable *table, int indent) {
    int base = g_cleanup_count;
    if (g_loop_depth < CG_MAX_LOOPS) g_loop_bases[g_loop_depth++] = base;
    pad(out, indent);
    fprintf(out, "do {\n");
    if (node->body) emit_block(out, node->body, table, indent + 1);
    pad(out, indent);
    fprintf(out, "} while (%s);\n", node->value);
    if (g_loop_depth > 0) g_loop_depth--;
}

static void codegen_for(FILE *out, ASTNode *node, SymbolTable *table, int indent) {
    (void)table;
    const char *var = node->name;
    const char *start = node->for_start;
    const char *end = node->for_end;
    const char *step = node->for_step;
    char op = node->for_step_op ? node->for_step_op : '+';
    int base = g_cleanup_count;
    if (g_loop_depth < CG_MAX_LOOPS) g_loop_bases[g_loop_depth++] = base;
    /* Use a C for loop so that 'continue' jumps to the step expression */
    pad(out, indent);
    fprintf(out, "{\n");
    if (op == 'E') {
        /* for-each: iterate over array elements (0-based internal order) */
        static int fe_id = 0;
        int id = fe_id++;
        pad(out, indent + 1);
        fprintf(out, "char *%s = _ltng_ds_new(\"\");\n", var);
        pad(out, indent + 1);
        fprintf(out, "for (int __fe_i%d = 0; __fe_i%d < %s_len; __fe_i%d++) {\n", id, id, start, id);
        pad(out, indent + 2);
        fprintf(out, "_ltng_ds_set(&%s, %s_data[__fe_i%d] ? %s_data[__fe_i%d] : \"\");\n",
            var, start, id, start, id);
        if (node->body) emit_block(out, node->body, table, indent + 2);
        pad(out, indent + 1);
        fprintf(out, "}\n");
        pad(out, indent);
        fprintf(out, "}\n");
        if (g_loop_depth > 0) g_loop_depth--;
        return;
    }
    pad(out, indent + 1);
    fprintf(out, "char *%s = _ltng_ds_new(\"0\");\n", var);
    pad(out, indent + 1);
    fprintf(out, "_ltng_ds_setfmt(&%s, \"%%lld\", (long long)(%s));\n", var, start);
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
        fprintf(out, "_ltng_ds_setfmt(&%s, \"%%lld\", (long long)atoll(%s) + (long long)(%s))", var, var, step);
    } else if (op == '-') {
        fprintf(out, "_ltng_ds_setfmt(&%s, \"%%lld\", (long long)atoll(%s) - (long long)(%s))", var, var, step);
    } else if (op == '*') {
        fprintf(out, "_ltng_ds_setfmt(&%s, \"%%lld\", (long long)atoll(%s) * (long long)(%s))", var, var, step);
    } else if (op == '/') {
        fprintf(out, "_ltng_ds_setfmt(&%s, \"%%lld\", _LTNG_DIV_LL((long long)atoll(%s), (long long)(%s), %d, %d))", var, var, step, node->line, node->col);
    } else {
        fprintf(out, "_ltng_ds_setfmt(&%s, \"%%lld\", (long long)atoll(%s) + (long long)(%s))", var, var, step);
    }
    fprintf(out, ") {\n");
    if (node->body) emit_block(out, node->body, table, indent + 2);
    pad(out, indent + 1);
    fprintf(out, "}\n");
    pad(out, indent);
    fprintf(out, "}\n");
    if (g_loop_depth > 0) g_loop_depth--;
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

static void emit_cleanup_unwind(FILE *out, SymbolTable *table, int indent, int target) {
    int saved = g_cleanup_count;
    for (int i = saved - 1; i >= target; i--) {
        CgCleanup *c = &g_cleanups[i];
        pad(out, indent);
        fprintf(out, "_ltng_try_top = %s;\n", c->top_expr);
        if (c->finally_body) {
            g_cleanup_count = i; /* a return/throw in finally must not re-run itself */
            emit_block(out, c->finally_body, table, indent);
            g_cleanup_count = saved;
        }
    }
}

static void codegen_catch_bind(FILE *out, ASTNode *node, int indent) {
    if (!node->name[0]) return;
    const char *cname = node->name;
    ObjSchema *sc = obj_lookup(cname);

    pad(out, indent);
    fprintf(out, "char *%s = _ltng_ds_new(\"\");\n", cname);
    pad(out, indent);
    fprintf(out, "_ltng_exc_ensure(); _ltng_ds_setfmt(&%s, \"%%s: %%s\", _ltng_exc_s(_ltng_exc_type), _ltng_exc_s(_ltng_exc_msg));\n",
        cname);

    if (!sc) return;
    for (int i = 0; i < sc->count; i++) {
        char fv[MAX_IDENTIFIER_LEN*2+8];
        obj_resolve_field(cname, sc->fields[i].name, fv, sizeof(fv));
        const char *fname = sc->fields[i].name;
        pad(out, indent);
        fprintf(out, "char *%s = _ltng_ds_new(\"\");\n", fv);
        pad(out, indent);
        if (strcmp(fname, "type") == 0) {
            fprintf(out, "_ltng_ds_set(&%s, _ltng_exc_type);\n", fv);
        } else if (strcmp(fname, "message") == 0) {
            fprintf(out, "_ltng_ds_set(&%s, _ltng_exc_msg);\n", fv);
        } else if (strcmp(fname, "trace") == 0) {
            fprintf(out, "_ltng_ds_set(&%s, _ltng_exc_trace);\n", fv);
        } else if (strcmp(fname, "line") == 0) {
            fprintf(out, "_ltng_ds_setfmt(&%s, \"%%d\", _ltng_exc_line);\n", fv);
        } else if (strcmp(fname, "column") == 0) {
            fprintf(out, "_ltng_ds_setfmt(&%s, \"%%d\", _ltng_exc_col);\n", fv);
        } else if (strcmp(fname, "function") == 0) {
            fprintf(out, "_ltng_ds_set(&%s, _ltng_exc_func);\n", fv);
        } else if (strcmp(fname, "value") == 0) {
            fprintf(out, "_ltng_ds_set(&%s, _ltng_exc_value);\n", fv);
        } else if (strcmp(fname, "obj") == 0) {
            fprintf(out, "_ltng_ds_set(&%s, %s);\n", fv, cname);
        } else {
            fprintf(out, "_ltng_ds_set(&%s, \"\" );\n", fv);
        }
    }
}

static void codegen_try(FILE *out, ASTNode *node, SymbolTable *table, int indent) {
    int id = g_try_uid++;
    char *tf=malloc(64),*tr=malloc(64),*cf=malloc(64),*cr=malloc(64),*es=malloc(64),*ec=malloc(64),*ef=malloc(64),*er=malloc(64),*ee=malloc(64);
    if(!tf||!tr||!cf||!cr||!es||!ec||!ef||!er||!ee){free(tf);free(tr);free(cf);free(cr);free(es);free(ec);free(ef);free(er);free(ee);return;}
    snprintf(tf, 64, "_ltng_tf%d", id);
    snprintf(tr, 64, "_ltng_tr%d", id);
    snprintf(cf, 64, "_ltng_cf%d", id);
    snprintf(cr, 64, "_ltng_cr%d", id);
    snprintf(es, 64, "_ltng_es%d", id);
    snprintf(ec, 64, "_ltng_ec%d", id);
    snprintf(ef, 64, "_ltng_ef%d", id);
    snprintf(er, 64, "_ltng_er%d", id);
    snprintf(ee, 64, "_ltng_ee%d", id);
    int base = g_cleanup_count;

    pad(out, indent); fprintf(out, "{\n");
    pad(out, indent + 1); fprintf(out, "LtngTryFrame %s;\n", tf);
    pad(out, indent + 1); fprintf(out, "%s.prev = _ltng_try_top;\n", tf);
    pad(out, indent + 1); fprintf(out, "%s.stack_depth = _ltng_stack_depth;\n", tf);
    pad(out, indent + 1); fprintf(out, "_ltng_try_top = &%s;\n", tf);
    pad(out, indent + 1); fprintf(out, "int %s = setjmp(%s.env);\n", tr, tf);
    pad(out, indent + 1); fprintf(out, "if (%s == 0) {\n", tr);

    if (g_cleanup_count < CG_MAX_CLEANUPS) {
        CgCleanup *c = &g_cleanups[g_cleanup_count++];
        c->top_expr = (char*)malloc(256); snprintf(c->top_expr, 256, "%s.prev", tf);
        c->finally_body = node->has_finally ? node->cases : NULL;
    }

    if (node->body) emit_block(out, node->body, table, indent + 2);

    int has_else = node->has_else && node->else_body;
    int has_finally = node->has_finally && node->cases;

    if (has_else) {
        /* Success path: restore the try frame before running else so that an
           exception from else is not caught by this construct's catch. */
        pad(out, indent + 2); fprintf(out, "_ltng_try_top = %s.prev;\n", tf);
        if (has_finally) {
            /* Guard the else body so a throw still runs finally, then
               propagates to an outer handler. */
            pad(out, indent + 2); fprintf(out, "{\n");
            pad(out, indent + 3); fprintf(out, "LtngTryFrame %s;\n", ef);
            pad(out, indent + 3); fprintf(out, "%s.prev = _ltng_try_top;\n", ef);
            pad(out, indent + 3); fprintf(out, "%s.stack_depth = _ltng_stack_depth;\n", ef);
            pad(out, indent + 3); fprintf(out, "_ltng_try_top = &%s;\n", ef);
            pad(out, indent + 3); fprintf(out, "int %s = setjmp(%s.env);\n", er, ef);
            pad(out, indent + 3); fprintf(out, "if (%s == 0) {\n", er);
            emit_block(out, node->else_body, table, indent + 4);
            pad(out, indent + 4); fprintf(out, "_ltng_try_top = %s.prev;\n", ef);
            if (g_cleanup_count > base) g_cleanup_count--;
            {
                int saved = g_cleanup_count;
                g_cleanup_count = base;
                emit_block(out, node->cases, table, indent + 4);
                g_cleanup_count = saved;
            }
            pad(out, indent + 3); fprintf(out, "} else {\n");
            pad(out, indent + 4); fprintf(out, "_ltng_try_top = %s.prev;\n", ef);
            pad(out, indent + 4); fprintf(out, "_ltng_stack_depth = %s.stack_depth;\n", ef);
            if (g_cleanup_count > base) g_cleanup_count--;
            pad(out, indent + 4); fprintf(out, "LtngExcSaved %s; _ltng_exc_saved_init(&%s);\n", ee, ee);
            pad(out, indent + 4); fprintf(out, "_ltng_exc_save_to(&%s);\n", ee);
            {
                int saved = g_cleanup_count;
                g_cleanup_count = base;
                emit_block(out, node->cases, table, indent + 4);
                g_cleanup_count = saved;
            }
            pad(out, indent + 4); fprintf(out, "_ltng_exc_restore_from(&%s); _ltng_exc_saved_free(&%s);\n", ee, ee);
            pad(out, indent + 4); fprintf(out, "_ltng_rethrow();\n");
            pad(out, indent + 3); fprintf(out, "}\n");
            pad(out, indent + 2); fprintf(out, "}\n");
        } else {
            if (g_cleanup_count > base) g_cleanup_count--;
            emit_block(out, node->else_body, table, indent + 2);
        }
    } else {
        if (g_cleanup_count > base) g_cleanup_count--;
        pad(out, indent + 2); fprintf(out, "_ltng_try_top = %s.prev;\n", tf);
        if (has_finally) {
            int saved = g_cleanup_count;
            g_cleanup_count = base;
            emit_block(out, node->cases, table, indent + 2);
            g_cleanup_count = saved;
        }
    }

    pad(out, indent + 1); fprintf(out, "} else {\n");
    pad(out, indent + 2); fprintf(out, "_ltng_try_top = %s.prev;\n", tf);
    pad(out, indent + 2); fprintf(out, "_ltng_stack_depth = %s.stack_depth;\n", tf);

    if (node->has_catch) {
        if (node->has_finally) {
            codegen_catch_bind(out, node, indent + 2);
            pad(out, indent + 2); fprintf(out, "LtngTryFrame %s;\n", cf);
            pad(out, indent + 2); fprintf(out, "%s.prev = _ltng_try_top;\n", cf);
            pad(out, indent + 2); fprintf(out, "%s.stack_depth = _ltng_stack_depth;\n", cf);
            pad(out, indent + 2); fprintf(out, "_ltng_try_top = &%s;\n", cf);
            pad(out, indent + 2); fprintf(out, "int %s = setjmp(%s.env);\n", cr, cf);
            pad(out, indent + 2); fprintf(out, "if (%s == 0) {\n", cr);

            if (g_cleanup_count < CG_MAX_CLEANUPS) {
                CgCleanup *c = &g_cleanups[g_cleanup_count++];
                c->top_expr = (char*)malloc(256); snprintf(c->top_expr, 256, "%s.prev", cf);
                c->finally_body = node->cases;
            }
            if (node->orelse) emit_block(out, node->orelse, table, indent + 3);
            if (g_cleanup_count > base) g_cleanup_count--;
            pad(out, indent + 3); fprintf(out, "_ltng_try_top = %s.prev;\n", cf);

            {
                int saved = g_cleanup_count;
                g_cleanup_count = base;
                if (node->cases) emit_block(out, node->cases, table, indent + 3);
                g_cleanup_count = saved;
            }

            pad(out, indent + 2); fprintf(out, "} else {\n");
            pad(out, indent + 3); fprintf(out, "_ltng_try_top = %s.prev;\n", cf);
            pad(out, indent + 3); fprintf(out, "_ltng_stack_depth = %s.stack_depth;\n", cf);
            pad(out, indent + 3); fprintf(out, "LtngExcSaved %s; _ltng_exc_saved_init(&%s);\n", ec, ec);
            pad(out, indent + 3); fprintf(out, "_ltng_exc_save_to(&%s);\n", ec);
            {
                int saved = g_cleanup_count;
                g_cleanup_count = base;
                if (node->cases) emit_block(out, node->cases, table, indent + 3);
                g_cleanup_count = saved;
            }
            pad(out, indent + 3); fprintf(out, "_ltng_exc_restore_from(&%s); _ltng_exc_saved_free(&%s);\n", ec, ec);
            pad(out, indent + 3); fprintf(out, "_ltng_rethrow();\n");
            pad(out, indent + 2); fprintf(out, "}\n");
        } else {
            codegen_catch_bind(out, node, indent + 2);
            if (node->orelse) emit_block(out, node->orelse, table, indent + 2);
        }
    } else {
        pad(out, indent + 2); fprintf(out, "LtngExcSaved %s; _ltng_exc_saved_init(&%s);\n", es, es);
        pad(out, indent + 2); fprintf(out, "_ltng_exc_save_to(&%s);\n", es);
        {
            int saved = g_cleanup_count;
            g_cleanup_count = base;
            if (node->cases) emit_block(out, node->cases, table, indent + 2);
            g_cleanup_count = saved;
        }
        pad(out, indent + 2); fprintf(out, "_ltng_exc_restore_from(&%s); _ltng_exc_saved_free(&%s);\n", es, es);
        pad(out, indent + 2); fprintf(out, "_ltng_rethrow();\n");
    }

    pad(out, indent + 1); fprintf(out, "}\n");
    pad(out, indent); fprintf(out, "}\n");
    free(tf);free(tr);free(cf);free(cr);free(es);free(ec);free(ef);free(er);free(ee);
}

static int _expr_is_call_buf(const char *v) {
    return v && (strstr(v, "_ltng_retbuf") || strstr(v, "__ltng_call_indirect"));
}

static void emit_return_value(FILE *out, ASTNode *node, const char *target, int indent) {
    if (node->value[0] == '\0' && node->is_expr == 0) {
        pad(out, indent); fprintf(out, "%s[0] = '\\0';\n", target);
        return;
    }
    /* Enforce annotated function return type coercion (int/float) for distinct semantics. */
    if (g_codegen_current_fn && g_codegen_current_fn->has_return_type) {
        VarType want = g_codegen_current_fn->return_type;
        int bare_call = (node->fs_sc == 1);
        if (want == TYPE_INT) {
            pad(out, indent);
            if (bare_call || node->is_variable || node->var_type == TYPE_STRING || node->var_type == TYPE_CHAR ||
                node->var_type == TYPE_OBJECT || node->var_type == TYPE_NULL ||
                strstr(node->value, "_ltng_add_sn") == node->value) {
                fprintf(out, "_ltng_ds_setfmt(&%s, \"%%lld\", (long long)atoll(%s));\n",
                    target, node->value[0] ? node->value : "\"0\"");
            } else if (node->var_type == TYPE_FLOAT) {
                fprintf(out, "_ltng_ds_setfmt(&%s, \"%%lld\", (long long)(double)(%s));\n",
                    target, node->value[0] ? node->value : "0");
            } else if (node->var_type == TYPE_BOOL) {
                fprintf(out, "_ltng_ds_setfmt(&%s, \"%%lld\", (long long)((%s)?1:0));\n",
                    target, node->value[0] ? node->value : "0");
            } else {
                fprintf(out, "_ltng_ds_setfmt(&%s, \"%%lld\", (long long)(%s));\n",
                    target, node->value[0] ? node->value : "0");
            }
            return;
        } else if (want == TYPE_FLOAT) {
            pad(out, indent);
            if (bare_call || node->is_variable || node->var_type == TYPE_STRING || node->var_type == TYPE_CHAR ||
                node->var_type == TYPE_OBJECT || node->var_type == TYPE_NULL ||
                strstr(node->value, "_ltng_add_sn") == node->value) {
                fprintf(out, "_ltng_ds_setfmt(&%s, \"%%g\", (double)atof(%s));\n",
                    target, node->value[0] ? node->value : "\"0\"");
            } else {
                fprintf(out, "_ltng_ds_setfmt(&%s, \"%%g\", (double)(%s));\n",
                    target, node->value[0] ? node->value : "0");
            }
            return;
        }
    }
    int bare_call = (node->fs_sc == 1);
    if (node->var_type == TYPE_STRING || node->var_type == TYPE_CHAR || node->var_type == TYPE_OBJECT ||
        node->var_type == TYPE_ARRAY || node->var_type == TYPE_NULL ||
        strstr(node->value, "_ltng_add_sn") == node->value) {
        pad(out, indent);
        fprintf(out, "_ltng_ds_set(&%s, %s);\n",
            target, node->value[0] ? node->value : "\"\"");
    } else if (node->var_type == TYPE_FLOAT) {
        pad(out, indent);
        if (bare_call) {
            fprintf(out, "_ltng_ds_setfmt(&%s, \"%%g\", (double)atof(%s));\n",
                target, node->value[0] ? node->value : "\"0\"");
        } else {
            fprintf(out, "_ltng_ds_setfmt(&%s, \"%%g\", (double)(%s));\n",
                target, node->value[0] ? node->value : "0");
        }
    } else if (node->var_type == TYPE_BOOL) {
        pad(out, indent);
        fprintf(out, "_ltng_ds_set(&%s, (%s) ? \"true\" : \"false\");\n",
            target, node->value[0] ? node->value : "0");
    } else {
        pad(out, indent);
        /* Function calls return string buffers (char*); parse when typed as numeric. */
        if (bare_call) {
            fprintf(out, "_ltng_ds_setfmt(&%s, \"%%lld\", (long long)atoll(%s));\n",
                target, node->value[0] ? node->value : "\"0\"");
        } else if (strchr(node->value, '.')) {
            fprintf(out, "_ltng_ds_setfmt(&%s, \"%%g\", (double)(%s));\n",
                target, node->value[0] ? node->value : "0");
        } else {
            fprintf(out, "_ltng_ds_setfmt(&%s, \"%%lld\", (long long)(%s));\n",
                target, node->value[0] ? node->value : "0");
        }
    }
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
        case NODE_ARRAY_PUSH: {
            /* Push a formatted value onto the dynamic array. */
            const char *fmt;
            const char *cast;
            if (node->var_type == TYPE_INT) { fmt = "%lld"; cast = "(long long)("; }
            else if (node->var_type == TYPE_FLOAT) { fmt = "%g"; cast = "(double)("; }
            else if (node->var_type == TYPE_BOOL) { fmt = "%s"; cast = "("; }
            else { fmt = "%s"; cast = ""; }
            char arrname[MAX_IDENTIFIER_LEN];
            snprintf(arrname, sizeof(arrname), "%s_data", node->name);
            pad(out, indent);
            if (node->var_type == TYPE_BOOL) {
                fprintf(out, "{ char *__apv = _ltng_ds_fmt(\"%s\", (%s) ? \"true\" : \"false\"); _ltng_arr_push(&%s, &%s_len, &%s_cap, __apv); free(__apv); }\n",
                    fmt, node->value, arrname, node->name, node->name);
            } else {
                fprintf(out, "{ char *__apv = _ltng_ds_fmt(\"%s\", %s%s%s); _ltng_arr_push(&%s, &%s_len, &%s_cap, __apv); free(__apv); }\n",
                    fmt, cast, node->value, cast[0] ? ")" : "",
                    arrname, node->name, node->name);
            }
            break;
        }
        case NODE_ARRAY_POP: {
            char arrname[MAX_IDENTIFIER_LEN];
            snprintf(arrname, sizeof(arrname), "%s_data", node->name);
            pad(out, indent);
            if (node->value[0] == '\0') {
                /* pop() - no args, discard result */
                fprintf(out, "_ltng_arr_pop(%s, &%s_len);\n", arrname, node->name);
            } else {
                /* pop(index) - discard result */
                fprintf(out, "{ int __pi = (int)(%s); _ltng_arr_pop_index(%s, &%s_len, __pi); }\n",
                    node->value, arrname, node->name);
            }
            break;
        }
        case NODE_ARRAY_SET: {
            char arrname[MAX_IDENTIFIER_LEN];
            snprintf(arrname, sizeof(arrname), "%s_data", node->name);
            pad(out, indent);
            if (node->is_expr) {
                if (node->var_type == TYPE_INT) {
                    fprintf(out, "{ int __aidx = %s; char *__asv = _ltng_ds_fmt(\"%%lld\", (long long)(%s)); _ltng_arr_set(%s, &%s_len, &%s_cap, __aidx, __asv); free(__asv); }\n",
                        node->array_index, node->value, arrname, node->name, node->name);
                } else if (node->var_type == TYPE_FLOAT) {
                    fprintf(out, "{ int __aidx = %s; char *__asv = _ltng_ds_fmt(\"%%g\", (double)(%s)); _ltng_arr_set(%s, &%s_len, &%s_cap, __aidx, __asv); free(__asv); }\n",
                        node->array_index, node->value, arrname, node->name, node->name);
                } else if (node->var_type == TYPE_BOOL) {
                    fprintf(out, "{ int __aidx = %s; char *__asv = _ltng_ds_fmt(\"%%s\", (%s) ? \"true\" : \"false\"); _ltng_arr_set(%s, &%s_len, &%s_cap, __aidx, __asv); free(__asv); }\n",
                        node->array_index, node->value, arrname, node->name, node->name);
                } else {
                    fprintf(out, "{ int __aidx = %s; _ltng_arr_set(%s, &%s_len, &%s_cap, __aidx, %s); }\n",
                        node->array_index, arrname, node->name, node->name, node->value);
                }
            } else {
                fprintf(out, "{ int __aidx = %s; _ltng_arr_set(%s, &%s_len, &%s_cap, __aidx, \"%s\"); }\n",
                    node->array_index, arrname, node->name, node->name, node->value);
            }
            break;
        }
        case NODE_ARRAY_INSERT: {
            char arrname[MAX_IDENTIFIER_LEN];
            snprintf(arrname, sizeof(arrname), "%s_data", node->name);
            /* arr.insert(i, x) statement: node->name = arr cname,
               node->value = index expr, node->array_index = element expr. */
            const char *fmt;
            const char *cast;
            if (node->var_type == TYPE_INT) { fmt = "%lld"; cast = "(long long)("; }
            else if (node->var_type == TYPE_FLOAT) { fmt = "%g"; cast = "(double)("; }
            else if (node->var_type == TYPE_BOOL) { fmt = "%s"; cast = ""; }
            else { fmt = "%s"; cast = ""; }
            pad(out, indent);
            if (node->var_type == TYPE_BOOL) {
                fprintf(out, "{ int __ii = (int)(%s); char *__aiv = _ltng_ds_fmt(\"%s\", (%s) ? \"true\" : \"false\"); _ltng_arr_insert(&%s, &%s_len, &%s_cap, __ii, __aiv); free(__aiv); }\n",
                    node->value, fmt, node->array_index, arrname, node->name, node->name);
            } else
            fprintf(out, "{ int __ii = (int)(%s); char *__aiv = _ltng_ds_fmt(\"%s\", %s%s%s); _ltng_arr_insert(&%s, &%s_len, &%s_cap, __ii, __aiv); free(__aiv); }\n",
                node->value, fmt, cast, node->array_index, cast[0] ? ")" : "",
                arrname, node->name, node->name);
            break;
        }
        case NODE_ARRAY_REMOVE: {
            char arrname[MAX_IDENTIFIER_LEN];
            snprintf(arrname, sizeof(arrname), "%s_data", node->name);
            /* arr.remove(i): node->name = arr cname, node->value = index expr. */
            pad(out, indent);
            fprintf(out, "_ltng_arr_remove(%s, &%s_len, (int)(%s));\n",
                arrname, node->name, node->value);
            break;
        }
        case NODE_ARRAY_COPY: {
            /* let b = a.copy(): node->name = dst cname, node->value = src cname. */
            char srcname[MAX_IDENTIFIER_LEN];
            snprintf(srcname, sizeof(srcname), "%s_data", node->value);
            char dstname[MAX_IDENTIFIER_LEN];
            snprintf(dstname, sizeof(dstname), "%s_data", node->name);
            pad(out, indent);
            fprintf(out, "_ltng_arr_copy(%s, &%s_data, &%s_len, &%s_cap, %s_len);\n",
                srcname, node->name, node->name, node->name, node->value);
            break;
        }
        case NODE_ARRAY_SLICE: {
            /* let b = a.slice(s, e): node->name = dst cname, node->value = src cname,
               node->array_index = "start,end" C expressions. */
            char srcname[MAX_IDENTIFIER_LEN];
            snprintf(srcname, sizeof(srcname), "%s_data", node->value);
            pad(out, indent);
            fprintf(out, "_ltng_arr_slice(%s, %s_len, %s, &%s_data, &%s_len, &%s_cap);\n",
                srcname, node->value, node->array_index, node->name, node->name, node->name);
            break;
        }
        case NODE_ARRAY_SORT: {
            /* arr.sort() statement */
            char arrname[MAX_IDENTIFIER_LEN];
            snprintf(arrname, sizeof(arrname), "%s_data", node->name);
            pad(out, indent);
            fprintf(out, "_ltng_arr_sort(%s, %s_len);\n", arrname, node->name);
            break;
        }
        case NODE_ARRAY_REVERSE: {
            /* arr.reverse() statement */
            char arrname[MAX_IDENTIFIER_LEN];
            snprintf(arrname, sizeof(arrname), "%s_data", node->name);
            pad(out, indent);
            fprintf(out, "_ltng_arr_reverse(%s, %s_len);\n", arrname, node->name);
            break;
        }
        case NODE_BLOCK:
            pad(out, indent);
            fprintf(out, "{\n");
            if (node->body) emit_block(out, node->body, table, indent + 1);
            pad(out, indent);
            fprintf(out, "}\n");
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
        case NODE_BREAK: {
            int base = (g_loop_depth > 0) ? g_loop_bases[g_loop_depth - 1] : 0;
            if (g_cleanup_count > base) {
                pad(out, indent); fprintf(out, "{\n");
                emit_cleanup_unwind(out, table, indent + 1, base);
                pad(out, indent + 1); fprintf(out, "break;\n");
                pad(out, indent); fprintf(out, "}\n");
            } else {
                pad(out, indent); fprintf(out, "break;\n");
            }
            break;
        }
        case NODE_CONTINUE: {
            int base = (g_loop_depth > 0) ? g_loop_bases[g_loop_depth - 1] : 0;
            if (g_cleanup_count > base) {
                pad(out, indent); fprintf(out, "{\n");
                emit_cleanup_unwind(out, table, indent + 1, base);
                pad(out, indent + 1); fprintf(out, "continue;\n");
                pad(out, indent); fprintf(out, "}\n");
            } else {
                pad(out, indent); fprintf(out, "continue;\n");
            }
            break;
        }
        case NODE_RETURN: {
            if (g_cleanup_count > 0) {
                pad(out, indent); fprintf(out, "{\n");
                pad(out, indent + 1); fprintf(out, "char *_ltng_rv = _ltng_ds_new(\"\");\n");
                if (g_codegen_current_fn) emit_obj_return_staging(out, g_codegen_current_fn, node, indent + 1);
                emit_return_value(out, node, "_ltng_rv", indent + 1);
                emit_cleanup_unwind(out, table, indent + 1, 0);
                if (g_codegen_in_function) {
                    pad(out, indent + 1); fprintf(out, "_ltng_fn_pop();\n");
                }
                pad(out, indent + 1); fprintf(out, "_ltng_ds_set(&_ltng_retval, _ltng_rv); free(_ltng_rv); return _ltng_retval;\n");
                pad(out, indent); fprintf(out, "}\n");
            } else {
                if (g_codegen_current_fn) emit_obj_return_staging(out, g_codegen_current_fn, node, indent);
                if (g_codegen_in_function) {
                    pad(out, indent); fprintf(out, "_ltng_fn_pop();\n");
                }
                emit_return_value(out, node, "_ltng_retval", indent);
                pad(out, indent); fprintf(out, "return _ltng_retval;\n");
            }
            break;
        }
        case NODE_TRY:
            codegen_try(out, node, table, indent);
            break;
        case NODE_THROW: {
            if (!node->is_expr) {
                pad(out, indent); fprintf(out, "_ltng_rethrow();\n");
            } else {
                const char *type_expr = node->name[0] ? node->name : "\"Error\"";
                const char *msg_expr = node->value[0] ? node->value : "\"\"";
                if (node->var_type == TYPE_INT) {
                    pad(out, indent);
                    fprintf(out, "{ char *_ltng_tm = _ltng_ds_fmt(\"%%lld\", (long long)(%s)); _ltng_throw_value(%s, _ltng_tm, %d, %d); free(_ltng_tm); }\n",
                        msg_expr, type_expr, node->line, node->col);
                } else if (node->var_type == TYPE_FLOAT) {
                    pad(out, indent);
                    fprintf(out, "{ char *_ltng_tm = _ltng_ds_fmt(\"%%g\", (double)(%s)); _ltng_throw_value(%s, _ltng_tm, %d, %d); free(_ltng_tm); }\n",
                        msg_expr, type_expr, node->line, node->col);
                } else {
                    pad(out, indent);
                    fprintf(out, "_ltng_throw_value(%s, %s, %d, %d);\n",
                        type_expr, msg_expr, node->line, node->col);
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
            if (node->is_expr && node->value[0] != '\0') {
                pad(out, indent);
                fprintf(out, "%s;\n", node->value);
                break;
            }
            /* base-assign call statement: receiver = func(receiver, args...)
               Used when the function's first parameter is named "base" and it
               is called via UFCS (receiver.method(args)). The return value is
               written back to the receiver variable. */
            if (node->is_base_assign && node->args && node->args->value[0] != '\0') {
                const char *recv = node->args->value;
                if (node->args->var_type == TYPE_ARRAY) {
                    char arrname[MAX_IDENTIFIER_LEN];
                    snprintf(arrname, sizeof(arrname), "%s_data", recv);
                    pad(out, indent);
                    fprintf(out, "{ char *__ahv = _ltng_ds_new(");
                    emit_call_expr(out, node);
                    fprintf(out, "); _ltng_ah *__ah = _ltng_ah_unpack(__ahv); if (__ah) { _ltng_arr_copy(__ah->d,&%s,&%s_len,&%s_cap,__ah->len); _ltng_ah_free(__ah); } free(__ahv); }\n",
                        arrname, node->args->value, node->args->value);
                } else {
                    pad(out, indent);
                    fprintf(out, "_ltng_ds_set(&%s, ", recv);
                    emit_call_expr(out, node);
                    fprintf(out, ");\n");
                }
                break;
            }
            /* call statement - invoke and discard result */
            pad(out, indent);
            emit_call_expr(out, node);
            fprintf(out, ";\n");
            break;
        }
        default:
            break;
    }
}

static void emit_block(FILE *out, ASTNode *node, SymbolTable *table, int indent) {
    for (ASTNode *cur = node; cur; cur = cur->next) {
        if (codegen_log && codegen_log->enabled && codegen_log->file) {
            char *buf = NULL;
            size_t buflen = 0;
            FILE *mem = open_memstream(&buf, &buflen);
            if (mem) {
                codegen_statement(mem, cur, table, indent);
                fflush(mem);
                fclose(mem);
                if (buf) {
                    fputs(buf, out);
                    log_emitted_node(codegen_log->src_file, cur, buf);
                    free(buf);
                } else {
                    codegen_statement(out, cur, table, indent);
                }
            } else {
                codegen_statement(out, cur, table, indent);
            }
        } else {
            codegen_statement(out, cur, table, indent);
        }
    }
}


static ASTNode *find_function_by_cname(ASTNode *fns, const char *cname) {
    for (ASTNode *fn = fns; fn; fn = fn->next) {
        if (strcmp(fn->name, cname) == 0) return fn;
    }
    return NULL;
}

typedef struct {
    char *fn_name;
    int param_index;
    char *arg_var;
} ParamSchema;
static ParamSchema *g_param_schemas = NULL;
static int g_param_schema_count = 0;
static int g_param_schema_cap = 0;

static void record_param_schema(ASTNode *call, ASTNode *fns) {
    ASTNode *fn = find_function_by_cname(fns, call->name);
    if (!fn) return;
    int pidx = 0;
    for (ASTNode *p = fn->params, *a = call->args; p && a; p = p->next, a = a->next, pidx++) {
        if (a->is_variable && a->value[0] != '\0' && obj_lookup(a->value)) {
            int dup = 0;
            for (int i = 0; i < g_param_schema_count; i++) {
                if (strcmp(g_param_schemas[i].fn_name, fn->name) == 0 &&
                    g_param_schemas[i].param_index == pidx &&
                    strcmp(g_param_schemas[i].arg_var, a->value) == 0) { dup = 1; break; }
            }
            if (dup) continue;
            if (g_param_schema_count >= g_param_schema_cap) {
                int nc = g_param_schema_cap ? g_param_schema_cap * 2 : 64;
                ParamSchema *ns = (ParamSchema*)realloc(g_param_schemas, nc * sizeof(ParamSchema));
                if (!ns) return;
                for (int j = g_param_schema_cap; j < nc; j++) { ns[j].fn_name = NULL; ns[j].arg_var = NULL; ns[j].param_index = 0; }
                g_param_schemas = ns; g_param_schema_cap = nc;
            }
            ParamSchema *e = &g_param_schemas[g_param_schema_count++];
            e->fn_name = ltng_strdup(fn->name ? fn->name : "");
            e->param_index = pidx;
            e->arg_var = ltng_strdup(a->value ? a->value : "");
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
    combined->var = ltng_strdup(fn_name ? fn_name : "");
    combined->default_val = ltng_strdup("");
    combined->fields = NULL; combined->count = 0; combined->cap = 0;
    for (int i = 0; i < g_param_schema_count; i++) {
        if (strcmp(g_param_schemas[i].fn_name, fn_name) != 0)
            continue;
        if (g_param_schemas[i].param_index != param_index)
            continue;
        ObjSchema *sc = obj_lookup(g_param_schemas[i].arg_var);
        if (!sc) continue;
        for (int fi = 0; fi < sc->count; fi++) {
            int dup = 0;
            for (int k = 0; k < combined->count; k++) {
                if (strcmp(combined->fields[k].name, sc->fields[fi].name) == 0) { dup = 1; break; }
            }
            if (dup) continue;
            if (combined->count >= combined->cap) {
                int nc = combined->cap ? combined->cap * 2 : 8;
                ObjField *nf = (ObjField*)realloc(combined->fields, nc * sizeof(ObjField));
                if (!nf) break;
                for (int j = combined->cap; j < nc; j++) { nf[j].name = ltng_strdup(""); nf[j].value = ltng_strdup(""); nf[j].type = TYPE_NULL; }
                combined->fields = nf; combined->cap = nc;
            }
            ObjField *dst = &combined->fields[combined->count];
            free(dst->name); dst->name = ltng_strdup(sc->fields[fi].name ? sc->fields[fi].name : "");
            free(dst->value); dst->value = ltng_strdup(sc->fields[fi].value ? sc->fields[fi].value : "");
            dst->type = sc->fields[fi].type;
            combined->count++;
        }
    }
    return combined->count;
}

void codegen(Program *program, SymbolTable *table, FILE *out, ErrorState *errors) {
    codegen_errors = errors;
    codegen_table = table;
    g_codegen_program_fns = program->functions;

    /* Pre-pass: record object schemas passed to function parameters at all call sites */
    if (program->functions) {
        traverse_record_param_schemas(program->head, program->functions);
        for (ASTNode *fn = program->functions; fn; fn = fn->next) {
            traverse_record_param_schemas(fn->body, program->functions);
        }
    }

    fprintf(out, "#define _GNU_SOURCE\n");
    fprintf(out, "#include <stdio.h>\n");
    fprintf(out, "#include <string.h>\n");
    fprintf(out, "#include <stdlib.h>\n");
    fprintf(out, "#include <stdarg.h>\n");
    fprintf(out, "#include <stdint.h>\n");
    fprintf(out, "#include <ctype.h>\n");
    fprintf(out, "#include <setjmp.h>\n");
    fprintf(out, "#include <math.h>\n");
    fprintf(out, "static const char *_ltng_src_file = \"");
    write_c_string_raw(out, (errors && errors->filename) ? errors->filename : "<input>");
    fprintf(out, "\";\n");
    /* Dynamic string/value runtime helpers (no fixed 256-byte truncation for values). */
    fprintf(out, "static char *_ltng_ds_new(const char *s){ if(!s) s=\"\"; size_t n=strlen(s)+1; char *p=(char*)malloc(n); if(!p) return NULL; memcpy(p,s,n); return p; }\n");
    fprintf(out, "static void _ltng_ds_set(char **dst, const char *s){ if(!dst) return; if(!s) s=\"\"; size_t n=strlen(s)+1; char *p=(char*)realloc(*dst,n); if(!p){ p=(char*)malloc(n); if(!p) return; } memcpy(p,s,n); *dst=p; }\n");
    fprintf(out, "static void _ltng_ds_append(char **dst, const char *s){ if(!dst) return; if(!s) s=\"\"; size_t a = (*dst)?strlen(*dst):0; size_t b = strlen(s); char *p=(char*)realloc(*dst,a+b+1); if(!p){ p=(char*)malloc(a+b+1); if(!p) return; if(*dst){ memcpy(p,*dst,a); } } memcpy(p+a,s,b+1); *dst=p; }\n");
    fprintf(out, "static char *_ltng_ds_take(char *s){ return s ? s : _ltng_ds_new(\"\"); }\n");
    fprintf(out, "static char *_ltng_ds_slot_set(char **slot, char *s){ if(!slot) { if(s) free(s); return \"\"; } if(*slot) free(*slot); if(!s) s=_ltng_ds_new(\"\"); *slot=s; return *slot; }\n");
    fprintf(out, "static char *_ltng_ds_slot_set_cstr(char **slot, const char *s){ if(!slot) return \"\"; char *n=_ltng_ds_new(s); return _ltng_ds_slot_set(slot,n); }\n");
    fprintf(out, "static char *_ltng_ds_vfmt(const char *fmt, va_list ap){ va_list ap2; va_copy(ap2,ap); int n=vsnprintf(NULL,0,fmt,ap); va_end(ap); if(n<0){ va_end(ap2); return _ltng_ds_new(\"\"); } char *p=(char*)malloc((size_t)n+1); if(!p){ va_end(ap2); return _ltng_ds_new(\"\"); } vsnprintf(p,(size_t)n+1,fmt,ap2); va_end(ap2); return p; }\n");
    fprintf(out, "static char *_ltng_ds_fmt(const char *fmt, ...){ va_list ap; va_start(ap,fmt); char *p=_ltng_ds_vfmt(fmt,ap); va_end(ap); return p; }\n");
    fprintf(out, "static void _ltng_ds_setfmt(char **dst, const char *fmt, ...){ if(!dst) return; va_list ap; va_start(ap,fmt); char *p=_ltng_ds_vfmt(fmt,ap); va_end(ap); _ltng_ds_slot_set(dst,p); }\n");
    fprintf(out, "static char *_ltng_concat(char *buf, size_t sz, int n, ...) { (void)buf; (void)sz; static char *slot[32]={0}; static int si=0; si=(si+1)%%32; va_list ap; va_start(ap,n); size_t total=1; va_list ap2; va_copy(ap2,ap); for(int i=0;i<n;i++){ const char *s=va_arg(ap2,const char*); if(s) total+=strlen(s); } va_end(ap2); char *p=(char*)realloc(slot[si],total); if(!p) p=(char*)malloc(total); if(!p){ va_end(ap); return \"\"; } slot[si]=p; p[0]=0; for(int i=0;i<n;i++){ const char *s=va_arg(ap,const char*); if(s) strcat(p,s); } va_end(ap); return p; }\n");
    fprintf(out, "static char *_ltng_concat_dyn(int n, ...) { static char *slot[32]={0}; static int si=0; si=(si+1)%%32; va_list ap; va_start(ap,n); size_t total=1; va_list ap2; va_copy(ap2,ap); for(int i=0;i<n;i++){ const char *s=va_arg(ap2,const char*); if(s) total+=strlen(s); } va_end(ap2); char *p=(char*)realloc(slot[si],total); if(!p) p=(char*)malloc(total); if(!p){ va_end(ap); return \"\"; } slot[si]=p; p[0]=0; for(int i=0;i<n;i++){ const char *s=va_arg(ap,const char*); if(s) strcat(p,s); } va_end(ap); return p; }\n");
    fprintf(out, "static char *_ltng_repeat(char *buf, size_t sz, const char *s, long long n) { (void)buf; (void)sz; static char *slot[32]={0}; static int si=0; si=(si+1)%%32; if(!s) s=\"\"; if(n<0) n=0; size_t sl=strlen(s); size_t total=(size_t)n*sl+1; char *p=(char*)realloc(slot[si],total); if(!p) p=(char*)malloc(total); if(!p) return \"\"; slot[si]=p; for(long long i=0;i<n;i++){ memcpy(p + (size_t)i*sl, s, sl); } p[(size_t)n*sl]=0; return p; }\n");
    fprintf(out, "static char *_ltng_repeat_dyn(const char *s, long long n) { static char *slot[32]={0}; static int si=0; si=(si+1)%%32; if(!s) s=\"\"; if(n<0) n=0; size_t sl=strlen(s); size_t total=(size_t)n*sl+1; char *p=(char*)realloc(slot[si],total); if(!p) p=(char*)malloc(total); if(!p) return \"\"; slot[si]=p; for(long long i=0;i<n;i++){ memcpy(p + (size_t)i*sl, s, sl); } p[(size_t)n*sl]=0; return p; }\n");
    fputs(
        "typedef struct LtngTryFrame {\n"
        "    jmp_buf env;\n"
        "    struct LtngTryFrame *prev;\n"
        "    int stack_depth;\n"
        "} LtngTryFrame;\n"
        "static LtngTryFrame *_ltng_try_top = NULL;\n"
        "static const char **_ltng_fn_name = NULL;\n"
        "static int *_ltng_fn_line = NULL;\n"
        "static int *_ltng_fn_col = NULL;\n"
        "static int _ltng_stack_depth = 0;\n"
        "static int _ltng_stack_cap = 0;\n"
        "static char *_ltng_exc_type = NULL;\n"
        "static char *_ltng_exc_msg = NULL;\n"
        "static char *_ltng_exc_value = NULL;\n"
        "static char *_ltng_exc_trace = NULL;\n"
        "static int _ltng_exc_line = 0;\n"
        "static int _ltng_exc_col = 0;\n"
        "static char *_ltng_exc_func = NULL;\n"
        "static const char *_ltng_exc_s(const char *s){ return s ? s : \"\"; }\n"
        "static void _ltng_exc_ensure(void){ if(!_ltng_exc_type) _ltng_exc_type=_ltng_ds_new(\"\"); if(!_ltng_exc_msg) _ltng_exc_msg=_ltng_ds_new(\"\"); if(!_ltng_exc_value) _ltng_exc_value=_ltng_ds_new(\"\"); if(!_ltng_exc_trace) _ltng_exc_trace=_ltng_ds_new(\"\"); if(!_ltng_exc_func) _ltng_exc_func=_ltng_ds_new(\"\"); }\n"
        "typedef struct {\n"
        "    char *type;\n"
        "    char *msg;\n"
        "    char *value;\n"
        "    char *trace;\n"
        "    int line;\n"
        "    int col;\n"
        "    char *func;\n"
        "} LtngExcSaved;\n"
        "static void _ltng_exc_saved_init(LtngExcSaved *s){ if(!s) return; s->type=_ltng_ds_new(\"\"); s->msg=_ltng_ds_new(\"\"); s->value=_ltng_ds_new(\"\"); s->trace=_ltng_ds_new(\"\"); s->func=_ltng_ds_new(\"\"); s->line=0; s->col=0; }\n"
        "static void _ltng_exc_saved_free(LtngExcSaved *s){ if(!s) return; if(s->type) free(s->type); if(s->msg) free(s->msg); if(s->value) free(s->value); if(s->trace) free(s->trace); if(s->func) free(s->func); s->type=s->msg=s->value=s->trace=s->func=NULL; }\n"
        "static void _ltng_fn_push(const char *name, int line, int col) {\n"
        "    if (_ltng_stack_depth >= _ltng_stack_cap) {\n"
        "        int nc = _ltng_stack_cap ? _ltng_stack_cap * 2 : 16;\n"
        "        const char **nn = (const char**)realloc(_ltng_fn_name, sizeof(const char*) * nc);\n"
        "        int *nl = (int*)realloc(_ltng_fn_line, sizeof(int) * nc);\n"
        "        int *nc2 = (int*)realloc(_ltng_fn_col, sizeof(int) * nc);\n"
        "        if (!nn || !nl || !nc2) return;\n"
        "        _ltng_fn_name = nn; _ltng_fn_line = nl; _ltng_fn_col = nc2; _ltng_stack_cap = nc;\n"
        "    }\n"
        "    _ltng_fn_name[_ltng_stack_depth] = name ? name : \"?\";\n"
        "    _ltng_fn_line[_ltng_stack_depth] = line;\n"
        "    _ltng_fn_col[_ltng_stack_depth] = col;\n"
        "    _ltng_stack_depth++;\n"
        "}\n"
        "static void _ltng_fn_pop(void) { if (_ltng_stack_depth > 0) _ltng_stack_depth--; }\n"
        "static void _ltng_capture_trace(int line, int col) {\n"
        "    _ltng_exc_ensure();\n"
        "    _ltng_exc_line = line;\n"
        "    _ltng_exc_col = col;\n"
        "    _ltng_ds_set(&_ltng_exc_func, \"\");\n"
        "    _ltng_ds_set(&_ltng_exc_trace, \"\");\n"
        "    if (_ltng_stack_depth > 0) {\n"
        "        _ltng_ds_set(&_ltng_exc_func, _ltng_fn_name[_ltng_stack_depth - 1]);\n"
        "        char *ln = _ltng_ds_fmt(\"  at %s (%s:%d:%d)\\n\", _ltng_fn_name[_ltng_stack_depth - 1], _ltng_src_file, line, col);\n"
        "        _ltng_ds_append(&_ltng_exc_trace, ln); free(ln);\n"
        "        for (int i = _ltng_stack_depth - 2; i >= 0; i--) {\n"
        "            ln = _ltng_ds_fmt(\"  at %s (%s:%d:%d)\\n\", _ltng_fn_name[i], _ltng_src_file, _ltng_fn_line[i + 1], _ltng_fn_col[i + 1]);\n"
        "            _ltng_ds_append(&_ltng_exc_trace, ln); free(ln);\n"
        "        }\n"
        "    } else {\n"
        "        char *ln = _ltng_ds_fmt(\"  at <top-level> (%s:%d:%d)\\n\", _ltng_src_file, line, col);\n"
        "        _ltng_ds_append(&_ltng_exc_trace, ln); free(ln);\n"
        "    }\n"
        "}\n"
        "static void _ltng_exc_save_to(LtngExcSaved *s) {\n"
        "    _ltng_exc_ensure();\n"
        "    _ltng_ds_set(&s->type, _ltng_exc_type);\n"
        "    _ltng_ds_set(&s->msg, _ltng_exc_msg);\n"
        "    _ltng_ds_set(&s->value, _ltng_exc_value);\n"
        "    _ltng_ds_set(&s->trace, _ltng_exc_trace);\n"
        "    s->line = _ltng_exc_line; s->col = _ltng_exc_col;\n"
        "    _ltng_ds_set(&s->func, _ltng_exc_func);\n"
        "}\n"
        "static void _ltng_exc_restore_from(const LtngExcSaved *s) {\n"
        "    _ltng_exc_ensure();\n"
        "    _ltng_ds_set(&_ltng_exc_type, _ltng_exc_s(s->type));\n"
        "    _ltng_ds_set(&_ltng_exc_msg, _ltng_exc_s(s->msg));\n"
        "    _ltng_ds_set(&_ltng_exc_value, _ltng_exc_s(s->value));\n"
        "    _ltng_ds_set(&_ltng_exc_trace, _ltng_exc_s(s->trace));\n"
        "    _ltng_exc_line = s->line; _ltng_exc_col = s->col;\n"
        "    _ltng_ds_set(&_ltng_exc_func, _ltng_exc_s(s->func));\n"
        "}\n"
        "static void _ltng_uncaught(void) {\n"
        "    _ltng_exc_ensure();\n"
        "    if (_ltng_exc_msg[0]) fprintf(stderr, \"%s: %s\\n\", _ltng_exc_type[0] ? _ltng_exc_type : \"Error\", _ltng_exc_msg);\n"
        "    else fprintf(stderr, \"%s\\n\", _ltng_exc_type[0] ? _ltng_exc_type : \"Error\");\n"
        "    if (_ltng_exc_trace[0]) fputs(_ltng_exc_trace, stderr);\n"
        "    fflush(stderr);\n"
        "    exit(1);\n"
        "}\n"
        "static void _ltng_throw_value(const char *type, const char *msg, int line, int col) {\n"
        "    _ltng_exc_ensure();\n"
        "    _ltng_ds_set(&_ltng_exc_type, (type && type[0]) ? type : \"Error\");\n"
        "    _ltng_ds_set(&_ltng_exc_msg, msg ? msg : \"\");\n"
        "    _ltng_ds_set(&_ltng_exc_value, msg ? msg : \"\");\n"
        "    _ltng_capture_trace(line, col);\n"
        "    if (!_ltng_try_top) _ltng_uncaught();\n"
        "    longjmp(_ltng_try_top->env, 1);\n"
        "}\n"
        "static void _ltng_rethrow(void) {\n"
        "    if (!_ltng_try_top) _ltng_uncaught();\n"
        "    longjmp(_ltng_try_top->env, 1);\n"
        "}\n",
        out);
    fprintf(out, "\n");
    /* Dynamic array runtime helpers. Arrays are represented as char **data plus
       int len and int cap. Each element is a strdup'd char buffer. Indexing is
       1-based for positive indices, counts from the end for negative indices,
       and index 0 is invalid (returns ""). All '%' below are escaped for fprintf.
       Functions that may realloc take char ***d so the caller's pointer is updated. */
    fprintf(out, "static char *_ltng_arr_strdup(const char *s){ if(!s) s=\"\"; size_t n=strlen(s)+1; char *p=(char*)malloc(n); if(p) memcpy(p,s,n); return p; }\n");
    fprintf(out, "static char **_ltng_arr_new(int cap){ if(cap<1)cap=1; char **d=(char**)malloc(sizeof(char*)*cap); for(int i=0;i<cap;i++) d[i]=NULL; return d; }\n");
    fprintf(out, "static void _ltng_arr_ensure(char ***d,int *len,int *cap,int need){ if(*cap>=need) return; int nc=*cap?*cap*2:1; while(nc<need) nc*=2; char **nd=(char**)realloc(*d,sizeof(char*)*nc); if(!nd) return; *d=nd; for(int i=*cap;i<nc;i++) (*d)[i]=NULL; *cap=nc; }\n");
    fprintf(out, "static void _ltng_arr_set_elem(char **d,int idx,const char *v){ if(d[idx]) free(d[idx]); d[idx]=v?_ltng_arr_strdup(v):_ltng_arr_strdup(\"\"); }\n");
    fprintf(out, "static const char *_ltng_arr_get(char **d,int len,int idx){ if(idx>=0&&idx<len) return d[idx]?d[idx]:\"\"; if(idx<0&&len+idx>=0) return d[len+idx]?d[len+idx]:\"\"; return \"\"; }\n");
    fprintf(out, "static void _ltng_arr_set(char **d,int *len,int *cap,int idx,const char *v){ (void)cap; if(idx<0||idx>=*len) return; _ltng_arr_set_elem(d,idx,v); }\n");
    fprintf(out, "static void _ltng_arr_push(char ***d,int *len,int *cap,const char *v){ _ltng_arr_ensure(d,len,cap,*len+1); _ltng_arr_set_elem(*d,*len,v); (*len)++; }\n");
    fprintf(out, "static char *_ltng_arr_pop(char **d,int *len){ static char *b[32]={0}; static int i; i=(i+1)%%32; if(*len<=0){ _ltng_ds_set(&b[i],\"\"); return b[i]; } (*len)--; const char *v=d[*len]?d[*len]:\"\"; _ltng_ds_set(&b[i],v); free(d[*len]); d[*len]=NULL; return b[i]; }\n");
    fprintf(out, "static char *_ltng_arr_pop_index(char **d,int *len,int idx){ static char *b[32]={0}; static int i; i=(i+1)%%32; /* idx is 1-based positive, negative from end, 0 invalid */ if(idx>0) idx=idx-1; else if(idx<0) idx=*len+idx; if(idx<0||idx>=*len){ _ltng_ds_set(&b[i],\"\"); return b[i]; } const char *v=d[idx]?d[idx]:\"\"; _ltng_ds_set(&b[i],v); free(d[idx]); for(int j=idx;j<*len-1;j++) d[j]=d[j+1]; d[*len-1]=NULL; (*len)--; return b[i]; }\n");
    fprintf(out, "static void _ltng_arr_insert(char ***d,int *len,int *cap,int idx,const char *v){ /* idx 1-based */ if(idx>0) idx=idx-1; else if(idx<0) idx=*len+idx; if(idx<0) idx=0; if(idx>*len) idx=*len; _ltng_arr_ensure(d,len,cap,*len+1); for(int j=*len;j>idx;j--) (*d)[j]=(*d)[j-1]; (*d)[idx]=_ltng_arr_strdup(v?v:\"\"); (*len)++; }\n");
    fprintf(out, "static void _ltng_arr_remove(char **d,int *len,int idx){ /* idx is 1-based positive, negative from end, 0 invalid */ if(idx>0) idx=idx-1; else if(idx<0) idx=*len+idx; if(idx<0||idx>=*len) return; free(d[idx]); for(int j=idx;j<*len-1;j++) d[j]=d[j+1]; d[*len-1]=NULL; (*len)--; }\n");
    fprintf(out, "static int _ltng_arr_contains(char **d,int len,const char *v){ for(int i=0;i<len;i++){ if(d[i]&&strcmp(d[i],v)==0) return 1; } return 0; }\n");
    fprintf(out, "static int _ltng_arr_index_of(char **d,int len,const char *v){ for(int i=0;i<len;i++){ if(d[i]&&strcmp(d[i],v)==0) return i+1; } return 0; }\n");
    fprintf(out, "static void _ltng_arr_reverse(char **d,int len){ for(int i=0,j=len-1;i<j;i++,j--){ char *t=d[i]; d[i]=d[j]; d[j]=t; } }\n");
    fprintf(out, "static int _ltng_arr_numcmp(const char *a,const char *b){ char *ea=NULL,*eb=NULL; double da=strtod(a?a:\"\",&ea); double db=strtod(b?b:\"\",&eb); int na=(ea&&*ea==0&&a&&*a); int nb=(eb&&*eb==0&&b&&*b); if(na&&nb){ if(da<db) return -1; if(da>db) return 1; return 0; } return strcmp(a?a:\"\", b?b:\"\"); }\n");
    fprintf(out, "static int _ltng_arr_cmp(const void *a,const void *b){ return _ltng_arr_numcmp(*(const char * const *)a, *(const char * const *)b); }\n");
    fprintf(out, "static void _ltng_arr_sort(char **d,int len){ qsort(d,len,sizeof(char *),_ltng_arr_cmp); }\n");
    fprintf(out, "static void _ltng_arr_copy(char **sd,char ***dd,int *dl,int *dc,int sl){ _ltng_arr_ensure(dd,dl,dc,sl); for(int i=sl;i<*dl;i++){ if((*dd)[i]){ free((*dd)[i]); (*dd)[i]=NULL; } } for(int i=0;i<sl;i++){ if((*dd)[i]) free((*dd)[i]); (*dd)[i]=sd[i]?_ltng_arr_strdup(sd[i]):_ltng_arr_strdup(\"\"); } *dl=sl; }\n");
    fprintf(out, "static void _ltng_arr_slice(char **sd,int sl,int start,int end,char ***dd,int *dl,int *dc){ if(start>0) start=start-1; else if(start<0) start=sl+start; if(end>0) end=end-1; else if(end<0) end=sl+end; if(start<0) start=0; if(end<0) end=0; if(end>sl) end=sl; if(start>sl) start=sl; if(start>end) start=end; int n=end-start; _ltng_arr_ensure(dd,dl,dc,n); for(int i=0;i<*dl;i++){ if((*dd)[i]){ free((*dd)[i]); (*dd)[i]=NULL; } } *dl=0; for(int i=0;i<n;i++){ _ltng_arr_push(dd,dl,dc,sd[start+i]?sd[start+i]:\"\"); } }\n");
    fprintf(out, "static void _ltng_arr_free(char **d,int len){ if(!d) return; for(int i=0;i<len;i++){ if(d[i]) free(d[i]); } free(d); }\n");
    fprintf(out, "static int _ltng_idx2(int i0, int i1, int d1) { return i0 * d1 + i1; }\n");
    fprintf(out, "static int _ltng_idx3(int i0, int i1, int i2, int d1, int d2) { return (i0 * d1 + i1) * d2 + i2; }\n");
    fprintf(out, "static char *_ltng_retbuf(void){ static char *b[32]={0}; static int i; i=(i+1)%%32; if(!b[i]) b[i]=_ltng_ds_new(\"\"); else b[i][0]=0; return b[i]; }\n");
    fprintf(out, "static char *_ltng_retbuf_fmt(const char *fmt, ...){ static char *b[32]={0}; static int i; i=(i+1)%%32; va_list ap; va_start(ap,fmt); char *p=_ltng_ds_vfmt(fmt,ap); va_end(ap); if(b[i]) free(b[i]); b[i]=p; return b[i]; }\n");
    fprintf(out, "static char *_ltng_ad(double v){ static char *b[32]={0}; static int i; i=(i+1)%%32; _ltng_ds_setfmt(&b[i],\"%%g\",v); return b[i]; }\n");
    fprintf(out, "static char *_ltng_as(const char *s){ static char *b[32]={0}; static int i; i=(i+1)%%32; _ltng_ds_set(&b[i],s); return b[i]; }\n");
    fprintf(out, "static char *_ltng_from_int(char *buf, long long v){ (void)buf; static char *b[32]={0}; static int i; i=(i+1)%32; _ltng_ds_setfmt(&b[i],\"%%lld\",v); return b[i]; }\n");
    fprintf(out, "static char *_ltng_from_double(char *buf, double v){ (void)buf; static char *b[32]={0}; static int i; i=(i+1)%32; _ltng_ds_setfmt(&b[i],\"%%g\",v); return b[i]; }\n");
    fprintf(out, "static int _ltng_is_num_str(const char *s){ if(!s||!*s) return 0; const char *p=s; if(*p=='-'||*p=='+') p++; int h=0; for(;*p;p++){ if(isdigit((unsigned char)*p)) h=1; else if(*p=='.'){} else return 0; } return h; }\n");
    fprintf(out, "static int _ltng_str_truthy(const char *s){ if(!s||!s[0]) return 0; if(strcmp(s,\"0\")==0||strcmp(s,\"false\")==0||strcmp(s,\"f\")==0) return 0; return 1; }\n");
    fprintf(out, "static char *_ltng_add_sn(const char *a, const char *b){ static char *buf[32]={0}; static int i=0; i=(i+1)%%32; if(_ltng_is_num_str(a)&&_ltng_is_num_str(b)){ double na=atof(a?a:\"0\"),nb=atof(b?b:\"0\"); _ltng_ds_setfmt(&buf[i],\"%%g\",na+nb); } else { _ltng_ds_setfmt(&buf[i],\"%%s%%s\",a?a:\"\",b?b:\"\"); } return buf[i]; }\n");
    fprintf(out, "static long long _ltng_div0_ll(int line, int col){ _ltng_throw_value(\"DivisionError\", \"Division by zero\", line, col); return 0; }\n");
    fprintf(out, "static long long _ltng_mod0_ll(int line, int col){ _ltng_throw_value(\"DivisionError\", \"Modulo by zero\", line, col); return 0; }\n");
    fprintf(out, "static double _ltng_div0_d(int line, int col){ _ltng_throw_value(\"DivisionError\", \"Division by zero\", line, col); return 0.0; }\n");
    fprintf(out, "static double _ltng_mod0_d(int line, int col){ _ltng_throw_value(\"DivisionError\", \"Modulo by zero\", line, col); return 0.0; }\n");
    fprintf(out, "#define _LTNG_DIV_LL(A,B,L,C) ({ long long _b=(long long)(B); _b==0?_ltng_div0_ll((L),(C)):((long long)(A)/_b); })\n");
    fprintf(out, "#define _LTNG_MOD_LL(A,B,L,C) ({ long long _b=(long long)(B); _b==0?_ltng_mod0_ll((L),(C)):((long long)(A)%%_b); })\n");
    fprintf(out, "#define _LTNG_DIV_D(A,B,L,C) ({ double _b=(double)(B); _b==0.0?_ltng_div0_d((L),(C)):((double)(A)/_b); })\n");
    fprintf(out, "#define _LTNG_FMOD_D(A,B,L,C) ({ double _b=(double)(B); _b==0.0?_ltng_mod0_d((L),(C)):fmod((double)(A),_b); })\n");
    fprintf(out, "#define _LTNG_NZDIV_LL(V,L,C) ({ long long _v=(long long)(V); _v==0?_ltng_div0_ll((L),(C)):_v; })\n");
    fprintf(out, "#define _LTNG_NZMOD_LL(V,L,C) ({ long long _v=(long long)(V); _v==0?_ltng_mod0_ll((L),(C)):_v; })\n");
    fprintf(out, "#define _LTNG_NZDIV_D(V,L,C) ({ double _v=(double)(V); _v==0.0?_ltng_div0_d((L),(C)):_v; })\n");
    fprintf(out, "#define _LTNG_NZMOD_D(V,L,C) ({ double _v=(double)(V); _v==0.0?_ltng_mod0_d((L),(C)):_v; })\n");
    fprintf(out, "static char *_ltng_ptr_to_str(void *p){ static char *b[32]={0}; static int i; i=(i+1)%%32; char *_r=_ltng_ds_fmt(\"%%llu\",(unsigned long long)(uintptr_t)p); _ltng_ds_set(&b[i],_r); free(_r); return b[i]; }\n");
    fprintf(out, "static void *_ltng_str_to_ptr(const char *s){ if(!s||!*s) return NULL; return (void*)(uintptr_t)strtoull(s, NULL, 10); }\n");
    fprintf(out, "typedef struct { char **d; int len; int cap; } _ltng_ah;\n");
    fprintf(out, "static char *_ltng_ah_pack(char **d, int len, int cap){ _ltng_ah *h=(_ltng_ah*)malloc(sizeof(_ltng_ah)); h->d=NULL; h->len=0; h->cap=0; _ltng_arr_copy(d,&h->d,&h->len,&h->cap,len); return _ltng_ptr_to_str(h); }\n");
    fprintf(out, "static _ltng_ah *_ltng_ah_unpack(const char *s){ return (_ltng_ah*)_ltng_str_to_ptr(s); }\n");
    fprintf(out, "static void _ltng_ah_free(_ltng_ah *h){ if(!h) return; _ltng_arr_free(h->d,h->len); free(h); }\n");
    fprintf(out, "\n");

    /* Staging storage for object-returning functions */
    if (program->functions) {
        for (ASTNode *fn = program->functions; fn; fn = fn->next) {
            ObjSchema *sc = fn_return_obj_schema(fn);
            if (!sc) continue;
            char stage_base[MAX_IDENTIFIER_LEN*2+16];
            snprintf(stage_base, sizeof(stage_base), "__ltng_oret_%s", fn->name);
            fprintf(out, "static char *%s = NULL;\n", stage_base);
            for (int i = 0; i < sc->count; i++) {
                char dst_f[MAX_IDENTIFIER_LEN*2+8];
                obj_resolve_field(stage_base, sc->fields[i].name, dst_f, sizeof(dst_f));
                fprintf(out, "static char *%s = NULL;\n", dst_f);
            }
        }
        fprintf(out, "\n");
    }

    /* Indirect call dispatcher prototype */
    fprintf(out, "static char *__ltng_call_indirect(const char *target, char *_ret, int _ltng_cl, int _ltng_cc, int argc, ...);\n\n");

    /* Emit function prototypes and definitions */
    if (program->functions) {
        int fncount = 0; for (ASTNode *fn = program->functions; fn; fn = fn->next) fncount++;
        ASTNode **fns = (ASTNode**)malloc(sizeof(ASTNode*) * (fncount > 0 ? fncount : 1));
        int fni = 0; for (ASTNode *fn = program->functions; fn; fn = fn->next) fns[fni++] = fn;
        /* prototypes */
        for (int i = fncount - 1; i >= 0; i--) {
            ASTNode *fn = fns[i];
            fprintf(out, "static char *%s(char *_ret, int _ltng_cl, int _ltng_cc", fn->name);
            for (int ci = 0; ci < fn->capture_count; ci++)
                fprintf(out, ", const char *_cap_%d", ci);
            for (ASTNode *p = fn->params; p; p = p->next)
                fprintf(out, ", const char *_a_%s", p->name);
            if (fn->is_variadic)
                fprintf(out, ", int _ltng_restc, const char **_ltng_restv");
            fprintf(out, ");\n");
        }
        fprintf(out, "\n");
        for (int i = fncount - 1; i >= 0; i--) {
            ASTNode *fn = fns[i];
            g_codegen_current_fn = fn;
            g_codegen_all_fns = program->functions;
            fprintf(out, "static char *%s(char *_ret, int _ltng_cl, int _ltng_cc", fn->name);
            for (int ci = 0; ci < fn->capture_count; ci++)
                fprintf(out, ", const char *_cap_%d", ci);
            for (ASTNode *p = fn->params; p; p = p->next)
                fprintf(out, ", const char *_a_%s", p->name);
            if (fn->is_variadic)
                fprintf(out, ", int _ltng_restc, const char **_ltng_restv");
            fprintf(out, ") {\n");
            fprintf(out, "    (void)_ret; char *_ltng_retval = _ltng_ds_new(\"\");\n");
            fprintf(out, "    _ltng_fn_push(\"");
            write_c_string_raw(out, fn->for_var[0] ? fn->for_var : fn->name);
            fprintf(out, "\", _ltng_cl, _ltng_cc);\n");
            /* bind captured variables (shared/mutable - decode pointer to shared storage) */
            for (int ci = 0; ci < fn->capture_count; ci++) {
                fprintf(out, "    char *%s = (char*)_ltng_str_to_ptr(_cap_%d);\n", fn->captures[ci], ci);
                fprintf(out, "    if(!%s) %s = _ltng_ds_new(\"\");\n",
                    fn->captures[ci], fn->captures[ci]);
            }
            for (ASTNode *p = fn->params; p; p = p->next) {
                if (p->var_type == TYPE_ARRAY) {
                    char arrname[MAX_IDENTIFIER_LEN];
                    snprintf(arrname, sizeof(arrname), "%s_data", p->name);
                    fprintf(out, "    int %s_len = 0;\n", p->name);
                    fprintf(out, "    int %s_cap = 0;\n", p->name);
                    fprintf(out, "    char **%s = NULL;\n", arrname);
                    fprintf(out, "    { char *__ahv = _ltng_ds_new(_a_%s ? _a_%s : \"\"); _ltng_ah *__ah = _ltng_ah_unpack(__ahv); if (__ah) { _ltng_arr_copy(__ah->d, &%s, &%s_len, &%s_cap, __ah->len); _ltng_ah_free(__ah); } free(__ahv); }\n",
                        p->name, p->name, arrname, p->name, p->name);
                } else if (is_var_captured_by_nested(fn, program->functions, p->name)) {
                    fprintf(out, "    char *%s = _ltng_ds_new(\"\");\n", p->name);
                    fprintf(out, "    _ltng_ds_set(&%s, _a_%s ? _a_%s : \"\");\n",
                        p->name, p->name, p->name);
                } else {
                    fprintf(out, "    char *%s = _ltng_ds_new(_a_%s ? _a_%s : \"\");\n",
                        p->name, p->name, p->name);
                }
            }
            /* Build the variadic rest parameter as a dynamic array. */
            if (fn->is_variadic && fn->variadic_name[0]) {
                char rest_cname[MAX_IDENTIFIER_LEN];
                mangle_name(fn->variadic_name, rest_cname, sizeof(rest_cname));
                fprintf(out, "    int %s_len = 0;\n", rest_cname);
                fprintf(out, "    int %s_cap = 0;\n", rest_cname);
                fprintf(out, "    char **%s_data = _ltng_arr_new(_ltng_restc > 0 ? _ltng_restc : 1);\n", rest_cname);
                fprintf(out, "    for (int __ri = 0; __ri < _ltng_restc; __ri++) _ltng_arr_push(&%s_data, &%s_len, &%s_cap, _ltng_restv ? _ltng_restv[__ri] : \"\");\n",
                    rest_cname, rest_cname, rest_cname);
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
                        fprintf(out, "    char *%s = _ltng_ds_new(\"", fv);
                        { const char *v=combined.fields[fi].value; for(;*v;v++){ if(*v=='\\'||*v=='\"') fputc('\\',out); if(*v=='\\n'){fputs("\\n",out);} else if(*v=='\\t'){fputs("\\t",out);} else fputc(*v,out);} }
                        fprintf(out, "\");\n");
                    }
                }
            }
            /* Locals are declared inline at let/const emission to support block scopes/shadowing. */
            {
                int saved_cleanup = g_cleanup_count;
                int saved_loop = g_loop_depth;
                int saved_infn = g_codegen_in_function;
                g_cleanup_count = 0;
                g_loop_depth = 0;
                g_codegen_in_function = 1;
                if (fn->body) emit_block(out, fn->body, table, 1);
                g_cleanup_count = saved_cleanup;
                g_loop_depth = saved_loop;
                g_codegen_in_function = saved_infn;
            }
            if (fn->base_is_array && fn->params && fn->params->name[0]) {
                fprintf(out, "    _ltng_ds_set(&_ltng_retval, _ltng_ah_pack(%s_data, %s_len, %s_cap));\n",
                    fn->params->name, fn->params->name, fn->params->name);
            }
            fprintf(out, "    _ltng_fn_pop();\n");
            fprintf(out, "    return _ltng_retval;\n");
            fprintf(out, "}\n\n");
        }
        g_codegen_current_fn = NULL;
        g_codegen_all_fns = NULL;
        free(fns);
    }

    /* Indirect call dispatcher for dynamic function values */
    fprintf(out, "static char *__ltng_call_indirect(const char *target, char *_ret, int _ltng_cl, int _ltng_cc, int argc, ...) {\n");
    fprintf(out, "    const char *_argv[64] = {");
    for (int _ai = 0; _ai < 64; _ai++) fprintf(out, "%s\"\"", _ai ? "," : "");
    fprintf(out, "};\n");
    fprintf(out, "    va_list ap; va_start(ap, argc);\n");
    fprintf(out, "    for (int i = 0; i < argc && i < 64; i++) { const char *v = va_arg(ap, const char*); _argv[i] = v ? v : \"\"; }\n");
    fprintf(out, "    va_end(ap);\n");
    fprintf(out, "    if (!target) target = \"\";\n");
    fprintf(out, "    char *_tname_buf = _ltng_ds_new(target);\n");
    fprintf(out, "    char *_tname = _tname_buf;\n");
    fprintf(out, "    const char *_caps[8] = {\"\",\"\",\"\",\"\",\"\",\"\",\"\"}; int _ncaps = 0;\n");
    fprintf(out, "    char *_sep = strchr(_tname, 1);\n");
    fprintf(out, "    if (_sep) { *_sep = 0; char *_p = _sep + 1; while (_p && *_p && _ncaps < 8) { char *_n = strchr(_p, 1); if (_n) { *_n = 0; _caps[_ncaps++] = _p; _p = _n + 1; } else { _caps[_ncaps++] = _p; break; } } }\n");
    if (program->functions) {
        int fncount_d = 0; for (ASTNode *fn = program->functions; fn; fn = fn->next) fncount_d++;
        ASTNode **fns_d = (ASTNode**)malloc(sizeof(ASTNode*) * (fncount_d > 0 ? fncount_d : 1));
        int fnj = 0; for (ASTNode *fn = program->functions; fn; fn = fn->next) fns_d[fnj++] = fn;
        for (int i = fncount_d - 1; i >= 0; i--) {
            ASTNode *fn = fns_d[i];
            int pc = 0;
            for (ASTNode *p = fn->params; p; p = p->next) pc++;
            if (fn->is_variadic) {
                fprintf(out, "    if (strcmp(_tname, \"%s\") == 0) {\n", fn->name);
                fprintf(out, "        int __rn = argc - %d; if (__rn < 0) __rn = 0; if (__rn > 64 - %d) __rn = 64 - %d;\n", pc, pc, pc);
                fprintf(out, "        const char *__rv[64];\n");
                fprintf(out, "        for (int __i = 0; __i < __rn; __i++) __rv[__i] = _argv[%d + __i];\n", pc);
                fprintf(out, "        char *_ir = %s(_ret, _ltng_cl, _ltng_cc", fn->name);
                for (int ci = 0; ci < fn->capture_count; ci++) fprintf(out, ", _caps[%d]", ci);
                for (int pi = 0; pi < pc; pi++) fprintf(out, ", _argv[%d]", pi);
                fprintf(out, ", __rn, (const char**)__rv);\n");
                fprintf(out, "        free(_tname_buf); return _ir;\n");
                fprintf(out, "    }\n");
            } else {
                fprintf(out, "    if (strcmp(_tname, \"%s\") == 0) {\n", fn->name);
                fprintf(out, "        char *_ir = %s(_ret, _ltng_cl, _ltng_cc", fn->name);
                for (int ci = 0; ci < fn->capture_count; ci++) fprintf(out, ", _caps[%d]", ci);
                for (int pi = 0; pi < pc; pi++) fprintf(out, ", _argv[%d]", pi);
                fprintf(out, ");\n");
                fprintf(out, "        free(_tname_buf); return _ir;\n");
                fprintf(out, "    }\n");
            }
        }
        free(fns_d);
    }
    fprintf(out, "    free(_tname_buf);\n");
    fprintf(out, "    _ltng_throw_value(\"Error\", \"Unknown function in indirect call\", _ltng_cl, _ltng_cc);\n");
    fprintf(out, "    return _ltng_ds_new(\"\");\n");
    fprintf(out, "}\n\n");


    fprintf(out, "int main(void) {\n");
    fprintf(out, "    _ltng_fn_push(\"main\", 1, 1);\n");
    g_cleanup_count = 0;
    g_loop_depth = 0;
    g_codegen_in_function = 0;

    for (int i = 0; i < table->count; i++) {
        if (table->vars[i].type == TYPE_ARRAY) {
            /* Array variables are declared in codegen_assign with their full
               array declaration. Skip here to avoid duplicate declaration. */
            continue;
        }
        if (table->vars[i].type == TYPE_OBJECT) {
            ObjSchema *sc = obj_lookup(table->vars[i].name);
            const char *dv = sc ? sc->default_val : "";
            fprintf(out, "    char *%s = _ltng_ds_new(\"", table->vars[i].name);
            write_c_string(out, dv);
            fprintf(out, "\");\n");
            if (sc) {
                for (int fi = 0; fi < sc->count; fi++) {
                    char fv[MAX_IDENTIFIER_LEN*2+8];
                    obj_resolve_field(table->vars[i].name, sc->fields[fi].name, fv, sizeof(fv));
                    int bsz = (strcmp(sc->fields[fi].name, "obj") == 0) ? 512 : 256;
                    fprintf(out, "    char *%s = _ltng_ds_new(\"", fv);
                    write_c_string(out, sc->fields[fi].value);
                    fprintf(out, "\");\n");
                }
            }
        } else {
            fprintf(out, "    char *%s = _ltng_ds_new(\"", table->vars[i].name);
            write_c_string(out, default_for(table->vars[i].type));
            fprintf(out, "\");\n");
        }
    }
    if (table->count > 0) fprintf(out, "\n");

    emit_block(out, program->head, table, 1);

    fprintf(out, "    return 0;\n");
    fprintf(out, "}\n");
}
