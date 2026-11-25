#include "parser_internal.h"

ErrorState *parser_errors = NULL;
int g_break_depth = 0;
ASTNode *g_list = NULL;
int g_stmt_line = 0;
int g_stmt_col = 0;
int g_expr_sl=0,g_expr_sc=0,g_expr_el=0,g_expr_ec=0;

char *_ltng_cfmt(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) { char *p = (char*)malloc(1); if(p) p[0]=0; return p; }
    char *p = (char*)malloc((size_t)n + 1);
    if (!p) return NULL;
    va_start(ap, fmt);
    vsnprintf(p, (size_t)n + 1, fmt, ap);
    va_end(ap);
    return p;
}

FuncInfo *g_funcs = NULL;
int g_func_count = 0;
int g_func_cap = 0;
void ensure_func_cap(void) {
    if (g_func_count >= g_func_cap) {
        int nc = g_func_cap ? g_func_cap * 2 : 32;
        FuncInfo *n = (FuncInfo*)realloc(g_funcs, nc * sizeof(FuncInfo));
        if (!n) return;
        for (int i = g_func_cap; i < nc; i++) memset(&n[i], 0, sizeof(FuncInfo));
        g_funcs = n; g_func_cap = nc;
    }
}
void funcinfo_init(FuncInfo *fi) {
    memset(fi, 0, sizeof(*fi));
    fi->passthrough_param = -1;
    fi->name = ltng_strdup("");
    fi->src = ltng_strdup("");
    fi->return_obj_base = ltng_strdup("");
    fi->return_type = TYPE_STRING;
}
void funcinfo_ensure_params(FuncInfo *fi, int n) {
    if (n <= fi->param_cap) return;
    int nc = fi->param_cap ? fi->param_cap * 2 : 8;
    while (nc < n) nc *= 2;
    fi->param_names = (char**)realloc(fi->param_names, nc * sizeof(char*));
    fi->has_default = (int*)realloc(fi->has_default, nc * sizeof(int));
    fi->def_type = (VarType*)realloc(fi->def_type, nc * sizeof(VarType));
    fi->def_start = (int*)realloc(fi->def_start, nc * sizeof(int));
    fi->def_end = (int*)realloc(fi->def_end, nc * sizeof(int));
    for (int i = fi->param_cap; i < nc; i++) { fi->param_names[i]=ltng_strdup(""); fi->has_default[i]=0; fi->def_type[i]=TYPE_NULL; fi->def_start[i]=0; fi->def_end[i]=0; }
    fi->param_cap = nc;
}

int g_in_function = 0;
int g_in_catch = 0;
Program *g_program_ref = NULL;
char *g_enclosing_cname = NULL;
int g_ret_obj_count = 0;
int g_anon_count = 0;

ObjSchema *g_objs = NULL;
int g_obj_count = 0;
int g_obj_cap = 0;

static void ensure_obj_cap(void) {
    if (g_obj_count < g_obj_cap) return;
    int nc = g_obj_cap ? g_obj_cap * 2 : 64;
    ObjSchema *n = (ObjSchema*)realloc(g_objs, nc * sizeof(ObjSchema));
    if (!n) return;
    for (int i = g_obj_cap; i < nc; i++) {
        memset(&n[i], 0, sizeof(ObjSchema));
        n[i].var = ltng_strdup("");
        n[i].default_val = ltng_strdup("");
    }
    g_objs = n; g_obj_cap = nc;
}

void obj_register(const char *mangled_var, ObjSchema *schema) {
    if (!mangled_var || !schema) return;
    for (int i = 0; i < g_obj_count; i++) {
        if (strcmp(g_objs[i].var, mangled_var) == 0) {
            objschema_free(&g_objs[i]);
            objschema_deep_copy(&g_objs[i], schema);
            free(g_objs[i].var); g_objs[i].var = ltng_strdup(mangled_var);
            return;
        }
    }
    ensure_obj_cap();
    if (g_obj_count < g_obj_cap) {
        objschema_deep_copy(&g_objs[g_obj_count], schema);
        free(g_objs[g_obj_count].var); g_objs[g_obj_count].var = ltng_strdup(mangled_var);
        g_obj_count++;
    }
}

ObjSchema *obj_lookup(const char *mangled_var) {
    if (!mangled_var) return NULL;
    for (int i = 0; i < g_obj_count; i++) {
        if (strcmp(g_objs[i].var, mangled_var) == 0) return &g_objs[i];
    }
    return NULL;
}

const char *obj_resolve_field(const char *mangled_var, const char *field, char *out, int out_size) {
    /* field var name: <mangled_var>__<mangled_field> */
    char *fm = mangle_name_alloc(field);
    snprintf(out, out_size, "%s__%s", mangled_var, fm ? fm : "");
    free(fm);
    return out;
}

char *obj_resolve_field_alloc(const char *mangled_var, const char *field) {
    char *fm = mangle_name_alloc(field);
    size_t n = strlen(mangled_var ? mangled_var : "") + 2 + strlen(fm ? fm : "") + 1;
    char *out = (char*)malloc(n);
    if (!out) { free(fm); return ltng_strdup(""); }
    snprintf(out, n, "%s__%s", mangled_var ? mangled_var : "", fm ? fm : "");
    free(fm);
    return out;
}


void objschema_free(ObjSchema *s) {
    if (!s) return;
    if (s->var) { free(s->var); s->var = NULL; }
    if (s->default_val) { free(s->default_val); s->default_val = NULL; }
    if (s->fields) {
        for (int i = 0; i < s->cap; i++) {
            free(s->fields[i].name); s->fields[i].name = NULL;
            free(s->fields[i].value); s->fields[i].value = NULL;
        }
        free(s->fields);
        s->fields = NULL;
    }
    s->count = 0; s->cap = 0;
}

void objschema_deep_copy(ObjSchema *dst, const ObjSchema *src) {
    if (!dst || !src) return;
    memset(dst, 0, sizeof(*dst));
    dst->var = ltng_strdup(src->var ? src->var : "");
    dst->default_val = ltng_strdup(src->default_val ? src->default_val : "");
    dst->count = src->count;
    dst->cap = src->count;
    if (dst->cap > 0) {
        dst->fields = (ObjField*)calloc((size_t)dst->cap, sizeof(ObjField));
        for (int i = 0; i < dst->cap; i++) {
            dst->fields[i].name = ltng_strdup(src->fields && i < src->count && src->fields[i].name ? src->fields[i].name : "");
            dst->fields[i].value = ltng_strdup(src->fields && i < src->count && src->fields[i].value ? src->fields[i].value : "");
            dst->fields[i].type = src->fields && i < src->count ? src->fields[i].type : TYPE_NULL;
        }
    }
}
void obj_build_all(ObjSchema *s) {
    /* "obj" field = all values in order, space-separated */
    size_t cap = 256, len = 0;
    char *buf = (char*)malloc(cap);
    if (!buf) return;
    buf[0] = 0;
    for (int i = 0; i < s->count; i++) {
        const char *v = s->fields[i].value ? s->fields[i].value : "";
        size_t vlen = strlen(v);
        size_t need = len + vlen + 2;
        if (need > cap) { while (cap < need) cap *= 2; char *nb = (char*)realloc(buf, cap); if (!nb) { free(buf); return; } buf = nb; }
        if (i > 0) buf[len++] = ' ';
        memcpy(buf + len, v, vlen); len += vlen; buf[len] = '\0';
    }
    /* store as a synthesized field named "obj" if not present, else update */
    int found = 0;
    for (int i = 0; i < s->count; i++) {
        if (s->fields[i].name && strcmp(s->fields[i].name, "obj") == 0) {
            free(s->fields[i].value); s->fields[i].value = ltng_strdup(buf);
            found = 1; break;
        }
    }
    if (!found) {
        objschema_ensure(s, s->count + 1);
        if (s->count < s->cap) {
            free(s->fields[s->count].name); s->fields[s->count].name = ltng_strdup("obj");
            free(s->fields[s->count].value); s->fields[s->count].value = ltng_strdup(buf);
            s->fields[s->count].type = TYPE_STRING;
            s->count++;
        }
    }
    /* default_val = field "default" if present, else "object" */
    free(s->default_val); s->default_val = ltng_strdup("object");
    for (int i = 0; i < s->count; i++) {
        if (s->fields[i].name && strcmp(s->fields[i].name, "default") == 0) {
            free(s->default_val); s->default_val = ltng_strdup(s->fields[i].value ? s->fields[i].value : "");
            break;
        }
    }
    free(buf);
}

void register_function(const char *name, int param_count) {
    if (g_func_count >= g_func_cap /*MAX_FUNCS*/) return;
    char *mangled = mangle_name_alloc(name);
    for (int i = 0; i < g_func_count; i++) {
        if (strcmp(g_funcs[i].src, name) == 0) {
            if (param_count > 0) g_funcs[i].param_count = param_count;
            free(mangled);
            return;
        }
    }
    ensure_func_cap();
    FuncInfo *_fi = &g_funcs[g_func_count];
    funcinfo_init(_fi);
    free(_fi->name); _fi->name = mangled;
    free(_fi->src); _fi->src = ltng_strdup(name);
    _fi->param_count = param_count;
    _fi->required_count = param_count;
    g_func_count++;
}

int is_function_name(const char *name) {
    char *mangled = mangle_name_alloc(name);
    for (int i = 0; i < g_func_count; i++) {
        if (strcmp(g_funcs[i].name, mangled) == 0 ||
            strcmp(g_funcs[i].src, name) == 0) { free(mangled); return 1; }
    }
    free(mangled);
    return 0;
}

void symbol_table_add_func(SymbolTable *table, const char *src, const char *cname, int param_count) {
    if (!table) return;
    for (int i = 0; i < table->func_count; i++) {
        if (strcmp(table->funcs[i].src, src) == 0) {
            free(table->funcs[i].cname); table->funcs[i].cname = ltng_strdup(cname);
            table->funcs[i].param_count = param_count;
            return;
        }
    }
    if (table->func_count >= table->funcs_cap) {
        int nc = table->funcs_cap ? table->funcs_cap * 2 : 8;
        FuncBind *nf = (FuncBind*)realloc(table->funcs, nc * sizeof(FuncBind));
        if (!nf) return;
        for (int i = table->funcs_cap; i < nc; i++) { nf[i].src = ltng_strdup(""); nf[i].cname = ltng_strdup(""); nf[i].param_count = 0; }
        table->funcs = nf; table->funcs_cap = nc;
    }
    FuncBind *b = &table->funcs[table->func_count++];
    free(b->src); b->src = ltng_strdup(src);
    free(b->cname); b->cname = ltng_strdup(cname);
    b->param_count = param_count;
}

const char *resolve_func_name(SymbolTable *table, const char *src, char *out, int out_size) {
    for (Scope *s = table; s; s = s->parent) {
        for (int i = 0; i < s->func_count; i++) {
            if (strcmp(s->funcs[i].src, src) == 0) {
                if (out && out_size > 0) {
                    strncpy(out, s->funcs[i].cname, out_size - 1);
                    out[out_size - 1] = 0;
                    return out;
                }
                return s->funcs[i].cname;
            }
        }
    }
    char *mangled = mangle_name_alloc(src);
    for (int i = 0; i < g_func_count; i++) {
        if (strcmp(g_funcs[i].name, mangled) == 0 || strcmp(g_funcs[i].src, src) == 0) {
            if (out && out_size > 0) {
                strncpy(out, g_funcs[i].name, out_size - 1);
                out[out_size - 1] = 0;
                free(mangled);
                return out;
            }
            const char *r = g_funcs[i].name;
            free(mangled);
            return r;
        }
    }
    free(mangled);
    return NULL;
}

int is_function_name_scoped(SymbolTable *table, const char *src) {
    return resolve_func_name(table, src, NULL, 0) != NULL;
}

void symbol_table_set_func_ref(SymbolTable *table, const char *name, const char *func_cname, int param_count);
int symbol_table_get_func_ref(SymbolTable *table, const char *name, char *out_cname, int out_size, int *out_param_count);
ASTNode *parse_function(Lexer *lexer, SymbolTable *table, const char *enclosing_cname);


int is_callable_ident(SymbolTable *table, const char *src) {
    if (is_function_name_scoped(table, src)) return 1;
    if (symbol_table_get_func_ref(table, src, NULL, 0, NULL)) return 1;
    {
        char *mangled = mangle_name_alloc(src);
        int has = symbol_table_has(table, mangled) || symbol_table_has(table, src);
        free(mangled);
        if (has) return 1;
    }
    return 0;
}
ASTNode *parse_block(Lexer *lexer, SymbolTable *table, int is_control);
ASTNode *parse_try(Lexer *lexer, SymbolTable *table);
void parse_throw(Lexer *lexer, SymbolTable *table);

ASTNode *reverse_list(ASTNode *head) {
    ASTNode *prev = NULL;
    ASTNode *current = head;
    while (current) {
        ASTNode *next = current->next;
        current->next = prev;
        prev = current;
        current = next;
    }
    return prev;
}

void emit(ASTNode *node) {
    if (!node) return;
    if (node->line == 0 && g_stmt_line > 0) { node->line = g_stmt_line; node->col = g_stmt_col; }
    node->next = g_list;
    g_list = node;
}

ASTNode *create_node(NodeType type, const char *name, const char *value) {
    ASTNode *node = (ASTNode *)malloc(sizeof(ASTNode));
    if (!node) {
        fprintf(stderr, "Error: Memory allocation failed\n");
        exit(1);
    }
    memset(node, 0, sizeof(ASTNode));
    node->passthrough_param = -1;
    node->type = type;
    node->name = ltng_strdup(name ? name : "");
    node->value = ltng_strdup(value ? value : "");
    node->variadic_name = ltng_strdup("");
    node->return_obj_base = ltng_strdup("");
    node->for_var = ltng_strdup("");
    node->for_start = ltng_strdup("");
    node->for_end = ltng_strdup("");
    node->for_step = ltng_strdup("");
    node->array_index = ltng_strdup("");
    node->var_type = TYPE_STRING;
    node->array_elem_type = TYPE_NULL;
    return node;
}

int is_bool_value(const char *value) {
    return strcmp(value, "true") == 0 || strcmp(value, "t") == 0 ||
           strcmp(value, "false") == 0 || strcmp(value, "f") == 0;
}

const char *bool_to_str(const char *val) {
    if (strcmp(val, "true") == 0 || strcmp(val, "t") == 0) return "true";
    return "false";
}

VarType detect_type(const char *value) {
    if (value[0] == '"') return TYPE_STRING;
    if (is_bool_value(value)) return TYPE_BOOL;

    const char *p = value;
    if (*p == '-') p++;

    int has_dot = 0;
    int has_digit = 0;
    for (; *p; p++) {
        if (*p == '.') {
            has_dot = 1;
        } else if (!isdigit(*p)) {
            return TYPE_STRING;
        } else {
            has_digit = 1;
        }
    }

    if (!has_digit) return TYPE_STRING;
    return has_dot ? TYPE_FLOAT : TYPE_INT;
}

int is_type_keyword(const char *name) {
    return strcmp(name, "int") == 0 ||
           strcmp(name, "float") == 0 ||
           strcmp(name, "string") == 0 ||
           strcmp(name, "char") == 0 ||
           strcmp(name, "bool") == 0 ||
           strcmp(name, "obj") == 0 ||
           strcmp(name, "array") == 0;
}

/* Return-type keywords accepted after '->'. Unlike declaration type keywords,
   this additionally allows 'void' to mark a function that returns no value. */
int is_return_type_keyword(const char *name) {
    return is_type_keyword(name) || strcmp(name, "void") == 0;
}

/* Return 1 for type conversion functions/calls: toint/int, tofloat/float,
   tostring/string, tobool/bool, tochar/char. */
int is_type_conversion(const char *name) {
    return strcmp(name, "toint") == 0 || strcmp(name, "int") == 0 ||
           strcmp(name, "tostring") == 0 || strcmp(name, "string") == 0 ||
           strcmp(name, "tobool") == 0 || strcmp(name, "bool") == 0 ||
           strcmp(name, "tofloat") == 0 || strcmp(name, "float") == 0 ||
           strcmp(name, "tochar") == 0 || strcmp(name, "char") == 0;
}

int is_ltng_reserved(const char *name) {
    return strcmp(name, "let") == 0 ||
           strcmp(name, "const") == 0 ||
           strcmp(name, "set") == 0 ||
           strcmp(name, "write") == 0 ||
           strcmp(name, "read") == 0 ||
           strcmp(name, "exit") == 0 ||
           strcmp(name, "typeof") == 0 ||
           strcmp(name, "while") == 0 ||
           strcmp(name, "do") == 0 ||
           strcmp(name, "for") == 0 ||
           strcmp(name, "if") == 0 ||
           strcmp(name, "else") == 0 ||
           strcmp(name, "null") == 0 ||
           strcmp(name, "try") == 0 ||
           strcmp(name, "catch") == 0 ||
           strcmp(name, "finally") == 0 ||
           strcmp(name, "throw") == 0 ||
           is_type_keyword(name) ||
           is_bool_value(name);
}

/* ltng keywords that cannot be used as function names (C reserved words like
   return/break/continue/switch/case/default are allowed as variable names). */
int is_ltng_keyword(const char *name) {
    return strcmp(name, "return") == 0 ||
           strcmp(name, "break") == 0 ||
           strcmp(name, "continue") == 0 ||
           strcmp(name, "function") == 0 ||
           strcmp(name, "switch") == 0 ||
           strcmp(name, "case") == 0 ||
           strcmp(name, "default") == 0 ||
           strcmp(name, "from") == 0 ||
           strcmp(name, "to") == 0 ||
           strcmp(name, "step") == 0 ||
           strcmp(name, "in") == 0 ||
           strcmp(name, "and") == 0 ||
           strcmp(name, "or") == 0 ||
           strcmp(name, "not") == 0 ||
           strcmp(name, "try") == 0 ||
           strcmp(name, "catch") == 0 ||
           strcmp(name, "finally") == 0 ||
           strcmp(name, "throw") == 0;
}

/* Tokens that can serve as identifier names in declaration/reference contexts.
   Keywords like return/break/switch/case/default/function/from/to/step/in carry
   their text in .value and can be used as mangled variable names. */
int token_is_name(Token t) {
    if (t.type == TOKEN_IDENT) return 1;
    switch (t.type) {
        case TOKEN_RETURN: case TOKEN_BREAK: case TOKEN_CONTINUE: case TOKEN_FUNCTION:
        case TOKEN_SWITCH: case TOKEN_CASE: case TOKEN_DEFAULT:
        case TOKEN_FROM: case TOKEN_TO: case TOKEN_STEP: case TOKEN_IN:
            return t.value[0] != '\0';
        default: return 0;
    }
}

int is_c_reserved(const char *name) {
    return strcmp(name, "double") == 0 ||
           strcmp(name, "long") == 0 ||
           strcmp(name, "short") == 0 ||
           strcmp(name, "unsigned") == 0 ||
           strcmp(name, "signed") == 0 ||
           strcmp(name, "void") == 0 ||
           strcmp(name, "if") == 0 ||
           strcmp(name, "else") == 0 ||
           strcmp(name, "while") == 0 ||
           strcmp(name, "for") == 0 ||
           strcmp(name, "do") == 0 ||
           strcmp(name, "return") == 0 ||
           strcmp(name, "break") == 0 ||
           strcmp(name, "continue") == 0 ||
           strcmp(name, "switch") == 0 ||
           strcmp(name, "case") == 0 ||
           strcmp(name, "default") == 0 ||
           strcmp(name, "struct") == 0 ||
           strcmp(name, "union") == 0 ||
           strcmp(name, "enum") == 0 ||
           strcmp(name, "typedef") == 0 ||
           strcmp(name, "sizeof") == 0 ||
           strcmp(name, "static") == 0 ||
           strcmp(name, "extern") == 0 ||
           strcmp(name, "auto") == 0 ||
           strcmp(name, "register") == 0 ||
           strcmp(name, "volatile") == 0 ||
           strcmp(name, "main") == 0 ||
           strcmp(name, "printf") == 0 ||
           strcmp(name, "scanf") == 0 ||
           strcmp(name, "snprintf") == 0 ||
           strcmp(name, "include") == 0;
}

void mangle_name(const char *in, char *out, int out_size) {
    if (!out || out_size <= 0) return;
    if (!in) { out[0] = 0; return; }
    if (is_c_reserved(in)) {
        snprintf(out, out_size, "%s_res", in);
    } else {
        strncpy(out, in, out_size - 1);
        out[out_size - 1] = '\0';
    }
}

char *mangle_name_alloc(const char *in) {
    if (!in) return ltng_strdup("");
    if (is_c_reserved(in)) {
        size_t n = strlen(in) + 5;
        char *out = (char*)malloc(n);
        if (!out) return ltng_strdup("");
        snprintf(out, n, "%s_res", in);
        return out;
    }
    return ltng_strdup(in);
}

VarType type_keyword_to_vartype(const char *name) {
    if (strcmp(name, "int") == 0) return TYPE_INT;
    if (strcmp(name, "float") == 0) return TYPE_FLOAT;
    if (strcmp(name, "string") == 0) return TYPE_STRING;
    if (strcmp(name, "char") == 0) return TYPE_CHAR;
    if (strcmp(name, "bool") == 0) return TYPE_BOOL;
    if (strcmp(name, "obj") == 0) return TYPE_OBJECT;
    if (strcmp(name, "array") == 0) return TYPE_ARRAY;
    if (strcmp(name, "void") == 0) return TYPE_VOID;
    return TYPE_STRING;
}

void scope_init(Scope *scope, Scope *parent, int disallow_decl) {
    if (!scope) return;
    memset(scope, 0, sizeof(Scope));
    scope->parent = parent;
    scope->disallow_decl = disallow_decl || (parent ? parent->disallow_decl : 0);
}

int g_scope_var_uid = 0;

void symbol_table_add_in_ex(SymbolTable *table, const char *name, VarType type, int unique_cname) {
    if (!table || !name) return;
    for (int i = 0; i < table->count; i++) {
        if (strcmp(table->vars[i].name, name) == 0) {
            table->vars[i].type = type;
            table->vars[i].is_func_ref = 0;
            { free(table->vars[i].func_ref_cname); table->vars[i].func_ref_cname = ltng_strdup(""); }
            table->vars[i].func_ref_param_count = -1;
            return;
        }
    }
    if (table->count >= table->vars_cap) {
        int nc = table->vars_cap ? table->vars_cap * 2 : 32;
        VarEntry *nv = (VarEntry *)realloc(table->vars, nc * sizeof(VarEntry));
        if (!nv) {
            if (parser_errors) error_state_add(parser_errors, 0, 0, "Out of memory adding variable");
            return;
        }
        for (int _i = table->vars_cap; _i < nc; _i++) {
            memset(&nv[_i], 0, sizeof(VarEntry));
            nv[_i].name = ltng_strdup("");
            nv[_i].cname = ltng_strdup("");
            nv[_i].func_ref_cname = ltng_strdup("");
        }
        table->vars = nv;
        table->vars_cap = nc;
    }
    VarEntry *ve = &table->vars[table->count];
    memset(ve, 0, sizeof(*ve));
    { free(ve->name); ve->name = ltng_strdup(name); }
    if (unique_cname) {
        { char *_s = _ltng_cfmt("_v%d_%s", g_scope_var_uid++, name); free(ve->cname); ve->cname = _s ? _s : ltng_strdup(""); }
    } else {
        { free(ve->cname); ve->cname = ltng_strdup(name); }
    }
    ve->type = type;
    ve->is_const = 0;
    ve->is_locked = 0;
    ve->is_param = 0;
    ve->array_dim_count = 0;
    ve->array_dims = NULL;
    ve->array_elem_type = TYPE_NULL;
    ve->is_func_ref = 0;
    { free(ve->func_ref_cname); ve->func_ref_cname = ltng_strdup(""); }
    ve->func_ref_param_count = -1;
    table->count++;
}

int symbol_table_has_in_parents(SymbolTable *table, const char *name) {
    for (Scope *s = table ? table->parent : NULL; s; s = s->parent) {
        for (int i = 0; i < s->count; i++) {
            if (strcmp(s->vars[i].name, name) == 0) return 1;
        }
    }
    return 0;
}

void symbol_table_add_in(SymbolTable *table, const char *name, VarType type) {
    /* Use unique C names only for declarations that shadow an outer variable. */
    int unique = (table && table->parent != NULL && symbol_table_has_in_parents(table, name));
    symbol_table_add_in_ex(table, name, type, unique);
}

/* Add/update in current scope */
void symbol_table_add(SymbolTable *table, const char *name, VarType type) {
    symbol_table_add_in(table, name, type);
}

/* Add in current scope only (no update search up) */
void symbol_table_add_local(SymbolTable *table, const char *name, VarType type) {
    symbol_table_add_in(table, name, type);
}

/* Add a function parameter (C name must match parameter variable name) */
void symbol_table_add_param(SymbolTable *table, const char *name, VarType type) {
    symbol_table_add_in_ex(table, name, type, 0);
    for (int i = 0; i < table->count; i++) {
        if (strcmp(table->vars[i].name, name) == 0) {
            table->vars[i].is_param = 1;
            return;
        }
    }
}

/* Add a variable and auto-lock it if it has a concrete (non-null) type.
   Used for let/const declarations so that declared variables are immutable
   by default unless unlock() is called. */
void symbol_table_add_locked(SymbolTable *table, const char *name, VarType type) {
    symbol_table_add_in(table, name, type);
    if (type != TYPE_NULL) {
        symbol_table_set_locked(table, name, 1);
    }
}

/* Update an existing variable in the scope chain (for set) */
void symbol_table_update(SymbolTable *table, const char *name, VarType type) {
    for (Scope *s = table; s; s = s->parent) {
        for (int i = 0; i < s->count; i++) {
            if (strcmp(s->vars[i].name, name) == 0) {
                s->vars[i].type = type;
                s->vars[i].is_func_ref = 0;
                { free(s->vars[i].func_ref_cname); s->vars[i].func_ref_cname = ltng_strdup(""); }
                s->vars[i].func_ref_param_count = -1;
                return;
            }
        }
    }
    symbol_table_add_in(table, name, type);
}

VarType symbol_table_get(SymbolTable *table, const char *name) {
    for (Scope *s = table; s; s = s->parent) {
        for (int i = 0; i < s->count; i++) {
            if (strcmp(s->vars[i].name, name) == 0) {
                return s->vars[i].type;
            }
        }
    }
    return TYPE_STRING;
}

void symbol_table_set_func_ref(SymbolTable *table, const char *name, const char *func_cname, int param_count) {
    for (Scope *s = table; s; s = s->parent) {
        for (int i = 0; i < s->count; i++) {
            if (strcmp(s->vars[i].name, name) == 0) {
                s->vars[i].is_func_ref = 1;
                { free(s->vars[i].func_ref_cname); s->vars[i].func_ref_cname = ltng_strdup(func_cname); }
                s->vars[i].func_ref_param_count = param_count;
                return;
            }
        }
    }
}

int symbol_table_get_func_ref(SymbolTable *table, const char *name, char *out_cname, int out_size, int *out_param_count) {
    char *mangled = mangle_name_alloc(name);
    for (Scope *s = table; s; s = s->parent) {
        for (int i = 0; i < s->count; i++) {
            if (strcmp(s->vars[i].name, name) == 0 || strcmp(s->vars[i].name, mangled) == 0) {
                if (!s->vars[i].is_func_ref) { free(mangled); return 0; }
                if (out_cname && out_size > 0) {
                    strncpy(out_cname, s->vars[i].func_ref_cname, out_size - 1);
                    out_cname[out_size - 1] = '\0';
                }
                if (out_param_count) *out_param_count = s->vars[i].func_ref_param_count;
                free(mangled);
                return 1;
            }
        }
    }
    free(mangled);
    return 0;
}

const char *symbol_table_get_cname(SymbolTable *table, const char *name) {
    for (Scope *s = table; s; s = s->parent) {
        for (int i = 0; i < s->count; i++) {
            if (strcmp(s->vars[i].name, name) == 0) {
                return s->vars[i].cname[0] ? s->vars[i].cname : s->vars[i].name;
            }
        }
    }
    return name;
}

int symbol_table_has(SymbolTable *table, const char *name) {
    for (Scope *s = table; s; s = s->parent) {
        for (int i = 0; i < s->count; i++) {
            if (strcmp(s->vars[i].name, name) == 0) {
                return 1;
            }
        }
    }
    return 0;
}

int symbol_table_has_local(SymbolTable *table, const char *name) {
    for (int i = 0; i < table->count; i++) {
        if (strcmp(table->vars[i].name, name) == 0) {
            return 1;
        }
    }
    return 0;
}

void symbol_table_set_const(SymbolTable *table, const char *name, int is_const) {
    for (Scope *s = table; s; s = s->parent) {
        for (int i = 0; i < s->count; i++) {
            if (strcmp(s->vars[i].name, name) == 0) {
                s->vars[i].is_const = is_const;
                return;
            }
        }
    }
}

int symbol_table_is_const(SymbolTable *table, const char *name) {
    for (Scope *s = table; s; s = s->parent) {
        for (int i = 0; i < s->count; i++) {
            if (strcmp(s->vars[i].name, name) == 0) {
                return s->vars[i].is_const;
            }
        }
    }
    return 0;
}

int symbol_table_get_locked(SymbolTable *table, const char *name) {
    for (Scope *s = table; s; s = s->parent) {
        for (int i = 0; i < s->count; i++) {
            if (strcmp(s->vars[i].name, name) == 0) {
                return s->vars[i].is_locked;
            }
        }
    }
    return 0;
}

void symbol_table_set_locked(SymbolTable *table, const char *name, int locked) {
    for (Scope *s = table; s; s = s->parent) {
        for (int i = 0; i < s->count; i++) {
            if (strcmp(s->vars[i].name, name) == 0) {
                s->vars[i].is_locked = locked;
                return;
            }
        }
    }
}

int symbol_table_is_param(SymbolTable *table, const char *name) {
    for (Scope *s = table; s; s = s->parent) {
        for (int i = 0; i < s->count; i++) {
            if (strcmp(s->vars[i].name, name) == 0) {
                return s->vars[i].is_param;
            }
        }
    }
    return 0;
}

void symbol_table_set_param(SymbolTable *table, const char *name, int is_param) {
    for (Scope *s = table; s; s = s->parent) {
        for (int i = 0; i < s->count; i++) {
            if (strcmp(s->vars[i].name, name) == 0) {
                s->vars[i].is_param = is_param;
                return;
            }
        }
    }
}

void symbol_table_set_array_dims(SymbolTable *table, const char *name,
                                 int dim_count, const int *dims) {
    for (Scope *s = table; s; s = s->parent) {
        for (int i = 0; i < s->count; i++) {
            if (strcmp(s->vars[i].name, name) == 0) {
                s->vars[i].array_dim_count = dim_count;
                free(s->vars[i].array_dims);
                if (dim_count > 0 && dims != NULL) {
                    s->vars[i].array_dims = malloc(dim_count * sizeof(int));
                    for (int d = 0; d < dim_count; d++) {
                        s->vars[i].array_dims[d] = dims[d];
                    }
                } else {
                    s->vars[i].array_dims = NULL;
                }
                return;
            }
        }
    }
}

int symbol_table_get_array_dims(SymbolTable *table, const char *name,
                                int **dims, int *dim_count) {
    for (Scope *s = table; s; s = s->parent) {
        for (int i = 0; i < s->count; i++) {
            if (strcmp(s->vars[i].name, name) == 0) {
                if (s->vars[i].type != TYPE_ARRAY || s->vars[i].array_dim_count <= 0) {
                    return 0;
                }
                *dims = s->vars[i].array_dims;
                *dim_count = s->vars[i].array_dim_count;
                return 1;
            }
        }
    }
    return 0;
}

void symbol_table_set_array_elem_type(SymbolTable *table, const char *name, VarType elem_type) {
    for (Scope *s = table; s; s = s->parent) {
        for (int i = 0; i < s->count; i++) {
            if (strcmp(s->vars[i].name, name) == 0) {
                s->vars[i].array_elem_type = elem_type;
                return;
            }
        }
    }
}

VarType symbol_table_get_array_elem_type(SymbolTable *table, const char *name) {
    for (Scope *s = table; s; s = s->parent) {
        for (int i = 0; i < s->count; i++) {
            if (strcmp(s->vars[i].name, name) == 0) {
                return s->vars[i].array_elem_type;
            }
        }
    }
    return TYPE_NULL;
}

void skip_to_newline(Lexer *lexer) {
    while (lexer->current < lexer->count) {
        Token t = lexer_peek(lexer);
        if (t.type == TOKEN_NEWLINE || t.type == TOKEN_EOF) break;
        lexer_next(lexer);
    }
}

int is_operator(TokenType t) {
    return t == TOKEN_PLUS || t == TOKEN_MINUS || t == TOKEN_STAR ||
           t == TOKEN_SLASH || t == TOKEN_PERCENT || t == TOKEN_AMP_AMP ||
           t == TOKEN_PIPE_PIPE || t == TOKEN_CARET_CARET || t == TOKEN_BANG || t == TOKEN_EQ_EQ ||
           t == TOKEN_BANG_EQ || t == TOKEN_LT || t == TOKEN_GT ||
           t == TOKEN_LT_EQ || t == TOKEN_GT_EQ ||
           t == TOKEN_PLUS_PLUS || t == TOKEN_MINUS_MINUS ||
           t == TOKEN_QUESTION;
}

int token_is_int(Token tok, SymbolTable *table) {
    if (tok.type == TOKEN_INT_LIT) return 1;
    if (tok.type == TOKEN_IDENT && !is_bool_value(tok.value) && !is_type_keyword(tok.value)) {
        char *mangled = mangle_name_alloc(tok.value);
        int ok = symbol_table_has(table, mangled) && symbol_table_get(table, mangled) == TYPE_INT;
        free(mangled);
        return ok;
    }
    return 0;
}

FuncInfo *find_funcinfo_by_cname(const char *cname);

VarType effective_param_default_type(const char *src_name, VarType sym_type) {
    if (!(g_enclosing_cname ? g_enclosing_cname[0] : 0)) return sym_type;
    /* Untyped params are typically stored as TYPE_FLOAT placeholders in scope.
       For params with known default types, prefer the default type in typing/inference. */
    if (sym_type != TYPE_FLOAT) return sym_type;
    FuncInfo *efi = find_funcinfo_by_cname(g_enclosing_cname);
    if (!efi) return sym_type;
    for (int pi = 0; pi < efi->param_count; pi++) {
        if (strcmp(efi->param_names[pi], src_name) == 0) {
            if (efi->has_default[pi] && efi->def_type[pi] != TYPE_NULL)
                return efi->def_type[pi];
            return sym_type;
        }
    }
    return sym_type;
}

int has_operator_ahead(Lexer *lexer);

int try_parse_typeof(Lexer *lexer, SymbolTable *table, Token *result);

int has_operator_ahead(Lexer *lexer) {
    int depth = 0;
    for (int i = lexer->current; i < lexer->count; i++) {
        Token t = lexer->tokens[i];
        if (t.type == TOKEN_NEWLINE || t.type == TOKEN_EOF) break;
        if (t.type == TOKEN_RPAREN) {
            if (depth == 0) break;
            depth--;
        }
        if (t.type == TOKEN_LPAREN) depth++;
        if (is_operator(t.type) && depth == 0) return 1;
        if (t.type == TOKEN_LBRACKET && depth == 0) return 1;
    }
    return 0;
}


/* (reserved for future string-concat transform) */



void transform_string_multiply(char *expr, int size) {
    if (strstr(expr, "*") == NULL) return;
    size_t elen = expr ? strlen(expr) : 0;
    size_t bufcap = elen + 64;
    if (bufcap < 2048) bufcap = 2048;
    char *tmp = (char*)malloc(bufcap); if(!tmp) return;
    memcpy(tmp, expr, elen + 1);
    char *start = tmp;
    char *left = (char*)calloc(1,bufcap); char *right = (char*)calloc(1,bufcap);
    if(!left||!right){free(tmp);free(left);free(right);return;}
    int found = 0;
    int depth_brace = 0;
    for (char *p = tmp; *p && !found; p++) {
        if (*p == '"') {
            p++;
            while (*p && *p != '"') {
                if (*p == '\\') p++;
                p++;
            }
            continue;
        }
        if (*p == '\'') {
            p++;
            while (*p && *p != '\'') {
                if (*p == '\\') p++;
                p++;
            }
            continue;
        }
        /* Multiplication inside a statement expression or block (e.g. an array
           index expression) is not a string repetition operator. */
        if (*p == '{') depth_brace++;
        else if (*p == '}') { if (depth_brace > 0) depth_brace--; }
        if (*p == '*' && depth_brace == 0) {
            *p = 0;
            char *s = start;
            while (*s == ' ' || *s == '\t') s++;
            char *e = s + strlen(s) - 1;
            while (e >= s && (*e == ' ' || *e == '\t')) {
                *e = 0;
                e--;
            }
            strncpy(left, s, bufcap - 1);
            left[bufcap - 1] = 0;
            s = p + 1;
            while (*s == ' ' || *s == '\t') s++;
            e = s + strlen(s) - 1;
            while (e >= s && (*e == ' ' || *e == '\t')) {
                *e = 0;
                e--;
            }
            strncpy(right, s, bufcap - 1);
            right[bufcap - 1] = 0;
            /* trim trailing tokens that belong to outer context (e.g. closing parens of wrapper) */
            /* keep only the first simple operand: number, identifier, or parenthesized expr */
            {
                char *rp = right;
                /* if starts with '(' (but not stmt expr), take balanced paren group */
                if (rp[0] == '(' && !(rp[0]=='(' && rp[1]=='{')) {
                    int d=0, inq2=0; char qc2=0; char *p2=rp;
                    for (; *p2; p2++) {
                        if (inq2) { if (*p2=='\\' && p2[1]) { p2++; continue; } if (*p2==qc2) inq2=0; continue; }
                        if (*p2=='"'||*p2=='\'') { inq2=1; qc2=*p2; continue; }
                        if (*p2=='(') d++; else if (*p2==')') { d--; if (d==0) { p2++; break; } }
                    }
                    *p2 = 0;
                } else if (isdigit((unsigned char)rp[0]) || (rp[0]=='-' && isdigit((unsigned char)rp[1]))) {
                    char *p2 = rp; if (*p2=='-') p2++;
                    while (isdigit((unsigned char)*p2) || *p2=='.') p2++;
                    *p2 = 0;
                } else if (isalpha((unsigned char)rp[0]) || rp[0]=='_') {
                    char *p2 = rp; while (isalnum((unsigned char)*p2) || *p2=='_') p2++;
                    *p2 = 0;
                } else if (rp[0]=='(' && rp[1]=='{') {
                    /* statement expr - find matching close: ({ ... }) */
                    int d=0, inq2=0; char qc2=0; char *p2=rp;
                    for (; *p2; p2++) {
                        if (inq2) { if (*p2=='\\' && p2[1]) { p2++; continue; } if (*p2==qc2) inq2=0; continue; }
                        if (*p2=='"'||*p2=='\'') { inq2=1; qc2=*p2; continue; }
                        if (*p2=='(') d++; else if (*p2==')') { d--; if (d==0) { p2++; break; } }
                    }
                    *p2 = 0;
                }
            }
            /* trim trailing whitespace again */
            { char *e = right + strlen(right) - 1; while (e >= right && (*e==' '||*e=='\t')) { *e=0; e--; } }
            /* balance left side: if it has unmatched leading '(' due to outer wrapper, strip it */
            {
                int balance = 0, inq2 = 0; char qc2 = 0;
                for (char *p2 = left; *p2; p2++) {
                    if (inq2) { if (*p2=='\\' && p2[1]) { p2++; continue; } if (*p2==qc2) inq2=0; continue; }
                    if (*p2=='"'||*p2=='\'') { inq2=1; qc2=*p2; continue; }
                    if (*p2=='(') balance++; else if (*p2==')') balance--;
                }
                while (balance > 0 && left[0] == '(') {
                    /* try removing leading '(' */
                    char *ls = left + 1;
                    while (*ls == ' ' || *ls == '\t') ls++;
                    memmove(left, ls, strlen(ls) + 1);
                    balance--;
                    /* re-trim */
                    char *e = left + strlen(left) - 1; while (e >= left && (*e==' '||*e=='\t')) { *e=0; e--; }
                }
            }
            found = 1;
        }
    }
    if (!found) { free(tmp); free(left); free(right); return; }
    size_t result_cap = bufcap * 2;
    char *result = (char*)malloc(result_cap); if(!result){free(tmp);free(left);free(right);return;}
    /* determine whether the count operand (right) is a pure numeric literal;
       if it's a variable/string buffer, convert with atoi() so we don't cast a pointer */
    int right_is_num = 1;
    const char *rs = right;
    if (*rs == '-') rs++;
    if (*rs == 0) right_is_num = 0;
    else {
        int hasdig = 0;
        for (; *rs; rs++) {
            if (isdigit(*rs)) hasdig = 1;
            else if (*rs == '.') { /* ok */ }
            else { right_is_num = 0; break; }
        }
        if (!hasdig) right_is_num = 0;
    }
    char *cntbuf = NULL;
    const char *count_expr;
    if (right_is_num) {
        count_expr = right;
    } else {
        size_t ccap = strlen(right) + 16;
        cntbuf = (char*)malloc(ccap); if(!cntbuf){free(tmp);free(left);free(right);free(result);return;}
        snprintf(cntbuf, ccap, "atoll(%s)", right);
        count_expr = cntbuf;
    }
    int left_is_num = 1;
    const char *ls = left;
    if (*ls == '-') ls++;
    if (*ls == 0) left_is_num = 0;
    else {
        int hasdig = 0;
        for (; *ls; ls++) {
            if (isdigit(*ls)) hasdig = 1;
            else if (*ls == '.') { /* ok */ }
            else { left_is_num = 0; break; }
        }
        if (!hasdig) left_is_num = 0;
    }
    if (left_is_num && (right[0] == '"' || (right[0] == '\'' && strlen(right) >= 3))) {
        /* number * "string" */
        snprintf(result, result_cap, "_ltng_repeat_dyn(%s,(long long)(%s))", right, left);
    } else if (right_is_num && (left[0] == '"' || (left[0] == '\'' && strlen(left) >= 3))) {
        /* "string" * number */
        snprintf(result, result_cap, "_ltng_repeat_dyn(%s,(long long)(%s))", left, right);
    } else {
        /* variable cases: assume left is string, right is count */
        snprintf(result, result_cap, "_ltng_repeat_dyn(%s,(long long)(%s))", left, count_expr);
    }
    strncpy(expr, result, size - 1);
    expr[size - 1] = 0;
    free(cntbuf); free(tmp); free(left); free(right); free(result);
}


ASTNode *parse_call_node(Lexer *lexer, SymbolTable *table, const char *fname_tok_val, int line, int col, ASTNode *first_arg);
void build_call_cexpr(ASTNode *call, char *cexpr, int cexpr_size);
int append_immediate_call_chains(Lexer *lexer, SymbolTable *table, char *cexpr, int cexpr_size);
ASTNode *make_receiver_arg(SymbolTable *table, Token rtok);


void sanitize_mod_atof(char *out, int out_size) {
    /* No-op: modulo/division rewriting and runtime macros handle operand typing. */
    (void)out; (void)out_size;
}

int part_is_stringy(const char *part);

/* Normalize a string-expression fragment: resolve '+' (concat) and '*'
   (repeat) with correct operator precedence, resolving innermost
   parenthesized groups first.  Statement expressions ({ ... }) are treated
   as atomic operands.  array_access_used selects _ltng_add_sn (array
   elements) vs the snprintf(...) concat form (general contexts).  Each
   pass resolves one group, so the number of passes is bounded by the
   string length - no fixed depth cap that would crash on deeply nested
   parentheses. */
void normalize_string_expr(char *buf, int buf_size, int array_access_used, SymbolTable *table) {
    if (!buf || buf_size < 2) return;
    size_t work_cap = strlen(buf) + 256; if (work_cap < 8192) work_cap = 8192;
    size_t big_cap = work_cap;
    size_t mid_cap = work_cap / 2; if (mid_cap < 4096) mid_cap = 4096;
    size_t small_cap = work_cap / 4; if (small_cap < 2048) small_cap = 2048;
    int parts_cap = 16; size_t part_w = small_cap;
    char *work = (char*)malloc(work_cap); if(!work) return;
    char *content = (char*)malloc(mid_cap);
    char *ires = (char*)malloc(mid_cap);
    char *lhs = (char*)malloc(mid_cap);
    char *rhs = (char*)malloc(small_cap);
    char *str_operand = (char*)malloc(mid_cap);
    char *nse_left = (char*)malloc(mid_cap);
    char *nse_right = (char*)malloc(small_cap);
    char *parts_buf = (char*)malloc((size_t)parts_cap * part_w);
    if(!content||!ires||!lhs||!rhs||!str_operand||!nse_left||!nse_right||!parts_buf){
        free(content);free(ires);free(lhs);free(rhs);free(str_operand);free(nse_left);free(nse_right);free(parts_buf);free(work);return;
    }
    memcpy(work, buf, strlen(buf) + 1);
    int just_wrapped = 0;

    for (int pass = 0; pass < (int)strlen(work); pass++) {
        int wlen = (int)strlen(work);
        int bs = -1, be = -1;
        int do_plus = 0, do_mul = 0;
        for (int ii = 0; ii < wlen; ii++) {
            if (work[ii] != '(') continue;
            if (ii + 1 < wlen && work[ii + 1] == '{') continue;
            int d = 0, inq = 0; char qc = 0; int jj = ii;
            while (jj < wlen) {
                if (inq) { if (work[jj] == '\\' && jj + 1 < wlen) { jj += 2; continue; } if (work[jj] == qc) inq = 0; jj++; continue; }
                if (work[jj] == '"' || work[jj] == '\'') { inq = 1; qc = work[jj]; jj++; continue; }
                if (work[jj] == '(') d++;
                else if (work[jj] == ')') { d--; if (d == 0) break; }
                jj++;
            }
            if (jj >= wlen) continue;
            int is_whole_wrap = 0;
            { char *pre = work; while (*pre == ' ' || *pre == '\t') pre++; const char *aft = work + jj + 1; while (*aft == ' ' || *aft == '\t') aft++; if (pre == work + ii && *aft == '\0') is_whole_wrap = 1; }
            int id = 0, iin = 0; char iqc = 0; int hp = 0, hm = 0;
            for (int kk = ii + 1; kk < jj; kk++) {
                char cc = work[kk];
                if (iin) { if (cc == '\\' && kk + 1 < jj) { kk++; continue; } if (cc == iqc) iin = 0; continue; }
                if (cc == '"' || cc == '\'') { iin = 1; iqc = cc; continue; }
                if (cc == '(' || cc == '{' || cc == '[') id++;
                else if (cc == ')' || cc == '}' || cc == ']') { if (id > 0) id--; }
                else if (cc == '+' && id == 0) hp = 1;
                else if (cc == '*' && id == 0) hm = 1;
            }
            if (!hp && !hm) continue;
            int has_inner = 0;
            for (int xi = ii + 1; xi < jj; xi++) {
                if (work[xi] != '(') continue;
                if (xi + 1 < wlen && work[xi + 1] == '{') continue;
                int xd = 0, xinq = 0; char xqc = 0; int xj = xi;
                while (xj < jj) {
                    if (xinq) { if (work[xj] == '\\' && xj + 1 < jj) { xj += 2; continue; } if (work[xj] == xqc) xinq = 0; xj++; continue; }
                    if (work[xj] == '"' || work[xj] == '\'') { xinq = 1; xqc = work[xj]; xj++; continue; }
                    if (work[xj] == '(') xd++;
                    else if (work[xj] == ')') { xd--; if (xd == 0) break; }
                    xj++;
                }
                if (xj >= jj) continue;
                int xid = 0, xiin = 0; char xiqc = 0; int xhp = 0, xhm = 0;
                for (int xk = xi + 1; xk < xj; xk++) {
                    char xc = work[xk];
                    if (xiin) { if (xc == '\\' && xk + 1 < xj) { xk++; continue; } if (xc == xiqc) xiin = 0; continue; }
                    if (xc == '"' || xc == '\'') { xiin = 1; xiqc = xc; continue; }
                    if (xc == '(' || xc == '{' || xc == '[') xid++;
                    else if (xc == ')' || xc == '}' || xc == ']') { if (xid > 0) xid--; }
                    else if (xc == '+' && xid == 0) xhp = 1;
                    else if (xc == '*' && xid == 0) xhm = 1;
                }
                if (xhp || xhm) { has_inner = 1; break; }
            }
            if (has_inner) continue;
            /* If the whole buffer is a single wrapped group with no inner
               resolvable groups, allow it to be resolved (e.g. when a
               prior pass produced ((result) * N)). */
            if (is_whole_wrap) just_wrapped = 0;
            bs = ii; be = jj; do_plus = hp; do_mul = hm; break;
        }
        if (bs == -1) {
            /* No parenthesized group needs resolving.  If a top-level
               '+' or '*' remains (expression not wrapped in parens), wrap
               the buffer in parentheses and let the next pass resolve it. */
            int wlen = (int)strlen(work);
            int d = 0, inq = 0; char qc = 0; int hp = 0, hm = 0;
            for (int kk = 0; kk < wlen; kk++) {
                char cc = work[kk];
                if (inq) { if (cc == '\\' && kk + 1 < wlen) { kk++; continue; } if (cc == qc) inq = 0; continue; }
                if (cc == '"' || cc == '\'') { inq = 1; qc = cc; continue; }
                if (cc == '(' || cc == '{' || cc == '[') d++; else if (cc == ')' || cc == '}' || cc == ']') { if (d > 0) d--; }
                else if (cc == '+' && d == 0) hp = 1;
                else if (cc == '*' && d == 0) hm = 1;
            }
            if (hp || hm) {
                size_t need = strlen(work) + 3;
                if (need > work_cap) { while (work_cap < need) work_cap *= 2; char *nw = (char*)realloc(work, work_cap); if(!nw){return;} work = nw; }
                memmove(work + 1, work, strlen(work) + 1);
                work[0] = '('; strcat(work, ")");
                just_wrapped = 1;
                continue;
            }
            break;
        }
        int clen = be - bs - 1;
        content[0]=0;
        if (clen >= (int)mid_cap) clen = (int)mid_cap - 1;
        memcpy(content, work + bs + 1, clen);
        content[clen] = 0;

        if (do_plus) {
            int nip = 0; char *ist = content;
            int idp = 0, idb = 0, idbr = 0, iins = 0; char iq2 = 0;
            for (char *pp = content; *pp && nip < 16; pp++) {
                if (iins) { if (*pp == iq2 && pp > content && *(pp - 1) != '\\') iins = 0; continue; }
                if (*pp == '"' || *pp == '\'') { iins = 1; iq2 = *pp; continue; }
                if (*pp == '(') idp++; else if (*pp == ')') { if (idp > 0) idp--; }
                else if (*pp == '[') idb++; else if (*pp == ']') { if (idb > 0) idb--; }
                else if (*pp == '{') idbr++; else if (*pp == '}') { if (idbr > 0) idbr--; }
                else if (*pp == '+' && idp == 0 && idb == 0 && idbr == 0) {
                    *pp = 0; char *s2 = ist; while (*s2 == ' ' || *s2 == '\t') s2++;
                    char *e2 = s2 + strlen(s2) - 1; while (e2 >= s2 && (*e2 == ' ' || *e2 == '\t')) { *e2 = 0; e2--; }
                    if(nip>=parts_cap){int nc=parts_cap*2; char *nb=(char*)realloc(parts_buf,(size_t)nc*part_w); if(nb){parts_buf=nb;parts_cap=nc;}} if(nip<parts_cap){strncpy(parts_buf+nip*(int)part_w,s2,part_w-1); parts_buf[nip*(int)part_w+part_w-1]=0; nip++;} ist = pp + 1;
                }
            }
            { char *s2 = ist; while (*s2 == ' ' || *s2 == '\t') s2++; char *e2 = s2 + strlen(s2) - 1; while (e2 >= s2 && (*e2 == ' ' || *e2 == '\t')) { *e2 = 0; e2--; } if(nip>=parts_cap){int nc=parts_cap*2; char *nb=(char*)realloc(parts_buf,(size_t)nc*part_w); if(nb){parts_buf=nb;parts_cap=nc;}} if(nip<parts_cap){strncpy(parts_buf+nip*(int)part_w,s2,part_w-1); parts_buf[nip*(int)part_w+part_w-1]=0; nip++;} }
            if (nip < 2) break;
            /* Helper logic inline: normalize a part and make it stringy for concat */
            ires[0]=0;
            strncpy(ires, parts_buf, (int)mid_cap - 1); ires[(int)mid_cap - 1] = 0;
            normalize_string_expr(ires, (int)mid_cap, array_access_used, table);
            for (int k = 1; k < nip; k++) {
                char *nse_p = (char*)malloc(small_cap); if(!nse_p) continue;
                strncpy(nse_p, parts_buf + k*(int)part_w, (int)small_cap - 1); nse_p[(int)small_cap - 1] = 0;
                normalize_string_expr(nse_p, (int)small_cap, array_access_used, table);
                /* Build concat step. In array-access context use _ltng_add_sn.
                   Otherwise, if either side is not stringy (e.g. atoi(x) from
                   int var in mixed context, or numeric literal), convert to
                   string form or use _ltng_add_sn. */
                strncpy(lhs, ires, mid_cap-1); lhs[mid_cap-1]=0;
                strncpy(rhs, nse_p, small_cap-1); rhs[small_cap-1]=0;
                /* If a part is atoi/atoll/atof(ident), in string concat we
                   want the original buffer (vars are char buffers). Track whether
                   each side was a numeric conversion wrapper (numeric-typed var). */
                int lhs_was_numwrap = 0, rhs_was_numwrap = 0;
                {
                    char *sides[2] = {lhs, rhs};
                    int slens[2] = {(int)mid_cap, (int)small_cap};
                    int *numwrap[2] = {&lhs_was_numwrap, &rhs_was_numwrap};
                    for (int si = 0; si < 2; si++) {
                        char *sp = sides[si];
                        while (*sp==' '||*sp=='\t') sp++;
                        int pfx = 0;
                        if (strncmp(sp, "atoll(", 6) == 0) pfx = 6;
                        else if (strncmp(sp, "atoi(", 5) == 0) pfx = 5;
                        else if (strncmp(sp, "atof(", 5) == 0) pfx = 5;
                        if (pfx) {
                            const char *inner = sp + pfx;
                            const char *start = inner;
                            while (*inner && (isalnum((unsigned char)*inner) || *inner == '_')) inner++;
                            if (*inner == ')' && *(inner+1) == '\0' && inner > start) {
                                char *tmpb = (char*)malloc(small_cap); if(!tmpb){sides[si][0]=0;} else { int tl = (int)(inner - start);
                                if (tl >= (int)small_cap) tl = (int)small_cap-1;
                                memcpy(tmpb, start, tl); tmpb[tl]=0;
                                strncpy(sides[si], tmpb, slens[si]-1); sides[si][slens[si]-1]=0; free(tmpb); }
                                *numwrap[si] = 1;
                            }
                        }
                    }
                }
                int lhs_s = part_is_stringy(lhs);
                int rhs_s = part_is_stringy(rhs);

                /* Build char* args for concat helpers (_ltng_add_sn/_ltng_concat_dyn take const char*). */
                char *lhs_arg = (char*)malloc(mid_cap+64); char *rhs_arg = (char*)malloc(mid_cap+64);
                if(!lhs_arg||!rhs_arg){free(lhs_arg);free(rhs_arg); free(nse_p); strncpy(ires,work,(int)mid_cap-1); ires[(int)mid_cap-1]=0; continue;}
                #define _LTNG_SN_ARG(src, dst, dstsz, is_str) do { \
                    const char *_ps = (src); while (*_ps==' '||*_ps=='\t') _ps++; \
                    if (is_str) { \
                        strncpy((dst), (src), (dstsz)-1); (dst)[(dstsz)-1]=0; \
                    } else { \
                        const char *_q = _ps; if (*_q=='-'||*_q=='+') _q++; \
                        int _isnum = 0, _hasd = 0, _dot = 0; \
                        if (*_q) { _isnum = 1; while (*_q) { \
                            if (isdigit((unsigned char)*_q)) _hasd=1; \
                            else if (*_q=='.' && !_dot) _dot=1; \
                            else { _isnum=0; break; } _q++; } } \
                        if (_isnum && _hasd) { \
                            snprintf((dst), (dstsz), "\"%s\"", _ps); \
                        } else { \
                            int _hd = (strchr(_ps,'.')!=NULL); \
                            snprintf((dst), (dstsz), \
                                "({ _ltng_retbuf_fmt(\"%s\",(%s)(%s)); })", \
                                _hd ? "%g" : "%lld", _hd ? "double" : "long long", _ps); \
                        } \
                    } \
                } while(0)
                _LTNG_SN_ARG(lhs, lhs_arg, (int)mid_cap+64, lhs_s);
                _LTNG_SN_ARG(rhs, rhs_arg, (int)mid_cap+64, rhs_s);
                #undef _LTNG_SN_ARG

                /* Helper: determine if a stringy bare-identifier part is a string-typed variable. */
                int lhs_is_str_var = 0, rhs_is_str_var = 0;
                if (table) {
                    const char *sides2[2] = {lhs, rhs};
                    int *flags[2] = {&lhs_is_str_var, &rhs_is_str_var};
                    int wraps[2] = {lhs_was_numwrap, rhs_was_numwrap};
                    for (int si = 0; si < 2; si++) {
                        if (wraps[si]) continue;
                        const char *sp = sides2[si];
                        while (*sp==' '||*sp=='\t') sp++;
                        const char *qe = sp;
                        if (*qe=='_' || isalpha((unsigned char)*qe)) {
                            while (*qe && (isalnum((unsigned char)*qe) || *qe=='_')) qe++;
                            const char *end = qe;
                            while (*end==' '||*end=='\t') end++;
                            if (*end == '\0') {
                                char *nm = (char*)malloc(small_cap); if(nm){ int nl = (int)(qe - sp);
                                if (nl >= (int)small_cap) nl = (int)small_cap-1;
                                memcpy(nm, sp, nl); nm[nl] = 0;
                                if (symbol_table_has(table, nm)) {
                                    VarType vt = symbol_table_get(table, nm);
                                    if (vt == TYPE_STRING || vt == TYPE_CHAR) *flags[si] = 1;
                                } else {
                                    char *mn = (char*)malloc(small_cap);
                                    if(mn){ mangle_name(nm, mn, (int)small_cap);
                                    if (symbol_table_has(table, mn)) {
                                        VarType vt = symbol_table_get(table, mn);
                                        if (vt == TYPE_STRING || vt == TYPE_CHAR) *flags[si] = 1;
                                    } free(mn); }
                                }
                                free(nm); }
                            }
                        }
                    }
                }

                char *st = (char*)malloc(mid_cap+64); if(!st) st=(char*)calloc(1,1);
                if (array_access_used) {
                    snprintf(st, mid_cap+64, "_ltng_add_sn(%s, %s)", lhs_arg, rhs_arg);
                } else {
                    int lhs_def_str = lhs_is_str_var || (lhs_s && !lhs_was_numwrap && lhs[0] == '"') || (!lhs_was_numwrap && lhs_s && strstr(lhs, "_ltng_concat") != NULL);
                    int rhs_def_str = rhs_is_str_var || (rhs_s && !rhs_was_numwrap && rhs[0] == '"') || (!rhs_was_numwrap && rhs_s && strstr(rhs, "_ltng_concat") != NULL);
                    int lhs_bare_ident = 0, rhs_bare_ident = 0;
                    {
                        const char *ss[2] = {lhs, rhs}; int *bf[2] = {&lhs_bare_ident, &rhs_bare_ident};
                        for (int si=0; si<2; si++) {
                            const char *sp = ss[si]; while (*sp==' '||*sp=='\t') sp++;
                            if (*sp=='_' || isalpha((unsigned char)*sp)) {
                                const char *qe=sp; while(*qe&&(isalnum((unsigned char)*qe)||*qe=='_')) qe++;
                                const char *end=qe; while(*end==' '||*end=='\t') end++;
                                if (*end=='\0') *bf[si]=1;
                            }
                        }
                    }
                    if (lhs_s && rhs_s && lhs_def_str && rhs_def_str)
                        snprintf(st, (int)mid_cap+64, "_ltng_concat_dyn(2, %s, %s)", lhs_arg, rhs_arg);
                    else if (lhs_s && rhs_s && lhs_bare_ident && rhs_bare_ident && !lhs_was_numwrap && !rhs_was_numwrap && !lhs_is_str_var && !rhs_is_str_var) {
                        int lhs_num = 0, rhs_num = 0;
                        if (table) {
                            const char *ss[2] = {lhs, rhs}; int *nf[2] = {&lhs_num, &rhs_num};
                            for (int si=0; si<2; si++) {
                                const char *sp = ss[si]; while (*sp==' '||*sp=='\t') sp++;
                                char *nm = (char*)malloc(small_cap); if(nm){ int nl=0; while(sp[nl]&&(isalnum((unsigned char)sp[nl])||sp[nl]=='_')){nm[nl]=sp[nl];nl++; if(nl>=(int)small_cap-1)break;} nm[nl]=0;
                                if (symbol_table_has(table, nm)) { VarType vt=symbol_table_get(table,nm); if(vt==TYPE_INT||vt==TYPE_FLOAT||vt==TYPE_BOOL) *nf[si]=1; }
                                else { char *mn = (char*)malloc(small_cap); if(mn){ mangle_name(nm,mn,(int)small_cap); if(symbol_table_has(table,mn)){VarType vt=symbol_table_get(table,mn); if(vt==TYPE_INT||vt==TYPE_FLOAT||vt==TYPE_BOOL)*nf[si]=1;} free(mn); } }
                                free(nm); }
                            }
                        }
                        if (lhs_num || rhs_num)
                            snprintf(st, (int)mid_cap+64, "_ltng_add_sn(%s, %s)", lhs_arg, rhs_arg);
                        else
                            snprintf(st, (int)mid_cap+64, "_ltng_concat_dyn(2, %s, %s)", lhs_arg, rhs_arg);
                    } else if (lhs_s && rhs_s && lhs_def_str && !rhs_def_str)
                        snprintf(st, (int)mid_cap+64, "_ltng_concat_dyn(2, %s, %s)", lhs_arg, rhs_arg);
                    else if (lhs_s && rhs_s && !lhs_def_str && rhs_def_str)
                        snprintf(st, (int)mid_cap+64, "_ltng_concat_dyn(2, %s, %s)", lhs_arg, rhs_arg);
                    else
                        snprintf(st, (int)mid_cap+64, "_ltng_add_sn(%s, %s)", lhs_arg, rhs_arg);
                }
                free(lhs_arg); free(rhs_arg);
                strncpy(ires, st, (int)mid_cap - 1); ires[(int)mid_cap - 1] = 0;
                free(st);
                free(nse_p);
            }
            size_t nb_cap = strlen(work) + strlen(ires) + 64;
            char *nb = (char*)malloc(nb_cap); if(!nb) nb=(char*)calloc(1,1);
            snprintf(nb, nb_cap, "%.*s%s%s", bs, work, ires, work + be + 1);
            strncpy(work, nb, (int)work_cap - 1); work[(int)work_cap - 1] = 0;
            free(nb);
        } else if (do_mul) {
            int d = 0, inq = 0; char qc = 0; int star = -1;
            for (int kk = 0; kk < clen; kk++) {
                char cc = content[kk];
                if (inq) { if (cc == '\\' && kk + 1 < clen) { kk++; continue; } if (cc == qc) inq = 0; continue; }
                if (cc == '"' || cc == '\'') { inq = 1; qc = cc; continue; }
                if (cc == '(') d++; else if (cc == ')') { if (d > 0) d--; }
                else if (cc == '[') d++; else if (cc == ']') { if (d > 0) d--; }
                else if (cc == '{') d++; else if (cc == '}') { if (d > 0) d--; }
                else if (cc == '*' && d == 0) {
                    const char *pp2 = content + kk + 1;
                    while (*pp2 == ' ' || *pp2 == '\t') pp2++;
                    if (*pp2 == '*' || *pp2 == ')' || *pp2 == '\0') continue;
                    star = kk; break;
                }
            }
            if (star < 0) break;
            nse_left[0]=0; nse_right[0]=0;
            int llen = star; if (llen >= (int)mid_cap) llen = (int)mid_cap - 1;
            memcpy(nse_left, content, llen); nse_left[llen] = 0;
            int rlen = clen - star - 1; if (rlen >= (int)small_cap) rlen = (int)small_cap - 1;
            memcpy(nse_right, content + star + 1, rlen); nse_right[rlen] = 0;
            { char *s = nse_left; while (*s == ' ' || *s == '\t') s++; if (s != nse_left) memmove(nse_left, s, strlen(s) + 1); }
            { char *e = nse_left + strlen(nse_left) - 1; while (e >= nse_left && (*e == ' ' || *e == '\t')) { *e = 0; e--; } }
            { char *s = nse_right; while (*s == ' ' || *s == '\t') s++; if (s != nse_right) memmove(nse_right, s, strlen(s) + 1); }
            { char *e = nse_right + strlen(nse_right) - 1; while (e >= nse_right && (*e == ' ' || *e == '\t')) { *e = 0; e--; } }
            if (nse_left[0] == '\0' || nse_right[0] == '\0') break;
            /* Determine numeric/string roles for repeat(count vs string). */
            int right_is_num = 1;
            {
                const char *rsp = nse_right;
                if (*rsp == '-') rsp++;
                if (*rsp == 0) right_is_num = 0;
                else { int hd = 0; for (; *rsp; rsp++) { if (isdigit((unsigned char)*rsp)) hd = 1; else if (*rsp == '.') {} else { right_is_num = 0; break; } } if (!hd) right_is_num = 0; }
            }
            int left_is_num = 1;
            {
                const char *lsp = nse_left;
                while (*lsp == ' ' || *lsp == '\t') lsp++;
                if (*lsp == '-') lsp++;
                if (*lsp == 0) left_is_num = 0;
                else { int hd = 0; for (; *lsp; lsp++) { if (isdigit((unsigned char)*lsp)) hd = 1; else if (*lsp == '.') {} else { left_is_num = 0; break; } } if (!hd) left_is_num = 0; }
            }
            str_operand[0]=0;
            char *countbuf = NULL;
            if (left_is_num && !right_is_num) {
                /* number * "string" (or number * string-expr) */
                strncpy(str_operand, nse_right, (int)mid_cap-1); str_operand[(int)mid_cap-1]=0;
                normalize_string_expr(str_operand, (int)mid_cap, array_access_used, table);
                countbuf = _ltng_cfmt("%s", nse_left);
            } else if (right_is_num) {
                strncpy(str_operand, nse_left, (int)mid_cap-1); str_operand[(int)mid_cap-1]=0;
                normalize_string_expr(str_operand, (int)mid_cap, array_access_used, table);
                countbuf = _ltng_cfmt("%s", nse_right);
            } else {
                /* variable * variable: assume nse_left is string, nse_right is count */
                strncpy(str_operand, nse_left, (int)mid_cap-1); str_operand[(int)mid_cap-1]=0;
                normalize_string_expr(str_operand, (int)mid_cap, array_access_used, table);
                countbuf = _ltng_cfmt("atoll(%s)", nse_right);
            }
            char *rep = _ltng_cfmt("_ltng_repeat_dyn(%s,(long long)(%s))", str_operand, countbuf ? countbuf : "0");
            char *nb = _ltng_cfmt("%.*s%s%s", bs, work, rep, work + be + 1);
            strncpy(work, nb, (int)work_cap - 1); work[(int)work_cap - 1] = 0;
            free(countbuf); free(rep); free(nb);
        }
    }

    strncpy(buf, work, buf_size - 1);
    buf[buf_size - 1] = 0;
    free(content); free(ires); free(lhs); free(rhs); free(str_operand); free(nse_left); free(nse_right); free(parts_buf);
    free(work);
}

/* Post-process: replace atof(X) + atof(Y), atof(X) + _ltng_add_sn(Y, Z),
   _ltng_add_sn(X, Y) + atof(Z), etc. with _ltng_add_sn(...) for runtime
   string/number detection on buffer variables (e.g. function params typed
   as float). We only replace atof() (float-typed), never atoi() (int-typed). */
int is_add_operand(const char *p, char *content, int content_size) {
    /* Try atof(X) */
    if (strncmp(p, "atof(", 5) == 0) {
        const char *op = p + 5;
        const char *start = op;
        while (*op && (isalnum((unsigned char)*op) || *op == '_')) op++;
        int len = (int)(op - start);
        if (*op == ')' && len > 0 && len < content_size - 1) {
            memcpy(content, start, len);
            content[len] = '\0';
            return 1; /* single ident */
        }
        return 0;
    }
    /* Try _ltng_add_sn(X, Y) */
    if (strncmp(p, "_ltng_add_sn(", 13) == 0) {
        const char *op = p + 13;
        const char *start1 = op;
        while (*op && (isalnum((unsigned char)*op) || *op == '_')) op++;
        int len1 = (int)(op - start1);
        if (*op != ',' || len1 == 0 || len1 >= content_size - 1) return 0;
        op++; /* skip , */
        while (*op == ' ' || *op == '\t') op++;
        const char *start2 = op;
        while (*op && (isalnum((unsigned char)*op) || *op == '_')) op++;
        int len2 = (int)(op - start2);
        if (*op != ')' || len2 == 0 || len2 >= content_size - 1) return 0;
        /* Build content: _ltng_add_sn(X, Y) */
        int n = snprintf(content, content_size, "_ltng_add_sn(%.*s, %.*s)",
                         len1, start1, len2, start2);
        return (n > 0 && n < content_size);
    }
    return 0;
}

int optimize_add_concat(char *out, int out_size, int pos) {
    (void)pos;
    int cap = out_size > 0 ? out_size : (int)strlen(out) + 64;
    if (cap < 4096) cap = 4096;
    char *result = (char*)calloc(1, (size_t)cap);
    char *left = (char*)malloc((size_t)cap);
    char *right = (char*)malloc((size_t)cap);
    if(!result||!left||!right){free(result);free(left);free(right);return 0;}
    const char *src = out;
    char *dst = result;
    int replaced = 0;

    while (*src && (int)(dst - result) < cap - 1) {
        left[0]=0; right[0]=0;

        if (is_add_operand(src, left, cap)) {
            const char *after_left = src;
            while (*after_left && (isalnum((unsigned char)*after_left) || *after_left == '_' ||
                                   *after_left == '(' || *after_left == ')' || *after_left == ',' ||
                                   *after_left == ' ' || *after_left == '\t')) after_left++;
            while (*after_left == ' ' || *after_left == '\t') after_left++;
            if (*after_left == '+') {
                const char *after_plus = after_left + 1;
                while (*after_plus == ' ' || *after_plus == '\t') after_plus++;
                if (is_add_operand(after_plus, right, cap)) {
                    const char *after_right = after_plus;
                    while (*after_right && (isalnum((unsigned char)*after_right) || *after_right == '_' ||
                                            *after_right == '(' || *after_right == ')' ||
                                            *after_right == ',' || *after_right == ' ' ||
                                            *after_right == '\t')) after_right++;
                    while (*after_right == ' ' || *after_right == '\t') after_right++;
                    /* Skip replacement if followed by another operator that would change precedence/meaning.
                       Note: ')' can close a grouping around the addition, so allow replacement. */
                    int next_op = (*after_right == '*' || *after_right == '/' || *after_right == '%' ||
                                   *after_right == '+' || *after_right == '-' || *after_right == '^' ||
                                   *after_right == '&' || *after_right == '|' || *after_right == '<' ||
                                   *after_right == '>' || *after_right == '!' || *after_right == '=');
                    if (!next_op) {
                        int n = snprintf(dst, cap - (int)(dst - result),
                                         "_ltng_add_sn(%s, %s)", left, right);
                        if (n < 0) n = 0;
                        dst += n;
                        src = after_right;
                        replaced = 1;
                        continue;
                    }
                }
            }
        }

        *dst++ = *src++;
    }
    *dst = '\0';

    if (replaced) {
        int newlen = (int)strlen(result);
        if (newlen < out_size) {
            memcpy(out, result, newlen + 1);
        }
        free(result); free(left); free(right);
        return 1;
    }
    free(result); free(left); free(right);
    return 0;
}

const char *type_name_str(VarType t);

int function_returns_string(const char *fname);

int part_is_stringy(const char *part) {
    const char *q = part;
    while (*q == ' ' || *q == '\t') q++;
    /* Strip one layer of balanced outer parentheses to inspect inner expression. */
    if (*q == '(' && strncmp(q, "({", 2) != 0) {
        int depth = 0, in_str = 0; char sq = 0;
        const char *end = NULL;
        for (const char *p = q; *p; p++) {
            if (in_str) {
                if (*p == '\\' && p[1]) { p++; continue; }
                if (*p == sq) in_str = 0;
                continue;
            }
            if (*p == '"' || *p == '\'') { in_str = 1; sq = *p; continue; }
            if (*p == '(') depth++;
            else if (*p == ')') {
                depth--;
                if (depth == 0) { end = p; break; }
            }
        }
        if (end) {
            const char *after = end + 1;
            while (*after == ' ' || *after == '\t') after++;
            if (*after == '\0') {
                /* whole part is wrapped in (...) - analyze inside */
                size_t ilen = (size_t)(end - (q + 1));
                char *inner = (char*)malloc(ilen + 1); if(!inner) return 0;
                memcpy(inner, q + 1, ilen);
                inner[ilen] = '\0';
                /* Trim */
                char *is = inner; while (*is == ' ' || *is == '\t') is++;
                char *ie = is + strlen(is);
                while (ie > is && (ie[-1] == ' ' || ie[-1] == '\t')) { ie--; *ie = '\0'; }
                /* If inner has top-level '+' or known string helpers, treat as stringy */
                {
                    int d = 0, in = 0; char qc = 0, has_plus = 0, has_star = 0;
                    for (const char *p = is; *p; p++) {
                        if (in) { if (*p == '\\' && p[1]) { p++; continue; } if (*p == qc) in = 0; continue; }
                        if (*p == '"' || *p == '\'') { in = 1; qc = *p; continue; }
                        if (*p == '(' || *p == '{' || *p == '[') d++;
                        else if (*p == ')' || *p == '}' || *p == ']') { if (d > 0) d--; }
                        else if (*p == '+' && d == 0) { has_plus = 1; }
                        else if (*p == '*' && d == 0) { has_star = 1; }
                    }
                    if (has_plus || has_star) {
                        /* Definitely stringy if it has string literals or string helpers */
                        if (strchr(is, '"') || strstr(is, "_ltng_concat") != NULL ||
                            strstr(is, "_ltng_repeat") != NULL || strstr(is, "_ltng_as(") != NULL)
                            { free(inner); return 1; }
                        /* _ltng_add_sn can appear in mixed contexts; if also has string indicators treat as stringy */
                        if (strstr(is, "_ltng_add_sn") != NULL) {
                            /* could be numeric runtime add or string concat - treat as stringy for concat processing */
                            { free(inner); return 1; }
                        }
                        /* If it looks like a pure numeric arithmetic expression (numbers, atoll/atof/atoi
                           conversions, arithmetic ops, parens, casts), treat as NOT stringy so it gets
                           stringified as a numeric value in concat context. */
                        int looks_numeric = 1;
                        /* Check for bare identifiers that are NOT inside atoll/atof/atoi - these might be
                           string vars. Identifiers inside numeric conversion calls are fine. */
                        {
                            const char *pp = is;
                            while (*pp) {
                                if (isalpha((unsigned char)*pp) || *pp == '_') {
                                    const char *start = pp;
                                    while (*pp && (isalnum((unsigned char)*pp) || *pp == '_')) pp++;
                                    int len = (int)(pp - start);
                                    /* Allow known numeric conversion/cast tokens and common numeric helpers */
                                    if ((len == 5 && strncmp(start, "atoll", 5) == 0) ||
                                        (len == 4 && strncmp(start, "atof", 4) == 0) ||
                                        (len == 4 && strncmp(start, "atoi", 4) == 0) ||
                                        (len == 4 && strncmp(start, "long", 4) == 0) ||
                                        (len == 6 && strncmp(start, "double", 6) == 0) ||
                                        (len == 3 && strncmp(start, "int", 3) == 0) ||
                                        (len == 4 && strncmp(start, "char", 4) == 0) ||
                                        (len == 8 && strncmp(start, "_LTNG_", 6) == 0) ||
                                        (len > 6 && strncmp(start, "_LTNG_", 6) == 0) ||
                                        (len > 5 && strncmp(start, "_ltng", 5) == 0 && strstr(start, "num") == NULL)) {
                                        /* _ltng numeric helpers are fine; string helpers already caught above */
                                        continue;
                                    }
                                    /* Check if this identifier is the argument of an immediately preceding
                                       atoll(/atof(/atoi( - if so, it's a numeric use. */
                                    int inside_numeric_conv = 0;
                                    if (start > is && *(start - 1) == '(') {
                                        if (start - is >= 6 && strncmp(start - 6, "atoll(", 6) == 0) inside_numeric_conv = 1;
                                        else if (start - is >= 5 && strncmp(start - 5, "atof(", 5) == 0) inside_numeric_conv = 1;
                                        else if (start - is >= 5 && strncmp(start - 5, "atoi(", 5) == 0) inside_numeric_conv = 1;
                                    }
                                    if (inside_numeric_conv) {
                                        continue;
                                    }
                                    /* Bare identifier (variable name) - could be string var; treat as stringy
                                       to be safe unless we have more info. */
                                    looks_numeric = 0;
                                    break;
                                } else {
                                    pp++;
                                }
                            }
                        }
                        if (looks_numeric) { free(inner); return 0; } /* numeric expression, not stringy */
                        { free(inner); return 1; }
                    }
                }
                if (strstr(is, "_ltng_add_sn") != NULL || strstr(is, "_ltng_concat") != NULL)
                    { free(inner); return 1; }
                { int __ps = part_is_stringy(is); free(inner); return __ps; }
            }
        }
    }

    if (*q == '"') return 1;
    if (strncmp(q, "({", 2) == 0) {
        if (strstr(part, "_ltng_repeat") != NULL || strstr(part, "_ltng_arr") != NULL ||
            strstr(part, "_ltng_add_sn") != NULL || strstr(part, "_ltng_concat") != NULL)
            return 1;
        /* statement expressions returning string results (e.g. variadic call builders) */
        if (strstr(part, "_ltng_retbuf") != NULL || strstr(part, "_ltng_arr_free") != NULL)
            return 1;
        return 0;
    }
    if (*q == '_' || isalpha((unsigned char)*q)) {
        const char *e = q;
        while (*e && (isalnum((unsigned char)*e) || *e == '_')) e++;
        if (*e == '(') {
            /* Function call: decide by the callee's return type. */
            int cl = (int)(e - q);
            char *callee = (char*)malloc((size_t)cl + 1);
            if (!callee) return 0;
            memcpy(callee, q, (size_t)cl);
            callee[cl] = '\0';
            if (strcmp(callee, "_ltng_add_sn") == 0 || strcmp(callee, "_ltng_concat") == 0 ||
                strcmp(callee, "_ltng_repeat") == 0 || strcmp(callee, "_ltng_as") == 0 ||
                strcmp(callee, "_ltng_ad") == 0 || strcmp(callee, "_ltng_retbuf") == 0 ||
                strcmp(callee, "__ltng_call_indirect") == 0) {
                free(callee);
                return 1;
            }
            int frs = function_returns_string(callee);
            free(callee);
            return frs;
        }
        /* simple identifier: treat as stringy (variables stored as char buffers) */
        const char *end = e;
        while (*end == ' ' || *end == '\t') end++;
        if (*end == '\0') return 1;
        /* identifier followed by '*' is a string repeat (stringy) */
        if (*end == '*') return 1;
    }
    /* contains string literal -> likely string expression */
    if (strchr(q, '"') != NULL) return 1;
    /* contains top-level '+' or '*' -> likely a string concat/repeat expression
       (e.g. (a+b)*2 in a larger concat context); treat as stringy so the
       normalizer can process it rather than misclassifying as numeric. */
    {
        int d = 0, in = 0; char qc = 0;
        for (const char *p = q; *p; p++) {
            if (in) { if (*p == '\\' && p[1]) { p++; continue; } if (*p == qc) in = 0; continue; }
            if (*p == '"' || *p == '\'') { in = 1; qc = *p; continue; }
            if (*p == '(' || *p == '{' || *p == '[') d++;
            else if (*p == ')' || *p == '}' || *p == ']') { if (d > 0) d--; }
            else if ((*p == '+' || *p == '*') && d == 0) return 1;
        }
    }
    return 0;
}

void token_append_source(char *out, int *pos, int out_size, Token tok);

/* Find a function AST node by its short source name or mangled C name. */
ASTNode *find_function_ast_by_name(const char *name);
ASTNode *find_function_node_by_cname(const char *cname);
int resolve_func_ref_info(SymbolTable *table, const char *name, char *out_cname, int out_size, int *out_pc);

/* Infer the return type of a user-defined function by scanning its return statements. */
VarType infer_function_return_type(const char *fname);
VarType infer_call_result_type(ASTNode *call);

/* Return 1 when a call to the named function is known to produce a string
   value (explicit `-> string` annotation, or inferred from its returns). */
int function_returns_string(const char *fname);
VarType collect_expression(Lexer *lexer, SymbolTable *table,
                                  char *out, int out_size,
                                  int stop_on_rparen, int stop_on_comma,
                                  int *out_sl, int *out_sc, int *out_el, int *out_ec);
VarType collect_expression_ex(Lexer *lexer, SymbolTable *table,
                                     char *out, int out_size,
                                     int stop_on_rparen, int stop_on_comma,
                                     int *out_sl, int *out_sc, int *out_el, int *out_ec,
                                     int stop_on_ternary_colon);

