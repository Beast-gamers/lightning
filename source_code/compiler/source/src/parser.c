#include "ltng.h"
#include <stdarg.h>
#include <stdarg.h>

static ErrorState *parser_errors = NULL;
static int g_break_depth = 0;
static ASTNode *g_list = NULL;
static int g_stmt_line = 0;
static int g_stmt_col = 0;
static int g_expr_sl=0,g_expr_sc=0,g_expr_el=0,g_expr_ec=0;

static char *_ltng_cfmt(const char *fmt, ...) {
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

#define MAX_FUNCS 256
#define MAX_FUNC_PARAMS 32
typedef struct {
    char name[MAX_IDENTIFIER_LEN];   /* emitted C name */
    char src[MAX_IDENTIFIER_LEN];    /* source (unmangled) name */
    int param_count;                 /* fixed parameter count (excludes ...rest) */
    int required_count;              /* fixed parameters without defaults */
    int is_variadic;                 /* trailing ...rest parameter present */
    int has_return_type;             /* explicit -> type annotation */
    VarType return_type;             /* annotated return type */
    char return_obj_base[MAX_IDENTIFIER_LEN]; /* schema source for object returns */
    int line, col;                   /* source position of the function name */
    int defined;                     /* body has been parsed */
    int top_level;                   /* signature discovered by the top-level prescan */
    char param_names[MAX_FUNC_PARAMS][MAX_IDENTIFIER_LEN];
    int has_default[MAX_FUNC_PARAMS];
    VarType def_type[MAX_FUNC_PARAMS]; /* inferred type of default expression */
    int def_start[MAX_FUNC_PARAMS];  /* token range of the default expression */
    int def_end[MAX_FUNC_PARAMS];
    int passthrough_param; /* index of param returned as simple pass-through, or -1 */
} FuncInfo;
static FuncInfo g_funcs[MAX_FUNCS];
static int g_func_count = 0;
static int g_in_function = 0;
static int g_in_catch = 0;
static Program *g_program_ref = NULL;
static char g_enclosing_cname[MAX_IDENTIFIER_LEN] = "";
static int g_ret_obj_count = 0;
static int g_anon_count = 0;

#define MAX_OBJS 128
static ObjSchema g_objs[MAX_OBJS];
static int g_obj_count = 0;

void obj_register(const char *mangled_var, ObjSchema *schema) {
    if (!mangled_var || !schema) return;
    for (int i = 0; i < g_obj_count; i++) {
        if (strcmp(g_objs[i].var, mangled_var) == 0) {
            g_objs[i] = *schema;
            strncpy(g_objs[i].var, mangled_var, MAX_IDENTIFIER_LEN - 1);
            g_objs[i].var[MAX_IDENTIFIER_LEN - 1] = '\0';
            return;
        }
    }
    if (g_obj_count < MAX_OBJS) {
        g_objs[g_obj_count] = *schema;
        strncpy(g_objs[g_obj_count].var, mangled_var, MAX_IDENTIFIER_LEN - 1);
        g_objs[g_obj_count].var[MAX_IDENTIFIER_LEN - 1] = '\0';
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
    char fm[MAX_IDENTIFIER_LEN];
    mangle_name(field, fm, MAX_IDENTIFIER_LEN);
    snprintf(out, out_size, "%s__%s", mangled_var, fm);
    return out;
}

static void obj_build_all(ObjSchema *s) {
    /* "obj" field = all values in order, space-separated */
    char buf[MAX_STRING_LEN];
    buf[0] = 0;
    for (int i = 0; i < s->count; i++) {
        if (i > 0) strncat(buf, " ", sizeof(buf)-strlen(buf)-1);
        strncat(buf, s->fields[i].value, sizeof(buf)-strlen(buf)-1);
    }
    /* store as a synthesized field named "obj" if not present, else update */
    int found = 0;
    for (int i = 0; i < s->count; i++) {
        if (strcmp(s->fields[i].name, "obj") == 0) {
            strncpy(s->fields[i].value, buf, MAX_STRING_LEN-1);
            found = 1; break;
        }
    }
    if (!found && s->count < MAX_OBJ_FIELDS) {
        strncpy(s->fields[s->count].name, "obj", MAX_IDENTIFIER_LEN-1);
        strncpy(s->fields[s->count].value, buf, MAX_STRING_LEN-1);
        s->fields[s->count].type = TYPE_STRING;
        s->count++;
    }
    /* default_val = field "default" if present, else "object" */
    strncpy(s->default_val, "object", MAX_STRING_LEN-1);
    for (int i = 0; i < s->count; i++) {
        if (strcmp(s->fields[i].name, "default") == 0) {
            strncpy(s->default_val, s->fields[i].value, MAX_STRING_LEN-1);
            break;
        }
    }
}

static void register_function(const char *name, int param_count) {
    if (g_func_count >= MAX_FUNCS) return;
    char mangled[MAX_IDENTIFIER_LEN];
    mangle_name(name, mangled, MAX_IDENTIFIER_LEN);
    /* avoid duplicates by source name */
    for (int i = 0; i < g_func_count; i++) {
        if (strcmp(g_funcs[i].src, name) == 0) {
            if (param_count > 0) g_funcs[i].param_count = param_count;
            return;
        }
    }
    g_funcs[g_func_count].passthrough_param = -1;
    strncpy(g_funcs[g_func_count].name, mangled, MAX_IDENTIFIER_LEN - 1);
    g_funcs[g_func_count].name[MAX_IDENTIFIER_LEN - 1] = '\0';
    strncpy(g_funcs[g_func_count].src, name, MAX_IDENTIFIER_LEN - 1);
    g_funcs[g_func_count].src[MAX_IDENTIFIER_LEN - 1] = '\0';
    g_funcs[g_func_count].param_count = param_count;
    g_funcs[g_func_count].required_count = param_count;
    g_funcs[g_func_count].is_variadic = 0;
    g_funcs[g_func_count].has_return_type = 0;
    g_funcs[g_func_count].return_type = TYPE_STRING;
    g_funcs[g_func_count].return_obj_base[0] = '\0';
    g_funcs[g_func_count].line = 0;
    g_funcs[g_func_count].col = 0;
    g_funcs[g_func_count].defined = 0;
    g_func_count++;
}

static int is_function_name(const char *name) {
    char mangled[MAX_IDENTIFIER_LEN];
    mangle_name(name, mangled, MAX_IDENTIFIER_LEN);
    for (int i = 0; i < g_func_count; i++) {
        if (strcmp(g_funcs[i].name, mangled) == 0 ||
            strcmp(g_funcs[i].src, name) == 0) return 1;
    }
    return 0;
}

void symbol_table_add_func(SymbolTable *table, const char *src, const char *cname, int param_count) {
    if (!table || table->func_count >= MAX_FUNCBINDS) return;
    for (int i = 0; i < table->func_count; i++) {
        if (strcmp(table->funcs[i].src, src) == 0) {
            strncpy(table->funcs[i].cname, cname, MAX_IDENTIFIER_LEN - 1);
            table->funcs[i].param_count = param_count;
            return;
        }
    }
    FuncBind *b = &table->funcs[table->func_count++];
    strncpy(b->src, src, MAX_IDENTIFIER_LEN - 1);
    b->src[MAX_IDENTIFIER_LEN - 1] = 0;
    strncpy(b->cname, cname, MAX_IDENTIFIER_LEN - 1);
    b->cname[MAX_IDENTIFIER_LEN - 1] = 0;
    b->param_count = param_count;
}

const char *resolve_func_name(SymbolTable *table, const char *src, char *out, int out_size) {
    for (Scope *s = table; s; s = s->parent) {
        for (int i = 0; i < s->func_count; i++) {
            if (strcmp(s->funcs[i].src, src) == 0) {
                strncpy(out, s->funcs[i].cname, out_size - 1);
                out[out_size - 1] = 0;
                return out;
            }
        }
    }
    /* fallback: global short name -> mangled */
    char mangled[MAX_IDENTIFIER_LEN];
    mangle_name(src, mangled, MAX_IDENTIFIER_LEN);
    for (int i = 0; i < g_func_count; i++) {
        if (strcmp(g_funcs[i].name, mangled) == 0 || strcmp(g_funcs[i].src, src) == 0) {
            strncpy(out, g_funcs[i].name, out_size - 1);
            out[out_size - 1] = 0;
            return out;
        }
    }
    return NULL;
}

int is_function_name_scoped(SymbolTable *table, const char *src) {
    char tmp[MAX_IDENTIFIER_LEN];
    return resolve_func_name(table, src, tmp, sizeof(tmp)) != NULL;
}

void symbol_table_set_func_ref(SymbolTable *table, const char *name, const char *func_cname, int param_count);
int symbol_table_get_func_ref(SymbolTable *table, const char *name, char *out_cname, int out_size, int *out_param_count);
static ASTNode *parse_function(Lexer *lexer, SymbolTable *table, const char *enclosing_cname);


static int is_callable_ident(SymbolTable *table, const char *src) {
    if (is_function_name_scoped(table, src)) return 1;
    if (symbol_table_get_func_ref(table, src, NULL, 0, NULL)) return 1;
    {
        char mangled[MAX_IDENTIFIER_LEN];
        mangle_name(src, mangled, sizeof(mangled));
        if (symbol_table_has(table, mangled) || symbol_table_has(table, src)) return 1;
    }
    return 0;
}
static ASTNode *parse_block(Lexer *lexer, SymbolTable *table, int is_control);
static ASTNode *parse_try(Lexer *lexer, SymbolTable *table);
static void parse_throw(Lexer *lexer, SymbolTable *table);

static ASTNode *reverse_list(ASTNode *head) {
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

static void emit(ASTNode *node) {
    if (!node) return;
    if (node->line == 0 && g_stmt_line > 0) { node->line = g_stmt_line; node->col = g_stmt_col; }
    node->next = g_list;
    g_list = node;
}

static ASTNode *create_node(NodeType type, const char *name, const char *value) {
    ASTNode *node = (ASTNode *)malloc(sizeof(ASTNode));
    if (!node) {
        fprintf(stderr, "Error: Memory allocation failed\n");
        exit(1);
    }
    memset(node, 0, sizeof(ASTNode));
    node->passthrough_param = -1;
    node->type = type;
    strncpy(node->name, name ? name : "", MAX_IDENTIFIER_LEN - 1);
    node->name[MAX_IDENTIFIER_LEN - 1] = '\0';
    strncpy(node->value, value ? value : "", MAX_STRING_LEN - 1);
    node->value[MAX_STRING_LEN - 1] = '\0';
    node->var_type = TYPE_STRING;
    node->array_elem_type = TYPE_NULL;
    return node;
}

static int is_bool_value(const char *value) {
    return strcmp(value, "true") == 0 || strcmp(value, "t") == 0 ||
           strcmp(value, "false") == 0 || strcmp(value, "f") == 0;
}

static const char *bool_to_str(const char *val) {
    if (strcmp(val, "true") == 0 || strcmp(val, "t") == 0) return "true";
    return "false";
}

static VarType detect_type(const char *value) {
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

static int is_type_keyword(const char *name) {
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
static int is_return_type_keyword(const char *name) {
    return is_type_keyword(name) || strcmp(name, "void") == 0;
}

/* Return 1 for type conversion functions/calls: toint/int, tofloat/float,
   tostring/string, tobool/bool, tochar/char. */
static int is_type_conversion(const char *name) {
    return strcmp(name, "toint") == 0 || strcmp(name, "int") == 0 ||
           strcmp(name, "tostring") == 0 || strcmp(name, "string") == 0 ||
           strcmp(name, "tobool") == 0 || strcmp(name, "bool") == 0 ||
           strcmp(name, "tofloat") == 0 || strcmp(name, "float") == 0 ||
           strcmp(name, "tochar") == 0 || strcmp(name, "char") == 0;
}

static int is_ltng_reserved(const char *name) {
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
static int is_ltng_keyword(const char *name) {
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
static int token_is_name(Token t) {
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
    if (is_c_reserved(in)) {
        snprintf(out, out_size, "%s_res", in);
    } else {
        strncpy(out, in, out_size - 1);
        out[out_size - 1] = '\0';
    }
}

static VarType type_keyword_to_vartype(const char *name) {
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

static int g_scope_var_uid = 0;

static void symbol_table_add_in_ex(SymbolTable *table, const char *name, VarType type, int unique_cname) {
    if (!table || !name) return;
    for (int i = 0; i < table->count; i++) {
        if (strcmp(table->vars[i].name, name) == 0) {
            table->vars[i].type = type;
            table->vars[i].is_func_ref = 0;
            table->vars[i].func_ref_cname[0] = '\0';
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
        table->vars = nv;
        table->vars_cap = nc;
    }
    VarEntry *ve = &table->vars[table->count];
    memset(ve, 0, sizeof(*ve));
    strncpy(ve->name, name, MAX_IDENTIFIER_LEN - 1);
    ve->name[MAX_IDENTIFIER_LEN - 1] = '\0';
    if (unique_cname) {
        snprintf(ve->cname, MAX_IDENTIFIER_LEN, "_v%d_%s", g_scope_var_uid++, name);
    } else {
        strncpy(ve->cname, name, MAX_IDENTIFIER_LEN - 1);
        ve->cname[MAX_IDENTIFIER_LEN - 1] = '\0';
    }
    ve->type = type;
    ve->is_const = 0;
    ve->is_locked = 0;
    ve->is_param = 0;
    ve->array_dim_count = 0;
    ve->array_dims = NULL;
    ve->array_elem_type = TYPE_NULL;
    ve->is_func_ref = 0;
    ve->func_ref_cname[0] = '\0';
    ve->func_ref_param_count = -1;
    table->count++;
}

static int symbol_table_has_in_parents(SymbolTable *table, const char *name) {
    for (Scope *s = table ? table->parent : NULL; s; s = s->parent) {
        for (int i = 0; i < s->count; i++) {
            if (strcmp(s->vars[i].name, name) == 0) return 1;
        }
    }
    return 0;
}

static void symbol_table_add_in(SymbolTable *table, const char *name, VarType type) {
    /* Use unique C names only for declarations that shadow an outer variable. */
    int unique = (table && table->parent != NULL && symbol_table_has_in_parents(table, name));
    symbol_table_add_in_ex(table, name, type, unique);
}

/* Add/update in current scope */
static void symbol_table_add(SymbolTable *table, const char *name, VarType type) {
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
                s->vars[i].func_ref_cname[0] = '\0';
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
                strncpy(s->vars[i].func_ref_cname, func_cname, MAX_IDENTIFIER_LEN - 1);
                s->vars[i].func_ref_cname[MAX_IDENTIFIER_LEN - 1] = '\0';
                s->vars[i].func_ref_param_count = param_count;
                return;
            }
        }
    }
}

int symbol_table_get_func_ref(SymbolTable *table, const char *name, char *out_cname, int out_size, int *out_param_count) {
    char mangled[MAX_IDENTIFIER_LEN];
    mangle_name(name, mangled, sizeof(mangled));
    for (Scope *s = table; s; s = s->parent) {
        for (int i = 0; i < s->count; i++) {
            if (strcmp(s->vars[i].name, name) == 0 || strcmp(s->vars[i].name, mangled) == 0) {
                if (!s->vars[i].is_func_ref) return 0;
                if (out_cname && out_size > 0) {
                    strncpy(out_cname, s->vars[i].func_ref_cname, out_size - 1);
                    out_cname[out_size - 1] = '\0';
                }
                if (out_param_count) *out_param_count = s->vars[i].func_ref_param_count;
                return 1;
            }
        }
    }
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

static void skip_to_newline(Lexer *lexer) {
    while (lexer->current < lexer->count) {
        Token t = lexer_peek(lexer);
        if (t.type == TOKEN_NEWLINE || t.type == TOKEN_EOF) break;
        lexer_next(lexer);
    }
}

static int is_operator(TokenType t) {
    return t == TOKEN_PLUS || t == TOKEN_MINUS || t == TOKEN_STAR ||
           t == TOKEN_SLASH || t == TOKEN_PERCENT || t == TOKEN_AMP_AMP ||
           t == TOKEN_PIPE_PIPE || t == TOKEN_CARET_CARET || t == TOKEN_BANG || t == TOKEN_EQ_EQ ||
           t == TOKEN_BANG_EQ || t == TOKEN_LT || t == TOKEN_GT ||
           t == TOKEN_LT_EQ || t == TOKEN_GT_EQ ||
           t == TOKEN_PLUS_PLUS || t == TOKEN_MINUS_MINUS ||
           t == TOKEN_QUESTION;
}

static int token_is_int(Token tok, SymbolTable *table) {
    if (tok.type == TOKEN_INT_LIT) return 1;
    if (tok.type == TOKEN_IDENT && !is_bool_value(tok.value) && !is_type_keyword(tok.value)) {
        char mangled[MAX_IDENTIFIER_LEN];
        mangle_name(tok.value, mangled, MAX_IDENTIFIER_LEN);
        if (symbol_table_has(table, mangled)) {
            return symbol_table_get(table, mangled) == TYPE_INT;
        }
    }
    return 0;
}

static FuncInfo *find_funcinfo_by_cname(const char *cname);

static VarType effective_param_default_type(const char *src_name, VarType sym_type) {
    if (!g_enclosing_cname[0]) return sym_type;
    /* Untyped params are typically stored as TYPE_FLOAT placeholders in scope.
       For params with known default types, prefer the default type in typing/inference. */
    if (sym_type != TYPE_FLOAT) return sym_type;
    FuncInfo *efi = find_funcinfo_by_cname(g_enclosing_cname);
    if (!efi) return sym_type;
    for (int pi = 0; pi < efi->param_count && pi < MAX_FUNC_PARAMS; pi++) {
        if (strcmp(efi->param_names[pi], src_name) == 0) {
            if (efi->has_default[pi] && efi->def_type[pi] != TYPE_NULL)
                return efi->def_type[pi];
            return sym_type;
        }
    }
    return sym_type;
}

static int has_operator_ahead(Lexer *lexer);

static int try_parse_typeof(Lexer *lexer, SymbolTable *table, Token *result);

static int has_operator_ahead(Lexer *lexer) {
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



static void transform_string_multiply(char *expr, int size) {
    if (strstr(expr, "*") == NULL) return;
    char *tmp = (char*)malloc(2048); if(!tmp) return;
    strncpy(tmp, expr, 2047);
    tmp[2047] = 0;
    char *start = tmp;
    char *left = (char*)calloc(1,512); char *right = (char*)calloc(1,512);
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
            strncpy(left, s, 511);
            left[511] = 0;
            s = p + 1;
            while (*s == ' ' || *s == '\t') s++;
            e = s + strlen(s) - 1;
            while (e >= s && (*e == ' ' || *e == '\t')) {
                *e = 0;
                e--;
            }
            strncpy(right, s, 511);
            right[511] = 0;
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
    char *result = (char*)malloc(2048); if(!result){free(tmp);free(left);free(right);return;}
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
        cntbuf = (char*)malloc(512); if(!cntbuf){free(tmp);free(left);free(right);free(result);return;}
        snprintf(cntbuf, 512, "atoll(%s)", right);
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
        snprintf(result, 2047, "_ltng_repeat_dyn(%s,(long long)(%s))", right, left);
    } else if (right_is_num && (left[0] == '"' || (left[0] == '\'' && strlen(left) >= 3))) {
        /* "string" * number */
        snprintf(result, 2047, "_ltng_repeat_dyn(%s,(long long)(%s))", left, right);
    } else {
        /* variable cases: assume left is string, right is count */
        snprintf(result, 2047, "_ltng_repeat_dyn(%s,(long long)(%s))", left, count_expr);
    }
    strncpy(expr, result, size - 1);
    expr[size - 1] = 0;
    free(cntbuf); free(tmp); free(left); free(right); free(result);
}


static ASTNode *parse_call_node(Lexer *lexer, SymbolTable *table, const char *fname_tok_val, int line, int col, ASTNode *first_arg);
static void build_call_cexpr(ASTNode *call, char *cexpr, int cexpr_size);
static int append_immediate_call_chains(Lexer *lexer, SymbolTable *table, char *cexpr, int cexpr_size);
static ASTNode *make_receiver_arg(SymbolTable *table, Token rtok);


static void sanitize_mod_atof(char *out, int out_size) {
    /* No-op: modulo/division rewriting and runtime macros handle operand typing. */
    (void)out; (void)out_size;
}

static int part_is_stringy(const char *part);

/* Normalize a string-expression fragment: resolve '+' (concat) and '*'
   (repeat) with correct operator precedence, resolving innermost
   parenthesized groups first.  Statement expressions ({ ... }) are treated
   as atomic operands.  array_access_used selects _ltng_add_sn (array
   elements) vs the snprintf(...) concat form (general contexts).  Each
   pass resolves one group, so the number of passes is bounded by the
   string length - no fixed depth cap that would crash on deeply nested
   parentheses. */
static void normalize_string_expr(char *buf, int buf_size, int array_access_used, SymbolTable *table) {
    if (!buf || buf_size < 2) return;
    char *work = (char*)malloc(8192); if(!work) return;
    strncpy(work, buf, 8192 - 1);
    work[8192 - 1] = 0;
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
                char *wbuf = (char*)malloc(8192); if(!wbuf){free(work); return;}
                snprintf(wbuf, 8192, "(%s)", work);
                strncpy(work, wbuf, 8192 - 1); work[8192 - 1] = 0;
                free(wbuf);
                just_wrapped = 1;
                continue;
            }
            break;
        }
        int clen = be - bs - 1;
        char content[4096];
        if (clen >= (int)sizeof(content)) clen = sizeof(content) - 1;
        memcpy(content, work + bs + 1, clen);
        content[clen] = 0;

        if (do_plus) {
            char parts[16][2048]; int nip = 0; char *ist = content;
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
                    strncpy(parts[nip++], s2, sizeof(parts[0]) - 1); parts[nip - 1][sizeof(parts[0]) - 1] = 0; ist = pp + 1;
                }
            }
            { char *s2 = ist; while (*s2 == ' ' || *s2 == '\t') s2++; char *e2 = s2 + strlen(s2) - 1; while (e2 >= s2 && (*e2 == ' ' || *e2 == '\t')) { *e2 = 0; e2--; } strncpy(parts[nip++], s2, sizeof(parts[0]) - 1); parts[nip - 1][sizeof(parts[0]) - 1] = 0; }
            if (nip < 2) break;
            /* Helper logic inline: normalize a part and make it stringy for concat */
            char ires[4096];
            strncpy(ires, parts[0], sizeof(ires) - 1); ires[sizeof(ires) - 1] = 0;
            normalize_string_expr(ires, sizeof(ires), array_access_used, table);
            for (int k = 1; k < nip; k++) {
                char p[2048];
                strncpy(p, parts[k], sizeof(p) - 1); p[sizeof(p) - 1] = 0;
                normalize_string_expr(p, sizeof(p), array_access_used, table);
                /* Build concat step. In array-access context use _ltng_add_sn.
                   Otherwise, if either side is not stringy (e.g. atoi(x) from
                   int var in mixed context, or numeric literal), convert to
                   string form or use _ltng_add_sn. */
                char lhs[4096]; strncpy(lhs, ires, sizeof(lhs)-1); lhs[sizeof(lhs)-1]=0;
                char rhs[2048]; strncpy(rhs, p, sizeof(rhs)-1); rhs[sizeof(rhs)-1]=0;
                /* If a part is atoi/atoll/atof(ident), in string concat we
                   want the original buffer (vars are char buffers). Track whether
                   each side was a numeric conversion wrapper (numeric-typed var). */
                int lhs_was_numwrap = 0, rhs_was_numwrap = 0;
                {
                    char *sides[2] = {lhs, rhs};
                    int slens[2] = {(int)sizeof(lhs), (int)sizeof(rhs)};
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
                                char *tmp = (char*)malloc(512); if(!tmp){sides[si][0]=0;} else { int tl = (int)(inner - start);
                                if (tl >= (int)512) tl = 512-1;
                                memcpy(tmp, start, tl); tmp[tl]=0;
                                strncpy(sides[si], tmp, slens[si]-1); sides[si][slens[si]-1]=0; free(tmp); }
                                *numwrap[si] = 1;
                            }
                        }
                    }
                }
                int lhs_s = part_is_stringy(lhs);
                int rhs_s = part_is_stringy(rhs);

                /* Build char* args for concat helpers (_ltng_add_sn/_ltng_concat_dyn take const char*). */
                char *lhs_arg = (char*)malloc(4200); char *rhs_arg = (char*)malloc(4200);
                if(!lhs_arg||!rhs_arg){free(lhs_arg);free(rhs_arg); strncpy(ires,work,sizeof(ires)-1); ires[sizeof(ires)-1]=0; continue;}
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
                _LTNG_SN_ARG(lhs, lhs_arg, 4200, lhs_s);
                _LTNG_SN_ARG(rhs, rhs_arg, 4200, rhs_s);
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
                                char *nm = (char*)malloc(512); if(nm){ int nl = (int)(qe - sp);
                                if (nl >= (int)512) nl = (int)512-1;
                                memcpy(nm, sp, nl); nm[nl] = 0;
                                if (symbol_table_has(table, nm)) {
                                    VarType vt = symbol_table_get(table, nm);
                                    if (vt == TYPE_STRING || vt == TYPE_CHAR) *flags[si] = 1;
                                } else {
                                    char *mn = (char*)malloc(512);
                                    if(mn){ mangle_name(nm, mn, 512);
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

                char *st = (char*)malloc(5000); if(!st) st=(char*)calloc(1,1);
                if (array_access_used) {
                    snprintf(st, 5000, "_ltng_add_sn(%s, %s)", lhs_arg, rhs_arg);
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
                        snprintf(st, 5000, "_ltng_concat_dyn(2, %s, %s)", lhs_arg, rhs_arg);
                    else if (lhs_s && rhs_s && lhs_bare_ident && rhs_bare_ident && !lhs_was_numwrap && !rhs_was_numwrap && !lhs_is_str_var && !rhs_is_str_var) {
                        int lhs_num = 0, rhs_num = 0;
                        if (table) {
                            const char *ss[2] = {lhs, rhs}; int *nf[2] = {&lhs_num, &rhs_num};
                            for (int si=0; si<2; si++) {
                                const char *sp = ss[si]; while (*sp==' '||*sp=='\t') sp++;
                                char *nm = (char*)malloc(512); if(nm){ int nl=0; while(sp[nl]&&(isalnum((unsigned char)sp[nl])||sp[nl]=='_')){nm[nl]=sp[nl];nl++; if(nl>=511)break;} nm[nl]=0;
                                if (symbol_table_has(table, nm)) { VarType vt=symbol_table_get(table,nm); if(vt==TYPE_INT||vt==TYPE_FLOAT||vt==TYPE_BOOL) *nf[si]=1; }
                                else { char *mn = (char*)malloc(512); if(mn){ mangle_name(nm,mn,512); if(symbol_table_has(table,mn)){VarType vt=symbol_table_get(table,mn); if(vt==TYPE_INT||vt==TYPE_FLOAT||vt==TYPE_BOOL)*nf[si]=1;} free(mn); } }
                                free(nm); }
                            }
                        }
                        if (lhs_num || rhs_num)
                            snprintf(st, 5000, "_ltng_add_sn(%s, %s)", lhs_arg, rhs_arg);
                        else
                            snprintf(st, 5000, "_ltng_concat_dyn(2, %s, %s)", lhs_arg, rhs_arg);
                    } else if (lhs_s && rhs_s && lhs_def_str && !rhs_def_str)
                        snprintf(st, 5000, "_ltng_concat_dyn(2, %s, %s)", lhs_arg, rhs_arg);
                    else if (lhs_s && rhs_s && !lhs_def_str && rhs_def_str)
                        snprintf(st, 5000, "_ltng_concat_dyn(2, %s, %s)", lhs_arg, rhs_arg);
                    else
                        snprintf(st, 5000, "_ltng_add_sn(%s, %s)", lhs_arg, rhs_arg);
                }
                free(lhs_arg); free(rhs_arg);
                strncpy(ires, st, sizeof(ires) - 1); ires[sizeof(ires) - 1] = 0;
                free(st);
            }
            char *nb = (char*)malloc(8192); if(!nb) nb=(char*)calloc(1,1);
            snprintf(nb, 8192, "%.*s%s%s", bs, work, ires, work + be + 1);
            strncpy(work, nb, 8192 - 1); work[8192 - 1] = 0;
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
            char left[4096] = {0}, right[1024] = {0};
            int llen = star; if (llen >= (int)sizeof(left)) llen = sizeof(left) - 1;
            memcpy(left, content, llen); left[llen] = 0;
            int rlen = clen - star - 1; if (rlen >= (int)sizeof(right)) rlen = sizeof(right) - 1;
            memcpy(right, content + star + 1, rlen); right[rlen] = 0;
            { char *s = left; while (*s == ' ' || *s == '\t') s++; if (s != left) memmove(left, s, strlen(s) + 1); }
            { char *e = left + strlen(left) - 1; while (e >= left && (*e == ' ' || *e == '\t')) { *e = 0; e--; } }
            { char *s = right; while (*s == ' ' || *s == '\t') s++; if (s != right) memmove(right, s, strlen(s) + 1); }
            { char *e = right + strlen(right) - 1; while (e >= right && (*e == ' ' || *e == '\t')) { *e = 0; e--; } }
            if (left[0] == '\0' || right[0] == '\0') break;
            /* Determine numeric/string roles for repeat(count vs string). */
            int right_is_num = 1;
            {
                const char *rsp = right;
                if (*rsp == '-') rsp++;
                if (*rsp == 0) right_is_num = 0;
                else { int hd = 0; for (; *rsp; rsp++) { if (isdigit((unsigned char)*rsp)) hd = 1; else if (*rsp == '.') {} else { right_is_num = 0; break; } } if (!hd) right_is_num = 0; }
            }
            int left_is_num = 1;
            {
                const char *lsp = left;
                while (*lsp == ' ' || *lsp == '\t') lsp++;
                if (*lsp == '-') lsp++;
                if (*lsp == 0) left_is_num = 0;
                else { int hd = 0; for (; *lsp; lsp++) { if (isdigit((unsigned char)*lsp)) hd = 1; else if (*lsp == '.') {} else { left_is_num = 0; break; } } if (!hd) left_is_num = 0; }
            }
            char str_operand[4096];
            char *countbuf = NULL;
            if (left_is_num && !right_is_num) {
                /* number * "string" (or number * string-expr) */
                strncpy(str_operand, right, sizeof(str_operand)-1); str_operand[sizeof(str_operand)-1]=0;
                normalize_string_expr(str_operand, sizeof(str_operand), array_access_used, table);
                countbuf = _ltng_cfmt("%s", left);
            } else if (right_is_num) {
                strncpy(str_operand, left, sizeof(str_operand)-1); str_operand[sizeof(str_operand)-1]=0;
                normalize_string_expr(str_operand, sizeof(str_operand), array_access_used, table);
                countbuf = _ltng_cfmt("%s", right);
            } else {
                /* variable * variable: assume left is string, right is count */
                strncpy(str_operand, left, sizeof(str_operand)-1); str_operand[sizeof(str_operand)-1]=0;
                normalize_string_expr(str_operand, sizeof(str_operand), array_access_used, table);
                countbuf = _ltng_cfmt("atoll(%s)", right);
            }
            char *rep = _ltng_cfmt("_ltng_repeat_dyn(%s,(long long)(%s))", str_operand, countbuf ? countbuf : "0");
            char *nb = _ltng_cfmt("%.*s%s%s", bs, work, rep, work + be + 1);
            strncpy(work, nb, 8192 - 1); work[8192 - 1] = 0;
            free(countbuf); free(rep); free(nb);
        }
    }

    strncpy(buf, work, buf_size - 1);
    buf[buf_size - 1] = 0;
    free(work);
}

/* Post-process: replace atof(X) + atof(Y), atof(X) + _ltng_add_sn(Y, Z),
   _ltng_add_sn(X, Y) + atof(Z), etc. with _ltng_add_sn(...) for runtime
   string/number detection on buffer variables (e.g. function params typed
   as float). We only replace atof() (float-typed), never atoi() (int-typed). */
static int is_add_operand(const char *p, char *content, int content_size) {
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

static int optimize_add_concat(char *out, int out_size, int pos) {
    (void)pos;
    char *result = (char*)calloc(1,4096);
    char *left = (char*)malloc(4096);
    char *right = (char*)malloc(4096);
    if(!result||!left||!right){free(result);free(left);free(right);return 0;}
    const char *src = out;
    char *dst = result;
    int replaced = 0;

    while (*src && (int)(dst - result) < 4095) {
        left[0]=0; right[0]=0;

        if (is_add_operand(src, left, 4096)) {
            const char *after_left = src;
            while (*after_left && (isalnum((unsigned char)*after_left) || *after_left == '_' ||
                                   *after_left == '(' || *after_left == ')' || *after_left == ',' ||
                                   *after_left == ' ' || *after_left == '\t')) after_left++;
            while (*after_left == ' ' || *after_left == '\t') after_left++;
            if (*after_left == '+') {
                const char *after_plus = after_left + 1;
                while (*after_plus == ' ' || *after_plus == '\t') after_plus++;
                if (is_add_operand(after_plus, right, 4096)) {
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
                        int n = snprintf(dst, 4096 - (int)(dst - result),
                                         "_ltng_add_sn(%s, %s)", left, right);
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

static const char *type_name_str(VarType t);

static int function_returns_string(const char *fname);

static int part_is_stringy(const char *part) {
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
                char *inner = (char*)malloc(4096); if(!inner) return 0;
                size_t ilen = (size_t)(end - (q + 1));
                if (ilen >= 4096) ilen = 4096 - 1;
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
            char callee[MAX_IDENTIFIER_LEN];
            int cl = (int)(e - q);
            if (cl >= MAX_IDENTIFIER_LEN) cl = MAX_IDENTIFIER_LEN - 1;
            memcpy(callee, q, (size_t)cl);
            callee[cl] = '\0';
            /* runtime helpers that return char* */
            if (strcmp(callee, "_ltng_add_sn") == 0 || strcmp(callee, "_ltng_concat") == 0 ||
                strcmp(callee, "_ltng_repeat") == 0 || strcmp(callee, "_ltng_as") == 0 ||
                strcmp(callee, "_ltng_ad") == 0 || strcmp(callee, "_ltng_retbuf") == 0 ||
                strcmp(callee, "__ltng_call_indirect") == 0)
                return 1;
            return function_returns_string(callee);
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

static void token_append_source(char *out, int *pos, int out_size, Token tok);

/* Find a function AST node by its short source name or mangled C name. */
static ASTNode *find_function_ast_by_name(const char *name);
static ASTNode *find_function_node_by_cname(const char *cname);
static int resolve_func_ref_info(SymbolTable *table, const char *name, char *out_cname, int out_size, int *out_pc);

/* Infer the return type of a user-defined function by scanning its return statements. */
static VarType infer_function_return_type(const char *fname);
static VarType infer_call_result_type(ASTNode *call);

/* Return 1 when a call to the named function is known to produce a string
   value (explicit `-> string` annotation, or inferred from its returns). */
static int function_returns_string(const char *fname);
static VarType collect_expression(Lexer *lexer, SymbolTable *table,
                                  char *out, int out_size,
                                  int stop_on_rparen, int stop_on_comma,
                                  int *out_sl, int *out_sc, int *out_el, int *out_ec);
static VarType collect_expression_ex(Lexer *lexer, SymbolTable *table,
                                     char *out, int out_size,
                                     int stop_on_rparen, int stop_on_comma,
                                     int *out_sl, int *out_sc, int *out_el, int *out_ec,
                                     int stop_on_ternary_colon);

static int parse_array_literal(Lexer *lexer, SymbolTable *table, ASTNode *node) {
    /* The caller has already consumed the opening '['. Parse the remaining
       elements/nested arrays until the matching ']'. Arrays are stored as a
       flat, row-major list of values. Arrays may have any number of dimensions.
       Element encoding in node->value (pipe-separated): a leading '\x01' marks
       a variable/C-expression element (its C name follows); otherwise the text
       is pushed as a string literal value. */
    int depth = 1;                 /* nesting level, outer '[' already consumed */
    int max_depth = 1;             /* deepest nesting level seen */
    int elem_count = 0;
    char values[4096] = "";
    int vpos = 0;

    /* Dynamically sized counters, grown as needed for arbitrary depth. */
    int cap_depth = 16;
    int *elem_counts = calloc(cap_depth, sizeof(int));
    int *max_elem_counts = calloc(cap_depth, sizeof(int));
    int *child_counts = calloc(cap_depth, sizeof(int));
    int *max_child_counts = calloc(cap_depth, sizeof(int));
    VarType elem_type = TYPE_NULL; /* inferred homogeneous element type (TYPE_NULL=unknown/mixed) */
    int elem_type_set = 0;

    while (lexer->current < lexer->count) {
        Token t = lexer_peek(lexer);

        if (t.type == TOKEN_NEWLINE || t.type == TOKEN_EOF) break;

        if (t.type == TOKEN_LBRACKET) {
            lexer_next(lexer);
            /* Starting a child group of the current group. */
            if (depth >= cap_depth) {
                int new_cap = cap_depth * 2;
                elem_counts = realloc(elem_counts, new_cap * sizeof(int));
                max_elem_counts = realloc(max_elem_counts, new_cap * sizeof(int));
                child_counts = realloc(child_counts, new_cap * sizeof(int));
                max_child_counts = realloc(max_child_counts, new_cap * sizeof(int));
                for (int d = cap_depth; d < new_cap; d++) {
                    elem_counts[d] = 0;
                    max_elem_counts[d] = 0;
                    child_counts[d] = 0;
                    max_child_counts[d] = 0;
                }
                cap_depth = new_cap;
            }
            child_counts[depth]++;
            if (child_counts[depth] > max_child_counts[depth]) {
                max_child_counts[depth] = child_counts[depth];
            }
            depth++;
            if (depth > max_depth) max_depth = depth;
            elem_counts[depth] = 0;
            child_counts[depth] = 0;
            continue;
        }

        if (t.type == TOKEN_RBRACKET) {
            lexer_next(lexer);
            if (depth == 1) {
                depth = 0;
                break; /* matching outer ']' */
            }
            if (elem_counts[depth] > max_elem_counts[depth]) {
                max_elem_counts[depth] = elem_counts[depth];
            }
            depth--;
            continue;
        }

        if (t.type == TOKEN_COMMA) {
            lexer_next(lexer);
            continue;
        }

        /* Element value: scalar expression or simple literal/identifier. */
        char val[MAX_STRING_LEN] = "";
        VarType this_elem_type = TYPE_NULL;
        Token t1 = lexer_peek(lexer);
        Token t2 = (lexer->current + 1 < lexer->count) ? lexer->tokens[lexer->current + 1] : t1;
        int simple_single = 0;
        if ((t1.type == TOKEN_STRING || t1.type == TOKEN_CHAR || t1.type == TOKEN_INT_LIT || t1.type == TOKEN_FLOAT_LIT) &&
            (t2.type == TOKEN_COMMA || t2.type == TOKEN_RBRACKET || t2.type == TOKEN_NEWLINE)) {
            simple_single = 1;
        } else if (t1.type == TOKEN_IDENT && (t2.type == TOKEN_COMMA || t2.type == TOKEN_RBRACKET || t2.type == TOKEN_NEWLINE)) {
            simple_single = 1;
        }
        if (simple_single) {
            lexer_next(lexer);
            if (t1.type == TOKEN_CHAR) {
                snprintf(val, sizeof(val), "'%s'", t1.value);
                this_elem_type = TYPE_CHAR;
            } else if (t1.type == TOKEN_INT_LIT) {
                snprintf(val, sizeof(val), "%s", t1.value);
                this_elem_type = TYPE_INT;
            } else if (t1.type == TOKEN_FLOAT_LIT) {
                snprintf(val, sizeof(val), "%s", t1.value);
                this_elem_type = TYPE_FLOAT;
            } else if (t1.type == TOKEN_STRING) {
                snprintf(val, sizeof(val), "%s", t1.value);
                this_elem_type = TYPE_STRING;
            } else if (t1.type == TOKEN_IDENT && !is_bool_value(t1.value) && !is_type_keyword(t1.value)) {
                char m[MAX_IDENTIFIER_LEN];
                mangle_name(t1.value, m, sizeof(m));
                if (symbol_table_has(table, m) || symbol_table_has(table, t1.value)) {
                    const char *cname = symbol_table_has(table, m) ? symbol_table_get_cname(table, m) : symbol_table_get_cname(table, t1.value);
                    val[0] = '\x01';
                    strncpy(val + 1, cname, sizeof(val) - 2);
                    val[sizeof(val) - 1] = '\0';
                } else {
                    snprintf(val, sizeof(val), "%s", t1.value);
                }
                /* variable reference element: runtime type unknown */
                this_elem_type = TYPE_NULL;
            } else if (t1.type == TOKEN_IDENT && is_bool_value(t1.value)) {
                if (strcmp(t1.value, "true") == 0 || strcmp(t1.value, "t") == 0)
                    snprintf(val, sizeof(val), "true");
                else
                    snprintf(val, sizeof(val), "false");
                this_elem_type = TYPE_BOOL;
            } else {
                snprintf(val, sizeof(val), "%s", t1.value);
                this_elem_type = TYPE_NULL;
            }
        } else {
            /* Parse as expression element (supports conversion calls, arithmetic, calls, etc.) */
            char expr[MAX_STRING_LEN];
            VarType etype = collect_expression(lexer, table, expr, sizeof(expr), 1, 1, NULL, NULL, NULL, NULL);
            if (expr[0] == '"') {
                /* C string literal result: store raw content (codegen will quote/escape) */
                size_t elen = strlen(expr);
                if (elen >= 2 && expr[elen-1] == '"') {
                    expr[elen-1] = '\0';
                    snprintf(val, sizeof(val), "%s", expr + 1);
                } else {
                    val[0] = '\x01'; strncpy(val + 1, expr, sizeof(val) - 2); val[sizeof(val)-1] = '\0';
                }
                this_elem_type = TYPE_STRING;
            } else if (etype == TYPE_STRING || etype == TYPE_OBJECT || etype == TYPE_ARRAY || etype == TYPE_CHAR) {
                val[0] = '\x01'; strncpy(val + 1, expr, sizeof(val) - 2); val[sizeof(val)-1] = '\0';
                this_elem_type = TYPE_STRING;
            } else if (etype == TYPE_BOOL) {
                char wrapped[MAX_STRING_LEN];
                snprintf(wrapped, sizeof(wrapped), "((%s)?\"true\":\"false\")", expr);
                val[0] = '\x01'; strncpy(val + 1, wrapped, sizeof(val) - 2); val[sizeof(val)-1] = '\0';
                this_elem_type = TYPE_BOOL;
            } else {
                /* numeric expression: convert to string at runtime */
                char wrapped[MAX_STRING_LEN];
                if (etype == TYPE_FLOAT) snprintf(wrapped, sizeof(wrapped), "_ltng_ad((double)(%s))", expr);
                else snprintf(wrapped, sizeof(wrapped), "_ltng_ad((double)(long long)(%s))", expr);
                val[0] = '\x01'; strncpy(val + 1, wrapped, sizeof(val) - 2); val[sizeof(val)-1] = '\0';
                this_elem_type = (etype == TYPE_FLOAT) ? TYPE_FLOAT : TYPE_INT;
            }
        }
        /* Merge element type for homogeneous numeric inference (Step 4). */
        if (!elem_type_set) {
            elem_type = this_elem_type;
            elem_type_set = 1;
        } else if (elem_type != TYPE_NULL) {
            if (this_elem_type == TYPE_NULL) {
                elem_type = TYPE_NULL;
            } else if (elem_type == this_elem_type) {
                /* same type */
            } else if ((elem_type == TYPE_INT && this_elem_type == TYPE_FLOAT) ||
                       (elem_type == TYPE_FLOAT && this_elem_type == TYPE_INT)) {
                elem_type = TYPE_FLOAT;
            } else {
                elem_type = TYPE_NULL; /* mixed/incompatible */
            }
        }
        int vlen = (int)strlen(val);
        if (vpos + vlen + 1 < (int)sizeof(values) - 1) {
            if (vpos > 0) values[vpos++] = '|';
            memcpy(values + vpos, val, vlen);
            vpos += vlen;
        }
        values[vpos] = '\0';
        elem_count++;
        elem_counts[depth]++;
        if (elem_counts[depth] > max_elem_counts[depth]) {
            max_elem_counts[depth] = elem_counts[depth];
        }
    }

    /* Compute dimensions:
       For depth d (1-indexed, d < max_depth): dims[d-1] = max_child_counts[d]
       For depth == max_depth: dims[max_depth-1] = max_elem_counts[max_depth] */
    int dim_count = max_depth;
    if (dim_count < 1) dim_count = 1;

    int *dims = malloc(dim_count * sizeof(int));
    if (dim_count == 1) {
        dims[0] = elem_count;
    } else {
        for (int d = 1; d < dim_count; d++) {
            dims[d - 1] = max_child_counts[d];
        }
        dims[dim_count - 1] = max_elem_counts[dim_count];
    }

    free(elem_counts);
    free(max_elem_counts);
    free(child_counts);
    free(max_child_counts);

    node->array_dim_count = dim_count;
    node->array_dims = dims;
    node->array_elem_count = elem_count;
    node->array_elem_type = elem_type_set ? elem_type : TYPE_NULL;
    strncpy(node->value, values, MAX_STRING_LEN - 1);
    node->value[MAX_STRING_LEN - 1] = '\0';

    return 1;
}



static int expr_is_string_value(const char *expr) {
    const char *p = expr;
    while (*p == ' ' || *p == '\t' || *p == '\n') p++;
    if (*p == '\0') return 0;
    /* Unwrap simple outer parentheses around the whole expression */
    const char *pend = p + strlen(p);
    while (*p == '(') {
        if (p[1] == '{') break; /* statement expression */
        int d = 0, inq = 0; char qc = 0; const char *endp = NULL;
        for (const char *q = p; q < pend; q++) {
            if (inq) { if (*q == '\\' && q[1] && q+1 < pend) { q++; continue; } if (*q == qc) inq = 0; continue; }
            if (*q == '"' || *q == '\'') { inq = 1; qc = *q; continue; }
            if (*q == '(') d++;
            else if (*q == ')') { d--; if (d == 0) { endp = q; break; } }
        }
        if (!endp) break;
        const char *after = endp + 1;
        while (after < pend && (*after == ' ' || *after == '\t' || *after == '\n')) after++;
        if (after == pend) {
            p++;
            pend = endp;
            while (*p == ' ' || *p == '\t' || *p == '\n') p++;
            while (pend > p && (pend[-1] == ' ' || pend[-1] == '\t' || pend[-1] == '\n')) pend--;
            continue;
        }
        break;
    }
    char inner_buf[4096];
    size_t ilen = (size_t)(pend - p);
    if (ilen >= sizeof(inner_buf)) ilen = sizeof(inner_buf) - 1;
    memcpy(inner_buf, p, ilen);
    inner_buf[ilen] = '\0';
    p = inner_buf;

    /* String literal */
    if (*p == '"') return 1;
    /* Numeric literal */
    {
        const char *q = p;
        if (*q == '-' || *q == '+') q++;
        int has_digit = 0, all_num = 1;
        while (*q) {
            if (isdigit((unsigned char)*q)) { has_digit = 1; q++; }
            else if (*q == '.' && has_digit) { q++; }
            else { all_num = 0; break; }
        }
        if (all_num && has_digit) return 0;
    }
    /* Known numeric conversion wrappers / casts => numeric */
    if (strncmp(p, "atoi(", 5) == 0 || strncmp(p, "atoll(", 6) == 0 || strncmp(p, "atof(", 5) == 0)
        return 0;
    if (strncmp(p, "(long long)", 11) == 0 || strncmp(p, "(int)", 5) == 0 ||
        strncmp(p, "(double)", 8) == 0 || strncmp(p, "(float)", 7) == 0)
        return 0;
    if (strncmp(p, "_ltng_from_double(", 18) == 0 || strncmp(p, "_ltng_from_int(", 15) == 0 ||
        strncmp(p, "_ltng_ad(", 9) == 0)
        return 0;
    /* Bool numeric forms */
    if (strstr(p, "[0]=='t'") != NULL || strstr(p, "[0]=='T'") != NULL) return 0;
    /* Known string-producing helpers/exprs (not generic buffers like _ltng_retbuf/_ltng_ad) */
    if (strstr(p, "_ltng_add_sn(") || strstr(p, "_ltng_repeat(") ||
        strstr(p, "_ltng_as(") || strstr(p, "_ltng_concat(") ||
        strstr(p, "_ltng_oret_"))
        return 1;
    /* Statement expression that builds strings */
    if (strncmp(p, "({", 2) == 0) {
        if (strstr(p, "_ltng_add_sn") || strstr(p, "_ltng_repeat") ||
            strstr(p, "_ltng_as(") || strstr(p, "_ltng_concat") ||
            strstr(p, "_ltng_oret_"))
            return 1;
        if (strstr(p, "char _ltc") || strstr(p, "char _ltm"))
            return 1;
        return 0;
    }
    /* Bare identifier => char buffer (string var emitted raw, numeric as atoi/atoll/atof) */
    {
        const char *q = p;
        if (*q == '_' || isalpha((unsigned char)*q)) {
            while (*q && (isalnum((unsigned char)*q) || *q == '_')) q++;
            while (*q == ' ' || *q == '\t') q++;
            if (*q == '\0') return 1;
        }
    }
    /* Numeric conversion calls/wrappers anywhere => numeric expression */
    if (strstr(p, "atof(") || strstr(p, "atoi(") || strstr(p, "atoll(") ||
        strstr(p, "_ltng_from_double(") || strstr(p, "_ltng_from_int(") || strstr(p, "_ltng_ad("))
        return 0;
    /* Contains C string literals => string expression */
    if (strchr(p, '"') != NULL) return 1;
    /* Other specific string helpers */
    if (strstr(p, "_ltng_add_sn") || strstr(p, "_ltng_repeat") || strstr(p, "_ltng_oret_"))
        return 1;
    /* No string indicators: detect string concat/repeat of bare identifiers */
    {
        int has_op = 0, has_ident = 0;
        for (const char *q = p; *q; q++) {
            unsigned char c = (unsigned char)*q;
            if (c=='+' || c=='*') has_op = 1;
            if (isalpha(c) || c == '_') has_ident = 1;
        }
        if (has_op && has_ident) return 1;
    }
    /* Pure numeric expression */
    {
        int all_num_expr = 1, has_num_char = 0;
        for (const char *q = p; *q; q++) {
            unsigned char c = (unsigned char)*q;
            if (isdigit(c) || c=='+' || c=='-' || c=='*' || c=='/' || c=='%' ||
                c=='(' || c==')' || c==' ' || c=='\t' || c=='.') { has_num_char = 1; }
            else { all_num_expr = 0; break; }
        }
        if (all_num_expr && has_num_char) return 0;
    }
    /* Generic function calls / unknown => not treated as string by default */
    return 0;
}
static void trim_c_expr(char *s) {
    char *p = s;
    while (*p == ' ' || *p == '\t' || *p == '\n') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t len = strlen(s);
    while (len > 0 && (s[len-1] == ' ' || s[len-1] == '\t' || s[len-1] == '\n')) { s[--len] = '\0'; }
}

/* Normalize a value subexpression that may be a string concat/repeat. */
static void normalize_value_expr(char *buf, int buf_size, int array_access_used, SymbolTable *table) {
    trim_c_expr(buf);
    if (!buf[0]) return;
    /* Check for + or * at any depth (outside C string literals) */
    int has_plus = 0, has_star = 0, in = 0; char qc = 0;
    for (const char *p = buf; *p; p++) {
        if (in) { if (*p == '\\' && p[1]) { p++; continue; } if (*p == qc) in = 0; continue; }
        if (*p == '"' || *p == '\'') { in = 1; qc = *p; continue; }
        if (*p == '+') has_plus = 1;
        if (*p == '*') has_star = 1;
    }
    if (has_plus || has_star) {
        /* Normalize if it's a string expression, or if it has string indicators */
        if (expr_is_string_value(buf) ||
            strchr(buf, '"') || strstr(buf, "_ltng_add_sn") || strstr(buf, "_ltng_repeat") ||
            strstr(buf, "_ltng_as(") || strstr(buf, "_ltng_ad(")) {
            normalize_string_expr(buf, buf_size, array_access_used, table);
        }
    }
}

static int split_top_level(const char *s, const char *op, char *left, int lsz, char *right, int rsz) {
    /* Find rightmost? or leftmost? For comparisons (non-assoc chain) and for &&/|| (left-assoc)
       we want leftmost split to preserve left-to-right: a&&b&&c => (a&&b)&&c - but easier to split
       leftmost for correct grouping? C is left-assoc for &&/||, so splitting at leftmost top-level
       gives left = "a", right = "b&&c" if we recurse on left? No. Better split at rightmost for
       left-associativity: "a&&b&&c" split on && rightmost => left="a&&b", right="c"? actually
       to produce (a&&b)&&c we can process left-to-right; splitting rightmost gives leftmost group.
       Simpler: find leftmost occurrence and recurse on right side when rebuilding - not quite.
       For correctness with same-precedence ops, split on rightmost: left = everything before last op,
       right = after. But we need to handle multi-char ops. */
    int oplen = (int)strlen(op);
    int found = -1;
    int d = 0, in = 0; char qc = 0;
    for (int i = 0; s[i]; i++) {
        char c = s[i];
        if (in) { if (c == '\\' && s[i+1]) { i++; continue; } if (c == qc) in = 0; continue; }
        if (c == '"' || c == '\'') { in = 1; qc = c; continue; }
        if (c == '(' || c == '{' || c == '[') { d++; continue; }
        if (c == ')' || c == '}' || c == ']') { if (d > 0) d--; continue; }
        if (d == 0 && strncmp(s + i, op, oplen) == 0) {
            /* Avoid matching < in <=, > in >=, = in ==, ! in !=, & in &&, | in || */
            if (oplen == 1) {
                if (op[0] == '<' && s[i+1] == '=') continue;
                if (op[0] == '>' && s[i+1] == '=') continue;
                if (op[0] == '!' && s[i+1] == '=') continue;
                if (op[0] == '=' && s[i+1] == '=') continue;
                if (op[0] == '&' && s[i+1] == '&') continue;
                if (op[0] == '|' && s[i+1] == '|') continue;
            }
            found = i; /* keep last (rightmost) for left-assoc */
        }
    }
    if (found < 0) return 0;
    int llen = found;
    if (llen >= lsz) llen = lsz - 1;
    memcpy(left, s, llen); left[llen] = '\0';
    strncpy(right, s + found + oplen, rsz - 1); right[rsz - 1] = '\0';
    trim_c_expr(left); trim_c_expr(right);
    return 1;
}

static void normalize_mixed_expr(char *buf, int buf_size, int array_access_used, SymbolTable *table);

static void normalize_or_expr(char *buf, int buf_size, int array_access_used, SymbolTable *table) {
    char left[4096], right[4096];
    if (split_top_level(buf, "||", left, sizeof(left), right, sizeof(right))) {
        normalize_mixed_expr(left, sizeof(left), array_access_used, table);
        normalize_mixed_expr(right, sizeof(right), array_access_used, table);
        snprintf(buf, buf_size, "(%s) || (%s)", left, right);
        return;
    }
    /* try && */
    if (split_top_level(buf, "&&", left, sizeof(left), right, sizeof(right))) {
        normalize_mixed_expr(left, sizeof(left), array_access_used, table);
        normalize_mixed_expr(right, sizeof(right), array_access_used, table);
        snprintf(buf, buf_size, "(%s) && (%s)", left, right);
        return;
    }
    /* comparisons: == != <= >= < > (check two-char first) */
    const char *ops[] = {"==", "!=", "<=", ">=", "<", ">", NULL};
    for (int i = 0; ops[i]; i++) {
        if (split_top_level(buf, ops[i], left, sizeof(left), right, sizeof(right))) {
            int is_str_cmp = 0;
            if (strcmp(ops[i], "==") == 0 || strcmp(ops[i], "!=") == 0) {
                char ltmp[4096], rtmp[4096];
                strncpy(ltmp, left, sizeof(ltmp)-1); ltmp[sizeof(ltmp)-1]=0;
                strncpy(rtmp, right, sizeof(rtmp)-1); rtmp[sizeof(rtmp)-1]=0;
                trim_c_expr(ltmp); trim_c_expr(rtmp);
                int lv = expr_is_string_value(ltmp);
                int rv = expr_is_string_value(rtmp);
                /* Numeric conversion helpers produce numeric values; compare numerically */
                if ((strstr(ltmp, "_ltng_from_double(") || strstr(ltmp, "_ltng_from_int(") || strstr(ltmp, "_ltng_ad(") ||
                     strstr(ltmp, "atof(") || strstr(ltmp, "atoi(") || strstr(ltmp, "atoll(")) &&
                    (strstr(rtmp, "_ltng_from_double(") || strstr(rtmp, "_ltng_from_int(") || strstr(rtmp, "_ltng_ad(") ||
                     strstr(rtmp, "atof(") || strstr(rtmp, "atoi(") || strstr(rtmp, "atoll(") ||
                     ((*rtmp=='-'||*rtmp=='+'||isdigit((unsigned char)*rtmp))))) {
                    is_str_cmp = 0;
                } else if ((strstr(rtmp, "_ltng_from_double(") || strstr(rtmp, "_ltng_from_int(") || strstr(rtmp, "_ltng_ad(") ||
                            strstr(rtmp, "atof(") || strstr(rtmp, "atoi(") || strstr(rtmp, "atoll(")) &&
                           (strstr(ltmp, "_ltng_from_double(") || strstr(ltmp, "_ltng_from_int(") || strstr(ltmp, "_ltng_ad(") ||
                            strstr(ltmp, "atof(") || strstr(ltmp, "atoi(") || strstr(ltmp, "atoll(") ||
                            ((*ltmp=='-'||*ltmp=='+'||isdigit((unsigned char)*ltmp))))) {
                    is_str_cmp = 0;
                } else if (lv || rv) {
                    /* Bool-ternary int expressions (e.g. (cond?1:0)) compared with 0/1 are numeric comparisons */
                    int l_bool_tern = (strstr(ltmp,"?1:0")!=NULL || strstr(ltmp,"? 1:0")!=NULL || strstr(ltmp,"?1 : 0")!=NULL || strstr(ltmp,"? 1 : 0")!=NULL);
                    int r_bool_tern = (strstr(rtmp,"?1:0")!=NULL || strstr(rtmp,"? 1:0")!=NULL || strstr(rtmp,"?1 : 0")!=NULL || strstr(rtmp,"? 1 : 0")!=NULL);
                    const char *ltp = ltmp; while(*ltp=='('||*ltp==' '||*ltp=='\t') ltp++;
                    const char *rtp = rtmp; while(*rtp=='('||*rtp==' '||*rtp=='\t') rtp++;
                    int l_num01 = (strcmp(ltp,"0")==0 || strcmp(ltp,"1")==0);
                    int r_num01 = (strcmp(rtp,"0")==0 || strcmp(rtp,"1")==0);
                    if ((l_bool_tern && r_num01) || (r_bool_tern && l_num01))
                        is_str_cmp = 0;
                    else
                        is_str_cmp = 1;
                }
            }
            if (is_str_cmp) {
                /* But if it's actually a numeric-conversion helper vs numeric literal/value, compare numerically */
                {
                    char lt[4096], rt[4096]; strncpy(lt,left,sizeof(lt)-1); lt[sizeof(lt)-1]=0; strncpy(rt,right,sizeof(rt)-1); rt[sizeof(rt)-1]=0; trim_c_expr(lt); trim_c_expr(rt);
                    int lnumh = (strstr(lt,"_ltng_from_double(")||strstr(lt,"_ltng_from_int(")||strstr(lt,"_ltng_ad(")||strstr(lt,"_LTNG_FMOD_D(")||strstr(lt,"_LTNG_DIV_D(")||strstr(lt,"atof(")||strstr(lt,"atoi(")||strstr(lt,"atoll("))!=NULL;
                    int rnumh = (strstr(rt,"_ltng_from_double(")||strstr(rt,"_ltng_from_int(")||strstr(rt,"_ltng_ad(")||strstr(rt,"_LTNG_FMOD_D(")||strstr(rt,"_LTNG_DIV_D(")||strstr(rt,"atof(")||strstr(rt,"atoi(")||strstr(rt,"atoll("))!=NULL;
                    const char *lq2=lt; while(*lq2==' '||*lq2=='\t') lq2++; const char *rq2=rt; while(*rq2==' '||*rq2=='\t') rq2++;
                    int lnl = (*lq2=='-'||*lq2=='+'||isdigit((unsigned char)*lq2));
                    int rnl = (*rq2=='-'||*rq2=='+'||isdigit((unsigned char)*rq2));
                    if ((lnumh && (rnumh || rnl)) || (rnumh && (lnumh || lnl))) {
                        normalize_value_expr(left, sizeof(left), array_access_used, table);
                        normalize_value_expr(right, sizeof(right), array_access_used, table);
                        snprintf(buf, buf_size, "(%s) %s (%s)", left, ops[i], right);
                        return;
                    }
                }
                /* Normalize both sides as string expressions (handle + / * concat/repeat) */
                /* In string comparison context, +/* on bare-buffer operands are string ops */
                {
                    int hp=0,hs=0,in=0; char qc=0;
                    for(const char *p=left;*p;p++){ if(in){if(*p=='\\'&&p[1]){p++;continue;} if(*p==qc)in=0; continue;} if(*p=='"'||*p=='\''){in=1;qc=*p;continue;} if(*p=='+')hp=1; if(*p=='*')hs=1; }
                    if(hp||hs) normalize_string_expr(left,sizeof(left),array_access_used, table);
                }
                {
                    int hp=0,hs=0,in=0; char qc=0;
                    for(const char *p=right;*p;p++){ if(in){if(*p=='\\'&&p[1]){p++;continue;} if(*p==qc)in=0; continue;} if(*p=='"'||*p=='\''){in=1;qc=*p;continue;} if(*p=='+')hp=1; if(*p=='*')hs=1; }
                    if(hp||hs) normalize_string_expr(right,sizeof(right),array_access_used, table);
                }
                normalize_value_expr(left, sizeof(left), array_access_used, table);
                normalize_value_expr(right, sizeof(right), array_access_used, table);
                /* Ensure numeric literals are quoted for strcmp */
                {
                    char lq[4096], rq[4096];
                    const char *lp = left; while (*lp==' '||*lp=='\t') lp++;
                    const char *rp = right; while (*rp==' '||*rp=='\t') rp++;
                    int ln = 0, rn = 0;
                    {
                        const char *q = lp;
                        while (*q=='(' || *q==' '||*q=='\t') q++;
                        if (*q=='-'||*q=='+') q++;
                        if (isdigit((unsigned char)*q)) { ln = 1; while (*q) { if (*q==')'||*q==' '||*q=='\t') { q++; continue; } if (!isdigit((unsigned char)*q) && *q!='.') { ln = 0; break; } q++; } }
                    }
                    {
                        const char *q = rp;
                        while (*q=='(' || *q==' '||*q=='\t') q++;
                        if (*q=='-'||*q=='+') q++;
                        if (isdigit((unsigned char)*q)) { rn = 1; while (*q) { if (*q==')'||*q==' '||*q=='\t') { q++; continue; } if (!isdigit((unsigned char)*q) && *q!='.') { rn = 0; break; } q++; } }
                    }
                    if (ln) snprintf(lq, sizeof(lq), "\"%s\"", lp); else { strncpy(lq, left, sizeof(lq)-1); lq[sizeof(lq)-1]=0; }
                    if (rn) snprintf(rq, sizeof(rq), "\"%s\"", rp); else { strncpy(rq, right, sizeof(rq)-1); rq[sizeof(rq)-1]=0; }
                    {
                        int lnumh = (strstr(left,"_ltng_from_double(")||strstr(left,"_ltng_from_int(")||strstr(left,"_ltng_ad(")||strstr(left,"_LTNG_FMOD_D(")||strstr(left,"_LTNG_DIV_D(")||strstr(left,"atof(")||strstr(left,"atoi(")||strstr(left,"atoll("))!=NULL;
                        int rnumh = (strstr(right,"_ltng_from_double(")||strstr(right,"_ltng_from_int(")||strstr(right,"_ltng_ad(")||strstr(right,"_LTNG_FMOD_D(")||strstr(right,"_LTNG_DIV_D(")||strstr(right,"atof(")||strstr(right,"atoi(")||strstr(right,"atoll("))!=NULL;
                        /* Numeric expression compared with quoted numeric string literal: compare numerically */
                        int lq_qnumlit = 0, rq_qnumlit = 0, lq_quoted = 0, rq_quoted = 0;
                        {
                            const char *lpq = lq; while (*lpq==' '||*lpq=='\t') lpq++;
                            if (*lpq=='"') { lq_quoted = 1; const char *p=lpq+1; if(*p=='+'||*p=='-') p++; int hd=0,ok=1,dot=0; while(*p && *p!='"'){ if(isdigit((unsigned char)*p)) hd=1; else if(*p=='.'&&!dot) dot=1; else {ok=0;break;} p++; } if(*p=='"' && hd && ok && p[1]=='\0') lq_qnumlit=1; }
                            const char *rpq = rq; while (*rpq==' '||*rpq=='\t') rpq++;
                            if (*rpq=='"') { rq_quoted = 1; const char *p=rpq+1; if(*p=='+'||*p=='-') p++; int hd=0,ok=1,dot=0; while(*p && *p!='"'){ if(isdigit((unsigned char)*p)) hd=1; else if(*p=='.'&&!dot) dot=1; else {ok=0;break;} p++; } if(*p=='"' && hd && ok && p[1]=='\0') rq_qnumlit=1; }
                        }
                        if ((lnumh && (rnumh || rn || rq_qnumlit)) || (rnumh && (lnumh || ln || lq_qnumlit))) {
                            /* if side is quoted numeric literal string, unquote for numeric compare */
                            char lcmp[4096], rcmp[4096];
                            strncpy(lcmp, left, sizeof(lcmp)-1); lcmp[sizeof(lcmp)-1]=0;
                            strncpy(rcmp, right, sizeof(rcmp)-1); rcmp[sizeof(rcmp)-1]=0;
                            if (lq_qnumlit) { const char *p=lq; while(*p==' '||*p=='\t') p++; if(*p=='"'){ size_t n=strlen(p); if(n>=2 && p[n-1]=='"'){ snprintf(lcmp,sizeof(lcmp),"%.*s",(int)(n-2),p+1); } } }
                            if (rq_qnumlit) { const char *p=rq; while(*p==' '||*p=='\t') p++; if(*p=='"'){ size_t n=strlen(p); if(n>=2 && p[n-1]=='"'){ snprintf(rcmp,sizeof(rcmp),"%.*s",(int)(n-2),p+1); } } }
                            snprintf(buf, buf_size, "(%s) %s (%s)", lcmp, ops[i], rcmp);
                        } else {
                            /* Ensure numeric helper expressions become string expressions for strcmp */
                            char *lsc = (char*)malloc(4200); char *rsc = (char*)malloc(4200);
                            if(!lsc||!rsc){
                                free(lsc); free(rsc);
                                snprintf(buf, buf_size, "strcmp(%s, %s) %s 0", lq, rq, ops[i]);
                            } else {
                                strncpy(lsc, lq, 4200-1); lsc[4200-1]=0;
                                strncpy(rsc, rq, 4200-1); rsc[4200-1]=0;
                                if (lnumh) {
                                    int is_f = (strstr(left,"atof(")||strstr(left,"_ltng_from_double(")||strstr(left,"_ltng_ad(")||strstr(left,"_LTNG_FMOD_D(")||strstr(left,"_LTNG_DIV_D("))!=NULL;
                                    if (is_f)
                                        snprintf(lsc, 4200, "({ _ltng_retbuf_fmt(\"%%g\",(double)(%s)); })", left);
                                    else
                                        snprintf(lsc, 4200, "({ _ltng_retbuf_fmt(\"%%lld\",(long long)(%s)); })", left);
                                }
                                if (rnumh) {
                                    int is_f = (strstr(right,"atof(")||strstr(right,"_ltng_from_double(")||strstr(right,"_ltng_ad(")||strstr(right,"_LTNG_FMOD_D(")||strstr(right,"_LTNG_DIV_D("))!=NULL;
                                    if (is_f)
                                        snprintf(rsc, 4200, "({ _ltng_retbuf_fmt(\"%%g\",(double)(%s)); })", right);
                                    else
                                        snprintf(rsc, 4200, "({ _ltng_retbuf_fmt(\"%%lld\",(long long)(%s)); })", right);
                                }
                                if (strcmp(ops[i], "==") == 0)
                                    snprintf(buf, buf_size, "(strcmp(%s, %s) == 0)", lsc, rsc);
                                else
                                    snprintf(buf, buf_size, "(strcmp(%s, %s) != 0)", lsc, rsc);
                                free(lsc); free(rsc);
                            }
                        }
                        /* fallback: numeric helper compared to numeric string literal should be numeric */
                        {
                                                        if ((strstr(lq,"_ltng_from_double(")||strstr(lq,"_ltng_from_int(")||strstr(lq,"_ltng_ad(")) && rq[0]=='"') {
                                /* rq is "num" */
                                char *num = (char*)malloc(128); if(num){ strncpy(num, rq+1, 127); num[127]=0; char *qe=strchr(num,'"'); if(qe) *qe=0;
                                int ok=1; char *nn=num; if(*nn=='-'||*nn=='+') nn++; if(!*nn) ok=0; for(char*t=nn;*t;t++){ if(!isdigit((unsigned char)*t)&&*t!='.') {ok=0;break;} }
                                if(ok) snprintf(buf, buf_size, "(%s) %s (%s)", left, ops[i], num); free(num); }
                            } else if ((strstr(rq,"_ltng_from_double(")||strstr(rq,"_ltng_from_int(")||strstr(rq,"_ltng_ad(")) && lq[0]=='"') {
                                char *num = (char*)malloc(128); if(num){ strncpy(num, lq+1, 127); num[127]=0; char *qe=strchr(num,'"'); if(qe) *qe=0;
                                int ok=1; char *nn=num; if(*nn=='-'||*nn=='+') nn++; if(!*nn) ok=0; for(char*t=nn;*t;t++){ if(!isdigit((unsigned char)*t)&&*t!='.') {ok=0;break;} }
                                if(ok) snprintf(buf, buf_size, "(%s) %s (%s)", num, ops[i], right); free(num); }
                            } else {
                                /* bool/int ternary expr compared with quoted "0"/"1" numeric literal: compare numerically */
                                int lq_qnum = (lq[0]=='"' && (strcmp(lq,"\"0\"")==0 || strcmp(lq,"\"1\"")==0));
                                int rq_qnum = (rq[0]=='"' && (strcmp(rq,"\"0\"")==0 || strcmp(rq,"\"1\"")==0));
                                int l_tern = (strstr(lq,"?1:0")!=NULL || strstr(lq,"? 1 : 0")!=NULL);
                                int r_tern = (strstr(rq,"?1:0")!=NULL || strstr(rq,"? 1 : 0")!=NULL);
                                if (lq_qnum && r_tern) snprintf(buf, buf_size, "(%s) %s (%s)", lq[1]=='1'?"1":"0", ops[i], rq);
                                else if (rq_qnum && l_tern) snprintf(buf, buf_size, "(%s) %s (%s)", lq, ops[i], rq[1]=='1'?"1":"0");
                            }
                        }
                    }
                }
            } else {
                normalize_value_expr(left, sizeof(left), array_access_used, table);
                normalize_value_expr(right, sizeof(right), array_access_used, table);
                /* Unwrap numeric-string helper results for numeric comparisons */
                {
                    char *sides[2] = {left, right};
                    for (int si = 0; si < 2; si++) {
                        char *s = sides[si];
                        char *p = s; while (*p==' '||*p=='\t') p++;
                        if (strncmp(p, "_ltng_from_double(_ltng_retbuf(), ", 34) == 0) {
                            /* extract inner expr up to matching final ) */
                            const char *inner = p + 34;
                            const char *end = s + strlen(s);
                            if (end > p && *(end-1) == ')') {
                                char tmp[4096]; size_t n = (size_t)((end-1) - inner);
                                if (n >= sizeof(tmp)) n = sizeof(tmp)-1;
                                memcpy(tmp, inner, n); tmp[n] = 0;
                                memmove(s, tmp, strlen(tmp)+1);
                            }
                        } else if (strncmp(p, "_ltng_from_int(_ltng_retbuf(), ", 32) == 0) {
                            const char *inner = p + 32;
                            const char *end = s + strlen(s);
                            if (end > p && *(end-1) == ')') {
                                char tmp[4096]; size_t n = (size_t)((end-1) - inner);
                                if (n >= sizeof(tmp)) n = sizeof(tmp)-1;
                                memcpy(tmp, inner, n); tmp[n] = 0;
                                memmove(s, tmp, strlen(tmp)+1);
                            }
                        } else if (strncmp(p, "_ltng_ad(", 9) == 0) {
                            const char *inner = p + 9;
                            const char *end = s + strlen(s);
                            if (end > p && *(end-1) == ')') {
                                char tmp[4096]; size_t n = (size_t)((end-1) - inner);
                                if (n >= sizeof(tmp)) n = sizeof(tmp)-1;
                                memcpy(tmp, inner, n); tmp[n] = 0;
                                memmove(s, tmp, strlen(tmp)+1);
                            }
                        }
                    }
                }
                /* Handle char*-returning calls compared with numeric literals: compare as strings */
                if (strcmp(ops[i], "==") == 0 || strcmp(ops[i], "!=") == 0) {
                    int ln = 0, rn = 0, lc = 0, rc = 0;
                    {
                        const char *q = left; while (*q==' '||*q=='\t') q++;
                        if (*q=='-'||*q=='+') q++;
                        if (isdigit((unsigned char)*q)) { ln = 1; while (*q) { if (!isdigit((unsigned char)*q) && *q!='.') { ln = 0; break; } q++; } }
                        q = right; while (*q==' '||*q=='\t') q++;
                        if (*q=='-'||*q=='+') q++;
                        if (isdigit((unsigned char)*q)) { rn = 1; while (*q) { if (!isdigit((unsigned char)*q) && *q!='.') { rn = 0; break; } q++; } }
                        q = left; while (*q==' '||*q=='\t') q++;
                        if (*q=='_'||isalpha((unsigned char)*q)) {
                            const char *fn = q;
                            while (*q && (isalnum((unsigned char)*q)||*q=='_')) q++;
                            int fnlen = (int)(q - fn);
                            while (*q==' '||*q=='\t') q++;
                            if (*q=='(') {
                                /* Exclude numeric conversion / numeric-string helpers */
                                if (!((fnlen==4 && strncmp(fn,"atoi",4)==0) ||
                                      (fnlen==4 && strncmp(fn,"atof",4)==0) ||
                                      (fnlen==5 && strncmp(fn,"atoll",5)==0) ||
                                      (fnlen==18 && strncmp(fn,"_ltng_from_double",18)==0) ||
                                      (fnlen==15 && strncmp(fn,"_ltng_from_int",15)==0) ||
                                      (fnlen==9 && strncmp(fn,"_ltng_ad",9)==0) ||
                                      (fnlen==12 && strncmp(fn,"_LTNG_FMOD_D",12)==0) ||
                                      (fnlen==11 && strncmp(fn,"_LTNG_DIV_D",11)==0)))
                                    lc = 1;
                            }
                        }
                        q = right; while (*q==' '||*q=='\t') q++;
                        if (*q=='_'||isalpha((unsigned char)*q)) {
                            const char *fn = q;
                            while (*q && (isalnum((unsigned char)*q)||*q=='_')) q++;
                            int fnlen = (int)(q - fn);
                            while (*q==' '||*q=='\t') q++;
                            if (*q=='(') {
                                if (!((fnlen==4 && strncmp(fn,"atoi",4)==0) ||
                                      (fnlen==4 && strncmp(fn,"atof",4)==0) ||
                                      (fnlen==5 && strncmp(fn,"atoll",5)==0) ||
                                      (fnlen==18 && strncmp(fn,"_ltng_from_double",18)==0) ||
                                      (fnlen==15 && strncmp(fn,"_ltng_from_int",15)==0) ||
                                      (fnlen==9 && strncmp(fn,"_ltng_ad",9)==0) ||
                                      (fnlen==12 && strncmp(fn,"_LTNG_FMOD_D",12)==0) ||
                                      (fnlen==11 && strncmp(fn,"_LTNG_DIV_D",11)==0)))
                                    rc = 1;
                            }
                        }
                    }
                    if ((ln && rc) || (rn && lc)) {
                        char lq[4096], rq[4096];
                        if (ln) snprintf(lq, sizeof(lq), "\"%s\"", left); else { strncpy(lq,left,sizeof(lq)-1); lq[sizeof(lq)-1]=0; }
                        if (rn) snprintf(rq, sizeof(rq), "\"%s\"", right); else { strncpy(rq,right,sizeof(rq)-1); rq[sizeof(rq)-1]=0; }
                        if (strcmp(ops[i], "==") == 0)
                            snprintf(buf, buf_size, "(strcmp(%s, %s) == 0)", lq, rq);
                        else
                            snprintf(buf, buf_size, "(strcmp(%s, %s) != 0)", lq, rq);
                        return;
                    }
                }
                snprintf(buf, buf_size, "(%s) %s (%s)", left, ops[i], right);
            }
            return;
        }
    }
    /* No top-level logical/comparison op: treat as value expr */
    normalize_value_expr(buf, buf_size, array_access_used, table);
}

static void normalize_mixed_expr(char *buf, int buf_size, int array_access_used, SymbolTable *table) {
    trim_c_expr(buf);
    /* Unwrap fully-enclosing outer parens if they wrap the whole expr? Not necessary;
       but split_top_level works on current level. Handle leading unary ! at top? */
    normalize_or_expr(buf, buf_size, array_access_used, table);
    /* Fix: strcmp(numeric_helper, "num") / strcmp("num", numeric_helper) -> numeric compare */
    for (int pass = 0; pass < 4; pass++) {
        char *s = buf;
        char *p = strstr(s, "strcmp(");
        if (!p) break;
        /* find matching end of strcmp(...) == 0 or != 0 */
        /* parse args: strcmp(arg1, arg2) */
        char *a1 = p + 7;
        int d = 1, inq = 0; char qc = 0; char *comma = NULL;
        char *q = a1;
        for (; *q && d > 0; q++) {
            char c = *q;
            if (inq) { if (c == '\\' && q[1]) { q++; continue; } if (c == qc) inq = 0; continue; }
            if (c == '"' || c == '\'') { inq = 1; qc = c; continue; }
            if (c == '(') d++;
            else if (c == ')') { d--; if (d == 0) { break; } }
            else if (c == ',' && d == 1 && !comma) comma = q;
        }
        if (d != 0 || !comma) break;
        char *endparen = q; /* at ')' */
        char *after = endparen + 1;
        while (*after == ' ' || *after == '\t') after++;
        int is_eq = 0, is_ne = 0;
        if (strncmp(after, "== 0", 4) == 0) is_eq = 1;
        else if (strncmp(after, "!= 0", 4) == 0) is_ne = 1;
        else break;
        char *endcmp = after + 4;
        /* extract args */
        char leftarg[4096], rightarg[4096];
        int l1 = (int)(comma - a1); if (l1 >= (int)sizeof(leftarg)) l1 = sizeof(leftarg)-1;
        memcpy(leftarg, a1, l1); leftarg[l1] = 0;
        char *a2 = comma + 1; while (*a2 == ' ' || *a2 == '\t') a2++;
        int l2 = (int)(endparen - a2); if (l2 >= (int)sizeof(rightarg)) l2 = sizeof(rightarg)-1;
        memcpy(rightarg, a2, l2); rightarg[l2] = 0;
        trim_c_expr(leftarg); trim_c_expr(rightarg);
        int lnumh = (strstr(leftarg,"_ltng_from_double(")||strstr(leftarg,"_ltng_from_int(")||strstr(leftarg,"_ltng_ad("))!=NULL;
        int rnumh = (strstr(rightarg,"_ltng_from_double(")||strstr(rightarg,"_ltng_from_int(")||strstr(rightarg,"_ltng_ad("))!=NULL;
        char *numside = (char*)malloc(128); if(!numside) numside=(char*)calloc(1,1); int have = 0; char *helperside = NULL; int helper_left = 0;
        if (lnumh && rightarg[0]=='"') {
            strncpy(numside, rightarg+1, 127); numside[127]=0; char *qe=strchr(numside,'"'); if(qe)*qe=0; helperside=leftarg; helper_left=1; have=1;
        } else if (rnumh && leftarg[0]=='"') {
            strncpy(numside, leftarg+1, 127); numside[127]=0; char *qe=strchr(numside,'"'); if(qe)*qe=0; helperside=rightarg; helper_left=0; have=1;
        }
        if (have) {
            int ok=1; char *nn=numside; if(*nn=='-'||*nn=='+') nn++; if(!*nn) ok=0; for(char*t=nn;*t;t++){ if(!isdigit((unsigned char)*t)&&*t!='.') {ok=0;break;} }
            if (ok) {
                /* unwrap helper to numeric expr */
                char *h = (char*)malloc(4096); if(!h) h=(char*)calloc(1,1); strncpy(h, helperside, 4096-1); h[4096-1]=0; trim_c_expr(h);
                char *repl = (char*)malloc(8192); if(!repl) repl=(char*)calloc(1,1);
                if (helper_left) snprintf(repl,8192,"((double)atof(%s) %s (%s))", h, is_eq?"==":"!=", numside);
                else snprintf(repl,8192,"((%s) %s (double)atof(%s))", numside, is_eq?"==":"!=", h);
                /* replace from p to endcmp */
                char *newbuf = (char*)malloc(16384); if(!newbuf){free(repl); continue;}
                int prefix = (int)(p - buf);
                snprintf(newbuf,16384,"%.*s%s%s", prefix, buf, repl, endcmp);
                strncpy(buf,newbuf,buf_size-1); buf[buf_size-1]=0;
                free(h); free(repl); free(newbuf);
                continue;
            }
        }
        break;
    }
}


static VarType collect_expression(Lexer *lexer, SymbolTable *table,
                                  char *out, int out_size,
                                  int stop_on_rparen, int stop_on_comma,
                                  int *out_sl, int *out_sc, int *out_el, int *out_ec) {
    return collect_expression_ex(lexer, table, out, out_size,
                                 stop_on_rparen, stop_on_comma,
                                 out_sl, out_sc, out_el, out_ec, 0);
}

static VarType collect_expression_ex(Lexer *lexer, SymbolTable *table,
                                     char *out, int out_size,
                                     int stop_on_rparen, int stop_on_comma,
                                     int *out_sl, int *out_sc, int *out_el, int *out_ec,
                                     int stop_on_ternary_colon) {
    int pos = 0;
    VarType result_type = TYPE_INT;
    int paren_depth = 0;
    int type_locked = 0;
    int arithmetic_used = 0;
    int has_string_concat = 0;
    int has_string_mult = 0;
    int array_access_used = 0;
    int has_dynamic_numeric = 0; /* unlocked numeric/unknown operands needing runtime add semantics */
    int sl=0, sc=0, el=0, ec=0, have_span=0;
    int pending_divmod = 0; /* 1 if last emitted operator was / or % in numeric context */
    int pending_divmod_is_mod = 0;
    int pending_divmod_line = 0, pending_divmod_col = 0;
    int pending_mod_op_pos = -1; /* output position of % char for float-fmod rewrite */
    int comparison_or_logical = 0;
    int depth_content_start[64];
    int depth_in_str_concat[64]; /* 1 if this paren depth is inside an outer string-concat context */
    depth_content_start[0] = 0;
    depth_in_str_concat[0] = 0;

    while (lexer->current < lexer->count) {
        Token tok = lexer_peek(lexer);
        if (tok.type == TOKEN_NEWLINE || tok.type == TOKEN_EOF) break;
        if (tok.type == TOKEN_LBRACE) break;
        if (tok.type == TOKEN_RBRACE && paren_depth == 0) break;
        if ((tok.type == TOKEN_RPAREN || tok.type == TOKEN_RBRACKET) && paren_depth == 0 && stop_on_rparen) break;
        if (tok.type == TOKEN_COMMA && stop_on_comma) break;
        if (tok.type == TOKEN_COLON && paren_depth == 0 && stop_on_ternary_colon) break;
        if (tok.type == TOKEN_FROM || tok.type == TOKEN_TO || tok.type == TOKEN_STEP) break;

        tok = lexer_next(lexer);
        if (!have_span) { sl = tok.line; sc = tok.col; have_span = 1; }
        el = tok.line;
        ec = tok.col + (int)strlen(tok.value);
        if (tok.type == TOKEN_STRING) ec = tok.col + (int)strlen(tok.value) + 2;
        else if (tok.type == TOKEN_CHAR) ec = tok.col + (int)strlen(tok.value) + 2;

        switch (tok.type) {
            case TOKEN_QUESTION: {
                {
                    /* Ternary conditional: cond ? true_expr : false_expr
                       Condition is the expression collected so far at this depth. */
                    int cstart = (paren_depth >= 0 && paren_depth < 64) ? depth_content_start[paren_depth] : 0;
                    if (cstart < 0) cstart = 0;
                    if (cstart > pos) cstart = 0;
                    char cond_buf[MAX_STRING_LEN];
                    int clen = pos - cstart;
                    if (clen < 0) clen = 0;
                    if (clen >= (int)sizeof(cond_buf)) clen = (int)sizeof(cond_buf) - 1;
                    memcpy(cond_buf, out + cstart, clen);
                    cond_buf[clen] = '\0';
                    VarType cond_type = result_type;

                    /* Post-process condition (subset of collect_expression end processing) */
                    sanitize_mod_atof(cond_buf, sizeof(cond_buf));
                    {
                        int _has_cmp = 0, _d = 0, _in = 0; char _qc = 0;
                        for (const char *_p = cond_buf; *_p; _p++) {
                            if (_in) { if (*_p == '\\' && _p[1]) { _p++; continue; } if (*_p == _qc) _in = 0; continue; }
                            if (*_p == '"' || *_p == '\'') { _in = 1; _qc = *_p; continue; }
                            if (*_p == '(' || *_p == '{' || *_p == '[') { _d++; continue; }
                            if (*_p == ')' || *_p == '}' || *_p == ']') { if (_d > 0) _d--; continue; }
                            if (_d == 0) {
                                if ((_p[0]=='='&&_p[1]=='=') || (_p[0]=='!'&&_p[1]=='=') ||
                                    (_p[0]=='<'&&_p[1]=='=') || (_p[0]=='>'&&_p[1]=='=') ||
                                    (_p[0]=='&'&&_p[1]=='&') || (_p[0]=='|'&&_p[1]=='|')) { _has_cmp = 1; break; }
                                if (*_p == '<' || *_p == '>') { _has_cmp = 1; break; }
                            }
                        }
                        if (_has_cmp) {
                            normalize_mixed_expr(cond_buf, sizeof(cond_buf), array_access_used, table);
                            cond_type = TYPE_INT;
                        }
                    }

                    /* Wrap string/dynamic conditions for truthiness */
                    {
                        int need_truthy = 0;
                        if (cond_type == TYPE_STRING || cond_type == TYPE_CHAR || cond_type == TYPE_OBJECT || cond_type == TYPE_NULL) {
                            need_truthy = 1;
                        }
                        /* Dynamic/untyped params emitted as atof/atoll(var) or raw var need string truthiness */
                        if (!need_truthy && has_dynamic_numeric) {
                            const char *cs = cond_buf;
                            while (*cs == ' ' || *cs == '\t') cs++;
                            /* check for atof(name) or atoll(name) or plain identifier */
                            const char *inner = NULL; int inner_len = 0;
                            if (strncmp(cs, "atof(", 5) == 0) {
                                inner = cs + 5;
                            } else if (strncmp(cs, "atoll(", 6) == 0) {
                                inner = cs + 6;
                            }
                            if (inner) {
                                const char *p = inner;
                                while (isalnum((unsigned char)*p) || *p == '_') p++;
                                inner_len = (int)(p - inner);
                                if (inner_len > 0 && *p == ')' && *(p+1) == '\0') {
                                    need_truthy = 1;
                                }
                            } else {
                                /* plain identifier */
                                const char *p = cs;
                                if (isalpha((unsigned char)*p) || *p == '_') {
                                    while (isalnum((unsigned char)*p) || *p == '_') p++;
                                    if (p != cs && *p == '\0') need_truthy = 1;
                                }
                            }
                            /* extract variable name for truthy wrapping */
                            if (need_truthy) {
                                char vname[MAX_IDENTIFIER_LEN] = {0};
                                if (strncmp(cs, "atof(", 5) == 0) {
                                    const char *p = cs + 5; int n = 0;
                                    while ((isalnum((unsigned char)*p) || *p == '_') && n < MAX_IDENTIFIER_LEN-1) vname[n++] = *p++;
                                    vname[n] = '\0';
                                } else if (strncmp(cs, "atoll(", 6) == 0) {
                                    const char *p = cs + 6; int n = 0;
                                    while ((isalnum((unsigned char)*p) || *p == '_') && n < MAX_IDENTIFIER_LEN-1) vname[n++] = *p++;
                                    vname[n] = '\0';
                                } else {
                                    strncpy(vname, cs, MAX_IDENTIFIER_LEN-1);
                                }
                                if (vname[0]) {
                                    snprintf(cond_buf, sizeof(cond_buf), "_ltng_str_truthy(%s)", vname);
                                }
                            }
                        }
                        if (need_truthy && !(has_dynamic_numeric && cond_type != TYPE_STRING && cond_type != TYPE_CHAR && cond_type != TYPE_OBJECT && cond_type != TYPE_NULL)) {
                            /* already handled above for dynamic numeric case, or handle string/char/object/null */
                        }
                        if (!need_truthy) { /* nothing */ }
                        else if (cond_type == TYPE_STRING || cond_type == TYPE_CHAR || cond_type == TYPE_OBJECT || cond_type == TYPE_NULL) {
                            char wrapped[MAX_STRING_LEN];
                            snprintf(wrapped, sizeof(wrapped), "_ltng_str_truthy(%s)", cond_buf);
                            strncpy(cond_buf, wrapped, sizeof(cond_buf) - 1);
                            cond_buf[sizeof(cond_buf) - 1] = '\0';
                        }
                    }

                    /* Collect true branch (stops at matching ':') */
                    char true_buf[MAX_STRING_LEN];
                    true_buf[0] = '\0';
                    VarType true_type = collect_expression_ex(lexer, table, true_buf, sizeof(true_buf),
                                                              1, 0,
                                                              NULL, NULL, NULL, NULL, 1);

                    /* Consume ':' separator */
                    if (lexer->current < lexer->count && lexer_peek(lexer).type == TOKEN_COLON) {
                        lexer_next(lexer);
                    } else {
                        error_state_add(parser_errors, tok.line, tok.col,
                                        "Expected ':' in ternary expression");
                    }

                    /* Collect false branch (conditional-expression, inherits stop_on_ternary_colon) */
                    char false_buf[MAX_STRING_LEN];
                    false_buf[0] = '\0';
                    VarType false_type = collect_expression_ex(lexer, table, false_buf, sizeof(false_buf),
                                                               1, stop_on_comma,
                                                               NULL, NULL, NULL, NULL, stop_on_ternary_colon);

                    /* Unwrap dynamic/untyped params emitted as atof/atoll(var) to preserve string values */
                    {
                        char *bufs[2] = {true_buf, false_buf};
                        VarType *types[2] = {&true_type, &false_type};
                        for (int bi = 0; bi < 2; bi++) {
                            const char *cs = bufs[bi]; while (*cs==' '||*cs=='\t') cs++;
                            const char *inner = NULL; int off = 0;
                            if (strncmp(cs, "atof(", 5) == 0) { inner = cs + 5; off = 5; }
                            else if (strncmp(cs, "atoll(", 6) == 0) { inner = cs + 6; off = 6; }
                            else if (strncmp(cs, "(double)atoll(", 14) == 0) { inner = cs + 14; off = 14; }
                            if (inner) {
                                const char *p = inner; char vname[MAX_IDENTIFIER_LEN]; int n = 0;
                                while ((isalnum((unsigned char)*p) || *p == '_') && n < MAX_IDENTIFIER_LEN-1) vname[n++] = *p++;
                                vname[n] = '\0';
                                int close_ok = 0;
                                if (off == 14) { /* (double)atoll(name) */ if (*p==')' && *(p+1)==')' && *(p+2)=='\0') close_ok = 1; }
                                else { if (*p==')' && *(p+1)=='\0') close_ok = 1; }
                                if (n > 0 && close_ok) {
                                    char mangled_v[MAX_IDENTIFIER_LEN]; mangle_name(vname, mangled_v, MAX_IDENTIFIER_LEN);
                                    int is_dyn = 0;
                                    if (symbol_table_has(table, mangled_v) && symbol_table_is_param(table, mangled_v) &&
                                        !symbol_table_get_locked(table, mangled_v)) {
                                        VarType st = symbol_table_get(table, mangled_v);
                                        VarType pdt = effective_param_default_type(vname, st);
                                        if (pdt == st) is_dyn = 1; /* untyped default */
                                    } else if (symbol_table_has(table, vname) && symbol_table_is_param(table, vname) &&
                                               !symbol_table_get_locked(table, vname)) {
                                        VarType st = symbol_table_get(table, vname);
                                        VarType pdt = effective_param_default_type(vname, st);
                                        if (pdt == st) is_dyn = 1;
                                    }
                                    if (is_dyn) {
                                        const char *cname_v = NULL;
                                        if (symbol_table_has(table, mangled_v)) cname_v = symbol_table_get_cname(table, mangled_v);
                                        else if (symbol_table_has(table, vname)) cname_v = symbol_table_get_cname(table, vname);
                                        if (!cname_v) cname_v = mangled_v;
                                        strncpy(bufs[bi], cname_v, MAX_STRING_LEN - 1);
                                        bufs[bi][MAX_STRING_LEN - 1] = '\0';
                                        *types[bi] = TYPE_STRING; /* treat as dynamic/string */
                                    }
                                }
                            }
                        }
                    }

                    /* Determine result type with numeric promotion */
                    VarType res_type = TYPE_STRING;
                    if (true_type == TYPE_BOOL && false_type == TYPE_BOOL) {
                        res_type = TYPE_BOOL;
                    } else if ((true_type == TYPE_INT || true_type == TYPE_BOOL) &&
                               (false_type == TYPE_INT || false_type == TYPE_BOOL)) {
                        res_type = TYPE_INT;
                    } else if ((true_type == TYPE_INT || true_type == TYPE_FLOAT || true_type == TYPE_BOOL) &&
                               (false_type == TYPE_INT || false_type == TYPE_FLOAT || false_type == TYPE_BOOL)) {
                        res_type = TYPE_FLOAT;
                    } else if (true_type == TYPE_FLOAT && false_type == TYPE_FLOAT) {
                        res_type = TYPE_FLOAT;
                    } else if (true_type == TYPE_STRING && false_type == TYPE_STRING) {
                        res_type = TYPE_STRING;
                    } else if (true_type == TYPE_CHAR && false_type == TYPE_CHAR) {
                        res_type = TYPE_CHAR;
                    } else if ((true_type == TYPE_STRING || false_type == TYPE_STRING ||
                                true_type == TYPE_CHAR || false_type == TYPE_CHAR ||
                                true_type == TYPE_NULL || false_type == TYPE_NULL ||
                                true_type == TYPE_OBJECT || false_type == TYPE_OBJECT ||
                                true_type == TYPE_ARRAY || false_type == TYPE_ARRAY)) {
                        res_type = TYPE_STRING;
                    } else {
                        res_type = TYPE_STRING;
                    }

                    /* Build combined expression, replacing from condition start at this depth */
                    int _saved_has_string_concat = has_string_concat;
                    int _saved_has_string_mult = has_string_mult;
                    int _saved_array_access_used = array_access_used;
                    int _saved_arithmetic_used = arithmetic_used;
                    VarType _saved_result_type = result_type;
                    pos = cstart;
                    pos += snprintf(out + pos, out_size - pos, "(%s?(%s):(%s))", cond_buf, true_buf, false_buf);
                    if (pos >= out_size) pos = out_size - 1;
                    out[pos] = '\0';
                    result_type = res_type;
                    type_locked = 0;
                    arithmetic_used = _saved_arithmetic_used;
                    comparison_or_logical = 0;
                    has_string_concat = _saved_has_string_concat;
                    has_string_mult = _saved_has_string_mult;
                    array_access_used = _saved_array_access_used;
                    has_dynamic_numeric = 0;
                    pending_divmod = 0; pending_mod_op_pos = -1;
                    /* If this ternary is inside an outer string-concat context, the overall
                       expression remains a string expression (ternary result will be stringified). */
                    if (_saved_has_string_concat || _saved_has_string_mult) {
                        has_string_concat = _saved_has_string_concat;
                    }
                    /* At paren_depth 0 the ternary consumed the rest of this expression level */
                    if (paren_depth == 0) {
                        if (out_sl) *out_sl = sl; if (out_sc) *out_sc = sc;
                        if (out_el) *out_el = el; if (out_ec) *out_ec = ec;
                        return result_type;
                    }
                    /* Inside parens: continue so outer paren/ops are processed */
                    continue;
                }
            }
            case TOKEN_PLUS_PLUS:
            case TOKEN_MINUS_MINUS: {
                /* Pre-increment/decrement as expression: ++x or --x
                   Generates a C statement expression that converts to number,
                   increments/decrements, stores back as string, returns result. */
                int is_inc = (tok.type == TOKEN_PLUS_PLUS);
                Token name = lexer_next(lexer);
                if (name.type != TOKEN_IDENT && !token_is_name(name)) {
                    error_state_add(parser_errors, tok.line, tok.col,
                        "Expected variable name after '++' or '--'");
                    break;
                }
                if (is_ltng_reserved(name.value)) {
                    error_state_add(parser_errors, name.line, name.col,
                        "Cannot use reserved word as variable name");
                    break;
                }
                char mangled[MAX_IDENTIFIER_LEN];
                mangle_name(name.value, mangled, MAX_IDENTIFIER_LEN);
                if (!symbol_table_has(table, mangled)) {
                    error_state_add(parser_errors, name.line, name.col,
                        "Variable not declared");
                    break;
                }
                VarType vt = symbol_table_get(table, mangled);
                if (vt == TYPE_FLOAT) {
                    if (is_inc)
                        pos += snprintf(out + pos, out_size - pos,
                            "({ double _n = (double)(atof(%s) + 1.0); snprintf(%s, sizeof(%s), \"%%g\", _n); _n; })",
                            mangled, mangled, mangled);
                    else
                        pos += snprintf(out + pos, out_size - pos,
                            "({ double _n = (double)(atof(%s) - 1.0); snprintf(%s, sizeof(%s), \"%%g\", _n); _n; })",
                            mangled, mangled, mangled);
                    if (!type_locked) result_type = TYPE_FLOAT;
                } else if (vt == TYPE_BOOL) {
                    if (is_inc)
                        pos += snprintf(out + pos, out_size - pos,
                            "({ long long _n = (long long)((%s[0]=='t'||%s[0]=='1'?1:0) + 1); snprintf(%s, sizeof(%s), \"%%lld\", _n); _n; })",
                            mangled, mangled, mangled, mangled);
                    else
                        pos += snprintf(out + pos, out_size - pos,
                            "({ long long _n = (long long)((%s[0]=='t'||%s[0]=='1'?1:0) - 1); snprintf(%s, sizeof(%s), \"%%lld\", _n); _n; })",
                            mangled, mangled, mangled, mangled);
                    if (!type_locked) result_type = TYPE_INT;
                } else {
                    if (is_inc)
                        pos += snprintf(out + pos, out_size - pos,
                            "({ long long _n = (long long)(atoll(%s) + 1); snprintf(%s, sizeof(%s), \"%%lld\", _n); _n; })",
                            mangled, mangled, mangled);
                    else
                        pos += snprintf(out + pos, out_size - pos,
                            "({ long long _n = (long long)(atoll(%s) - 1); snprintf(%s, sizeof(%s), \"%%lld\", _n); _n; })",
                            mangled, mangled, mangled);
                    if (!type_locked) result_type = TYPE_INT;
                }
                break;
            }
            case TOKEN_INT_LIT:
                if (pending_divmod && (strcmp(tok.value, "0") == 0)) {
                    error_state_add(parser_errors, tok.line, tok.col,
                        "Division or modulo by zero");
                }
                if (pending_divmod) {
                    int _is_fmod = (result_type == TYPE_FLOAT && pending_divmod_is_mod);
                    int _mop = pending_mod_op_pos;
                    if (_is_fmod && (_mop < 0 || _mop >= pos || out[_mop] != '%')) {
                        for (int _i = pos - 1; _i >= 0; _i--) { if (out[_i] == '%') { _mop = _i; break; } }
                    }
                    if (_is_fmod && _mop >= 0 && _mop < pos && out[_mop] == '%') {
                        char *_left = (char*)malloc(4096); if(_left){ int _llen = _mop; if (_llen >= 4096) _llen = 4095;
                        memcpy(_left, out, _llen); _left[_llen] = 0;
                        pos = 0;
                        pos += snprintf(out + pos, out_size - pos, "_LTNG_FMOD_D(%s, %s, %d, %d)", _left, tok.value, pending_divmod_line, pending_divmod_col); free(_left); }
                        if (!type_locked) result_type = TYPE_FLOAT;
                    } else {
                        pos += snprintf(out + pos, out_size - pos, "(%s(%s,%d,%d))",
                            (result_type == TYPE_FLOAT ? (pending_divmod_is_mod?"_LTNG_NZMOD_D":"_LTNG_NZDIV_D") : (pending_divmod_is_mod?"_LTNG_NZMOD_LL":"_LTNG_NZDIV_LL")),
                            tok.value, pending_divmod_line, pending_divmod_col);
                    }
                } else {
                    pos += snprintf(out + pos, out_size - pos, "%s", tok.value);
                }
                if (!type_locked && result_type == TYPE_BOOL) result_type = TYPE_INT;
                pending_divmod = 0; pending_mod_op_pos = -1;
                break;
            case TOKEN_FLOAT_LIT:
                if (pending_divmod && atof(tok.value) == 0.0) {
                    error_state_add(parser_errors, tok.line, tok.col,
                        "Division or modulo by zero");
                }
                if (pending_divmod) {
                    int _is_fmod = pending_divmod_is_mod;
                    if (_is_fmod && pending_mod_op_pos >= 0 && pending_mod_op_pos < pos && out[pending_mod_op_pos] == '%') {
                        char *_left = (char*)malloc(4096); if(_left){ int _llen = pending_mod_op_pos; if (_llen >= 4096) _llen = 4095;
                        memcpy(_left, out, _llen); _left[_llen] = 0;
                        pos = 0;
                        pos += snprintf(out + pos, out_size - pos, "_LTNG_FMOD_D(%s, %s, %d, %d)", _left, tok.value, pending_divmod_line, pending_divmod_col); free(_left); }
                        if (!type_locked) result_type = TYPE_FLOAT;
                    } else {
                        pos += snprintf(out + pos, out_size - pos, "(%s(%s,%d,%d))",
                            pending_divmod_is_mod ? "_LTNG_NZMOD_D" : "_LTNG_NZDIV_D",
                            tok.value, pending_divmod_line, pending_divmod_col);
                    }
                } else {
                    pos += snprintf(out + pos, out_size - pos, "%s", tok.value);
                }
                if (!type_locked) result_type = TYPE_FLOAT;
                pending_divmod = 0; pending_mod_op_pos = -1;
                break;
            case TOKEN_STRING:
                {
                    if (pos < out_size - 1) out[pos++] = '"';
                    for (const char *sp = tok.value; *sp && pos < out_size - 2; sp++) {
                        unsigned char c = (unsigned char)*sp;
                        if (c == '\\') { out[pos++]='\\'; out[pos++]='\\'; }
                        else if (c == '"') { out[pos++]='\\'; out[pos++]='"'; }
                        else if (c == '\n') { out[pos++]='\\'; out[pos++]='n'; }
                        else if (c == '\t') { out[pos++]='\\'; out[pos++]='t'; }
                        else if (c == '\r') { out[pos++]='\\'; out[pos++]='r'; }
                        else { out[pos++] = c; }
                    }
                    if (pos < out_size - 1) out[pos++] = '"';
                    out[pos] = '\0';
                    if (!type_locked) result_type = TYPE_STRING;
                }
                break;
            case TOKEN_CHAR:
                if (tok.value[0] != '\0') {
                    pos += snprintf(out + pos, out_size - pos, "\"%s\"", tok.value);
                } else {
                    pos += snprintf(out + pos, out_size - pos, "\"\"");
                }
                if (!type_locked) result_type = TYPE_STRING;
                break;
            case TOKEN_IDENT: {
                int saved_pos = pos;
                int _pd_save = pending_divmod, _pd_mod_save = pending_divmod_is_mod;
                int _pd_l_save = pending_divmod_line, _pd_c_save = pending_divmod_col;
                int _pd_mop_save = pending_mod_op_pos;
                pending_divmod = 0; pending_mod_op_pos = -1; /* prevent leaking into sub-parsing; restore for final operand emission if needed */
                /* Type conversion functions: toint/tofloat/tostring/tobool/tochar and aliases */
                if ((is_type_conversion(tok.value) ||
                     strcmp(tok.value, "int") == 0 || strcmp(tok.value, "float") == 0 ||
                     strcmp(tok.value, "string") == 0 || strcmp(tok.value, "bool") == 0 ||
                     strcmp(tok.value, "char") == 0) && lexer_peek(lexer).type == TOKEN_LPAREN) {
                    int _ck_int = (strcmp(tok.value,"toint")==0||strcmp(tok.value,"int")==0);
                    int _ck_float = (strcmp(tok.value,"tofloat")==0||strcmp(tok.value,"float")==0);
                    int _ck_str = (strcmp(tok.value,"tostring")==0||strcmp(tok.value,"string")==0);
                    int _ck_bool = (strcmp(tok.value,"tobool")==0||strcmp(tok.value,"bool")==0);
                    int _ck_char = (strcmp(tok.value,"tochar")==0||strcmp(tok.value,"char")==0);
                    lexer_next(lexer); /* consume ( */
                    char inner[MAX_STRING_LEN];
                    int ipos = 0;
                    int depth = 1;
                    while (lexer->current < lexer->count && depth > 0) {
                        Token itok = lexer_peek(lexer);
                        if (itok.type == TOKEN_LPAREN) depth++;
                        else if (itok.type == TOKEN_RPAREN) { depth--; if (depth == 0) break; }
                        else if (itok.type == TOKEN_NEWLINE || itok.type == TOKEN_EOF) break;
                        itok = lexer_next(lexer);
                        token_append_source(inner, &ipos, MAX_STRING_LEN, itok);
                    }
                    /* Trim trailing whitespace from inner */
                    while (ipos > 0 && (inner[ipos-1] == ' ' || inner[ipos-1] == '\t')) ipos--;
                    inner[ipos] = '\0';

                    if (lexer->current < lexer->count && lexer_peek(lexer).type == TOKEN_RPAREN) {
                        lexer_next(lexer); /* consume ) */
                    }

                    /* Helper: check if inner is a simple number literal */
                    int is_num_lit = 0;
                    int num_has_dot = 0;
                    if (ipos >= 1) {
                        const char *p = inner;
                        if (*p == '-' || *p == '+') p++;
                        if (*p != '\0') {
                            int has_digit = 0;
                            for (; *p; p++) {
                                if (isdigit((unsigned char)*p)) has_digit = 1;
                                else if (*p == '.') { num_has_dot = 1; }
                                else { has_digit = 0; break; }
                            }
                            if (has_digit) is_num_lit = 1;
                        }
                    }
                    (void)num_has_dot;

                    /* Helper: check if inner is a simple quoted string literal */
                    int is_simple_str_lit = 0;
                    int lit_start = 0, lit_end = 0; /* indices of content inside quotes */
                    if (ipos >= 2 && inner[0] == '"') {
                        int end_quote = -1;
                        for (int i = 1; i < ipos; i++) {
                            if (inner[i] == '\\' && i + 1 < ipos) i++;
                            else if (inner[i] == '"') { end_quote = i; break; }
                        }
                        if (end_quote == ipos - 1) { is_simple_str_lit = 1; lit_start = 1; lit_end = ipos - 1; }
                    } else if (ipos >= 4 && inner[0] == '\\' && inner[1] == '"' && inner[ipos-2] == '\\' && inner[ipos-1] == '"') {
                        /* escaped-quote literal form: \"...\" */
                        is_simple_str_lit = 1; lit_start = 2; lit_end = ipos - 2;
                    }
                    /* Validate numeric string literals for toint/tofloat (and aliases) */
                    if (is_simple_str_lit && (_ck_int || _ck_float)) {
                        const char *sv = inner + lit_start;
                        int slen = lit_end - lit_start;
                        /* allow optional sign, digits, optional .digits */
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
                            char *msg = _ltng_cfmt("Invalid conversion: cannot convert \"%.*s\" with %s", slen, sv, tok.value);
                            error_state_add(parser_errors, tok.line, tok.col, msg ? msg : "");
                            free(msg);
                        }
                    }

                    /* Determine arg form/type. Compile complex expressions; keep simple idents/lits raw. */
                    char inner_c[MAX_STRING_LEN]; inner_c[0] = '\0';
                    const char *inner_expr = inner;
                    VarType inner_ctype = TYPE_STRING;
                    int inner_compiled = 0;
                    int is_simple_ident_arg = 0;
                    if (is_num_lit) {
                        inner_ctype = num_has_dot ? TYPE_FLOAT : TYPE_INT;
                    } else if (is_simple_str_lit) {
                        inner_ctype = TYPE_STRING;
                    } else {
                        /* Check if simple identifier (optionally with no complex ops) */
                        int simple_ident = 0;
                        {
                            const char *p = inner; while (*p==' '||*p=='\t') p++;
                            if (isalpha((unsigned char)*p) || *p=='_') {
                                const char *q = p+1;
                                while (isalnum((unsigned char)*q) || *q=='_') q++;
                                while (*q==' '||*q=='\t') q++;
                                if (*q=='\0') simple_ident = 1;
                            }
                        }
                        if (simple_ident) {
                            is_simple_ident_arg = 1;
                            char mname[MAX_IDENTIFIER_LEN]; mangle_name(inner, mname, MAX_IDENTIFIER_LEN);
                            if (symbol_table_has(table, mname)) {
                                inner_ctype = symbol_table_get(table, mname);
                                inner_expr = symbol_table_get_cname(table, mname);
                            } else if (is_bool_value(inner)) {
                                inner_ctype = TYPE_BOOL;
                                inner_expr = inner;
                            } else {
                                inner_ctype = TYPE_STRING;
                                inner_expr = inner;
                            }
                        } else {
                            /* Complex expression: compile */
                            inner_ctype = parse_expression_from_string(inner, table, inner_c, sizeof(inner_c));
                            if (inner_c[0] != '\0') { inner_expr = inner_c; inner_compiled = 1; }
                            else { inner_ctype = TYPE_STRING; }
                        }
                    }

                    /* helper: inner is a runtime C numeric value only when compiled to numeric expr */
                    int inner_is_cnum = inner_compiled && (inner_ctype == TYPE_INT || inner_ctype == TYPE_FLOAT || inner_ctype == TYPE_BOOL);
                    if (_ck_int) {
                        if (is_simple_str_lit) {
                            char sval[MAX_STRING_LEN]; int sp=0;
                            for (int i=lit_start;i<lit_end && sp<MAX_STRING_LEN-1;i++){ if(inner[i]=='\\' && i+1<lit_end) sval[sp++]=inner[++i]; else sval[sp++]=inner[i]; }
                            sval[sp]='\0';
                            pos += snprintf(out + pos, out_size - pos, "%lld", (long long)atoll(sval));
                        } else if (is_num_lit) {
                            pos += snprintf(out + pos, out_size - pos, "%lld", (long long)atoll(inner));
                        } else if (is_bool_value(inner)) {
                            pos += snprintf(out + pos, out_size - pos, "%d", (strcmp(inner,"true")==0||strcmp(inner,"t")==0)?1:0);
                        } else if (inner_is_cnum) {
                            pos += snprintf(out + pos, out_size - pos, "(long long)(%s)", inner_expr);
                        } else {
                            pos += snprintf(out + pos, out_size - pos, "(long long)atoll(%s)", inner_expr);
                        }
                        if (!type_locked) {
                            if (result_type == TYPE_FLOAT) { /* keep float promotion */ }
                            else result_type = TYPE_INT;
                        }
                    } else if (_ck_float) {
                        if (is_simple_str_lit) {
                            char sval[MAX_STRING_LEN]; int sp=0;
                            for (int i=lit_start;i<lit_end && sp<MAX_STRING_LEN-1;i++){ if(inner[i]=='\\' && i+1<lit_end) sval[sp++]=inner[++i]; else sval[sp++]=inner[i]; }
                            sval[sp]='\0';
                            pos += snprintf(out + pos, out_size - pos, "%.17g", atof(sval));
                        } else if (is_num_lit) {
                            pos += snprintf(out + pos, out_size - pos, "%.17g", atof(inner));
                        } else if (is_bool_value(inner)) {
                            pos += snprintf(out + pos, out_size - pos, "%s", (strcmp(inner,"true")==0||strcmp(inner,"t")==0)?"1.0":"0.0");
                        } else if (inner_is_cnum) {
                            pos += snprintf(out + pos, out_size - pos, "(double)(%s)", inner_expr);
                        } else {
                            pos += snprintf(out + pos, out_size - pos, "(double)atof(%s)", inner_expr);
                        }
                        if (!type_locked) result_type = TYPE_FLOAT;
                    } else if (_ck_str) {
                        if (ipos == 4 && memcmp(inner, "true", 4) == 0) {
                            pos += snprintf(out + pos, out_size - pos, "\"true\"");
                        } else if (ipos == 5 && memcmp(inner, "false", 5) == 0) {
                            pos += snprintf(out + pos, out_size - pos, "\"false\"");
                        } else if (ipos == 1 && inner[0] == 't') {
                            pos += snprintf(out + pos, out_size - pos, "\"true\"");
                        } else if (ipos == 1 && inner[0] == 'f') {
                            pos += snprintf(out + pos, out_size - pos, "\"false\"");
                        } else if (is_num_lit) {
                            pos += snprintf(out + pos, out_size - pos, "\"%s\"", inner);
                        } else if (inner_is_cnum && inner_ctype == TYPE_BOOL) {
                            pos += snprintf(out + pos, out_size - pos, "((%s)?\"true\":\"false\")", inner_expr);
                        } else if (inner_is_cnum && inner_ctype == TYPE_INT) {
                            pos += snprintf(out + pos, out_size - pos, "_ltng_from_int(_ltng_retbuf(), (long long)(%s))", inner_expr);
                        } else if (inner_is_cnum && inner_ctype == TYPE_FLOAT) {
                            pos += snprintf(out + pos, out_size - pos, "_ltng_from_double(_ltng_retbuf(), (double)(%s))", inner_expr);
                        } else {
                            pos += snprintf(out + pos, out_size - pos, "%s", inner_expr);
                        }
                        if (!type_locked) result_type = TYPE_STRING;
                    } else if (_ck_bool) {
                        if (ipos == 1 && inner[0] == '0') {
                            pos += snprintf(out + pos, out_size - pos, "0");
                        } else if (is_num_lit) {
                            pos += snprintf(out + pos, out_size - pos, "%d", (atof(inner)!=0.0)?1:0);
                        } else if (is_simple_str_lit) {
                            int bv = 1;
                            int slen = lit_end - lit_start;
                            if (slen == 0) bv = 0;
                            else if (slen == 1 && inner[lit_start] == '0') bv = 0;
                            else if (slen == 1 && inner[lit_start] == 'f') bv = 0;
                            else if (slen == 5 && memcmp(inner + lit_start, "false", 5) == 0) bv = 0;
                            pos += snprintf(out + pos, out_size - pos, "%d", bv);
                        } else if (inner_is_cnum && inner_ctype == TYPE_BOOL) {
                            pos += snprintf(out + pos, out_size - pos, "((%s)?1:0)", inner_expr);
                        } else if (inner_is_cnum) {
                            pos += snprintf(out + pos, out_size - pos, "((%s)!=0?1:0)", inner_expr);
                        } else {
                            pos += snprintf(out + pos, out_size - pos,
                                "((%s)[0]!='\\0'&&strcmp(%s,\"0\")!=0&&strcmp(%s,\"false\")!=0&&strcmp(%s,\"f\")!=0?1:0)",
                                inner_expr, inner_expr, inner_expr, inner_expr);
                        }
                        if (!type_locked) {
                            if (arithmetic_used || comparison_or_logical) {
                                if (result_type != TYPE_FLOAT) result_type = TYPE_INT;
                            } else {
                                result_type = TYPE_BOOL;
                            }
                        }
                    } else if (_ck_char) {
                        if (is_simple_str_lit && ipos >= 3) {
                            char ch = inner[lit_start];
                            if (ch == '\\' && lit_start + 1 < lit_end) {
                                char *esc = (char*)malloc(3); if(esc){ esc[0]=inner[lit_start]; esc[1]=inner[lit_start+1]; esc[2]=0;
                                pos += snprintf(out + pos, out_size - pos, "\"%s\"", esc); free(esc); }
                            } else {
                                pos += snprintf(out + pos, out_size - pos, "\"%c\"", ch);
                            }
                        } else if (is_num_lit) {
                            int iv = (int)atoll(inner);
                            if (iv >= 0 && iv <= 127 && iv != '\\' && iv != '"' && iv != '\n' && iv != '\t' && iv != '\r' && iv != '\0') {
                                pos += snprintf(out + pos, out_size - pos, "\"%c\"", iv);
                            } else {
                                pos += snprintf(out + pos, out_size - pos, "({ char *_tc=_ltng_ds_new(\"\"); int _cv=(int)(%d); if(_cv=='\\n') _ltng_ds_set(&_tc,\"\\n\"); else if(_cv=='\\t') _ltng_ds_set(&_tc,\"\\t\"); else if(_cv=='\\r') _ltng_ds_set(&_tc,\"\\r\"); else if(_cv=='\\\\') _ltng_ds_set(&_tc,\"\\\\\"); else if(_cv=='\\\"') _ltng_ds_set(&_tc,\"\\\"\"); else _ltng_ds_setfmt(&_tc,\"%%c\",(char)_cv); char *_r=_ltng_as(_tc); free(_tc); _r; })", iv);
                            }
                        } else if (inner_is_cnum) {
                            pos += snprintf(out + pos, out_size - pos,
                                "({ char *_tc=_ltng_ds_fmt(\"%%c\",(char)(int)(%s)); char *_r=_ltng_as(_tc); free(_tc); _r; })",
                                inner_expr);
                        } else {
                            pos += snprintf(out + pos, out_size - pos,
                                "_ltng_as((_ltng_is_num_str(%s)?((char[]){(char)atoll(%s),0}):((char[]){(%s)[0]? (%s)[0]:0,0})))",
                                inner_expr, inner_expr, inner_expr, inner_expr);
                        }
                        if (!type_locked) result_type = TYPE_CHAR;
                    }
                    continue;
                }
                if (is_bool_value(tok.value)) {
                    pos += snprintf(out + pos, out_size - pos, "%s",
                        (strcmp(tok.value, "true") == 0 || strcmp(tok.value, "t") == 0) ? "1" : "0");
                    if (!type_locked && !arithmetic_used && !comparison_or_logical) result_type = TYPE_BOOL;
                    else if (!type_locked && (arithmetic_used || comparison_or_logical) && result_type != TYPE_FLOAT) result_type = TYPE_INT;
                } else if (is_type_keyword(tok.value)) {
                    pos += snprintf(out + pos, out_size - pos, "%s", tok.value);
                } else if (is_callable_ident(table, tok.value) && lexer_peek(lexer).type == TOKEN_LPAREN) {
                    lexer_next(lexer); /* consume ( */
                    ASTNode *call = parse_call_node(lexer, table, tok.value, tok.line, tok.col, NULL);
                    if (call->is_base_assign) {
                        error_state_add(parser_errors, tok.line, tok.col,
                            "function with 'base' parameter must be called with method syntax (receiver.method(args))");
                        skip_to_newline(lexer);
                        pos += snprintf(out + pos, out_size - pos, "0");
                        break;
                    }
                    char cexpr[MAX_STRING_LEN];
                    build_call_cexpr(call, cexpr, sizeof(cexpr));
                    int chained_call = append_immediate_call_chains(lexer, table, cexpr, sizeof(cexpr));
                    {
                        VarType crt = chained_call ? TYPE_STRING : infer_call_result_type(call);
                        if (chained_call) {
                            pos += snprintf(out + pos, out_size - pos, "%s", cexpr);
                            if (!type_locked) result_type = TYPE_STRING;
                        } else if (!type_locked && crt == TYPE_STRING && function_returns_string(call->name)) {
                            pos += snprintf(out + pos, out_size - pos, "%s", cexpr);
                            result_type = TYPE_STRING;
                        } else if (crt == TYPE_INT) {
                            pos += snprintf(out + pos, out_size - pos, "(double)atoll(%s)", cexpr);
                            if (!type_locked) result_type = TYPE_INT;
                        } else if (crt == TYPE_FLOAT || crt == TYPE_NULL || crt == TYPE_VOID) {
                            /* for passthrough-specialized non-string types we got concrete type; NULL/VOID means unknown */
                            if (crt == TYPE_NULL || crt == TYPE_VOID) {
                                pos += snprintf(out + pos, out_size - pos, "atof(%s)", cexpr);
                                if (!type_locked) result_type = TYPE_FLOAT;
                            } else {
                                pos += snprintf(out + pos, out_size - pos, "atof(%s)", cexpr);
                                if (!type_locked) result_type = TYPE_FLOAT;
                            }
                        } else if (crt == TYPE_STRING) {
                            pos += snprintf(out + pos, out_size - pos, "%s", cexpr);
                            if (!type_locked) result_type = TYPE_STRING;
                        } else if (crt == TYPE_BOOL) {
                            pos += snprintf(out + pos, out_size - pos, "(%s[0]=='t'||%s[0]=='T'||%s[0]=='1'?1:0)", cexpr, cexpr, cexpr);
                            if (!type_locked) result_type = TYPE_BOOL;
                        } else if (crt == TYPE_CHAR) {
                            pos += snprintf(out + pos, out_size - pos, "%s", cexpr);
                            if (!type_locked) result_type = TYPE_STRING;
                        } else if (crt == TYPE_ARRAY) {
                            pos += snprintf(out + pos, out_size - pos,
                                "({ char *__ahv=_ltng_ds_new(%s); _ltng_ah *__ah=_ltng_ah_unpack(__ahv); char *__r=_ltng_ds_new(\"\"); if(__ah){ for(int _i=0;_i<__ah->len;_i++){ if(_i) _ltng_ds_append(&__r,\", \"); _ltng_ds_append(&__r,__ah->d[_i]?__ah->d[_i]:\"\"); } _ltng_ah_free(__ah); } free(__ahv); __r; })",
                                cexpr);
                            if (!type_locked) result_type = TYPE_STRING;
                        } else {
                            pos += snprintf(out + pos, out_size - pos, "atof(%s)", cexpr);
                            if (!type_locked) result_type = TYPE_FLOAT;
                        }
                    }
                } else if (lexer_peek(lexer).type == TOKEN_LPAREN && strcmp(tok.value, "typeof") != 0) {
                    char mangled[MAX_IDENTIFIER_LEN];
                    mangle_name(tok.value, mangled, MAX_IDENTIFIER_LEN);
                    if (symbol_table_has(table, mangled) || symbol_table_has(table, tok.value)) {
                        /* dynamic indirect call through variable */
                        lexer_next(lexer); /* consume ( */
                        ASTNode *call = parse_call_node(lexer, table, tok.value, tok.line, tok.col, NULL);
                        if (call->is_base_assign) {
                            error_state_add(parser_errors, tok.line, tok.col,
                                "function with 'base' parameter must be called with method syntax (receiver.method(args))");
                            skip_to_newline(lexer);
                            pos += snprintf(out + pos, out_size - pos, "0");
                            break;
                        }
                        char cexpr[MAX_STRING_LEN];
                        build_call_cexpr(call, cexpr, sizeof(cexpr));
                        int chained_call = append_immediate_call_chains(lexer, table, cexpr, sizeof(cexpr));
                        {
                            if (chained_call) {
                                pos += snprintf(out + pos, out_size - pos, "%s", cexpr);
                                if (!type_locked) result_type = TYPE_STRING;
                            } else {
                            char fr_cname2[MAX_IDENTIFIER_LEN] = {0}; int fr_pc2 = -1;
                            VarType irt = TYPE_NULL;
                            if (symbol_table_get_func_ref(table, tok.value, fr_cname2, sizeof(fr_cname2), &fr_pc2) && fr_cname2[0]) {
                                irt = infer_function_return_type(fr_cname2);
                            }
                            if (irt == TYPE_INT) {
                                pos += snprintf(out + pos, out_size - pos, "(double)atoll(%s)", cexpr);
                                if (!type_locked) result_type = TYPE_INT;
                            } else if (irt == TYPE_FLOAT || irt == TYPE_VOID || irt == TYPE_NULL) {
                                /* unknown/void/float: in numeric context treat as numeric */
                                if (irt == TYPE_NULL && type_locked && result_type == TYPE_STRING) {
                                    pos += snprintf(out + pos, out_size - pos, "%s", cexpr);
                                } else if (irt == TYPE_NULL && !type_locked && result_type == TYPE_STRING) {
                                    pos += snprintf(out + pos, out_size - pos, "%s", cexpr);
                                } else {
                                    pos += snprintf(out + pos, out_size - pos, "atof(%s)", cexpr);
                                    if (!type_locked) result_type = TYPE_FLOAT;
                                }
                            } else if (irt == TYPE_STRING) {
                                pos += snprintf(out + pos, out_size - pos, "%s", cexpr);
                                if (!type_locked) result_type = TYPE_STRING;
                            } else if (irt == TYPE_ARRAY) {
                                pos += snprintf(out + pos, out_size - pos,
                                    "({ char *__ahv=_ltng_ds_new(%s); _ltng_ah *__ah=_ltng_ah_unpack(__ahv); char *__r=_ltng_ds_new(\"\"); if(__ah){ for(int _i=0;_i<__ah->len;_i++){ if(_i) _ltng_ds_append(&__r,\", \"); _ltng_ds_append(&__r,__ah->d[_i]?__ah->d[_i]:\"\"); } _ltng_ah_free(__ah); } free(__ahv); __r; })",
                                    cexpr);
                                if (!type_locked) result_type = TYPE_STRING;
                            } else {
                                pos += snprintf(out + pos, out_size - pos, "atof(%s)", cexpr);
                                if (!type_locked) result_type = TYPE_FLOAT;
                            }
                            }
                        }
                    } else {
                        goto collect_ident_var;
                    }
                } else if (lexer_peek(lexer).type == TOKEN_DOT) {
                    /* Check for array methods - let TYPE_ARRAY case handle
                       these as inline C expressions (with chaining support) */
                    if (lexer->current + 2 < lexer->count &&
                        (lexer->tokens[lexer->current + 1].type == TOKEN_IDENT ||
                         token_is_name(lexer->tokens[lexer->current + 1]))) {
                        const char *_am = lexer->tokens[lexer->current + 1].value;
                        if (strcmp(_am, "push") == 0 || strcmp(_am, "pop") == 0 ||
                            strcmp(_am, "insert") == 0 || strcmp(_am, "remove") == 0 ||
                            strcmp(_am, "contains") == 0 || strcmp(_am, "index_of") == 0 ||
                            strcmp(_am, "sort") == 0 || strcmp(_am, "reverse") == 0 ||
                            strcmp(_am, "copy") == 0 || strcmp(_am, "slice") == 0) {
                            char _am_mangled[MAX_IDENTIFIER_LEN];
                            mangle_name(tok.value, _am_mangled, MAX_IDENTIFIER_LEN);
                            if (symbol_table_has(table, _am_mangled) &&
                                symbol_table_get(table, _am_mangled) == TYPE_ARRAY) {
                                /* Don't consume - TYPE_ARRAY case handles array methods */
                                goto collect_ident_var;
                            }
                        }
                    }
                    int saved_m = lexer->current;
                    lexer_next(lexer); /* dot */
                    Token mname = lexer_next(lexer);
                    /* Array .length property (general ident context) */
                    {
                        char tmangled[MAX_IDENTIFIER_LEN];
                        mangle_name(tok.value, tmangled, MAX_IDENTIFIER_LEN);
                        if (symbol_table_has(table, tmangled) &&
                            symbol_table_get(table, tmangled) == TYPE_ARRAY &&
                            strcmp(mname.value, "length") == 0 &&
                            lexer_peek(lexer).type != TOKEN_LPAREN) {
                            const char *tcname = symbol_table_get_cname(table, tmangled);
                            char tlenname[MAX_IDENTIFIER_LEN];
                            snprintf(tlenname, sizeof(tlenname), "%s_len", tcname);
                            pos += snprintf(out + pos, out_size - pos,
                                "(int)(%s)", tlenname);
                            result_type = TYPE_INT;
                            break;
                        }
                    }
                    if ((mname.type == TOKEN_IDENT || token_is_name(mname)) &&
                        lexer_peek(lexer).type == TOKEN_LPAREN &&
                        is_function_name_scoped(table, mname.value)) {
                        /* method call: receiver.method(args) => method(receiver, args) */
                        lexer_next(lexer); /* ( */
                        ASTNode *recv = make_receiver_arg(table, tok);
                        ASTNode *call = parse_call_node(lexer, table, mname.value, mname.line, mname.col, recv);
                        if (call->is_base_assign) {
                            /* Value-consumed context: keep receiver intact, use return value */
                            call->is_base_assign = 0;
                        }
                        char cexpr[MAX_STRING_LEN];
                        build_call_cexpr(call, cexpr, sizeof(cexpr));
                        int chained_call = append_immediate_call_chains(lexer, table, cexpr, sizeof(cexpr));
                        {
                            VarType crt = chained_call ? TYPE_STRING : infer_call_result_type(call);
                            if (chained_call) {
                                pos += snprintf(out + pos, out_size - pos, "%s", cexpr);
                                if (!type_locked) result_type = TYPE_STRING;
                            } else if (!type_locked && crt == TYPE_STRING && function_returns_string(mname.value)) {
                                pos += snprintf(out + pos, out_size - pos, "%s", cexpr);
                                result_type = TYPE_STRING;
                            } else if (crt == TYPE_INT) {
                                pos += snprintf(out + pos, out_size - pos, "(double)atoll(%s)", cexpr);
                                if (!type_locked) result_type = TYPE_INT;
                            } else if (crt == TYPE_FLOAT || crt == TYPE_NULL || crt == TYPE_VOID) {
                                pos += snprintf(out + pos, out_size - pos, "atof(%s)", cexpr);
                                if (!type_locked) result_type = TYPE_FLOAT;
                            } else if (crt == TYPE_STRING) {
                                pos += snprintf(out + pos, out_size - pos, "%s", cexpr);
                                if (!type_locked) result_type = TYPE_STRING;
                            } else if (crt == TYPE_BOOL) {
                                pos += snprintf(out + pos, out_size - pos, "(%s[0]=='t'||%s[0]=='T'||%s[0]=='1'?1:0)", cexpr, cexpr, cexpr);
                                if (!type_locked) result_type = TYPE_BOOL;
                            } else if (crt == TYPE_CHAR) {
                                pos += snprintf(out + pos, out_size - pos, "%s", cexpr);
                                if (!type_locked) result_type = TYPE_STRING;
                            } else if (crt == TYPE_ARRAY) {
                                pos += snprintf(out + pos, out_size - pos,
                                    "({ char *__ahv=_ltng_ds_new(%s); _ltng_ah *__ah=_ltng_ah_unpack(__ahv); char *__r=_ltng_ds_new(\"\"); if(__ah){ for(int _i=0;_i<__ah->len;_i++){ if(_i) _ltng_ds_append(&__r,\", \"); _ltng_ds_append(&__r,__ah->d[_i]?__ah->d[_i]:\"\"); } _ltng_ah_free(__ah); } free(__ahv); __r; })",
                                    cexpr);
                                if (!type_locked) result_type = TYPE_STRING;
                            } else {
                                pos += snprintf(out + pos, out_size - pos, "atof(%s)", cexpr);
                                if (!type_locked) result_type = TYPE_FLOAT;
                            }
                        }
                    } else if (mname.type == TOKEN_IDENT || token_is_name(mname)) {
                        /* property access: obj.field */
                        char mangled[MAX_IDENTIFIER_LEN];
                        mangle_name(tok.value, mangled, MAX_IDENTIFIER_LEN);
                        char fv[MAX_IDENTIFIER_LEN*2+8];
                        const char *field_base = symbol_table_get_cname(table, mangled);
                        if (!field_base || !field_base[0]) field_base = mangled;
                        obj_resolve_field(field_base, mname.value, fv, sizeof(fv));
                        VarType ftype = TYPE_STRING;
                        ObjSchema *osc = obj_lookup(field_base);
                        if (!osc) osc = obj_lookup(mangled);
                        if (osc) {
                            for (int oi = 0; oi < osc->count; oi++) {
                                if (strcmp(osc->fields[oi].name, mname.value) == 0) { ftype = osc->fields[oi].type; break; }
                            }
                        }
                        if (ftype == TYPE_BOOL) {
                            pos += snprintf(out + pos, out_size - pos, "(%s[0]=='t'?1:0)", fv);
                            if (!type_locked) result_type = TYPE_BOOL;
                        } else if (ftype == TYPE_STRING) {
                            pos += snprintf(out + pos, out_size - pos, "%s", fv);
                            if (!type_locked) result_type = TYPE_STRING;
                        } else if (ftype == TYPE_INT) {
                            pos += snprintf(out + pos, out_size - pos, "atoll(%s)", fv);
                            if (!type_locked) result_type = TYPE_INT;
                        } else if (ftype == TYPE_FLOAT) {
                            pos += snprintf(out + pos, out_size - pos, "atof(%s)", fv);
                            if (!type_locked) result_type = TYPE_FLOAT;
                        } else {
                            pos += snprintf(out + pos, out_size - pos, "%s", fv);
                            if (!type_locked) result_type = TYPE_STRING;
                        }
                    } else {
                        lexer->current = saved_m;
                        goto collect_ident_var;
                    }
                } else if (strcmp(tok.value, "typeof") == 0) {
                    /* typeof was already consumed by lexer_next; parse typeof(arg) */
                    lexer->current--; /* unconsume typeof for try_parse_typeof */
                    Token tres;
                    if (try_parse_typeof(lexer, table, &tres)) {
                        pos += snprintf(out + pos, out_size - pos, "\"%s\"", tres.value);
                        if (!type_locked) result_type = TYPE_STRING;
                        continue;
                    }
                    /* try_parse_typeof restored position on failure; consume typeof and fallback */
                    lexer_next(lexer);
                    Token open = lexer_peek(lexer);
                    if (open.type != TOKEN_LPAREN) {
                        goto collect_ident_var;
                    }
                    lexer_next(lexer); /* consume ( */
                    Token arg = lexer_next(lexer);
                    Token close = lexer_next(lexer);
                    if (close.type != TOKEN_RPAREN) {
                        error_state_add(parser_errors, close.line, close.col,
                            "Expected ')' after typeof argument");
                        skip_to_newline(lexer);
                        pos += snprintf(out + pos, out_size - pos, "0");
                        continue;
                    }
                    VarType vt = TYPE_STRING;
                    if (arg.type == TOKEN_INT_LIT) vt = TYPE_INT;
                    else if (arg.type == TOKEN_FLOAT_LIT) vt = TYPE_FLOAT;
                    else if (arg.type == TOKEN_STRING) vt = TYPE_STRING;
                    else if (arg.type == TOKEN_CHAR) vt = TYPE_STRING;
                    else if ((arg.type == TOKEN_IDENT || token_is_name(arg)) && is_bool_value(arg.value)) vt = TYPE_BOOL;
                    else if (arg.type == TOKEN_IDENT || token_is_name(arg)) {
                        char mangled[MAX_IDENTIFIER_LEN]; mangle_name(arg.value, mangled, MAX_IDENTIFIER_LEN);
                        if (symbol_table_has(table, mangled)) vt = symbol_table_get(table, mangled);
                    }
                    pos += snprintf(out + pos, out_size - pos, "\"%s\"", type_name_str(vt));
                    if (!type_locked) result_type = TYPE_STRING;
                    continue;
                } else {
                collect_ident_var:
                    ; /* C99: declaration after label */
                    pending_divmod = _pd_save; pending_divmod_is_mod = _pd_mod_save;
                    pending_divmod_line = _pd_l_save; pending_divmod_col = _pd_c_save;
                    pending_mod_op_pos = _pd_mop_save;
                    char mangled[MAX_IDENTIFIER_LEN];
                    mangle_name(tok.value, mangled, MAX_IDENTIFIER_LEN);
                    if (!symbol_table_has(table, mangled)) {
                        ASTNode *fndef = find_function_ast_by_name(tok.value);
                        if (fndef) {
                            pos += snprintf(out + pos, out_size - pos, "\"%s\"", fndef->name);
                            if (!type_locked) result_type = TYPE_STRING;
                        } else {
                            char *errmsg = _ltng_cfmt("Variable '%s' not declared", tok.value);
                            error_state_add(parser_errors, tok.line, tok.col, errmsg ? errmsg : "");
                            free(errmsg);
                            pos += snprintf(out + pos, out_size - pos, "0");
                        }
                    } else {
                        VarType vtype = symbol_table_get(table, mangled);
                        const char *cname = symbol_table_get_cname(table, mangled);
                        /* For parameters with a known default type, prefer that type for
                           typing/inference in expressions (and for emission of the value).
                           Untyped params with no default remain dynamic. */
                        VarType _eff_type = vtype;
                        int _has_default_type = 0;
                        if (symbol_table_is_param(table, mangled)) {
                            VarType _pdt = effective_param_default_type(tok.value, vtype);
                            if (_pdt != vtype && _pdt != TYPE_NULL) {
                                _eff_type = _pdt;
                                _has_default_type = 1;
                            }
                        }
                        VarType _emit_type = _eff_type;
                        /* in a string-building context, use the raw buffer
                           (array variables are handled by the index logic below),
                           unless the next operator requires numeric form. */
                        int _strctx_raw = (result_type == TYPE_STRING && !type_locked && _emit_type != TYPE_ARRAY);
                        if (_strctx_raw && (_emit_type == TYPE_INT || _emit_type == TYPE_FLOAT || _emit_type == TYPE_BOOL)) {
                            Token _nxt = lexer_peek(lexer);
                            if (_nxt.type == TOKEN_MINUS || _nxt.type == TOKEN_SLASH || _nxt.type == TOKEN_PERCENT ||
                                _nxt.type == TOKEN_EQ_EQ || _nxt.type == TOKEN_BANG_EQ ||
                                _nxt.type == TOKEN_LT || _nxt.type == TOKEN_GT ||
                                _nxt.type == TOKEN_LT_EQ || _nxt.type == TOKEN_GT_EQ ||
                                _nxt.type == TOKEN_AMP_AMP || _nxt.type == TOKEN_PIPE_PIPE ||
                                _nxt.type == TOKEN_CARET_CARET ||
                                ((_nxt.type == TOKEN_PLUS || _nxt.type == TOKEN_STAR) && paren_depth > 0)) {
                                _strctx_raw = 0;
                            }
                        }
                        if (_strctx_raw) {
                            pos += snprintf(out + pos, out_size - pos, "%s", cname);
                        } else switch (_emit_type) {
                            case TYPE_INT: {
                                char *_opnd = _ltng_cfmt("atoll(%s)", cname);
                                if (pending_divmod) {
                                    int _is_fmod = (result_type == TYPE_FLOAT && pending_divmod_is_mod);
                                    if (_is_fmod && pending_mod_op_pos >= 0 && pending_mod_op_pos < pos && out[pending_mod_op_pos] == '%') {
                                        char *_left = (char*)malloc(4096); if(_left){ int _llen = pending_mod_op_pos; if (_llen >= 4096) _llen = 4095;
                                        memcpy(_left, out, _llen); _left[_llen] = 0;
                                        pos = 0;
                                        pos += snprintf(out + pos, out_size - pos, "_LTNG_FMOD_D(%s, %s, %d, %d)", _left, _opnd, pending_divmod_line, pending_divmod_col); free(_left); }
                                    } else {
                                        pos += snprintf(out + pos, out_size - pos, "(%s(%s,%d,%d))", (result_type == TYPE_FLOAT ? (pending_divmod_is_mod?"_LTNG_NZMOD_D":"_LTNG_NZDIV_D") : (pending_divmod_is_mod?"_LTNG_NZMOD_LL":"_LTNG_NZDIV_LL")), _opnd, pending_divmod_line, pending_divmod_col);
                                    }
                                } else pos += snprintf(out + pos, out_size - pos, "%s", _opnd);
                                /* Untyped/dynamic parameters (no known default type) need runtime add semantics.
                                   Parameters with known default types are treated as typed by default. */
                                if (!_has_default_type && symbol_table_is_param(table, mangled) && !symbol_table_get_locked(table, mangled)) has_dynamic_numeric = 1;
                                free(_opnd); pending_divmod = 0; pending_mod_op_pos = -1;
                                break; }
                            case TYPE_FLOAT: {
                                char *_opnd = _ltng_cfmt("atof(%s)", cname);
                                if (pending_divmod) {
                                    int _is_fmod = pending_divmod_is_mod;
                                    if (_is_fmod && pending_mod_op_pos >= 0 && pending_mod_op_pos < pos && out[pending_mod_op_pos] == '%') {
                                        char *_left = (char*)malloc(4096); if(_left){ int _llen = pending_mod_op_pos; if (_llen >= 4096) _llen = 4095;
                                        memcpy(_left, out, _llen); _left[_llen] = 0;
                                        pos = 0;
                                        pos += snprintf(out + pos, out_size - pos, "_LTNG_FMOD_D(%s, %s, %d, %d)", _left, _opnd, pending_divmod_line, pending_divmod_col); free(_left); }
                                    } else {
                                        pos += snprintf(out + pos, out_size - pos, "(%s(%s,%d,%d))", (pending_divmod_is_mod?"_LTNG_NZMOD_D":"_LTNG_NZDIV_D"), _opnd, pending_divmod_line, pending_divmod_col);
                                    }
                                } else pos += snprintf(out + pos, out_size - pos, "%s", _opnd);
                                if (!type_locked) { if (result_type == TYPE_INT) result_type = TYPE_FLOAT; }
                                /* Untyped/dynamic parameters (no known default type) need runtime add semantics.
                                   Parameters with known default types are treated as typed by default. */
                                if (!_has_default_type && symbol_table_is_param(table, mangled) && !symbol_table_get_locked(table, mangled)) has_dynamic_numeric = 1;
                                free(_opnd); pending_divmod = 0; pending_mod_op_pos = -1;
                                break; }
                            case TYPE_CHAR:
                            case TYPE_STRING:
                            case TYPE_OBJECT:
                                pos += snprintf(out + pos, out_size - pos, "%s", cname);
                                if (!type_locked) result_type = TYPE_STRING;
                                pending_divmod = 0; pending_mod_op_pos = -1;
                                break;
                            case TYPE_BOOL: {
                                char *_opnd = _ltng_cfmt("(%s[0]=='t'?1:0)", cname);
                                if (pending_divmod) {
                                    int _is_fmod = (result_type == TYPE_FLOAT && pending_divmod_is_mod);
                                    if (_is_fmod && pending_mod_op_pos >= 0 && pending_mod_op_pos < pos && out[pending_mod_op_pos] == '%') {
                                        char *_left = (char*)malloc(4096); if(_left){ int _llen = pending_mod_op_pos; if (_llen >= 4096) _llen = 4095;
                                        memcpy(_left, out, _llen); _left[_llen] = 0;
                                        pos = 0;
                                        pos += snprintf(out + pos, out_size - pos, "_LTNG_FMOD_D(%s, %s, %d, %d)", _left, _opnd, pending_divmod_line, pending_divmod_col); free(_left); }
                                    } else {
                                        pos += snprintf(out + pos, out_size - pos, "(%s(%s,%d,%d))", (result_type == TYPE_FLOAT ? (pending_divmod_is_mod?"_LTNG_NZMOD_D":"_LTNG_NZDIV_D") : (pending_divmod_is_mod?"_LTNG_NZMOD_LL":"_LTNG_NZDIV_LL")), _opnd, pending_divmod_line, pending_divmod_col);
                                    }
                                } else pos += snprintf(out + pos, out_size - pos, "%s", _opnd);
                                if (!type_locked) result_type = TYPE_BOOL;
                                free(_opnd); pending_divmod = 0; pending_mod_op_pos = -1;
                                break; }
                            case TYPE_NULL:
                            default:
                                pos += snprintf(out + pos, out_size - pos, "%s", cname);
                                has_dynamic_numeric = 1;
                                if (!type_locked) result_type = TYPE_STRING;
                                pending_divmod = 0; pending_mod_op_pos = -1;
                                break;
                            case TYPE_ARRAY:
                                /* Check for .length property and array method calls */
                                {
                                    Token nxt = lexer_peek(lexer);
                                    if (nxt.type == TOKEN_DOT) {
                                        int saved_dot = lexer->current;
                                        lexer_next(lexer); /* consume . */
                                        Token mname = lexer_next(lexer);

                                        char arrname[MAX_IDENTIFIER_LEN];
                                        snprintf(arrname, sizeof(arrname), "%s_data", cname);
                                        char lenname[MAX_IDENTIFIER_LEN];
                                        snprintf(lenname, sizeof(lenname), "%s_len", cname);
                                        char capname[MAX_IDENTIFIER_LEN];
                                        snprintf(capname, sizeof(capname), "%s_cap", cname);

                                        /* .length property (no parens) */
                                        if ((mname.type == TOKEN_IDENT || token_is_name(mname)) &&
                                            strcmp(mname.value, "length") == 0 &&
                                            lexer_peek(lexer).type != TOKEN_LPAREN) {
                                            pos += snprintf(out + pos, out_size - pos,
                                                "(int)(%s)", lenname);
                                            result_type = TYPE_INT;
                                            return result_type;
                                        }

                                        /* Array methods with (args) */
                                        if ((mname.type == TOKEN_IDENT || token_is_name(mname)) &&
                                            (strcmp(mname.value, "push") == 0 ||
                                             strcmp(mname.value, "pop") == 0 ||
                                             strcmp(mname.value, "insert") == 0 ||
                                             strcmp(mname.value, "remove") == 0 ||
                                             strcmp(mname.value, "contains") == 0 ||
                                             strcmp(mname.value, "index_of") == 0 ||
                                             strcmp(mname.value, "sort") == 0 ||
                                             strcmp(mname.value, "reverse") == 0)) {
                                            if (lexer_peek(lexer).type != TOKEN_LPAREN) {
                                                char *errmsg = _ltng_cfmt("Expected '(' after '%s'", mname.value);
                                                error_state_add(parser_errors, mname.line, mname.col, errmsg ? errmsg : "");
                                                free(errmsg);
                                                skip_to_newline(lexer);
                                                pos += snprintf(out + pos, out_size - pos, "0");
                                                break;
                                            }
                                            /* Array method call(s) with chaining support.
                                               Value-consumed context: operate on a temp copy to keep
                                               the original receiver intact, return method result. */
                                            char *_chain_prefix = (char*)malloc(16384); if(!_chain_prefix){pos+=snprintf(out+pos,out_size-pos,"0");goto _arr_method_result_done;} _chain_prefix[0]=0;
                                            int _cp = 0;
                                            int _chain_step = 0;
                                            char _final_expr[4096] = {0};
                                            int _is_chained = 0;
                                            int _chain_err = 0;
                                            Token _cm = mname;
                                            /* Copy original array into temp working array */
                                            _cp += snprintf(_chain_prefix + _cp, 16384 - _cp,
                                                "char **__td=NULL; int __tl=0; int __tc=0; _ltng_arr_copy(%s, &__td, &__tl, &__tc, %s); ",
                                                arrname, lenname);
                                            const char *_wa = "__td"; /* working array data */
                                            const char *_wl = "__tl"; /* working array len */
                                            const char *_wc = "__tc"; /* working array cap */
                                            for (;;) {
                                                _chain_step++;
                                                if (lexer_peek(lexer).type != TOKEN_LPAREN) {
                                                    error_state_add(parser_errors, _cm.line, _cm.col, "Expected '(' after array method name");
                                                    skip_to_newline(lexer); pos += snprintf(out + pos, out_size - pos, "0"); _chain_err = 1; break;
                                                }
                                                lexer_next(lexer); /* consume ( */

                                                if (strcmp(_cm.value, "push") == 0) {
                                                    char arg_expr[MAX_STRING_LEN];
                                                    VarType arg_type = collect_expression(lexer, table, arg_expr, sizeof(arg_expr), 1, 0, NULL, NULL, NULL, NULL);
                                                    if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                                                        error_state_add(parser_errors, lexer_peek(lexer).line, lexer_peek(lexer).col, "Expected ')' after push argument");
                                                        skip_to_newline(lexer); pos += snprintf(out + pos, out_size - pos, "0"); _chain_err = 1; break;
                                                    }
                                                    lexer_next(lexer);
                                                    {
                                                        const char *_fmt = (arg_type==TYPE_INT)?"%lld":(arg_type==TYPE_FLOAT)?"%g":"%s";
                                                        const char *_cast = (arg_type==TYPE_INT)?"(long long)":(arg_type==TYPE_FLOAT)?"(double)":"";
                                                        _cp += snprintf(_chain_prefix + _cp, 16384 - _cp,
                                                            "char *__apv%d = _ltng_ds_fmt(\"%s\",%s(%s)); _ltng_arr_push(&%s,&%s,&%s,__apv%d); char *__apr%d=_ltng_retbuf_fmt(\"%%s\",__apv%d); free(__apv%d); ",
                                                            _chain_step, _fmt, _cast, arg_expr, _wa, _wl, _wc, _chain_step, _chain_step, _chain_step, _chain_step);
                                                    }
                                                    snprintf(_final_expr, sizeof(_final_expr), "__apr%d", _chain_step);
                                                    result_type = TYPE_STRING;
                                                } else if (strcmp(_cm.value, "pop") == 0) {
                                                    if (lexer_peek(lexer).type == TOKEN_RPAREN) {
                                                        lexer_next(lexer);
                                                        _cp += snprintf(_chain_prefix + _cp, 16384 - _cp,
                                                            "char *__pv%d = _ltng_arr_pop(%s, &%s); ", _chain_step, _wa, _wl);
                                                        snprintf(_final_expr, sizeof(_final_expr), "__pv%d", _chain_step);
                                                    } else {
                                                        char idx_expr[MAX_STRING_LEN];
                                                        collect_expression(lexer, table, idx_expr, sizeof(idx_expr), 1, 0, NULL, NULL, NULL, NULL);
                                                        if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                                                            error_state_add(parser_errors, lexer_peek(lexer).line, lexer_peek(lexer).col, "Expected ')' after pop index");
                                                            skip_to_newline(lexer); pos += snprintf(out + pos, out_size - pos, "0"); _chain_err = 1; break;
                                                        }
                                                        lexer_next(lexer);
                                                        _cp += snprintf(_chain_prefix + _cp, 16384 - _cp,
                                                            "char *__pv%d = _ltng_arr_pop_index(%s, &%s, (int)(%s)); ",
                                                            _chain_step, _wa, _wl, idx_expr);
                                                        snprintf(_final_expr, sizeof(_final_expr), "__pv%d", _chain_step);
                                                    }
                                                    result_type = TYPE_STRING;
                                                } else if (strcmp(_cm.value, "insert") == 0) {
                                                    char idx_expr[MAX_STRING_LEN];
                                                    collect_expression(lexer, table, idx_expr, sizeof(idx_expr), 1, 1, NULL, NULL, NULL, NULL);
                                                    if (lexer_peek(lexer).type != TOKEN_COMMA) {
                                                        error_state_add(parser_errors, lexer_peek(lexer).line, lexer_peek(lexer).col, "Expected ',' after insert index");
                                                        skip_to_newline(lexer); pos += snprintf(out + pos, out_size - pos, "0"); _chain_err = 1; break;
                                                    }
                                                    lexer_next(lexer);
                                                    char arg_expr[MAX_STRING_LEN];
                                                    VarType arg_type = collect_expression(lexer, table, arg_expr, sizeof(arg_expr), 1, 0, NULL, NULL, NULL, NULL);
                                                    if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                                                        error_state_add(parser_errors, lexer_peek(lexer).line, lexer_peek(lexer).col, "Expected ')' after insert argument");
                                                        skip_to_newline(lexer); pos += snprintf(out + pos, out_size - pos, "0"); _chain_err = 1; break;
                                                    }
                                                    lexer_next(lexer);
                                                    {
                                                        const char *_fmt = (arg_type==TYPE_INT)?"%lld":(arg_type==TYPE_FLOAT)?"%g":"%s";
                                                        const char *_cast = (arg_type==TYPE_INT)?"(long long)":(arg_type==TYPE_FLOAT)?"(double)":"";
                                                        _cp += snprintf(_chain_prefix + _cp, 16384 - _cp,
                                                            "char *__aiv%d = _ltng_ds_fmt(\"%s\",%s(%s)); _ltng_arr_insert(&%s,&%s,&%s,(int)(%s),__aiv%d); char *__air%d=_ltng_retbuf_fmt(\"%%s\",__aiv%d); free(__aiv%d); ",
                                                            _chain_step, _fmt, _cast, arg_expr, _wa, _wl, _wc, idx_expr, _chain_step, _chain_step, _chain_step, _chain_step);
                                                    }
                                                    snprintf(_final_expr, sizeof(_final_expr), "__air%d", _chain_step);
                                                    result_type = TYPE_STRING;
                                                } else if (strcmp(_cm.value, "remove") == 0) {
                                                    char idx_expr[MAX_STRING_LEN];
                                                    collect_expression(lexer, table, idx_expr, sizeof(idx_expr), 1, 0, NULL, NULL, NULL, NULL);
                                                    if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                                                        error_state_add(parser_errors, lexer_peek(lexer).line, lexer_peek(lexer).col, "Expected ')' after remove index");
                                                        skip_to_newline(lexer); pos += snprintf(out + pos, out_size - pos, "0"); _chain_err = 1; break;
                                                    }
                                                    lexer_next(lexer);
                                                    _cp += snprintf(_chain_prefix + _cp, 16384 - _cp,
                                                        "char *__rv%d = _ltng_arr_pop_index(%s, &%s, (int)(%s)); ",
                                                        _chain_step, _wa, _wl, idx_expr);
                                                    snprintf(_final_expr, sizeof(_final_expr), "__rv%d", _chain_step);
                                                    result_type = TYPE_STRING;
                                                } else if (strcmp(_cm.value, "contains") == 0) {
                                                    char arg_expr[MAX_STRING_LEN];
                                                    VarType arg_type = collect_expression(lexer, table, arg_expr, sizeof(arg_expr), 1, 0, NULL, NULL, NULL, NULL);
                                                    if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                                                        error_state_add(parser_errors, lexer_peek(lexer).line, lexer_peek(lexer).col, "Expected ')' after contains argument");
                                                        skip_to_newline(lexer); pos += snprintf(out + pos, out_size - pos, "0"); _chain_err = 1; break;
                                                    }
                                                    lexer_next(lexer);
                                                    {
                                                        const char *_fmt = (arg_type==TYPE_INT)?"%lld":(arg_type==TYPE_FLOAT)?"%g":"%s";
                                                        const char *_cast = (arg_type==TYPE_INT)?"(long long)":(arg_type==TYPE_FLOAT)?"(double)":"";
                                                        _cp += snprintf(_chain_prefix + _cp, 16384 - _cp,
                                                            "char *__cv%d = _ltng_ds_fmt(\"%s\",%s(%s)); int __cr%d=_ltng_arr_contains(%s,%s,__cv%d); free(__cv%d); ",
                                                            _chain_step, _fmt, _cast, arg_expr, _chain_step, _wa, _wl, _chain_step, _chain_step);
                                                    }
                                                    snprintf(_final_expr, sizeof(_final_expr),
                                                        "({ _ltng_retbuf_fmt(\"%%s\",__cr%d?\"true\":\"false\"); })", _chain_step);
                                                    result_type = TYPE_STRING;
                                                } else if (strcmp(_cm.value, "index_of") == 0) {
                                                    char arg_expr[MAX_STRING_LEN];
                                                    VarType arg_type = collect_expression(lexer, table, arg_expr, sizeof(arg_expr), 1, 0, NULL, NULL, NULL, NULL);
                                                    if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                                                        error_state_add(parser_errors, lexer_peek(lexer).line, lexer_peek(lexer).col, "Expected ')' after index_of argument");
                                                        skip_to_newline(lexer); pos += snprintf(out + pos, out_size - pos, "0"); _chain_err = 1; break;
                                                    }
                                                    lexer_next(lexer);
                                                    {
                                                        const char *_fmt = (arg_type==TYPE_INT)?"%lld":(arg_type==TYPE_FLOAT)?"%g":"%s";
                                                        const char *_cast = (arg_type==TYPE_INT)?"(long long)":(arg_type==TYPE_FLOAT)?"(double)":"";
                                                        _cp += snprintf(_chain_prefix + _cp, 16384 - _cp,
                                                            "char *__iv%d = _ltng_ds_fmt(\"%s\",%s(%s)); int __ir%d=_ltng_arr_index_of(%s,%s,__iv%d); free(__iv%d); ",
                                                            _chain_step, _fmt, _cast, arg_expr, _chain_step, _wa, _wl, _chain_step, _chain_step);
                                                    }
                                                    snprintf(_final_expr, sizeof(_final_expr),
                                                        "({ _ltng_retbuf_fmt(\"%%d\",__ir%d); })", _chain_step);
                                                    result_type = TYPE_STRING;
                                                } else if (strcmp(_cm.value, "sort") == 0) {
                                                    if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                                                        error_state_add(parser_errors, lexer_peek(lexer).line, lexer_peek(lexer).col, "Expected ')' after sort");
                                                        skip_to_newline(lexer); pos += snprintf(out + pos, out_size - pos, "0"); _chain_err = 1; break;
                                                    }
                                                    lexer_next(lexer);
                                                    _cp += snprintf(_chain_prefix + _cp, 16384 - _cp,
                                                        "_ltng_arr_sort(%s, %s); ", _wa, _wl);
                                                    snprintf(_final_expr, sizeof(_final_expr),
                                                        "({ _ltng_retbuf_fmt(\"true\"); })");
                                                    result_type = TYPE_STRING;
                                                } else if (strcmp(_cm.value, "reverse") == 0) {
                                                    if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                                                        error_state_add(parser_errors, lexer_peek(lexer).line, lexer_peek(lexer).col, "Expected ')' after reverse");
                                                        skip_to_newline(lexer); pos += snprintf(out + pos, out_size - pos, "0"); _chain_err = 1; break;
                                                    }
                                                    lexer_next(lexer);
                                                    _cp += snprintf(_chain_prefix + _cp, 16384 - _cp,
                                                        "_ltng_arr_reverse(%s, %s); ", _wa, _wl);
                                                    snprintf(_final_expr, sizeof(_final_expr),
                                                        "({ _ltng_retbuf_fmt(\"true\"); })");
                                                    result_type = TYPE_STRING;
                                                } else {
                                                    error_state_add(parser_errors, _cm.line, _cm.col, "Unknown array method");
                                                    pos += snprintf(out + pos, out_size - pos, "0");
                                                    _chain_err = 1; break;
                                                }

                                                /* Check for chained .method() on same array */
                                                if (lexer_peek(lexer).type == TOKEN_DOT) {
                                                    int saved_cdot = lexer->current;
                                                    lexer_next(lexer); /* consume . */
                                                    Token nm = lexer_peek(lexer);
                                                    if ((nm.type == TOKEN_IDENT || token_is_name(nm)) &&
                                                        (strcmp(nm.value, "push") == 0 || strcmp(nm.value, "pop") == 0 ||
                                                         strcmp(nm.value, "insert") == 0 || strcmp(nm.value, "remove") == 0 ||
                                                         strcmp(nm.value, "contains") == 0 || strcmp(nm.value, "index_of") == 0 ||
                                                         strcmp(nm.value, "sort") == 0 || strcmp(nm.value, "reverse") == 0)) {
                                                        lexer_next(lexer); /* consume method name */
                                                        _cm = nm;
                                                        _is_chained = 1;
                                                        continue;
                                                    }
                                                    lexer->current = saved_cdot; /* not a chainable array method */
                                                }
                                                break;
                                            }
                                            if (!_chain_err) {
                                                /* Capture final result, free temp array copy, return result */
                                                pos += snprintf(out + pos, out_size - pos,
                                                    "({ %schar *__fres = (char *)(%s); for(int _fi=0;_fi<__tl;_fi++){if(__td[_fi])free(__td[_fi]);} free(__td); __fres; })",
                                                    _chain_prefix, _final_expr);
                                                /* result_type already set by last method */
                                            } else {
                                                /* On error, still emit cleanup if possible */
                                                pos += snprintf(out + pos, out_size - pos,
                                                    "({ %sfor(int _fi=0;_fi<__tl;_fi++){if(__td[_fi])free(__td[_fi]);} free(__td); (char *)\"0\"; })",
                                                    _chain_prefix);
                                            }
                                            goto _arr_method_result_done;
                                        }

                                        /* copy()/slice() in general expression: create temp array,
                                           allow chaining, return final scalar or formatted array. */
                                        if ((mname.type == TOKEN_IDENT || token_is_name(mname)) &&
                                            (strcmp(mname.value, "copy") == 0 ||
                                             strcmp(mname.value, "slice") == 0)) {
                                            if (lexer_peek(lexer).type != TOKEN_LPAREN) {
                                                error_state_add(parser_errors, mname.line, mname.col, "Expected '(' after array method");
                                                skip_to_newline(lexer);
                                                pos += snprintf(out + pos, out_size - pos, "0");
                                                goto _arr_method_result_done;
                                            }
                                            lexer_next(lexer); /* consume ( */
                                            char *_chain_prefix = (char*)malloc(16384); if(!_chain_prefix){pos+=snprintf(out+pos,out_size-pos,"0");goto _arr_method_result_done;} _chain_prefix[0]=0; int _cp = 0;
                                            _cp += snprintf(_chain_prefix + _cp, 16384 - _cp,
                                                "char **__td=NULL; int __tl=0; int __tc=0; ");
                                            if (strcmp(mname.value, "copy") == 0) {
                                                if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                                                    error_state_add(parser_errors, lexer_peek(lexer).line, lexer_peek(lexer).col, "Expected ')' after copy()");
                                                    skip_to_newline(lexer); pos += snprintf(out + pos, out_size - pos, "0"); goto _arr_method_result_done;
                                                }
                                                lexer_next(lexer); /* consume ) */
                                                _cp += snprintf(_chain_prefix + _cp, 16384 - _cp,
                                                    "_ltng_arr_copy(%s, &__td, &__tl, &__tc, %s); ",
                                                    arrname, lenname);
                                            } else { /* slice */
                                                char s0[MAX_STRING_LEN], s1[MAX_STRING_LEN];
                                                collect_expression(lexer, table, s0, sizeof(s0), 1, 1, NULL, NULL, NULL, NULL);
                                                if (lexer_peek(lexer).type != TOKEN_COMMA) {
                                                    error_state_add(parser_errors, lexer_peek(lexer).line, lexer_peek(lexer).col, "Expected ',' after slice start");
                                                    skip_to_newline(lexer); pos += snprintf(out + pos, out_size - pos, "0"); goto _arr_method_result_done;
                                                }
                                                lexer_next(lexer); /* , */
                                                collect_expression(lexer, table, s1, sizeof(s1), 1, 0, NULL, NULL, NULL, NULL);
                                                if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                                                    error_state_add(parser_errors, lexer_peek(lexer).line, lexer_peek(lexer).col, "Expected ')' after slice end");
                                                    skip_to_newline(lexer); pos += snprintf(out + pos, out_size - pos, "0"); goto _arr_method_result_done;
                                                }
                                                lexer_next(lexer); /* ) */
                                                _cp += snprintf(_chain_prefix + _cp, 16384 - _cp,
                                                    "_ltng_arr_slice(%s, %s, (int)(%s), (int)(%s), &__td, &__tl, &__tc); ",
                                                    arrname, lenname, s0, s1);
                                            }
                                            const char *_wa = "__td", *_wl = "__tl", *_wc = "__tc";
                                            char _final_expr[4096] = {0};
                                            int _chain_err = 0;
                                            int _has_scalar_result = 0;
                                            int _chain_step = 0;
                                            /* check for chained array methods */
                                            Token _cm = {0};
                                            int _has_chain = 0;
                                            if (lexer_peek(lexer).type == TOKEN_DOT) {
                                                int saved_cdot = lexer->current;
                                                lexer_next(lexer);
                                                Token nm = lexer_peek(lexer);
                                                if ((nm.type == TOKEN_IDENT || token_is_name(nm)) &&
                                                    (strcmp(nm.value, "push")==0 || strcmp(nm.value, "pop")==0 ||
                                                     strcmp(nm.value, "insert")==0 || strcmp(nm.value, "remove")==0 ||
                                                     strcmp(nm.value, "contains")==0 || strcmp(nm.value, "index_of")==0 ||
                                                     strcmp(nm.value, "sort")==0 || strcmp(nm.value, "reverse")==0)) {
                                                    lexer_next(lexer);
                                                    _cm = nm; _has_chain = 1;
                                                } else { lexer->current = saved_cdot; }
                                            }
                                            while (_has_chain && !_chain_err) {
                                                _chain_step++;
                                                if (lexer_peek(lexer).type != TOKEN_LPAREN) { _chain_err = 1; break; }
                                                lexer_next(lexer);
                                                _has_scalar_result = 1;
                                                if (strcmp(_cm.value, "push") == 0) {
                                                    char arg_expr[MAX_STRING_LEN]; VarType at = collect_expression(lexer, table, arg_expr, sizeof(arg_expr), 1, 0, NULL, NULL, NULL, NULL);
                                                    if (lexer_peek(lexer).type != TOKEN_RPAREN) { _chain_err=1; break; } lexer_next(lexer);
                                                    const char *fmt = (at==TYPE_INT)?"%lld":(at==TYPE_FLOAT)?"%g":"%s";
                                                    const char *cast = (at==TYPE_INT)?"(long long)":(at==TYPE_FLOAT)?"(double)":"";
                                                    _cp += snprintf(_chain_prefix+_cp,16384-_cp, "char *__apv%d=_ltng_ds_fmt(\"%s\",%s(%s)); _ltng_arr_push(&%s,&%s,&%s,__apv%d); char *__apr%d=_ltng_retbuf_fmt(\"%%s\",__apv%d); free(__apv%d); ", _chain_step,fmt,cast,arg_expr,_wa,_wl,_wc,_chain_step,_chain_step,_chain_step,_chain_step);
                                                    snprintf(_final_expr,sizeof(_final_expr),"__apr%d",_chain_step);
                                                } else if (strcmp(_cm.value, "pop") == 0) {
                                                    if (lexer_peek(lexer).type == TOKEN_RPAREN) { lexer_next(lexer); _cp += snprintf(_chain_prefix+_cp,16384-_cp,"char *__pv%d=_ltng_arr_pop(%s,&%s); ",_chain_step,_wa,_wl); snprintf(_final_expr,sizeof(_final_expr),"__pv%d",_chain_step); }
                                                    else { char ie[MAX_STRING_LEN]; collect_expression(lexer,table,ie,sizeof(ie),1,0,NULL,NULL,NULL,NULL); if(lexer_peek(lexer).type!=TOKEN_RPAREN){_chain_err=1;break;} lexer_next(lexer); _cp += snprintf(_chain_prefix+_cp,16384-_cp,"char *__pv%d=_ltng_arr_pop_index(%s,&%s,(int)(%s)); ",_chain_step,_wa,_wl,ie); snprintf(_final_expr,sizeof(_final_expr),"__pv%d",_chain_step); }
                                                } else if (strcmp(_cm.value, "insert") == 0) {
                                                    char ie[MAX_STRING_LEN]; collect_expression(lexer,table,ie,sizeof(ie),1,1,NULL,NULL,NULL,NULL);
                                                    if(lexer_peek(lexer).type!=TOKEN_COMMA){_chain_err=1;break;} lexer_next(lexer);
                                                    char ae[MAX_STRING_LEN]; VarType at=collect_expression(lexer,table,ae,sizeof(ae),1,0,NULL,NULL,NULL,NULL);
                                                    if(lexer_peek(lexer).type!=TOKEN_RPAREN){_chain_err=1;break;} lexer_next(lexer);
                                                    const char *fmt = (at==TYPE_INT)?"%lld":(at==TYPE_FLOAT)?"%g":"%s";
                                                    const char *cast = (at==TYPE_INT)?"(long long)":(at==TYPE_FLOAT)?"(double)":"";
                                                    _cp += snprintf(_chain_prefix+_cp,16384-_cp,"char *__aiv%d=_ltng_ds_fmt(\"%s\",%s(%s)); _ltng_arr_insert(&%s,&%s,&%s,(int)(%s),__aiv%d); char *__air%d=_ltng_retbuf_fmt(\"%%s\",__aiv%d); free(__aiv%d); ",_chain_step,fmt,cast,ae,_wa,_wl,_wc,ie,_chain_step,_chain_step,_chain_step,_chain_step);
                                                    snprintf(_final_expr,sizeof(_final_expr),"__air%d",_chain_step);
                                                } else if (strcmp(_cm.value, "remove") == 0) {
                                                    char ie[MAX_STRING_LEN]; collect_expression(lexer,table,ie,sizeof(ie),1,0,NULL,NULL,NULL,NULL);
                                                    if(lexer_peek(lexer).type!=TOKEN_RPAREN){_chain_err=1;break;} lexer_next(lexer);
                                                    _cp += snprintf(_chain_prefix+_cp,16384-_cp,"char *__rv%d=_ltng_arr_pop_index(%s,&%s,(int)(%s)); ",_chain_step,_wa,_wl,ie);
                                                    snprintf(_final_expr,sizeof(_final_expr),"__rv%d",_chain_step);
                                                } else if (strcmp(_cm.value, "sort") == 0) {
                                                    if(lexer_peek(lexer).type!=TOKEN_RPAREN){_chain_err=1;break;} lexer_next(lexer);
                                                    _cp += snprintf(_chain_prefix+_cp,16384-_cp,"_ltng_arr_sort(%s,%s); ",_wa,_wl);
                                                    snprintf(_final_expr,sizeof(_final_expr),"({ _ltng_retbuf_fmt(\"true\"); })");
                                                } else if (strcmp(_cm.value, "reverse") == 0) {
                                                    if(lexer_peek(lexer).type!=TOKEN_RPAREN){_chain_err=1;break;} lexer_next(lexer);
                                                    _cp += snprintf(_chain_prefix+_cp,16384-_cp,"_ltng_arr_reverse(%s,%s); ",_wa,_wl);
                                                    snprintf(_final_expr,sizeof(_final_expr),"({ _ltng_retbuf_fmt(\"true\"); })");
                                                } else if (strcmp(_cm.value, "contains") == 0) {
                                                    char ae[MAX_STRING_LEN]; VarType at=collect_expression(lexer,table,ae,sizeof(ae),1,0,NULL,NULL,NULL,NULL);
                                                    if(lexer_peek(lexer).type!=TOKEN_RPAREN){_chain_err=1;break;} lexer_next(lexer);
                                                    const char *fmt = (at==TYPE_INT)?"%lld":(at==TYPE_FLOAT)?"%g":"%s";
                                                    const char *cast = (at==TYPE_INT)?"(long long)":(at==TYPE_FLOAT)?"(double)":"";
                                                    _cp += snprintf(_chain_prefix+_cp,16384-_cp,"char *__cv%d=_ltng_ds_fmt(\"%s\",%s(%s)); int __cr%d=_ltng_arr_contains(%s,%s,__cv%d); free(__cv%d); ",_chain_step,fmt,cast,ae,_chain_step,_wa,_wl,_chain_step,_chain_step);
                                                    snprintf(_final_expr,sizeof(_final_expr),"({ _ltng_retbuf_fmt(\"%%s\",__cr%d?\"true\":\"false\"); })",_chain_step);
                                                } else if (strcmp(_cm.value, "index_of") == 0) {
                                                    char ae[MAX_STRING_LEN]; VarType at=collect_expression(lexer,table,ae,sizeof(ae),1,0,NULL,NULL,NULL,NULL);
                                                    if(lexer_peek(lexer).type!=TOKEN_RPAREN){_chain_err=1;break;} lexer_next(lexer);
                                                    const char *fmt = (at==TYPE_INT)?"%lld":(at==TYPE_FLOAT)?"%g":"%s";
                                                    const char *cast = (at==TYPE_INT)?"(long long)":(at==TYPE_FLOAT)?"(double)":"";
                                                    _cp += snprintf(_chain_prefix+_cp,16384-_cp,"char *__iv%d=_ltng_ds_fmt(\"%s\",%s(%s)); int __ir%d=_ltng_arr_index_of(%s,%s,__iv%d); free(__iv%d); ",_chain_step,fmt,cast,ae,_chain_step,_wa,_wl,_chain_step,_chain_step);
                                                    snprintf(_final_expr,sizeof(_final_expr),"({ _ltng_retbuf_fmt(\"%%d\",__ir%d); })",_chain_step);
                                                } else { _chain_err=1; break; }
                                                /* next chain? */
                                                _has_chain = 0;
                                                if (lexer_peek(lexer).type == TOKEN_DOT) {
                                                    int sd = lexer->current; lexer_next(lexer); Token nm = lexer_peek(lexer);
                                                    if ((nm.type==TOKEN_IDENT||token_is_name(nm)) &&
                                                        (strcmp(nm.value,"push")==0||strcmp(nm.value,"pop")==0||strcmp(nm.value,"insert")==0||strcmp(nm.value,"remove")==0||strcmp(nm.value,"contains")==0||strcmp(nm.value,"index_of")==0||strcmp(nm.value,"sort")==0||strcmp(nm.value,"reverse")==0)) { lexer_next(lexer); _cm=nm; _has_chain=1; }
                                                    else lexer->current = sd;
                                                }
                                            }
                                            if (_chain_err) { pos += snprintf(out+pos,out_size-pos,"0"); goto _arr_method_result_done; }
                                            if (!_has_scalar_result) {
                                                /* No chain - return array formatted as string */
                                                snprintf(_final_expr,sizeof(_final_expr),
                                                    "({ char *__r=_ltng_ds_new(\"\"); for(int _fi=0;_fi<__tl;_fi++){ if(_fi) _ltng_ds_append(&__r,\", \"); _ltng_ds_append(&__r,__td[_fi]?__td[_fi]:\"\"); } __r; })");
                                            }
                                            pos += snprintf(out+pos,out_size-pos,
                                                "({ %schar *__fres=(char*)(%s); for(int _fi=0;_fi<__tl;_fi++){if(__td[_fi])free(__td[_fi]);} free(__td); __fres; })",
                                                _chain_prefix, _final_expr);
                                            result_type = TYPE_STRING;
                                            goto _arr_method_result_done;
                                        }

                                        /* Not a recognized array operation - rewind to the dot */
                                        lexer->current = saved_dot;
                                    }
                                }
                                /* Plain array variable reference: emit ah handle for expression contexts.
                                   But if followed by [index] or unrecognized .method, let later handlers deal with it. */
                                {
                                    Token _nxt2 = lexer_peek(lexer);
                                    if (_nxt2.type == TOKEN_LBRACKET) {
                                        /* let array-access handler below handle it */
                                        break;
                                    }
                                }
                                pos += snprintf(out + pos, out_size - pos,
                                    "_ltng_ah_pack(%s_data, %s_len, %s_cap)", cname, cname, cname);
                                if (!type_locked) result_type = TYPE_ARRAY;
                                pending_divmod = 0; pending_mod_op_pos = -1;
                                break;
                        }
                    }
                }
                /* check for postfix ++ or -- */
                {
                    Token nxt = lexer_peek(lexer);
                    if (nxt.type == TOKEN_PLUS_PLUS || nxt.type == TOKEN_MINUS_MINUS) {
                        lexer_next(lexer); /* consume postfix op */
                        if (!is_bool_value(tok.value) && !is_type_keyword(tok.value) && !is_ltng_reserved(tok.value)) {
                            char mangled2[MAX_IDENTIFIER_LEN];
                            mangle_name(tok.value, mangled2, MAX_IDENTIFIER_LEN);
                            if (symbol_table_has(table, mangled2) && !symbol_table_is_const(table, mangled2)) {
                                /* Rewind output: replace variable conversion with statement expression */
                                pos = saved_pos;
                                if (nxt.type == TOKEN_PLUS_PLUS) {
                                    VarType vt = symbol_table_get(table, mangled2);
                                    if (vt == TYPE_FLOAT) {
                                        pos += snprintf(out + pos, out_size - pos,
                                            "({ double _t = atof(%s); snprintf(%s, sizeof(%s), \"%%g\", _t + 1.0); _t; })",
                                            mangled2, mangled2, mangled2);
                                        if (!type_locked) result_type = TYPE_FLOAT;
                                    } else if (vt == TYPE_BOOL) {
                                        pos += snprintf(out + pos, out_size - pos,
                                            "({ long long _t = (long long)(%s[0]=='t'||%s[0]=='1'?1:0); snprintf(%s, sizeof(%s), \"%%lld\", _t + 1); _t; })",
                                            mangled2, mangled2, mangled2, mangled2);
                                        if (!type_locked) result_type = TYPE_INT;
                                    } else {
                                        pos += snprintf(out + pos, out_size - pos,
                                            "({ long long _t = (long long)atoll(%s); snprintf(%s, sizeof(%s), \"%%lld\", _t + 1); _t; })",
                                            mangled2, mangled2, mangled2);
                                        if (!type_locked) result_type = TYPE_INT;
                                    }
                                } else { /* MINUS_MINUS */
                                    VarType vt = symbol_table_get(table, mangled2);
                                    if (vt == TYPE_FLOAT) {
                                        pos += snprintf(out + pos, out_size - pos,
                                            "({ double _t = atof(%s); snprintf(%s, sizeof(%s), \"%%g\", _t - 1.0); _t; })",
                                            mangled2, mangled2, mangled2);
                                        if (!type_locked) result_type = TYPE_FLOAT;
                                    } else if (vt == TYPE_BOOL) {
                                        pos += snprintf(out + pos, out_size - pos,
                                            "({ long long _t = (long long)(%s[0]=='t'||%s[0]=='1'?1:0); snprintf(%s, sizeof(%s), \"%%lld\", _t - 1); _t; })",
                                            mangled2, mangled2, mangled2, mangled2);
                                        if (!type_locked) result_type = TYPE_INT;
                                    } else {
                                        pos += snprintf(out + pos, out_size - pos,
                                            "({ long long _t = (long long)atoll(%s); snprintf(%s, sizeof(%s), \"%%lld\", _t - 1); _t; })",
                                            mangled2, mangled2, mangled2);
                                        if (!type_locked) result_type = TYPE_INT;
                                    }
                                }
                                continue;
                            } else {
                                pos = saved_pos;
                                pos += snprintf(out + pos, out_size - pos,
                                    "({ snprintf(%s, sizeof(%s), \"%%s\", %s); %s; })",
                                    mangled2, mangled2, mangled2, mangled2);
                                continue;
                            }
                        }
                    }
                }
                /* Array access: x[1], m[1][1], t[1][1][1] (1-based, negative wraps) */
                {
                    Token nxt = lexer_peek(lexer);
                    if (nxt.type == TOKEN_LBRACKET) {
                        char mangled[MAX_IDENTIFIER_LEN];
                        mangle_name(tok.value, mangled, MAX_IDENTIFIER_LEN);
                        VarType vt = symbol_table_get(table, mangled);

                        if (vt == TYPE_ARRAY) {
                            char index_exprs[32][MAX_STRING_LEN];
                            int index_count = 0;
                            int index_error = 0;

                            while (nxt.type == TOKEN_LBRACKET) {
                                lexer_next(lexer); /* consume [ */
                                if (index_count >= 32) {
                                    error_state_add(parser_errors, nxt.line, nxt.col,
                                        "Too many array dimensions (max 32)");
                                    skip_to_newline(lexer);
                                    index_error = 1;
                                    break;
                                }
                                collect_expression(lexer, table, index_exprs[index_count],
                                                   sizeof(index_exprs[index_count]), 1, 0, NULL, NULL, NULL, NULL);
                                Token rbracket = lexer_peek(lexer);
                                if (rbracket.type != TOKEN_RBRACKET) {
                                    error_state_add(parser_errors, rbracket.line, rbracket.col,
                                        "Expected ']' after array index");
                                    skip_to_newline(lexer);
                                    index_error = 1;
                                    break;
                                }
                                lexer_next(lexer); /* consume ] */
                                index_count++;
                                nxt = lexer_peek(lexer);
                            }

                            if (!index_error && index_count > 0) {
                                int *dims = NULL;
                                int dim_count = 0;
                                int have_dims = symbol_table_get_array_dims(
                                    table, mangled, &dims, &dim_count);

                                if (!have_dims) {
                                    /* Unknown/legacy arrays behave like 1D. */
                                    dim_count = 1;
                                    dims = malloc(sizeof(int));
                                    dims[0] = 0;
                                }

                                if (index_count != dim_count) {
                                    char *errmsg = _ltng_cfmt("Array '%s' has %d dimension(s) but %d index(es) were given", tok.value, dim_count, index_count);
                                    error_state_add(parser_errors, tok.line, tok.col, errmsg ? errmsg : "");
                                    free(errmsg);
                                    pos += snprintf(out + pos, out_size - pos, "0");
                                } else {
                                    char arrname[MAX_IDENTIFIER_LEN];
                                    snprintf(arrname, sizeof(arrname), "%s_data", mangled);
                                    char lenname[MAX_IDENTIFIER_LEN];
                                    snprintf(lenname, sizeof(lenname), "%s_len", mangled);

                                    pos += snprintf(out + pos, out_size - pos, "({ ");
                                    for (int d = 0; d < index_count; d++) {
                                        /* For 1-D arrays use the live length so negative indices see
                                           pushes/pops. Static dimensions remain the shape for N-D. */
                                        if (dim_count == 1) {
                                            /* Live length plus a non-negative OOB sentinel avoids
                                               double-adjustment in the runtime getter. */
                                            pos += snprintf(out + pos, out_size - pos,
                                                "int __i%d = (int)(%s); __i%d = __i%d > 0 ? __i%d - 1 : __i%d + (%s); if (__i%d < 0 || __i%d >= %s) __i%d = %s; ",
                                                d, index_exprs[d], d, d, d, d, lenname, d, d, lenname, d, lenname);
                                        } else {
                                            pos += snprintf(out + pos, out_size - pos,
                                                "int __i%d = (int)(%s); __i%d = __i%d > 0 ? __i%d - 1 : __i%d + (%d); ",
                                                d, index_exprs[d], d, d, d, d, dims[d]);
                                        }
                                    }
                                    /* Compute linear index: __idx = __i0; __idx = __idx * d1 + __i1; ... */
                                    pos += snprintf(out + pos, out_size - pos, "int __idx = __i0");
                                    for (int d = 1; d < index_count; d++) {
                                        pos += snprintf(out + pos, out_size - pos,
                                            "; __idx = __idx * %d + __i%d", dims[d], d);
                                    }
                                    VarType _aet = symbol_table_get_array_elem_type(table, mangled);
                                    /* If we're already in a string-building context, keep string form.
                                       Otherwise, emit typed access for known homogeneous element types
                                       to support numeric/bool semantics, typeof, and comparisons. */
                                    int _in_str_ctx = (result_type == TYPE_STRING && !type_locked);
                                    if (!_in_str_ctx && _aet == TYPE_INT) {
                                        pos += snprintf(out + pos, out_size - pos,
                                            "; const char *__av = _ltng_arr_get(%s, %s, __idx); atoll(__av ? __av : \"\"); })",
                                            arrname, lenname);
                                        if (!type_locked) result_type = TYPE_INT;
                                    } else if (!_in_str_ctx && _aet == TYPE_FLOAT) {
                                        pos += snprintf(out + pos, out_size - pos,
                                            "; const char *__av = _ltng_arr_get(%s, %s, __idx); atof(__av ? __av : \"\"); })",
                                            arrname, lenname);
                                        if (!type_locked) {
                                            if (result_type == TYPE_INT) result_type = TYPE_FLOAT;
                                            else result_type = TYPE_FLOAT;
                                        }
                                    } else if (!_in_str_ctx && _aet == TYPE_BOOL) {
                                        pos += snprintf(out + pos, out_size - pos,
                                            "; const char *__av = _ltng_arr_get(%s, %s, __idx); (__av && (__av[0]=='t' || __av[0]=='1')) ? 1 : 0; })",
                                            arrname, lenname);
                                        if (!type_locked) result_type = TYPE_BOOL;
                                    } else {
                                        pos += snprintf(out + pos, out_size - pos,
                                            "; _ltng_arr_get(%s, %s, __idx); })",
                                            arrname, lenname);
                                        if (_aet == TYPE_STRING || _aet == TYPE_CHAR) {
                                            result_type = TYPE_STRING;
                                            array_access_used = 1;
                                        } else if (_aet == TYPE_INT || _aet == TYPE_FLOAT || _aet == TYPE_BOOL) {
                                            /* In string context, treat as string value (OOB-safe). */
                                            result_type = TYPE_STRING;
                                            array_access_used = 1;
                                        } else {
                                            result_type = TYPE_STRING;
                                            array_access_used = 1;
                                        }
                                    }
                                }
                                if (!have_dims) free(dims);
                            } else if (!index_error) {
                                pos += snprintf(out + pos, out_size - pos, "0");
                            }
                        } else {
                            char *errmsg = _ltng_cfmt("Variable '%s' is not an array", tok.value);
                            error_state_add(parser_errors, tok.line, tok.col, errmsg ? errmsg : "");
                            free(errmsg);
                            pos += snprintf(out + pos, out_size - pos, "0");
                            /* Consume remaining index brackets so parsing stays in sync. */
                            while (lexer_peek(lexer).type == TOKEN_LBRACKET) {
                                lexer_next(lexer);
                                skip_to_newline(lexer);
                            }
                        }
                    }
                }
                _arr_method_result_done: ;
                }
                break;
            case TOKEN_LPAREN:
                pos += snprintf(out + pos, out_size - pos, "(");
                paren_depth++;
                if (paren_depth < 64) {
                    depth_content_start[paren_depth] = pos;
                    depth_in_str_concat[paren_depth] = (has_string_concat || result_type == TYPE_STRING) ? 1 : 0;
                }
                break;
            case TOKEN_RPAREN:
                pos += snprintf(out + pos, out_size - pos, ")");
                if (paren_depth > 0) {
                    int _was_str_concat = (paren_depth < 64) ? depth_in_str_concat[paren_depth] : 0;
                    paren_depth--;
                    /* If we were inside a string-concat context at this paren level,
                       restore string context so subsequent outer + operations are treated
                       as string concat, not numeric addition. */
                    if (_was_str_concat && has_string_concat) {
                        if (result_type != TYPE_STRING) {
                            /* The paren subexpr produced a numeric result but is inside
                               string concat; the overall expression is still string concat. */
                            result_type = TYPE_STRING;
                        }
                    }
                }
                break;
            case TOKEN_PLUS:
            case TOKEN_MINUS:
            case TOKEN_STAR:
            case TOKEN_SLASH:
            case TOKEN_PERCENT:
                if (result_type == TYPE_STRING) {
                    if (tok.type == TOKEN_PLUS) {
                        /* In string context, + can be string concat or numeric add.
                           Inside parens in a string-concat subexpr, prefer numeric add when
                           both operands look numeric. */
                        int _in_str_sub = (paren_depth > 0 && paren_depth < 64 && depth_in_str_concat[paren_depth]);
                        int _numeric_add = 0;
                        if (_in_str_sub) {
                            Token _rtok = lexer_peek(lexer);
                            int _right_numeric = 0;
                            if (_rtok.type == TOKEN_INT_LIT || _rtok.type == TOKEN_FLOAT_LIT) {
                                _right_numeric = 1;
                            } else if ((_rtok.type == TOKEN_IDENT || token_is_name(_rtok)) && !is_bool_value(_rtok.value)) {
                                char _rm[MAX_IDENTIFIER_LEN]; mangle_name(_rtok.value, _rm, MAX_IDENTIFIER_LEN);
                                VarType _rvt = TYPE_NULL;
                                if (symbol_table_has(table, _rm)) _rvt = symbol_table_get(table, _rm);
                                if (_rvt == TYPE_INT || _rvt == TYPE_FLOAT || _rvt == TYPE_BOOL) _right_numeric = 1;
                            } else if (_rtok.type == TOKEN_LPAREN) {
                                /* nested subexpr - could be numeric */
                                _right_numeric = 1;
                            }
                            if (_right_numeric) {
                                /* check left side is numeric */
                                int _left_num = 0;
                                if (pos >= 1) {
                                    int _i = pos - 1;
                                    while (_i >= 0 && (out[_i] == ' ' || out[_i] == '\t')) _i--;
                                    if (_i >= 0 && (isdigit((unsigned char)out[_i]) || out[_i] == ')')) _left_num = 1;
                                }
                                if (!_left_num && pos >= 6) {
                                    { char *_tail = (char*)calloc(1,32);
                                    int _tl = pos < 30 ? pos : 30;
                                    if(_tail){ memcpy(_tail, out + pos - _tl, _tl);
                                    if (strstr(_tail, "atoll(") || strstr(_tail, "atof(")) _left_num = 1; free(_tail); } }
                                }
                                _numeric_add = _left_num;
                            }
                        }
                        if (_numeric_add) {
                            pos += snprintf(out + pos, out_size - pos, " + ");
                            arithmetic_used = 1;
                            result_type = TYPE_INT;
                        } else {
                            pos += snprintf(out + pos, out_size - pos, " + ");
                            arithmetic_used = 1;
                            has_string_concat = 1;
                        }
                    } else if (tok.type == TOKEN_STAR) {
                        /* * in string context: string repeat, or numeric multiply inside parens
                           in a string-concat subexpression when left operand is numeric. */
                        int _in_str_sub = (paren_depth > 0 && paren_depth < 64 && depth_in_str_concat[paren_depth]);
                        int _numeric_mul = 0;
                        if (_in_str_sub) {
                            /* Check if this looks like numeric multiply (not string repeat) */
                            /* Peek right operand type */
                            Token _rtok = lexer_peek(lexer);
                            if (_rtok.type == TOKEN_INT_LIT || _rtok.type == TOKEN_FLOAT_LIT) {
                                /* "str" * 3 is string repeat, but (x * 3) where x is numeric is multiply.
                                   Check left side: if buffer ends with a numeric form, it's numeric. */
                                int _left_num = 0;
                                if (pos >= 1) {
                                    /* look for atoll(, atof(, digit, ) as end of numeric expr */
                                    int _i = pos - 1;
                                    while (_i >= 0 && (out[_i] == ' ' || out[_i] == '\t')) _i--;
                                    if (_i >= 0 && (isdigit((unsigned char)out[_i]) || out[_i] == ')')) _left_num = 1;
                                }
                                /* Also check if left side contains atoll/atof at end */
                                if (!_left_num && pos >= 6) {
                                    if (strstr(out + (pos > 20 ? pos - 20 : 0), "atoll(") ||
                                        strstr(out + (pos > 20 ? pos - 20 : 0), "atof(")) _left_num = 1;
                                }
                                /* If left side starts with " in this subexpr context, it's string repeat */
                                if (_left_num) {
                                    /* but verify left side of * in recent buffer isn't a string literal */
                                    int _j = pos - 1;
                                    while (_j >= 0 && out[_j] != '"' && out[_j] != '(' && out[_j] != '+' && out[_j] != '*') _j--;
                                    /* rough heuristic - if we see quote right before in same paren, string repeat */
                                }
                                _numeric_mul = _left_num;
                            } else if ((_rtok.type == TOKEN_IDENT || token_is_name(_rtok)) && !is_bool_value(_rtok.value)) {
                                char _rm[MAX_IDENTIFIER_LEN]; mangle_name(_rtok.value, _rm, MAX_IDENTIFIER_LEN);
                                VarType _rvt = TYPE_NULL;
                                if (symbol_table_has(table, _rm)) _rvt = symbol_table_get(table, _rm);
                                if (_rvt == TYPE_INT || _rvt == TYPE_FLOAT || _rvt == TYPE_BOOL) {
                                    /* right is numeric; check if left is also numeric */
                                    int _left_num = 0;
                                    if (pos >= 1) {
                                        int _i = pos - 1;
                                        while (_i >= 0 && (out[_i] == ' ' || out[_i] == '\t')) _i--;
                                        if (_i >= 0 && (out[_i] == ')')) _left_num = 1;
                                        /* check for numeric conversions */
                                    }
                                    if (!_left_num && pos >= 6) {
                                        char *_tail = (char*)calloc(1,32);
                                        int _tl = pos < 30 ? pos : 30;
                                        if(_tail){ memcpy(_tail, out + pos - _tl, _tl);
                                        if (strstr(_tail, "atoll(") || strstr(_tail, "atof(")) _left_num = 1; free(_tail); }
                                    }
                                    _numeric_mul = _left_num;
                                }
                            }
                        }
                        if (_numeric_mul) {
                            pos += snprintf(out + pos, out_size - pos, " * ");
                            arithmetic_used = 1;
                            /* stay in numeric subexpr mode - result_type becomes numeric context */
                            result_type = TYPE_INT;
                        } else {
                            pos += snprintf(out + pos, out_size - pos, " * ");
                            arithmetic_used = 1;
                            has_string_mult = 1;
                        }
                    } else {
                        /* -, /, % are never string operations. */
                        int _in_str_sub = (paren_depth > 0 && paren_depth < 64 && depth_in_str_concat[paren_depth]);
                        if (_in_str_sub) {
                            /* Numeric subexpression inside string-concat context */
                            const char *_ops = tok.type == TOKEN_MINUS ? " - " :
                                tok.type == TOKEN_SLASH ? " / " : " % ";
                            if (tok.type == TOKEN_PERCENT) pending_mod_op_pos = pos;
                            else pending_mod_op_pos = -1;
                            pos += snprintf(out + pos, out_size - pos, "%s", _ops);
                            arithmetic_used = 1;
                            /* switch to numeric mode for this subexpr */
                            result_type = TYPE_INT;
                            if (tok.type == TOKEN_SLASH || tok.type == TOKEN_PERCENT) {
                                pending_divmod = 1;
                                pending_divmod_is_mod = (tok.type == TOKEN_PERCENT);
                                pending_divmod_line = tok.line;
                                pending_divmod_col = tok.col;
                            } else {
                                pending_divmod = 0;
                            }
                        } else {
                            error_state_add(parser_errors, tok.line, tok.col,
                                "Cannot use arithmetic on a string");
                            pos += snprintf(out + pos, out_size - pos, "0");
                            pending_divmod = 0;
                        }
                    }
                } else {
                    {
                        const char *_ops = tok.type == TOKEN_PLUS ? "+" :
                            tok.type == TOKEN_MINUS ? "-" :
                            tok.type == TOKEN_STAR ? "*" :
                            tok.type == TOKEN_SLASH ? "/" : "%";
                        if (tok.type == TOKEN_PERCENT) pending_mod_op_pos = pos;
                        else pending_mod_op_pos = -1;
                        pos += snprintf(out + pos, out_size - pos, "%s", _ops);
                    }
                    arithmetic_used = 1;
                    if (!type_locked && result_type == TYPE_BOOL) result_type = TYPE_INT;
                    if (tok.type == TOKEN_SLASH || tok.type == TOKEN_PERCENT) {
                        pending_divmod = 1;
                        pending_divmod_is_mod = (tok.type == TOKEN_PERCENT);
                        pending_divmod_line = tok.line;
                        pending_divmod_col = tok.col;
                    } else {
                        pending_divmod = 0;
                    }
                }
                break;
            case TOKEN_AMP_AMP:
            case TOKEN_PIPE_PIPE: {
                Token next_tok = lexer_peek(lexer);
                /* Use logical &&/|| when the left side is a comparison/logical result,
                   or when the right side is not a plain integer value (e.g. a comparison).
                   Reserve bitwise &/| for explicit integer-to-integer contexts. */
                int use_bitwise = 0;
                if (!comparison_or_logical && result_type == TYPE_INT && token_is_int(next_tok, table)) {
                    /* Both sides look like plain ints and left is not a comparison result.
                       Peek further: if next is followed by comparison op, it's logical. */
                    int saved_c = lexer->current;
                    lexer_next(lexer); /* consume next token temporarily */
                    Token after = lexer_peek(lexer);
                    if (after.type == TOKEN_EQ_EQ || after.type == TOKEN_BANG_EQ ||
                        after.type == TOKEN_LT || after.type == TOKEN_GT ||
                        after.type == TOKEN_LT_EQ || after.type == TOKEN_GT_EQ) {
                        use_bitwise = 0; /* right side is a comparison, use logical */
                    } else {
                        use_bitwise = 1;
                    }
                    lexer->current = saved_c;
                }
                if (use_bitwise) {
                    pos += snprintf(out + pos, out_size - pos, "%s",
                        (tok.type == TOKEN_AMP_AMP) ? " & " : " | ");
                } else {
                    pos += snprintf(out + pos, out_size - pos, "%s",
                        (tok.type == TOKEN_AMP_AMP) ? " && " : " || ");
                }
                result_type = TYPE_INT;
                comparison_or_logical = 1;
                break;
            }
            case TOKEN_CARET_CARET:
                pos += snprintf(out + pos, out_size - pos, " ^ ");
                result_type = TYPE_INT;
                comparison_or_logical = 1;
                break;
            case TOKEN_BANG: {
                Token next_tok = lexer_peek(lexer);
                if (token_is_int(next_tok, table)) {
                    pos += snprintf(out + pos, out_size - pos, "~");
                } else {
                    pos += snprintf(out + pos, out_size - pos, "!");
                }
                result_type = TYPE_INT;
                comparison_or_logical = 1;
                break;
            }
            case TOKEN_EQ_EQ:   pos += snprintf(out + pos, out_size - pos, "=="); result_type = TYPE_INT; comparison_or_logical = 1; break;
            case TOKEN_BANG_EQ: pos += snprintf(out + pos, out_size - pos, "!="); result_type = TYPE_INT; comparison_or_logical = 1; break;
            case TOKEN_LT:      pos += snprintf(out + pos, out_size - pos, "<");  result_type = TYPE_INT; comparison_or_logical = 1; break;
            case TOKEN_GT:      pos += snprintf(out + pos, out_size - pos, ">");  result_type = TYPE_INT; comparison_or_logical = 1; break;
            case TOKEN_LT_EQ:   pos += snprintf(out + pos, out_size - pos, "<="); result_type = TYPE_INT; comparison_or_logical = 1; break;
            case TOKEN_GT_EQ:   pos += snprintf(out + pos, out_size - pos, ">="); result_type = TYPE_INT; comparison_or_logical = 1; break;
            case TOKEN_RETURN: case TOKEN_BREAK: case TOKEN_CONTINUE: case TOKEN_FUNCTION:
            case TOKEN_SWITCH: case TOKEN_CASE: case TOKEN_DEFAULT:
            case TOKEN_FROM: case TOKEN_TO: case TOKEN_STEP: {
                /* keyword-as-identifier in expressions (C-reserved compat) */
                char mangled[MAX_IDENTIFIER_LEN];
                mangle_name(tok.value, mangled, MAX_IDENTIFIER_LEN);
                if (!symbol_table_has(table, mangled)) {
                    char *errmsg = _ltng_cfmt("Variable '%s' not declared", tok.value);
                    error_state_add(parser_errors, tok.line, tok.col, errmsg ? errmsg : "");
                    free(errmsg);
                    pos += snprintf(out + pos, out_size - pos, "0");
                } else {
                    VarType vtype = symbol_table_get(table, mangled);
                    if (vtype == TYPE_STRING) { pos += snprintf(out + pos, out_size - pos, "atof(%s)", mangled); if (!type_locked) result_type = TYPE_INT; }
                    else if (vtype == TYPE_INT) pos += snprintf(out + pos, out_size - pos, "atoll(%s)", mangled);
                    else if (vtype == TYPE_BOOL) pos += snprintf(out + pos, out_size - pos, "(%s[0]=='t'||%s[0]=='T'||%s[0]=='1'?1:0)", mangled, mangled, mangled);
                    else { pos += snprintf(out + pos, out_size - pos, "atof(%s)", mangled); if (!type_locked) result_type = TYPE_INT; }
                }
                break;
            }
            case TOKEN_COMMA:
                if (!stop_on_comma) {
                    error_state_add(parser_errors, tok.line, tok.col,
                        "Unexpected ',' in expression");
                }
                break;
            default:
                /* Unexpected token in expression context: report instead of silently dropping. */
                if (tok.type != TOKEN_EOF && tok.type != TOKEN_NEWLINE) {
                    char *errmsg = _ltng_cfmt("Unexpected token '%s' in expression", tok.value[0] ? tok.value : "?");
                    error_state_add(parser_errors, tok.line, tok.col, errmsg ? errmsg : "");
                    free(errmsg);
                }
                break;
        }
        if (pos >= out_size - 1) break;
    }
    out[pos] = '\0';
    sanitize_mod_atof(out, out_size);
    /* Convert atof/atof add patterns to runtime _ltng_add_sn for string-producing
       or dynamic/untyped numeric expressions (where + may mean concat at runtime).
       Keep pure statically-typed numeric arithmetic to preserve int/float semantics. */
    if (!has_string_concat && !has_string_mult &&
        (result_type == TYPE_STRING || has_dynamic_numeric)) {
        while (optimize_add_concat(out, out_size, pos)) {
            result_type = TYPE_STRING;
        }
    }
    /* If the expression contains top-level comparisons or logical operators,
       normalize string subexpressions per-segment and emit proper string
       comparisons (strcmp) instead of treating the whole buffer as one
       string concat/mult expression. */
    {
        int _has_cmp = 0, _d = 0, _in = 0; char _qc = 0;
        for (const char *_p = out; *_p; _p++) {
            if (_in) { if (*_p == '\\' && _p[1]) { _p++; continue; } if (*_p == _qc) _in = 0; continue; }
            if (*_p == '"' || *_p == '\'') { _in = 1; _qc = *_p; continue; }
            if (*_p == '(' || *_p == '{' || *_p == '[') { _d++; continue; }
            if (*_p == ')' || *_p == '}' || *_p == ']') { if (_d > 0) _d--; continue; }
            if (_d == 0) {
                if ((_p[0]=='='&&_p[1]=='=') || (_p[0]=='!'&&_p[1]=='=') ||
                    (_p[0]=='<'&&_p[1]=='=') || (_p[0]=='>'&&_p[1]=='=') ||
                    (_p[0]=='&'&&_p[1]=='&') || (_p[0]=='|'&&_p[1]=='|')) { _has_cmp = 1; break; }
                if (*_p == '<' || *_p == '>') { _has_cmp = 1; break; }
            }
        }
        if (_has_cmp) {
            normalize_mixed_expr(out, out_size, array_access_used, table);
            result_type = TYPE_INT;
            if (out_sl) *out_sl = sl;
            if (out_sc) *out_sc = sc;
            if (out_el) *out_el = el;
            if (out_ec) *out_ec = ec;
            return result_type;
        }
    }
    if (has_string_concat || has_string_mult || array_access_used || (result_type == TYPE_STRING && (strstr(out, "+") != NULL || strstr(out, "*") != NULL))) {
        /* Handle multiplication with higher precedence - split by + at top level first */
        if (strstr(out, "+") != NULL) {
            /* split by + */
            char tmp[2048];
            strncpy(tmp, out, 2047);
            tmp[2047] = 0;
            char parts[32][2048];
            int nparts = 0;
            char *start = tmp;
            int depth_paren = 0, depth_bracket = 0, depth_brace = 0;
            int in_str = 0;
            char q = 0;
            for (char *p = tmp; *p && nparts < 32; p++) {
                if (in_str) {
                    if (*p == q && (p == tmp || *(p-1) != '\\')) in_str = 0;
                    continue;
                }
                if (*p == '"' || *p == '\'') {
                    in_str = 1; q = (*p == '\'') ? '\'' : *p; continue;
                }
                if (*p == '(') depth_paren++;
                else if (*p == ')') { if (depth_paren>0) depth_paren--; }
                else if (*p == '[') depth_bracket++;
                else if (*p == ']') { if (depth_bracket>0) depth_bracket--; }
                else if (*p == '{') depth_brace++;
                else if (*p == '}') { if (depth_brace>0) depth_brace--; }
                else if (*p == '+' && depth_paren==0 && depth_bracket==0 && depth_brace==0) {
                    *p = 0;
                    char *s = start;
                    while (*s == ' ' || *s == '\t') s++;
                    char *e = s + strlen(s) - 1;
                    while (e >= s && (*e == ' ' || *e == '\t')) { *e = 0; e--; }
                    strncpy(parts[nparts++], s, sizeof(parts[0]) - 1);
                    parts[nparts-1][sizeof(parts[0]) - 1] = 0;
                    start = p + 1;
                }
            }
            {
                char *s = start;
                while (*s == ' ' || *s == '\t') s++;
                char *e = s + strlen(s) - 1;
                while (e >= s && (*e == ' ' || *e == '\t')) { *e = 0; e--; }
                strncpy(parts[nparts++], s, sizeof(parts[0]) - 1);
                parts[nparts-1][sizeof(parts[0]) - 1] = 0;
            }
            /* transform each part */
            /* If no actual concatenation at top level (e.g. all '+' inside parens),
               try to normalize by unwrapping outer parens to expose top-level '+'. */
            if (nparts <= 1 && has_string_concat) {
                char norm_buf[2048];
                strncpy(norm_buf, parts[0], sizeof(norm_buf) - 1);
                norm_buf[sizeof(norm_buf) - 1] = 0;
                /* Unwrap whole-expression paren layers dynamically. Each pass either
                   exposes a top-level '+' (then we split) or strips one wrapping layer,
                   so the number of passes is bounded by the string length - no fixed
                   depth cap that would crash on deeply nested parentheses. */
                int unwrap_limit = (int)strlen(norm_buf);
                if (unwrap_limit < 1) unwrap_limit = 1;
                for (int unwrap_try = 0; unwrap_try < unwrap_limit && nparts <= 1; unwrap_try++) {
                    /* try splitting current norm_buf by top-level '+' first */
                    {
                        char split_tmp[2048]; strncpy(split_tmp, norm_buf, sizeof(split_tmp)-1); split_tmp[sizeof(split_tmp)-1]=0;
                        char *ist2 = split_tmp; int d2p=0,d2b=0,d2br=0,in2=0; char q2c=0; int new_n=0; char new_parts[32][2048];
                        for (char *pp = split_tmp; *pp && new_n < 32; pp++) {
                            if (in2) { if (*pp == q2c && (pp == split_tmp || *(pp-1) != '\\')) in2 = 0; continue; }
                            if (*pp == '"' || *pp == '\'') { in2 = 1; q2c = *pp; continue; }
                            if (*pp == '(') d2p++; else if (*pp == ')') { if (d2p>0) d2p--; }
                            else if (*pp == '[') d2b++; else if (*pp == ']') { if (d2b>0) d2b--; }
                            else if (*pp == '{') d2br++; else if (*pp == '}') { if (d2br>0) d2br--; }
                            else if (*pp == '+' && d2p==0 && d2b==0 && d2br==0) {
                                *pp = 0; char *s2 = ist2; while (*s2==' '||*s2=='\t') s2++;
                                char *e2 = s2 + strlen(s2) - 1; while (e2>=s2 && (*e2==' '||*e2=='\t')) { *e2=0; e2--; }
                                strncpy(new_parts[new_n++], s2, sizeof(new_parts[0])-1); new_parts[new_n-1][sizeof(new_parts[0])-1]=0; ist2 = pp+1;
                            }
                        }
                        { char *s2 = ist2; while (*s2==' '||*s2=='\t') s2++;
                          char *e2 = s2 + strlen(s2) - 1; while (e2>=s2 && (*e2==' '||*e2=='\t')) { *e2=0; e2--; }
                          strncpy(new_parts[new_n++], s2, sizeof(new_parts[0])-1); new_parts[new_n-1][sizeof(new_parts[0])-1]=0; }
                        if (new_n > 1) {
                            for (int pi = 0; pi < new_n && pi < 32; pi++) { strncpy(parts[pi], new_parts[pi], sizeof(parts[0])-1); parts[pi][sizeof(parts[0])-1]=0; }
                            nparts = new_n;
                            break;
                        }
                    }
                    /* if not splittable, try to unwrap one layer of whole-expression parens */
                    char *nq = norm_buf; while (*nq == ' ' || *nq == '\t') nq++;
                    if (!(nq[0] == '(' && strncmp(nq, "({", 2) != 0)) break;
                    int npd = 0, nins = 0; char nsq = 0; const char *nend = NULL;
                    for (const char *pp = nq; *pp; pp++) {
                        if (nins) { if (*pp == '\\' && pp[1]) { pp++; continue; } if (*pp == nsq) nins = 0; continue; }
                        if (*pp == '"' || *pp == '\'') { nins = 1; nsq = *pp; continue; }
                        if (*pp == '(') npd++; else if (*pp == ')') { npd--; if (npd == 0) { nend = pp; break; } }
                    }
                    if (!nend) break;
                    const char *naft = nend + 1; while (*naft == ' ' || *naft == '\t') naft++;
                    if (*naft != '\0') break; /* doesn't wrap whole expression, can't unwrap */
                    size_t nil = (size_t)(nend - (nq + 1));
                    char inner_tmp[2048]; if (nil >= sizeof(inner_tmp)) nil = sizeof(inner_tmp) - 1;
                    memcpy(inner_tmp, nq + 1, nil); inner_tmp[nil] = 0;
                    strncpy(norm_buf, inner_tmp, sizeof(norm_buf)-1); norm_buf[sizeof(norm_buf)-1]=0;
                    strncpy(parts[0], inner_tmp, sizeof(parts[0])-1); parts[0][sizeof(parts[0])-1]=0;
                }
            }
            /* If no actual concatenation (single part), skip wrapping */
            if (nparts <= 1) {
                /* Handle string-multiply of a (possibly nested) string-concat expression: e.g. (a+b)*n, ((a+b)*n + "x")*m */
                if (has_string_concat && has_string_mult) {
                    /* Resolve nested '+' (concat) and '*' (repeat) with correct
                       precedence, handling arbitrary parenthesis depth. */
                    normalize_string_expr(out, out_size, array_access_used, table);
                    result_type = TYPE_STRING;
                    nparts = -1;
                }
                /* No concatenation needed, keep original expression, or handled above */
            } else if (array_access_used) {
                /* Array elements are string buffers; use the runtime
                   numeric-or-string add so `arr[1] + arr[2]` adds numbers
                   and concatenates strings. */
                char cparts[32][600];
                for (int i = 0; i < nparts; i++) {
                    const char *part = parts[i];
                    char *unwrapped = (char*)calloc(1,512);
                    int is_num_wrap = 0;
                    {
                        const char *pw = part;
                        while (*pw == ' ' || *pw == '\t') pw++;
                        if (strncmp(pw, "atoi(", 5) == 0 || strncmp(pw, "atof(", 5) == 0 || strncmp(pw, "atoll(", 6) == 0) {
                            int off = (strncmp(pw, "atoll(", 6) == 0) ? 6 : 5;
                            const char *inner = pw + off;
                            const char *start = inner;
                            while (*inner && (isalnum((unsigned char)*inner) || *inner == '_')) inner++;
                            if (*inner == ')' && *(inner+1) == '\0' && inner > start) {
                                int tl = (int)(inner - start);
                                if (tl >= (int)512) tl = (int)512 - 1;
                                memcpy(unwrapped, start, tl);
                                unwrapped[tl] = '\0';
                                is_num_wrap = 1;
                            }
                        }
                    }
                    if (is_num_wrap) {
                        strncpy(cparts[i], unwrapped, sizeof(cparts[i]) - 1);
                        cparts[i][sizeof(cparts[i]) - 1] = 0;
                    } else if (!part_is_stringy(part)) {
                        int is_float_expr = (strchr(part, '.') != NULL ||
                            strstr(part, "atof(") != NULL ||
                            strstr(part, "(double)") != NULL ||
                            strstr(part, "_LTNG_NZDIV_D") != NULL ||
                            strstr(part, "_LTNG_NZMOD_D") != NULL ||
                            strstr(part, "_LTNG_FMOD_D") != NULL);
                        snprintf(cparts[i], sizeof(cparts[i]),
                                 "({ _ltng_retbuf_fmt(\"%s\",(%s)(%s)); })",
                                 is_float_expr ? "%g" : "%lld",
                                 is_float_expr ? "double" : "long long",
                                 part);
                    } else {
                        strncpy(cparts[i], part, sizeof(cparts[i]) - 1);
                        cparts[i][sizeof(cparts[i]) - 1] = 0;
                        normalize_string_expr(cparts[i], sizeof(cparts[i]), array_access_used, table);

                    }
                    free(unwrapped);
                }
                char add_result[2048] = {0};
                strncpy(add_result, cparts[0], sizeof(add_result) - 1);
                add_result[sizeof(add_result) - 1] = 0;
                for (int i = 1; i < nparts; i++) {
                    char step[2048];
                    snprintf(step, sizeof(step), "_ltng_add_sn(%s, %s)",
                             add_result, cparts[i]);
                    strncpy(add_result, step, sizeof(add_result) - 1);
                    add_result[sizeof(add_result) - 1] = 0;
                }
                strncpy(out, add_result, out_size - 1);
                out[out_size - 1] = 0;
                result_type = TYPE_STRING;
            } else {
            /* concatenate: build expression from parts */
            char add_result[2048] = {0};
            int all_stringy = 1;
            for (int i = 0; i < nparts; i++) {
                const char *part = parts[i];
                char cpart[2048];
                /* In string concatenation, atof/atoi/atoll(ident) wrappers came from
                   numeric-context emission of variables/params; use the original char
                   buffer so string values are preserved (e.g. untyped params). */
                char *unwrapped = (char*)calloc(1,512);
                int is_num_wrap = 0;
                {
                    const char *pw = part;
                    while (*pw == ' ' || *pw == '\t') pw++;
                    if (strncmp(pw, "atoi(", 5) == 0 || strncmp(pw, "atof(", 5) == 0 || strncmp(pw, "atoll(", 6) == 0) {
                        int off = (strncmp(pw, "atoll(", 6) == 0) ? 6 : 5;
                        const char *inner = pw + off;
                        const char *start = inner;
                        while (*inner && (isalnum((unsigned char)*inner) || *inner == '_')) inner++;
                        if (*inner == ')' && *(inner+1) == '\0' && inner > start) {
                            int tl = (int)(inner - start);
                            if (tl >= (int)512) tl = (int)512 - 1;
                            memcpy(unwrapped, start, tl);
                            unwrapped[tl] = '\0';
                            is_num_wrap = 1;
                        }
                    }
                }
                if (is_num_wrap) {
                    /* Treat as string buffer for concatenation (runtime _ltng_add_sn handles numeric-vs-string). */
                    strncpy(cpart, unwrapped, sizeof(cpart) - 1);
                    cpart[sizeof(cpart) - 1] = 0;
                    all_stringy = 0; /* force _ltng_add_sn for runtime-correct behavior */
                } else if (!part_is_stringy(part)) {
                    all_stringy = 0;
                    int is_float_expr = (strchr(part, '.') != NULL ||
                        strstr(part, "atof(") != NULL ||
                        strstr(part, "(double)") != NULL ||
                        strstr(part, "_LTNG_NZDIV_D") != NULL ||
                        strstr(part, "_LTNG_NZMOD_D") != NULL ||
                        strstr(part, "_LTNG_FMOD_D") != NULL);
                    snprintf(cpart, sizeof(cpart), "({ _ltng_retbuf_fmt(\"%s\",(%s)(%s)); })",
                             is_float_expr ? "%g" : "%lld",
                             is_float_expr ? "double" : "long long",
                             part);
                } else {
                    strncpy(cpart, part, sizeof(cpart) - 1);
                    cpart[sizeof(cpart) - 1] = 0;
                    normalize_string_expr(cpart, sizeof(cpart), array_access_used, table);

                }
                free(unwrapped);
                if (i == 0) {
                    strncpy(add_result, cpart, sizeof(add_result) - 1);
                    add_result[sizeof(add_result) - 1] = 0;
                } else {
                    char step[2048];
                    if (all_stringy) {
                        snprintf(step, sizeof(step), "_ltng_concat_dyn(2, %s, %s)", add_result, cpart);
                    } else {
                        snprintf(step, sizeof(step), "_ltng_add_sn(%s, %s)", add_result, cpart);
                    }
                    strncpy(add_result, step, sizeof(add_result) - 1);
                    add_result[sizeof(add_result) - 1] = 0;
                }
            }
            strncpy(out, add_result, out_size - 1);
            out[out_size - 1] = 0;
            result_type = TYPE_STRING;
            }
        } else if (strstr(out, "*") != NULL) {
            /* Use normalize_string_expr for general multiply expressions
               (including nested/chained repeats like (("a"*2)*2)*2). */
            normalize_string_expr(out, out_size, array_access_used, table);
            result_type = TYPE_STRING;
        }
    }
    g_expr_sl=sl; g_expr_sc=sc; g_expr_el=el; g_expr_ec=ec;
    if (out_sl) *out_sl = sl; if (out_sc) *out_sc = sc; if (out_el) *out_el = el; if (out_ec) *out_ec = ec;
    return result_type;
}

static const char *var_type_name(VarType t) {
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
    return "string";
}

/* Infer a VarType from a textual expression without leaking parser diagnostics
   into the enclosing interpolation. Used by `$[typeof(...)]` for array elements
   and call results, which need the same typing rules as direct `typeof(...)`. */
static VarType infer_expression_text_type(const char *expr, SymbolTable *table) {
    char buf[MAX_STRING_LEN];
    snprintf(buf, sizeof(buf), "%s\n", expr);
    Lexer lexer;
    lexer_init(&lexer, buf);
    ErrorState tmp_errors;
    memset(&tmp_errors, 0, sizeof(tmp_errors));
    tmp_errors.filename = strdup("<typeof>");
    lexer_tokenize(&lexer, &tmp_errors);

    ErrorState *saved = parser_errors;
    parser_errors = &tmp_errors;
    char scratch[MAX_STRING_LEN];
    VarType type = collect_expression(&lexer, table, scratch, sizeof(scratch),
                                       1, 0, NULL, NULL, NULL, NULL);
    parser_errors = saved;
    lexer_free(&lexer);
    free(tmp_errors.filename);
    return type;
}

static void append_resolved_text(char *resolved, int *rlen, const char *text) {
    int avail = MAX_STRING_LEN - *rlen;
    if (avail <= 0) return;
    int n = snprintf(resolved + *rlen, (size_t)avail, "%s", text);
    if (n > 0) *rlen += (n < avail ? n : avail - 1);
}

/* Resolve the inner text of a `$[typeof(...)]` interpolation. Returns 1 when
   handled (including conversion helpers, calls, array elements, and plain vars). */
static int resolve_typeof_interpolation(const char *vname, SymbolTable *table,
                                        int line, int col, char *resolved, int *rlen) {
    int vlen = (int)strlen(vname);
    if (strncmp(vname, "typeof(", 7) != 0 || vlen < 8 || vname[vlen - 1] != ')') return 0;

    char inner[MAX_IDENTIFIER_LEN];
    int ilen = vlen - 8;
    if (ilen >= (int)sizeof(inner)) ilen = (int)sizeof(inner) - 1;
    memcpy(inner, vname + 7, ilen);
    inner[ilen] = '\0';

    /* Conversion calls: only fast-path when the call is the entire inner expression. */
    {
        int _conv_prefix = 0; const char *_conv_tname = NULL; const char *_conv_cname = NULL;
        if (strncmp(inner, "toint(", 6) == 0)      { _conv_prefix = 6; _conv_tname = "int";    _conv_cname = "toint"; }
        else if (strncmp(inner, "int(", 4) == 0)    { _conv_prefix = 4; _conv_tname = "int";    _conv_cname = "int"; }
        else if (strncmp(inner, "tofloat(", 8) == 0){ _conv_prefix = 8; _conv_tname = "float";  _conv_cname = "tofloat"; }
        else if (strncmp(inner, "float(", 6) == 0)  { _conv_prefix = 6; _conv_tname = "float";  _conv_cname = "float"; }
        else if (strncmp(inner, "tostring(", 9) == 0){ _conv_prefix = 9; _conv_tname = "string"; _conv_cname = "tostring"; }
        else if (strncmp(inner, "string(", 7) == 0) { _conv_prefix = 7; _conv_tname = "string"; _conv_cname = "string"; }
        else if (strncmp(inner, "tochar(", 7) == 0)  { _conv_prefix = 7; _conv_tname = "char";   _conv_cname = "tochar"; }
        else if (strncmp(inner, "char(", 5) == 0)    { _conv_prefix = 5; _conv_tname = "char";   _conv_cname = "char"; }
        else if (strncmp(inner, "tobool(", 7) == 0)  { _conv_prefix = 7; _conv_tname = "bool";   _conv_cname = "tobool"; }
        else if (strncmp(inner, "bool(", 5) == 0)    { _conv_prefix = 5; _conv_tname = "bool";   _conv_cname = "bool"; }
        if (_conv_prefix > 0) {
            /* Check if the conversion call spans the entire inner expression */
            int _d = 0, _end = -1, _il = (int)strlen(inner), _inq = 0; char _qc = 0;
            for (int _i = _conv_prefix - 1; _i < _il; _i++) {
                char _ch = inner[_i];
                if (_inq) { if (_ch == '\\' && _i + 1 < _il) { _i++; continue; } if (_ch == _qc) _inq = 0; continue; }
                if (_ch == '"' || _ch == '\'') { _inq = 1; _qc = _ch; continue; }
                if (_ch == '(') _d++;
                else if (_ch == ')') { _d--; if (_d == 0) { _end = _i; break; } }
            }
            if (_end == _il - 1) {
                /* Sole conversion call: validate invalid literal numeric strings for toint/tofloat */
                if (strcmp(_conv_tname, "int") == 0 || strcmp(_conv_tname, "float") == 0) {
                    if (_il > _conv_prefix + 2 && inner[_conv_prefix] == '"' && inner[_il - 2] == '"') {
                        char sv[MAX_STRING_LEN]; int sl = _il - _conv_prefix - 3;
                        if (sl >= (int)sizeof(sv)) sl = (int)sizeof(sv) - 1;
                        memcpy(sv, inner + _conv_prefix + 1, sl); sv[sl] = '\0';
                        int ok = 0, k = 0; if (sv[k]=='+'||sv[k]=='-') k++; int digits=0,dot=0,valid=1;
                        for (; sv[k]; k++) { if (isdigit((unsigned char)sv[k])) digits=1; else if (sv[k]=='.'&&!dot) dot=1; else {valid=0;break;} }
                        if (valid && digits) ok = 1;
                        if (!ok) {
                            char *msg = _ltng_cfmt("Invalid conversion: cannot convert \"%s\" with %s", sv, _conv_cname);
                            error_state_add(parser_errors, line, col, msg ? msg : "");
                            free(msg);
                        }
                    }
                }
                append_resolved_text(resolved, rlen, _conv_tname); return 1;
            }
            /* Part of a larger expression: infer from full expression */
            VarType t = infer_expression_text_type(inner, table);
            append_resolved_text(resolved, rlen, var_type_name(t == TYPE_NULL ? TYPE_STRING : t));
            return 1;
        }
    }

    const char *p = inner;
    while (*p && (isalnum((unsigned char)*p) || *p == '_')) p++;
    if (p > inner && *p == '(') {
        char fname[MAX_IDENTIFIER_LEN];
        int flen = (int)(p - inner);
        if (flen >= (int)sizeof(fname)) flen = (int)sizeof(fname) - 1;
        memcpy(fname, inner, flen);
        fname[flen] = '\0';
        if (find_function_ast_by_name(fname) || is_callable_ident(table, fname) ||
            is_function_name_scoped(table, fname)) {
            VarType t = infer_expression_text_type(inner, table);
            append_resolved_text(resolved, rlen, var_type_name(t == TYPE_NULL ? TYPE_STRING : t));
        } else {
            append_resolved_text(resolved, rlen, "string");
        }
        return 1;
    }

    if (strchr(inner, '[')) {
        VarType t = infer_expression_text_type(inner, table);
        append_resolved_text(resolved, rlen, var_type_name(t == TYPE_NULL ? TYPE_STRING : t));
        return 1;
    }

    /* Nested typeof(...) keeps the legacy string fallback. */
    const char *q = inner;
    while (strncmp(q, "typeof(", 7) == 0) {
        int nl = (int)strlen(q);
        if (nl < 8 || q[nl - 1] != ')') break;
        char stripped[MAX_IDENTIFIER_LEN];
        int slen = nl - 8;
        if (slen >= (int)sizeof(stripped)) slen = (int)sizeof(stripped) - 1;
        memcpy(stripped, q + 7, slen);
        stripped[slen] = '\0';
        snprintf(inner, sizeof(inner), "%s", stripped);
        q = inner;
    }
    if (q != inner || strncmp(inner, "typeof(", 7) == 0) {
        append_resolved_text(resolved, rlen, "string");
        return 1;
    }

    /* Simple member/property access inside typeof: typeof(o.n) where base and field are identifiers. */
    {
        const char *dot = strchr(inner, '.');
        if (dot) {
            char base[MAX_IDENTIFIER_LEN];
            int blen = (int)(dot - inner);
            const char *field = dot + 1;
            int simple_member = 0;
            if (blen > 0 && *field != '\0' && !strchr(field, '.')) {
                int base_ok = 1, field_ok = 1;
                if (!(isalpha((unsigned char)inner[0]) || inner[0] == '_')) base_ok = 0;
                for (int bi = 0; base_ok && bi < blen; bi++) {
                    if (!(isalnum((unsigned char)inner[bi]) || inner[bi] == '_')) base_ok = 0;
                }
                if (!(isalpha((unsigned char)field[0]) || field[0] == '_')) field_ok = 0;
                for (const char *fp = field; field_ok && *fp; fp++) {
                    if (!(isalnum((unsigned char)*fp) || *fp == '_')) field_ok = 0;
                }
                simple_member = base_ok && field_ok;
            }
            if (simple_member) {
                if (blen >= (int)sizeof(base)) blen = (int)sizeof(base) - 1;
                memcpy(base, inner, blen);
                base[blen] = '\0';
                char mvbase[MAX_IDENTIFIER_LEN];
                mangle_name(base, mvbase, MAX_IDENTIFIER_LEN);
                if (!symbol_table_has(table, mvbase)) {
                    char *errmsg = _ltng_cfmt("Variable '%s' not declared (used in typeof)", inner);
                    error_state_add(parser_errors, line, col, errmsg ? errmsg : "");
                    free(errmsg);
                    return 1;
                }
                VarType ftype = TYPE_STRING;
                ObjSchema *osc = obj_lookup(mvbase);
                if (!osc) {
                    const char *cname = symbol_table_get_cname(table, mvbase);
                    if (cname) osc = obj_lookup(cname);
                }
                if (osc) {
                    for (int oi = 0; oi < osc->count; oi++) {
                        if (strcmp(osc->fields[oi].name, field) == 0) { ftype = osc->fields[oi].type; break; }
                    }
                }
                append_resolved_text(resolved, rlen, var_type_name(ftype));
                return 1;
            }
            /* Complex expression containing '.' (e.g. float literals, method chains): infer */
            VarType t = infer_expression_text_type(inner, table);
            append_resolved_text(resolved, rlen, var_type_name(t == TYPE_NULL ? TYPE_STRING : t));
            return 1;
        }
    }

    /* Simple identifier, literal keyword, or type keyword */
    {
        int simple_ident = 0;
        if (is_bool_value(inner) || is_type_keyword(inner)) simple_ident = 1;
        else if (isalpha((unsigned char)inner[0]) || inner[0] == '_') {
            simple_ident = 1;
            for (const char *cp = inner; *cp; cp++) {
                if (!(isalnum((unsigned char)*cp) || *cp == '_')) { simple_ident = 0; break; }
            }
        }
        if (simple_ident) {
            char mvname[MAX_IDENTIFIER_LEN];
            mangle_name(inner, mvname, MAX_IDENTIFIER_LEN);
            if (!symbol_table_has(table, mvname) && !is_type_keyword(inner) && !is_bool_value(inner)) {
                char *errmsg = _ltng_cfmt("Variable '%s' not declared (used in typeof)", inner);
                error_state_add(parser_errors, line, col, errmsg ? errmsg : "");
                free(errmsg);
            } else if (symbol_table_has(table, mvname)) {
                append_resolved_text(resolved, rlen,
                    var_type_name(effective_param_default_type(inner, symbol_table_get(table, mvname))));
            } else if (is_bool_value(inner)) {
                append_resolved_text(resolved, rlen, "bool");
            } else {
                append_resolved_text(resolved, rlen, inner);
            }
            return 1;
        }
    }

    /* General expression fallback */
    {
        VarType t = infer_expression_text_type(inner, table);
        append_resolved_text(resolved, rlen, var_type_name(t == TYPE_NULL ? TYPE_STRING : t));
    }
    return 1;
}

static void resolve_string_interpolation(Token *tok, SymbolTable *table, int line, int col) {
    if (tok->type != TOKEN_STRING) return;
    if (!strchr(tok->value, '$')) return;

    char resolved[MAX_STRING_LEN];
    int rlen = 0;
    for (const char *sp = tok->value; *sp; sp++) {
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
                char vname[MAX_IDENTIFIER_LEN];
                if (vlen >= MAX_IDENTIFIER_LEN) vlen = MAX_IDENTIFIER_LEN - 1;
                memcpy(vname, vstart, vlen);
                vname[vlen] = '\0';
                if (resolve_typeof_interpolation(vname, table, line, col, resolved, &rlen)) {
                    /* skip generic variable/expression validation below */
                } else {
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
                        char mvname[MAX_IDENTIFIER_LEN];
                        mangle_name(vname, mvname, MAX_IDENTIFIER_LEN);
                        if (!symbol_table_has(table, mvname) && !is_type_keyword(vname) && !is_bool_value(vname)) {
                            char *errmsg = _ltng_cfmt("Variable '%s' not declared (used in string interpolation)", vname);
                            error_state_add(parser_errors, line, col, errmsg ? errmsg : "");
                            free(errmsg);
                        }
                        int voff = (int)(vstart - 2 - tok->value);
                        int vtotal = (int)(sp - (vstart - 2) + 1);
                        for (int ci = voff; ci < voff + vtotal && rlen < MAX_STRING_LEN - 1; ci++) {
                            resolved[rlen++] = tok->value[ci];
                        }
                    } else {
                        /* Interpolation expressions: try safe constant folding for conversion calls,
                           otherwise preserve $[...] for runtime evaluation by codegen. */
                        int folded = 0;
                        const char *conv_name = NULL; int prefix = 0;
                        if (strncmp(vname, "toint(", 6) == 0) { conv_name = "toint"; prefix = 6; }
                        else if (strncmp(vname, "int(", 4) == 0) { conv_name = "toint"; prefix = 4; }
                        else if (strncmp(vname, "tofloat(", 8) == 0) { conv_name = "tofloat"; prefix = 8; }
                        else if (strncmp(vname, "float(", 6) == 0) { conv_name = "tofloat"; prefix = 6; }
                        else if (strncmp(vname, "tostring(", 9) == 0) { conv_name = "tostring"; prefix = 9; }
                        else if (strncmp(vname, "string(", 7) == 0) { conv_name = "tostring"; prefix = 7; }
                        else if (strncmp(vname, "tobool(", 7) == 0) { conv_name = "tobool"; prefix = 7; }
                        else if (strncmp(vname, "bool(", 5) == 0) { conv_name = "tobool"; prefix = 5; }
                        else if (strncmp(vname, "tochar(", 7) == 0) { conv_name = "tochar"; prefix = 7; }
                        else if (strncmp(vname, "char(", 5) == 0) { conv_name = "tochar"; prefix = 5; }
                        if (conv_name && vname[vlen - 1] == ')') {
                            char inner[MAX_STRING_LEN];
                            int ilen = vlen - prefix - 1; if (ilen < 0) ilen = 0;
                            if (ilen >= MAX_STRING_LEN) ilen = MAX_STRING_LEN - 1;
                            memcpy(inner, vname + prefix, ilen); inner[ilen] = '\0';
                            while (ilen > 0 && (inner[ilen-1] == ' ' || inner[ilen-1] == '\t')) ilen--; inner[ilen] = '\0';
                            int is_str_lit = (ilen >= 2 && inner[0] == '"' && inner[ilen-1] == '"');
                            int is_num_lit = 0, has_dot = 0;
                            if (!is_str_lit && ilen > 0) {
                                const char *p = inner; if (*p=='+'||*p=='-') p++;
                                int d=0,v=1; for(;*p;p++){ if(isdigit((unsigned char)*p)) d=1; else if(*p=='.'&&!has_dot) has_dot=1; else {v=0;break;} } if(d&&v) is_num_lit=1;
                            }
                            int is_bool_lit = (!is_str_lit && !is_num_lit && (strcmp(inner,"true")==0||strcmp(inner,"false")==0||strcmp(inner,"t")==0||strcmp(inner,"f")==0));
                            if (strcmp(conv_name, "toint") == 0) {
                                if (is_str_lit) {
                                    /* value of numeric string literal as string */
                                    for (int i=1;i<ilen-1 && rlen<MAX_STRING_LEN-1;i++) resolved[rlen++]=inner[i];
                                    resolved[rlen]='\0'; folded=1;
                                } else if (is_num_lit) {
                                    rlen += snprintf(resolved + rlen, MAX_STRING_LEN - rlen, "%s", inner); folded=1;
                                }
                            } else if (strcmp(conv_name, "tofloat") == 0) {
                                if (is_str_lit) {
                                    char sval[MAX_STRING_LEN]; int cpos=0;
                                    for (int i=1;i<ilen-1 && cpos<MAX_STRING_LEN-1;i++){ if(inner[i]=='\\' && i+1<ilen-1) sval[cpos++]=inner[++i]; else sval[cpos++]=inner[i]; }
                                    sval[cpos]='\0';
                                    rlen += snprintf(resolved + rlen, MAX_STRING_LEN - rlen, "%g", atof(sval)); folded=1;
                                } else if (is_num_lit) {
                                    rlen += snprintf(resolved + rlen, MAX_STRING_LEN - rlen, "%g", atof(inner)); folded=1;
                                }
                            } else if (strcmp(conv_name, "tostring") == 0) {
                                if (is_str_lit) {
                                    for (int i=1;i<ilen-1 && rlen<MAX_STRING_LEN-1;i++) resolved[rlen++]=inner[i];
                                    resolved[rlen]='\0'; folded=1;
                                } else if (ilen==4 && memcmp(inner,"true",4)==0) { rlen += snprintf(resolved + rlen, MAX_STRING_LEN - rlen, "true"); folded=1; }
                                else if (ilen==5 && memcmp(inner,"false",5)==0) { rlen += snprintf(resolved + rlen, MAX_STRING_LEN - rlen, "false"); folded=1; }
                                else if (ilen==1 && inner[0]=='t') { rlen += snprintf(resolved + rlen, MAX_STRING_LEN - rlen, "true"); folded=1; }
                                else if (ilen==1 && inner[0]=='f') { rlen += snprintf(resolved + rlen, MAX_STRING_LEN - rlen, "false"); folded=1; }
                                else if (is_num_lit) { rlen += snprintf(resolved + rlen, MAX_STRING_LEN - rlen, "%s", inner); folded=1; }
                            } else if (strcmp(conv_name, "tobool") == 0) {
                                if (is_num_lit) { rlen += snprintf(resolved + rlen, MAX_STRING_LEN - rlen, "%s", (atof(inner)!=0.0)?"true":"false"); folded=1; }
                                else if (is_bool_lit) { rlen += snprintf(resolved + rlen, MAX_STRING_LEN - rlen, "%s", (inner[0]=='t')?"true":"false"); folded=1; }
                                else if (is_str_lit) {
                                    if (ilen==2 || (ilen==3 && inner[1]=='0') || (ilen==3 && inner[1]=='f') || (ilen==7 && memcmp(inner+1,"false",5)==0))
                                        rlen += snprintf(resolved + rlen, MAX_STRING_LEN - rlen, "false");
                                    else rlen += snprintf(resolved + rlen, MAX_STRING_LEN - rlen, "true");
                                    folded=1;
                                }
                            } else if (strcmp(conv_name, "tochar") == 0) {
                                if (is_str_lit) {
                                    char ch = (ilen>=3)?inner[1]:'\0';
                                    if (ch=='\\' && ilen>=4) { if (rlen < MAX_STRING_LEN-2){ resolved[rlen++]=inner[1]; resolved[rlen++]=inner[2]; resolved[rlen]='\0'; } }
                                    else { if (rlen < MAX_STRING_LEN-1){ resolved[rlen++]=ch; resolved[rlen]='\0'; } }
                                    folded=1;
                                } else if (is_num_lit && !has_dot) {
                                    rlen += snprintf(resolved + rlen, MAX_STRING_LEN - rlen, "%c", (unsigned char)atoi(inner)); folded=1;
                                }
                            }
                        }
                        if (!folded) {
                            /* Validate/parse expression, but preserve original $[...] for runtime evaluation */
                            char check[MAX_STRING_LEN];
                            (void)parse_expression_from_string(vname, table, check, sizeof(check));
                            int voff = (int)(vstart - 2 - tok->value);
                            int vtotal = (int)(sp - (vstart - 2) + 1);
                            for (int ci = voff; ci < voff + vtotal && rlen < MAX_STRING_LEN - 1; ci++) {
                                resolved[rlen++] = tok->value[ci];
                            }
                        }
                    }
                }
            } else {
                if (rlen + 2 < MAX_STRING_LEN) { resolved[rlen++] = '$'; resolved[rlen++] = '['; }
            }
        } else {
            if (rlen < MAX_STRING_LEN - 1) resolved[rlen++] = *sp;
        }
    }
    resolved[rlen] = '\0';
    strncpy(tok->value, resolved, MAX_STRING_LEN - 1);
    tok->value[MAX_STRING_LEN - 1] = '\0';
}

static int try_parse_typeof(Lexer *lexer, SymbolTable *table, Token *result) {
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
        Token inner_result;
        if (try_parse_typeof(lexer, table, &inner_result)) {
            Token close = lexer_next(lexer);
            if (close.type != TOKEN_RPAREN) {
                error_state_add(parser_errors, arg.line, arg.col,
                    "Expected ')' after typeof argument");
                lexer->current = saved_pos;
                return 0;
            }
            result->type = TOKEN_STRING;
            strncpy(result->value, "string", MAX_STRING_LEN - 1);
            result->value[MAX_STRING_LEN - 1] = '\0';
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
        Token inner_tok = {0};
        int depth = 1;
        while (lexer->current < lexer->count && depth > 0) {
            Token itok = lexer_peek(lexer);
            if (itok.type == TOKEN_LPAREN) depth++;
            else if (itok.type == TOKEN_RPAREN) { depth--; if (depth == 0) break; }
            else if (itok.type == TOKEN_NEWLINE || itok.type == TOKEN_EOF) break;
            if (depth == 1 && inner_depth == 0) { inner_tok = itok; inner_tokens++; }
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
            strncpy(result->value, var_type_name(cvt), MAX_STRING_LEN - 1);
            result->value[MAX_STRING_LEN - 1] = '\0';
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
                strncpy(result->value, var_type_name(ftype == TYPE_NULL ? TYPE_STRING : ftype), MAX_STRING_LEN - 1);
                result->value[MAX_STRING_LEN - 1] = '\0';
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
        char mangled_arr[MAX_IDENTIFIER_LEN];
        mangle_name(arg.value, mangled_arr, sizeof(mangled_arr));
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
                strncpy(result->value, var_type_name(aet), MAX_STRING_LEN - 1);
                result->value[MAX_STRING_LEN - 1] = '\0';
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
            strncpy(result->value, "reserved", MAX_STRING_LEN - 1);
            result->value[MAX_STRING_LEN - 1] = '\0';
            result->line = typeof_tok.line;
            result->col = typeof_tok.col;
            return 1;
        } else if (arg.type == TOKEN_WRITE || arg.type == TOKEN_READ ||
                   arg.type == TOKEN_LET || arg.type == TOKEN_CONST ||
                   arg.type == TOKEN_SET) {
            result->type = TOKEN_STRING;
            strncpy(result->value, "reserved", MAX_STRING_LEN - 1);
            result->value[MAX_STRING_LEN - 1] = '\0';
            result->line = typeof_tok.line;
            result->col = typeof_tok.col;
            return 1;
        } else if (arg.type == TOKEN_IDENT || token_is_name(arg)) {
            char mangled[MAX_IDENTIFIER_LEN];
            mangle_name(arg.value, mangled, MAX_IDENTIFIER_LEN);
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
        strncpy(result->value, var_type_name(vtype), MAX_STRING_LEN - 1);
        result->value[MAX_STRING_LEN - 1] = '\0';
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
        char exprbuf[MAX_STRING_LEN];
        VarType etype = collect_expression(lexer, table, exprbuf, sizeof(exprbuf), 1, 0, NULL, NULL, NULL, NULL);
        Token rclose = lexer_peek(lexer);
        if (rclose.type == TOKEN_RPAREN) {
            lexer_next(lexer); /* consume ) */
            /* Discard errors introduced by speculative typeof arg parse */
            if (parser_errors && parser_errors->count > err_before) parser_errors->count = err_before;
            result->type = TOKEN_STRING;
            strncpy(result->value, var_type_name(etype), MAX_STRING_LEN - 1);
            result->value[MAX_STRING_LEN - 1] = '\0';
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

static void parse_write(Lexer *lexer, SymbolTable *table) {
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
            char rm[MAX_IDENTIFIER_LEN];
            mangle_name(first.value, rm, MAX_IDENTIFIER_LEN);
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
            char arrname[MAX_IDENTIFIER_LEN];
            snprintf(arrname, sizeof(arrname), "%s_data", rm);
            char lenname[MAX_IDENTIFIER_LEN];
            snprintf(lenname, sizeof(lenname), "%s_len", rm);
            char capname[MAX_IDENTIFIER_LEN];
            snprintf(capname, sizeof(capname), "%s_cap", rm);
            /* Value-consumed context: use general expression parsing so array
               methods operate on a temp copy and do not mutate the original. */
            goto _write_array_method_fallback;
            /* current points to arr; consume arr, ., method, ( */
            lexer_next(lexer); /* consume arr (already peeked as first) */
            lexer_next(lexer); /* consume . */
            lexer_next(lexer); /* consume push/pop */
            lexer_next(lexer); /* consume ( */
            if (strcmp(third_tok.value, "push") == 0) {
                char arg_expr[MAX_STRING_LEN];
                VarType arg_type = collect_expression(lexer, table, arg_expr, sizeof(arg_expr), 1, 0, NULL, NULL, NULL, NULL);
                int esl=g_expr_sl,esc=g_expr_sc,eel=g_expr_el,eec=g_expr_ec;
                if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                    error_state_add(parser_errors, lexer_peek(lexer).line,
                        lexer_peek(lexer).col, "Expected ')' after push argument");
                    skip_to_newline(lexer);
                    return;
                }
                lexer_next(lexer); /* consume pop's ) */
                lexer_next(lexer); /* consume write's ) */
                char expr[MAX_STRING_LEN];
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
                snprintf(expr, sizeof(expr),
                    "({ char *__apv = _ltng_ds_fmt(\"%s\", %s); _ltng_arr_push(&%s, &%s, &%s, __apv); char *__r = _ltng_retbuf_fmt(\"%%s\", __apv); free(__apv); __r; })",
                    fmt, arg_expr, arrname, lenname, capname);
                ASTNode *node = create_node(NODE_WRITE, "", expr);
                node->is_variable = 0;
                node->is_expr = 1;
                node->var_type = TYPE_STRING;
                emit(node);
                return;
            } else if (strcmp(third_tok.value, "insert") == 0) {
                char idx_expr[MAX_STRING_LEN];
                collect_expression(lexer, table, idx_expr, sizeof(idx_expr), 1, 1, NULL, NULL, NULL, NULL);
                if (lexer_peek(lexer).type != TOKEN_COMMA) {
                    error_state_add(parser_errors, lexer_peek(lexer).line,
                        lexer_peek(lexer).col, "Expected ',' after insert index");
                    skip_to_newline(lexer);
                    return;
                }
                lexer_next(lexer); /* consume , */
                char arg_expr[MAX_STRING_LEN];
                VarType arg_type = collect_expression(lexer, table, arg_expr, sizeof(arg_expr), 1, 0, NULL, NULL, NULL, NULL);
                if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                    error_state_add(parser_errors, lexer_peek(lexer).line,
                        lexer_peek(lexer).col, "Expected ')' after insert argument");
                    skip_to_newline(lexer);
                    return;
                }
                lexer_next(lexer); /* consume insert's ) */
                lexer_next(lexer); /* consume write's ) */
                char expr[MAX_STRING_LEN];
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
                snprintf(expr, sizeof(expr),
                    "({ char *__apv = _ltng_ds_fmt(\"%s\", %s); _ltng_arr_insert(&%s, &%s, &%s, (int)(%s), __apv); char *__r = _ltng_retbuf_fmt(\"%%s\", __apv); free(__apv); __r; })",
                    fmt, arg_expr, arrname, lenname, capname, idx_expr);
                ASTNode *node = create_node(NODE_WRITE, "", expr);
                node->is_variable = 0;
                node->is_expr = 1;
                node->var_type = TYPE_STRING;
                emit(node);
                return;
            } else if (strcmp(third_tok.value, "remove") == 0) {
                char idx_expr[MAX_STRING_LEN];
                collect_expression(lexer, table, idx_expr, sizeof(idx_expr), 1, 0, NULL, NULL, NULL, NULL);
                if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                    error_state_add(parser_errors, lexer_peek(lexer).line,
                        lexer_peek(lexer).col, "Expected ')' after remove index");
                    skip_to_newline(lexer);
                    return;
                }
                lexer_next(lexer); /* consume remove's ) */
                lexer_next(lexer); /* consume write's ) */
                char expr[MAX_STRING_LEN];
                snprintf(expr, sizeof(expr),
                    "({ char *__r = _ltng_arr_pop_index(%s, &%s, (int)(%s)); __r; })",
                    arrname, lenname, idx_expr);
                ASTNode *node = create_node(NODE_WRITE, "", expr);
                node->is_variable = 0;
                node->is_expr = 1;
                node->var_type = TYPE_STRING;
                emit(node);
                return;
            } else if (strcmp(third_tok.value, "contains") == 0) {
                char arg_expr[MAX_STRING_LEN];
                VarType arg_type = collect_expression(lexer, table, arg_expr, sizeof(arg_expr), 1, 0, NULL, NULL, NULL, NULL);
                if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                    error_state_add(parser_errors, lexer_peek(lexer).line,
                        lexer_peek(lexer).col, "Expected ')' after contains argument");
                    skip_to_newline(lexer);
                    return;
                }
                lexer_next(lexer); /* consume contains's ) */
                lexer_next(lexer); /* consume write's ) */
                char expr[MAX_STRING_LEN];
                if (arg_type == TYPE_INT) {
                    snprintf(expr, sizeof(expr),
                        "({ char *__cv = _ltng_ds_fmt(\"%%lld\", (long long)(%s)); char *__r = _ltng_retbuf_fmt(\"%%s\", _ltng_arr_contains(%s, %s, __cv) ? \"true\" : \"false\"); free(__cv); __r; })",
                        arg_expr, arrname, lenname);
                } else if (arg_type == TYPE_FLOAT) {
                    snprintf(expr, sizeof(expr),
                        "({ char *__cv = _ltng_ds_fmt(\"%%g\", (double)(%s)); char *__r = _ltng_retbuf_fmt(\"%%s\", _ltng_arr_contains(%s, %s, __cv) ? \"true\" : \"false\"); free(__cv); __r; })",
                        arg_expr, arrname, lenname);
                } else {
                    snprintf(expr, sizeof(expr),
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
                char arg_expr[MAX_STRING_LEN];
                VarType arg_type = collect_expression(lexer, table, arg_expr, sizeof(arg_expr), 1, 0, NULL, NULL, NULL, NULL);
                if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                    error_state_add(parser_errors, lexer_peek(lexer).line,
                        lexer_peek(lexer).col, "Expected ')' after index_of argument");
                    skip_to_newline(lexer);
                    return;
                }
                lexer_next(lexer); /* consume index_of's ) */
                lexer_next(lexer); /* consume write's ) */
                char expr[MAX_STRING_LEN];
                if (arg_type == TYPE_INT) {
                    snprintf(expr, sizeof(expr),
                        "({ char *__iv = _ltng_ds_fmt(\"%%lld\", (long long)(%s)); char *__r = _ltng_retbuf_fmt(\"%%d\", _ltng_arr_index_of(%s, %s, __iv)); free(__iv); __r; })",
                        arg_expr, arrname, lenname);
                } else if (arg_type == TYPE_FLOAT) {
                    snprintf(expr, sizeof(expr),
                        "({ char *__iv = _ltng_ds_fmt(\"%%g\", (double)(%s)); char *__r = _ltng_retbuf_fmt(\"%%d\", _ltng_arr_index_of(%s, %s, __iv)); free(__iv); __r; })",
                        arg_expr, arrname, lenname);
                } else {
                    snprintf(expr, sizeof(expr),
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
                char chain_ops[4096] = {0};
                int cop = 0;
                Token cmname = third_tok;
                for (;;) {
                    if (strcmp(cmname.value, "sort") == 0) {
                        if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                            error_state_add(parser_errors, lexer_peek(lexer).line,
                                lexer_peek(lexer).col, "Expected ')' after sort");
                            skip_to_newline(lexer); return;
                        }
                        lexer_next(lexer); /* consume ) */
                        cop += snprintf(chain_ops + cop, sizeof(chain_ops) - cop, "_ltng_arr_sort(%s, %s); ", arrname, lenname);
                    } else if (strcmp(cmname.value, "reverse") == 0) {
                        if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                            error_state_add(parser_errors, lexer_peek(lexer).line,
                                lexer_peek(lexer).col, "Expected ')' after reverse");
                            skip_to_newline(lexer); return;
                        }
                        lexer_next(lexer); /* consume ) */
                        cop += snprintf(chain_ops + cop, sizeof(chain_ops) - cop, "_ltng_arr_reverse(%s, %s); ", arrname, lenname);
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
                    skip_to_newline(lexer); return;
                }
                lexer_next(lexer); /* consume write's ) */
                char expr[MAX_STRING_LEN * 2];
                snprintf(expr, sizeof(expr),
                    "({ %s_ltng_retbuf_fmt(\"true\"); })",
                    chain_ops);
                ASTNode *node = create_node(NODE_WRITE, "", expr);
                node->is_variable = 0;
                node->is_expr = 1;
                node->var_type = TYPE_STRING;
                emit(node);
                return;
            } else if (strcmp(third_tok.value, "pop") == 0) {
                /* pop() or pop(index) */
                if (lexer_peek(lexer).type == TOKEN_RPAREN) {
                    /* pop() - no args */
                    lexer_next(lexer); /* consume pop's ) */
                    lexer_next(lexer); /* consume write's ) */
                    char expr[MAX_STRING_LEN];
                    snprintf(expr, sizeof(expr),
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
                    char idx_expr[MAX_STRING_LEN];
                    collect_expression(lexer, table, idx_expr, sizeof(idx_expr), 1, 0, NULL, NULL, NULL, NULL);
                    if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                        error_state_add(parser_errors, lexer_peek(lexer).line,
                            lexer_peek(lexer).col, "Expected ')' after pop index");
                        skip_to_newline(lexer);
                        return;
                    }
                    lexer_next(lexer); /* consume pop index's ) */
                    lexer_next(lexer); /* consume write's ) */
                    char expr[MAX_STRING_LEN];
                    snprintf(expr, sizeof(expr),
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
            char cexpr[MAX_STRING_LEN];
            build_call_cexpr(call, cexpr, sizeof(cexpr));
            int chained = 0;
            while (lexer_peek(lexer).type == TOKEN_LPAREN) {
                if ((int)strlen(cexpr) > 2000) {
                    error_state_add(parser_errors, lexer_peek(lexer).line, lexer_peek(lexer).col,
                        "Chained call expression too deep/complex");
                    lexer->current = saved_call; goto _write_call_fallback;
                }
                chained = 1;
                int cline = lexer_peek(lexer).line, ccol = lexer_peek(lexer).col;
                lexer_next(lexer); /* consume ( */
                ASTNode *args = NULL, *last = NULL;
                if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                    while (1) {
                        char arg_expr[MAX_STRING_LEN];
                        VarType at = collect_expression(lexer, table, arg_expr, sizeof(arg_expr), 1, 1, NULL, NULL, NULL, NULL);
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
                char next_expr[MAX_STRING_LEN]; int p = 0;
                static int _wcid = 0; int cid = _wcid++;
                p += snprintf(next_expr+p, sizeof(next_expr)-p, "({ char *__ct%d=_ltng_ds_fmt(\"%%s\",%s); char *__ctr%d=__ltng_call_indirect(__ct%d,_ltng_retbuf(),%d,%d,%d",
                    cid,cexpr,cid,cid,cline>0?cline:1,ccol>0?ccol:1,argc);
                if (p >= (int)sizeof(next_expr)) p = (int)sizeof(next_expr) - 1;
                for (ASTNode *a = args; a && p < (int)sizeof(next_expr) - 1; a = a->next) {
                    int rem = (int)sizeof(next_expr) - p;
                    if (a->var_type == TYPE_STRING && a->value[0] == '"') p += snprintf(next_expr+p, rem, ",%s", a->value);
                    else if (a->is_variable && a->value[0] != '\0') p += snprintf(next_expr+p, rem, ",%s", a->value);
                    else if ((a->var_type == TYPE_STRING || a->var_type == TYPE_OBJECT) && a->value[0] != '\0') p += snprintf(next_expr+p, rem, ",%s", a->value);
                    else if (a->value[0] == '\0') p += snprintf(next_expr+p, rem, ",\"\"");
                    else p += snprintf(next_expr+p, rem, ",_ltng_ad((double)(%s))", a->value);
                    if (p >= (int)sizeof(next_expr)) p = (int)sizeof(next_expr) - 1;
                }
                if (p < (int)sizeof(next_expr) - 1)
                    p += snprintf(next_expr+p, (int)sizeof(next_expr)-p, "); free(__ct%d); __ctr%d; })", cid, cid);
                next_expr[sizeof(next_expr)-1]=0;
                if (p >= (int)sizeof(next_expr) - 1) {
                    error_state_add(parser_errors, cline, ccol, "Chained call expression too deep/complex");
                    lexer->current = saved_call; goto _write_call_fallback;
                }
                strncpy(cexpr, next_expr, sizeof(cexpr)-1); cexpr[sizeof(cexpr)-1]=0;
            }
            if (lexer_peek(lexer).type == TOKEN_RPAREN) {
                lexer_next(lexer); /* consume write's ) */
                VarType _wct = chained ? TYPE_STRING : infer_call_result_type(call);
                if (_wct == TYPE_ARRAY) {
                    char _fmt[MAX_STRING_LEN];
                    snprintf(_fmt, sizeof(_fmt),
                        "({ char *__ahv=_ltng_ds_new(%s); _ltng_ah *__ah=_ltng_ah_unpack(__ahv); char *__r=_ltng_ds_new(\"\"); if(__ah){ for(int _i=0;_i<__ah->len;_i++){ if(_i) _ltng_ds_append(&__r,\", \"); _ltng_ds_append(&__r,__ah->d[_i]?__ah->d[_i]:\"\"); } _ltng_ah_free(__ah); } free(__ahv); __r; })",
                        cexpr);
                    strncpy(cexpr, _fmt, sizeof(cexpr)-1); cexpr[sizeof(cexpr)-1]='\0';
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
        char rm_arr[MAX_IDENTIFIER_LEN];
        mangle_name(first.value, rm_arr, sizeof(rm_arr));
        if (symbol_table_has(table, rm_arr) && symbol_table_get(table, rm_arr) == TYPE_ARRAY) {
            int saved_a = lexer->current;
            Token arrtok = lexer_next(lexer); /* consume arr ident */
            (void)arrtok;
            #define MAX_IDX_W 32
            char (*idx_exprs_w)[1024] = calloc(MAX_IDX_W, 1024);
            int idx_n_w = 0;
            int ok_w = idx_exprs_w != NULL;
            while (ok_w && lexer_peek(lexer).type == TOKEN_LBRACKET) {
                lexer_next(lexer); /* [ */
                if (idx_n_w >= MAX_IDX_W) { ok_w = 0; break; }
                collect_expression(lexer, table, idx_exprs_w[idx_n_w], 1024, 1, 0, NULL, NULL, NULL, NULL);
                Token rb = lexer_peek(lexer);
                if (rb.type != TOKEN_RBRACKET) { ok_w = 0; break; }
                lexer_next(lexer); /* ] */
                idx_n_w++;
            }
            if (ok_w && idx_n_w > 0 && lexer_peek(lexer).type == TOKEN_RPAREN) {
                lexer_next(lexer); /* consume write's ) */
                char arrname_w[MAX_IDENTIFIER_LEN], lenname_w[MAX_IDENTIFIER_LEN];
                snprintf(arrname_w, sizeof(arrname_w), "%s_data", rm_arr);
                snprintf(lenname_w, sizeof(lenname_w), "%s_len", rm_arr);
                int *dims_w = NULL; int dimc_w = 0;
                int have_dims_w = symbol_table_get_array_dims(table, rm_arr, &dims_w, &dimc_w);
                /* For 1D or matching dims, emit _ltng_arr_get; for unknown dims treat as 1D */
                int edim = (have_dims_w && dimc_w > 0) ? dimc_w : 1;
                if (idx_n_w == edim) {
                    char *expr = malloc(MAX_STRING_LEN);
                    if (!expr) { free(idx_exprs_w); lexer->current = saved_a; goto _write_array_fallback; }
                    int p = 0;
                    int esz = MAX_STRING_LEN;
                    #define WAPP(...) do { if (p < esz - 1) { int _n = snprintf(expr + p, esz - p, __VA_ARGS__); if (_n > 0) { p += _n; if (p >= esz) p = esz - 1; } } } while(0)
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
                    free(idx_exprs_w);
                    #undef MAX_IDX_W
                    return;
                }
                /* fall back to general expression handling for mismatched dims */
                free(idx_exprs_w);
                #undef MAX_IDX_W
                lexer->current = saved_a;
                goto _write_array_fallback;
            }
            if (idx_exprs_w) free(idx_exprs_w);
            #undef MAX_IDX_W
            _write_array_fallback:
            lexer->current = saved_a;
        }
    }

    if (first.type == TOKEN_LPAREN || first.type == TOKEN_MINUS || first.type == TOKEN_BANG ||
        first.type == TOKEN_PLUS_PLUS || first.type == TOKEN_MINUS_MINUS ||
        has_operator_ahead(lexer) || is_conv_call ||
        (first.type == TOKEN_IDENT && second.type == TOKEN_DOT) ||
        (first.type == TOKEN_IDENT && second.type == TOKEN_LPAREN && is_callable_ident(table, first.value))) {
        char expr[MAX_STRING_LEN];
        VarType expr_type = collect_expression(lexer, table, expr, sizeof(expr), 1, 0, NULL, NULL, NULL, NULL);
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

    Token typeof_val;
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
            char resolved[MAX_STRING_LEN];
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
                        char vname[MAX_IDENTIFIER_LEN];
                        if (vlen >= MAX_IDENTIFIER_LEN) vlen = MAX_IDENTIFIER_LEN - 1;
                        memcpy(vname, vstart, vlen);
                        vname[vlen] = '\0';
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
                                char mvname[MAX_IDENTIFIER_LEN];
                                mangle_name(vname, mvname, MAX_IDENTIFIER_LEN);
                                if (!symbol_table_has(table, mvname) && !is_type_keyword(vname) && !is_bool_value(vname)) {
                                    char *errmsg = _ltng_cfmt("Variable '%s' not declared (used in string interpolation)", vname);
                                    error_state_add(parser_errors, arg.line, arg.col, errmsg ? errmsg : "");
                                    free(errmsg);
                                }
                            } else {
                                char check[MAX_STRING_LEN];
                                parse_expression_from_string(vname, table, check, sizeof(check));
                            }
                            int voff = (int)(vstart - 2 - arg.value);
                            int vtotal = (int)(sp - (vstart - 2) + 1);
                            for (int ci = voff; ci < voff + vtotal && rlen < MAX_STRING_LEN - 1; ci++) {
                                resolved[rlen++] = arg.value[ci];
                            }
                        }
                    } else {
                        if (rlen + 2 < MAX_STRING_LEN) { resolved[rlen++] = '$'; resolved[rlen++] = '['; }
                    }
                } else {
                    if (rlen < MAX_STRING_LEN - 1) resolved[rlen++] = *sp;
                }
            }
            resolved[rlen] = '\0';
            strncpy(node->value, resolved, MAX_STRING_LEN - 1);
            node->value[MAX_STRING_LEN - 1] = '\0';
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
            char cexpr[MAX_STRING_LEN];
            build_call_cexpr(call, cexpr, sizeof(cexpr));
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
                char cexpr[MAX_STRING_LEN];
                build_call_cexpr(call, cexpr, sizeof(cexpr));
                node = create_node(NODE_WRITE, "", cexpr);
                node->is_variable = 0;
                node->is_expr = 1;
                node->var_type = TYPE_STRING;
            } else if (mname.type == TOKEN_IDENT || token_is_name(mname)) {
                /* property access: obj.field, or array .length */
                char mwname[MAX_IDENTIFIER_LEN];
                mangle_name(arg.value, mwname, MAX_IDENTIFIER_LEN);
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
                    char lenname[MAX_IDENTIFIER_LEN];
                    snprintf(lenname, sizeof(lenname), "%s_len", cwname);
                    node = create_node(NODE_WRITE, "", (char *)lenname);
                    node->is_variable = 0;
                    node->is_expr = 1;
                    node->var_type = TYPE_INT;
                } else {
                    char fv[MAX_IDENTIFIER_LEN*2+8];
                    obj_resolve_field(cwname, mname.value, fv, sizeof(fv));
                    node = create_node(NODE_WRITE, fv, "");
                    node->is_variable = 1;
                    node->var_type = TYPE_STRING; /* fields stored as strings */
                }
            } else {
                lexer->current = saved_m;
                char mwname[MAX_IDENTIFIER_LEN];
                mangle_name(arg.value, mwname, MAX_IDENTIFIER_LEN);
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
            char mwname[MAX_IDENTIFIER_LEN];
            mangle_name(arg.value, mwname, MAX_IDENTIFIER_LEN);
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

static void parse_read(Lexer *lexer, SymbolTable *table) {
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

    char mangled[MAX_IDENTIFIER_LEN];
    mangle_name(arg.value, mangled, MAX_IDENTIFIER_LEN);

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
static FuncInfo *find_funcinfo_by_cname(const char *cname) {
    for (int i = 0; i < g_func_count; i++) {
        if (strcmp(g_funcs[i].name, cname) == 0) return &g_funcs[i];
    }
    return NULL;
}

static void append_arg_expr(char *out, int *pos, int out_size, ASTNode *a) {
    int rem = out_size - *pos;
    if (rem <= 1) return;
    if (a->var_type == TYPE_ARRAY && a->value[0] != '\0') {
        if (a->is_variable)
            *pos += snprintf(out + *pos, rem, "_ltng_ah_pack(%s_data, %s_len, %s_cap)", a->value, a->value, a->value);
        else
            *pos += snprintf(out + *pos, rem, "%s", a->value);
    } else if (a->var_type == TYPE_STRING && a->value[0] == '"') {
        *pos += snprintf(out + *pos, rem, "%s", a->value);
    } else if (a->is_variable && a->value[0] != '\0') {
        *pos += snprintf(out + *pos, rem, "%s", a->value);
    } else if ((a->var_type == TYPE_STRING || a->var_type == TYPE_OBJECT || a->var_type == TYPE_CHAR) && a->value[0] != '\0') {
        *pos += snprintf(out + *pos, rem, "%s", a->value);
    } else if (a->var_type == TYPE_BOOL && a->value[0] != '\0') {
        *pos += snprintf(out + *pos, rem, "((%s)?\"true\":\"false\")", a->value);
    } else if (a->value[0] == '\0') {
        *pos += snprintf(out + *pos, rem, "\"\"");
    } else {
        *pos += snprintf(out + *pos, rem, "_ltng_ad((double)(%s))", a->value);
    }
    if (*pos >= out_size) *pos = out_size - 1;
}

static void build_call_cexpr(ASTNode *call, char *cexpr, int cexpr_size) {
    int cpos = 0;
    int cline = call->line > 0 ? call->line : 1;
    int ccol = call->col > 0 ? call->col : 1;
    if (strcmp(call->name, "__ltng_indirect") == 0) {
        int argc = 0;
        for (ASTNode *a = call->args; a; a = a->next) argc++;
        cpos += snprintf(cexpr + cpos, cexpr_size - cpos, "__ltng_call_indirect(%s, _ltng_retbuf(), %d, %d, %d", call->for_var, cline, ccol, argc);
        for (ASTNode *a = call->args; a && cpos < cexpr_size - 1; a = a->next) {
            cpos += snprintf(cexpr + cpos, cexpr_size - cpos, ", ");
            append_arg_expr(cexpr, &cpos, cexpr_size, a);
        }
        cpos += snprintf(cexpr + cpos, cexpr_size - cpos, ")");
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
            cpos += snprintf(cexpr + cpos, cexpr_size - cpos,
                "({ int __rn = 0; int __rc = 0; char **__rv = _ltng_arr_new(%d);",
                restn > 0 ? restn : 1);
            for (ASTNode *a = call->rest_args; a && cpos < cexpr_size - 1; a = a->next) {
                cpos += snprintf(cexpr + cpos, cexpr_size - cpos,
                    " { char *__av = _ltng_ds_fmt(\"%%s\", ");
                append_arg_expr(cexpr, &cpos, cexpr_size, a);
                cpos += snprintf(cexpr + cpos, cexpr_size - cpos,
                    "); _ltng_arr_push(&__rv, &__rn, &__rc, __av); free(__av); }");
            }
            cpos += snprintf(cexpr + cpos, cexpr_size - cpos,
                " char *__rr = %s(_ltng_retbuf(), %d, %d", call->name, cline, ccol);
            for (ASTNode *a = call->args; a && cpos < cexpr_size - 1; a = a->next) {
                cpos += snprintf(cexpr + cpos, cexpr_size - cpos, ", ");
                append_arg_expr(cexpr, &cpos, cexpr_size, a);
            }
            cpos += snprintf(cexpr + cpos, cexpr_size - cpos,
                ", __rn, (const char**)__rv); _ltng_arr_free(__rv, __rn); __rr; })");
        } else {
            cpos += snprintf(cexpr + cpos, cexpr_size - cpos, "%s(_ltng_retbuf(), %d, %d", call->name, cline, ccol);
            for (ASTNode *a = call->args; a && cpos < cexpr_size - 1; a = a->next) {
                cpos += snprintf(cexpr + cpos, cexpr_size - cpos, ", ");
                append_arg_expr(cexpr, &cpos, cexpr_size, a);
            }
            cpos += snprintf(cexpr + cpos, cexpr_size - cpos, ")");
        }
    }
    cexpr[cexpr_size - 1] = '\0';
}

static int append_immediate_call_chains(Lexer *lexer, SymbolTable *table, char *cexpr, int cexpr_size) {
    int chained = 0;
    while (1) {
        if (lexer_peek(lexer).type == TOKEN_LPAREN) {
            chained = 1;
            if ((int)strlen(cexpr) > 2000) {
                error_state_add(parser_errors, lexer_peek(lexer).line, lexer_peek(lexer).col,
                    "Chained call expression too deep/complex");
                /* consume remaining chained call groups */
                while (lexer_peek(lexer).type == TOKEN_LPAREN) {
                    int d = 0;
                    do {
                        Token t = lexer_next(lexer);
                        if (t.type == TOKEN_LPAREN) d++;
                        else if (t.type == TOKEN_RPAREN) d--;
                        else if (t.type == TOKEN_NEWLINE || t.type == TOKEN_EOF) { d = 0; break; }
                    } while (d > 0);
                }
                return chained;
            }
            int cline = lexer_peek(lexer).line, ccol = lexer_peek(lexer).col;
            lexer_next(lexer); /* consume ( */
            ASTNode *args = NULL, *last = NULL;
            if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                while (1) {
                    char arg_expr[MAX_STRING_LEN];
                    VarType at = collect_expression(lexer, table, arg_expr, sizeof(arg_expr), 1, 1, NULL, NULL, NULL, NULL);
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
            char next_expr[MAX_STRING_LEN]; int p = 0;
            static int _chain_id = 0; int cid = _chain_id++;
            p += snprintf(next_expr + p, sizeof(next_expr) - p,
                "({ char *__ct%d=_ltng_ds_fmt(\"%%s\",%s); char *__ctr%d=__ltng_call_indirect(__ct%d,_ltng_retbuf(),%d,%d,%d",
                cid, cexpr, cid, cid, cline > 0 ? cline : 1, ccol > 0 ? ccol : 1, argc);
            if (p >= (int)sizeof(next_expr)) p = (int)sizeof(next_expr) - 1;
            for (ASTNode *a = args; a && p < (int)sizeof(next_expr) - 1; a = a->next) {
                int rem = (int)sizeof(next_expr) - p;
                if (a->var_type == TYPE_STRING && a->value[0] == '"')
                    p += snprintf(next_expr + p, rem, ",%s", a->value);
                else if (a->is_variable && a->value[0] != '\0')
                    p += snprintf(next_expr + p, rem, ",%s", a->value);
                else if ((a->var_type == TYPE_STRING || a->var_type == TYPE_OBJECT || a->var_type == TYPE_CHAR) && a->value[0] != '\0')
                    p += snprintf(next_expr + p, rem, ",%s", a->value);
                else if (a->var_type == TYPE_BOOL && a->value[0] != '\0')
                    p += snprintf(next_expr + p, rem, ",((%s)?\"true\":\"false\")", a->value);
                else if (a->value[0] == '\0')
                    p += snprintf(next_expr + p, rem, ",\"\"");
                else
                    p += snprintf(next_expr + p, rem, ",_ltng_ad((double)(%s))", a->value);
                if (p >= (int)sizeof(next_expr)) p = (int)sizeof(next_expr) - 1;
            }
            if (p < (int)sizeof(next_expr) - 1)
                p += snprintf(next_expr + p, (int)sizeof(next_expr) - p, "); free(__ct%d); __ctr%d; })", cid, cid);
            next_expr[sizeof(next_expr) - 1] = '\0';
            if (p >= (int)sizeof(next_expr) - 1) {
                error_state_add(parser_errors, cline, ccol, "Chained call expression too deep/complex");
                return chained;
            }
            strncpy(cexpr, next_expr, cexpr_size - 1);
            cexpr[cexpr_size - 1] = '\0';
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
                char *prefix = (char*)malloc(16384); if(!prefix){snprintf(cexpr,cexpr_size,"");chained=1;continue;} prefix[0]=0; int pp = 0;
                pp += snprintf(prefix+pp,16384-pp,
                    "char *__ahsrc%d=_ltng_ds_new(%s); _ltng_ah *__ah%d=_ltng_ah_unpack(__ahsrc%d); char **__td%d=NULL; int __tl%d=0,__tc%d=0; if(__ah%d){ _ltng_arr_copy(__ah%d->d,&__td%d,&__tl%d,&__tc%d,__ah%d->len); } ",
                    aid,cexpr,aid,aid,aid,aid,aid,aid,aid,aid,aid,aid,aid);
                char final_expr[4096]="\"\""; int chain_err=0; int has_scalar=0; int step=0;
                Token cm = mname; int has_chain=1;
                while (has_chain && !chain_err) {
                    step++;
                    if (lexer_peek(lexer).type != TOKEN_LPAREN) { chain_err=1; break; }
                    lexer_next(lexer);
                    has_scalar = 1;
                    if (strcmp(cm.value,"copy")==0) {
                        if(lexer_peek(lexer).type!=TOKEN_RPAREN){chain_err=1;break;} lexer_next(lexer);
                        /* copy is no-op on temp (already a copy), but keep as temp */
                        snprintf(final_expr,sizeof(final_expr),"({ char *__r=_ltng_ds_new(\"\"); for(int _i=0;_i<__tl%d;_i++){ if(_i) _ltng_ds_append(&__r,\", \"); _ltng_ds_append(&__r,__td%d[_i]?__td%d[_i]:\"\"); } __r; })",aid,aid,aid);
                        has_scalar = 1;
                    } else if (strcmp(cm.value,"slice")==0) {
                        char s0[MAX_STRING_LEN],s1[MAX_STRING_LEN];
                        collect_expression(lexer,table,s0,sizeof(s0),1,1,NULL,NULL,NULL,NULL);
                        if(lexer_peek(lexer).type!=TOKEN_COMMA){chain_err=1;break;} lexer_next(lexer);
                        collect_expression(lexer,table,s1,sizeof(s1),1,0,NULL,NULL,NULL,NULL);
                        if(lexer_peek(lexer).type!=TOKEN_RPAREN){chain_err=1;break;} lexer_next(lexer);
                        pp += snprintf(prefix+pp,sizeof(prefix)-pp,"{ char **__ns=NULL; int __nl=0,__nc=0; _ltng_arr_slice(__td%d,__tl%d,(int)(%s),(int)(%s),&__ns,&__nl,&__nc); for(int _i=0;_i<__tl%d;_i++){if(__td%d[_i])free(__td%d[_i]);} free(__td%d); __td%d=__ns; __tl%d=__nl; __tc%d=__nc; } ",aid,aid,s0,s1,aid,aid,aid,aid,aid,aid,aid);
                        snprintf(final_expr,sizeof(final_expr),"({ char *__r=_ltng_ds_new(\"\"); for(int _i=0;_i<__tl%d;_i++){ if(_i) _ltng_ds_append(&__r,\", \"); _ltng_ds_append(&__r,__td%d[_i]?__td%d[_i]:\"\"); } __r; })",aid,aid,aid);
                        has_scalar = 1;
                    } else if (strcmp(cm.value,"push")==0) {
                        char ae[MAX_STRING_LEN]; VarType at=collect_expression(lexer,table,ae,sizeof(ae),1,0,NULL,NULL,NULL,NULL);
                        if(lexer_peek(lexer).type!=TOKEN_RPAREN){chain_err=1;break;} lexer_next(lexer);
                        const char *fmt=(at==TYPE_INT)?"%lld":(at==TYPE_FLOAT)?"%g":"%s";
                        const char *cast=(at==TYPE_INT)?"(long long)":(at==TYPE_FLOAT)?"(double)":"";
                        pp += snprintf(prefix+pp,sizeof(prefix)-pp,"char *__v%d_%d = _ltng_ds_fmt(\"%s\",%s(%s)); _ltng_arr_push(&__td%d,&__tl%d,&__tc%d,__v%d_%d); ",aid,step,fmt,cast,ae,aid,aid,aid,aid,step);
                        snprintf(final_expr,sizeof(final_expr),"({ _ltng_retbuf_fmt(\"%%s\",__v%d_%d); })",aid,step);
                    } else if (strcmp(cm.value,"pop")==0) {
                        if(lexer_peek(lexer).type==TOKEN_RPAREN){ lexer_next(lexer); pp+=snprintf(prefix+pp,sizeof(prefix)-pp,"char *__pv%d_%d=_ltng_arr_pop(__td%d,&__tl%d); ",aid,step,aid,aid); snprintf(final_expr,sizeof(final_expr),"__pv%d_%d",aid,step); }
                        else { char ie[MAX_STRING_LEN]; collect_expression(lexer,table,ie,sizeof(ie),1,0,NULL,NULL,NULL,NULL); if(lexer_peek(lexer).type!=TOKEN_RPAREN){chain_err=1;break;} lexer_next(lexer); pp+=snprintf(prefix+pp,sizeof(prefix)-pp,"char *__pv%d_%d=_ltng_arr_pop_index(__td%d,&__tl%d,(int)(%s)); ",aid,step,aid,aid,ie); snprintf(final_expr,sizeof(final_expr),"__pv%d_%d",aid,step); }
                    } else if (strcmp(cm.value,"insert")==0) {
                        char ie[MAX_STRING_LEN]; collect_expression(lexer,table,ie,sizeof(ie),1,1,NULL,NULL,NULL,NULL);
                        if(lexer_peek(lexer).type!=TOKEN_COMMA){chain_err=1;break;} lexer_next(lexer);
                        char ae[MAX_STRING_LEN]; VarType at=collect_expression(lexer,table,ae,sizeof(ae),1,0,NULL,NULL,NULL,NULL);
                        if(lexer_peek(lexer).type!=TOKEN_RPAREN){chain_err=1;break;} lexer_next(lexer);
                        const char *fmt=(at==TYPE_INT)?"%lld":(at==TYPE_FLOAT)?"%g":"%s";
                        const char *cast=(at==TYPE_INT)?"(long long)":(at==TYPE_FLOAT)?"(double)":"";
                        pp+=snprintf(prefix+pp,sizeof(prefix)-pp,"char *__iv%d_%d = _ltng_ds_fmt(\"%s\",%s(%s)); _ltng_arr_insert(&__td%d,&__tl%d,&__tc%d,(int)(%s),__iv%d_%d); ",aid,step,fmt,cast,ae,aid,aid,aid,ie,aid,step);
                        snprintf(final_expr,sizeof(final_expr),"({ _ltng_retbuf_fmt(\"%%s\",__iv%d_%d); })",aid,step);
                    } else if (strcmp(cm.value,"remove")==0) {
                        char ie[MAX_STRING_LEN]; collect_expression(lexer,table,ie,sizeof(ie),1,0,NULL,NULL,NULL,NULL);
                        if(lexer_peek(lexer).type!=TOKEN_RPAREN){chain_err=1;break;} lexer_next(lexer);
                        pp+=snprintf(prefix+pp,sizeof(prefix)-pp,"char *__rv%d_%d=_ltng_arr_pop_index(__td%d,&__tl%d,(int)(%s)); ",aid,step,aid,aid,ie);
                        snprintf(final_expr,sizeof(final_expr),"__rv%d_%d",aid,step);
                    } else if (strcmp(cm.value,"sort")==0) {
                        if(lexer_peek(lexer).type!=TOKEN_RPAREN){chain_err=1;break;} lexer_next(lexer);
                        pp+=snprintf(prefix+pp,sizeof(prefix)-pp,"_ltng_arr_sort(__td%d,__tl%d); ",aid,aid);
                        snprintf(final_expr,sizeof(final_expr),"({ _ltng_retbuf_fmt(\"true\"); })");
                    } else if (strcmp(cm.value,"reverse")==0) {
                        if(lexer_peek(lexer).type!=TOKEN_RPAREN){chain_err=1;break;} lexer_next(lexer);
                        pp+=snprintf(prefix+pp,sizeof(prefix)-pp,"_ltng_arr_reverse(__td%d,__tl%d); ",aid,aid);
                        snprintf(final_expr,sizeof(final_expr),"({ _ltng_retbuf_fmt(\"true\"); })");
                    } else if (strcmp(cm.value,"contains")==0) {
                        char ae[MAX_STRING_LEN]; VarType at=collect_expression(lexer,table,ae,sizeof(ae),1,0,NULL,NULL,NULL,NULL);
                        if(lexer_peek(lexer).type!=TOKEN_RPAREN){chain_err=1;break;} lexer_next(lexer);
                        const char *fmt=(at==TYPE_INT)?"%lld":(at==TYPE_FLOAT)?"%g":"%s";
                        const char *cast=(at==TYPE_INT)?"(long long)":(at==TYPE_FLOAT)?"(double)":"";
                        pp+=snprintf(prefix+pp,sizeof(prefix)-pp,"char *__cv%d_%d = _ltng_ds_fmt(\"%s\",%s(%s)); int __cr%d_%d=_ltng_arr_contains(__td%d,__tl%d,__cv%d_%d); ",aid,step,fmt,cast,ae,aid,step,aid,aid,aid,step);
                        snprintf(final_expr,sizeof(final_expr),"({ _ltng_retbuf_fmt(\"%%s\",__cr%d_%d?\"true\":\"false\"); })",aid,step);
                    } else if (strcmp(cm.value,"index_of")==0) {
                        char ae[MAX_STRING_LEN]; VarType at=collect_expression(lexer,table,ae,sizeof(ae),1,0,NULL,NULL,NULL,NULL);
                        if(lexer_peek(lexer).type!=TOKEN_RPAREN){chain_err=1;break;} lexer_next(lexer);
                        const char *fmt=(at==TYPE_INT)?"%lld":(at==TYPE_FLOAT)?"%g":"%s";
                        const char *cast=(at==TYPE_INT)?"(long long)":(at==TYPE_FLOAT)?"(double)":"";
                        pp+=snprintf(prefix+pp,sizeof(prefix)-pp,"char *__ix%d_%d = _ltng_ds_fmt(\"%s\",%s(%s)); int __ir%d_%d=_ltng_arr_index_of(__td%d,__tl%d,__ix%d_%d); ",aid,step,fmt,cast,ae,aid,step,aid,aid,aid,step);
                        snprintf(final_expr,sizeof(final_expr),"({ _ltng_retbuf_fmt(\"%%d\",__ir%d_%d); })",aid,step);
                    } else { chain_err=1; break; }
                    has_chain=0;
                    if(lexer_peek(lexer).type==TOKEN_DOT){ int sd=lexer->current; lexer_next(lexer); Token nm=lexer_peek(lexer); if((nm.type==TOKEN_IDENT||token_is_name(nm)) && (strcmp(nm.value,"push")==0||strcmp(nm.value,"pop")==0||strcmp(nm.value,"insert")==0||strcmp(nm.value,"remove")==0||strcmp(nm.value,"contains")==0||strcmp(nm.value,"index_of")==0||strcmp(nm.value,"sort")==0||strcmp(nm.value,"reverse")==0||strcmp(nm.value,"copy")==0||strcmp(nm.value,"slice")==0)){ lexer_next(lexer); cm=nm; has_chain=1; } else lexer->current=sd; }
                }
                if (chain_err) { snprintf(cexpr,cexpr_size,"\"\""); free(prefix); }
                else {
                    char *buf = (char*)malloc(16384);
                    if(buf){ snprintf(buf,16384,"({ %schar *__fres%d=(char*)(%s); for(int _i=0;_i<__tl%d;_i++){if(__td%d[_i])free(__td%d[_i]);} free(__td%d); _ltng_ah_free(__ah%d); free(__ahsrc%d); __fres%d; })",
                        prefix,aid,final_expr,aid,aid,aid,aid,aid,aid,aid);
                        strncpy(cexpr,buf,cexpr_size-1); cexpr[cexpr_size-1]='\0';
                        free(buf);
                    } else { snprintf(cexpr,cexpr_size,""); }
                }
                free(prefix);
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
            strncpy(recv->value, cexpr, MAX_STRING_LEN - 1);
            recv->value[MAX_STRING_LEN - 1] = '\0';
            ASTNode *call = parse_call_node(lexer, table, mname.value, mname.line, mname.col, recv);
            if (call->is_base_assign) {
                /* Value-consumed context: keep receiver intact, use return value */
                call->is_base_assign = 0;
            }
            build_call_cexpr(call, cexpr, cexpr_size);
            chained = 1;
            continue;
        }
        break;
    }
    return chained;
}

static ASTNode *make_receiver_arg(SymbolTable *table, Token rtok) {
    ASTNode *recv = create_node(NODE_PARAM, "", "");
    char rm[MAX_IDENTIFIER_LEN];
    mangle_name(rtok.value, rm, MAX_IDENTIFIER_LEN);
    if (symbol_table_has(table, rm)) {
        recv->is_variable = 1;
        strncpy(recv->value, rm, MAX_STRING_LEN - 1);
        recv->var_type = symbol_table_get(table, rm);
    } else if (symbol_table_has(table, rtok.value)) {
        recv->is_variable = 1;
        strncpy(recv->value, rtok.value, MAX_STRING_LEN - 1);
        recv->var_type = symbol_table_get(table, rtok.value);
    } else {
        recv->is_expr = 1;
        strncpy(recv->value, rtok.value, MAX_STRING_LEN - 1);
        recv->var_type = TYPE_FLOAT;
    }
    return recv;
}

static int get_function_passthrough_param(const char *fname);

/* Get the effective result type of a call, applying pass-through specialization
   when feasible. For direct calls to pass-through functions, uses the argument
   type of the passed-through parameter. */
static VarType infer_call_result_type(ASTNode *call) {
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

static int try_parse_sole_call_expr(Lexer *lexer, SymbolTable *table, char *cexpr, int cexpr_size, int allow_base_assign, VarType *out_type, char *out_call_cname, int out_call_cname_sz) {
    Token first = lexer_peek(lexer);
    int saved = lexer->current;
    if (out_type) *out_type = TYPE_STRING;
    if (out_call_cname && out_call_cname_sz > 0) out_call_cname[0] = '\0';
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
    char cur_expr[MAX_STRING_LEN];
    build_call_cexpr(call, cur_expr, sizeof(cur_expr));
    int chain = 0;
    while (lexer_peek(lexer).type == TOKEN_LPAREN) {
        if ((int)strlen(cur_expr) > 2000) {
            error_state_add(parser_errors, lexer_peek(lexer).line, lexer_peek(lexer).col,
                "Chained call expression too deep/complex");
            lexer->current = saved; return 0;
        }
        chain = 1;
        int cline = lexer_peek(lexer).line;
        int ccol = lexer_peek(lexer).col;
        lexer_next(lexer); /* consume ( */
        /* parse args as indirect call */
        ASTNode *args = NULL, *last = NULL;
        if (lexer_peek(lexer).type != TOKEN_RPAREN) {
            while (1) {
                char arg_expr[MAX_STRING_LEN];
                VarType at = collect_expression(lexer, table, arg_expr, sizeof(arg_expr), 1, 1, NULL, NULL, NULL, NULL);
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
        char next_expr[MAX_STRING_LEN];
        int p = 0;
        static int _scid = 0; int cid = _scid++;
        p += snprintf(next_expr + p, sizeof(next_expr) - p, "({ char *__ct%d=_ltng_ds_fmt(\"%%s\",%s); char *__ctr%d=__ltng_call_indirect(__ct%d,_ltng_retbuf(),%d,%d,%d",
            cid, cur_expr, cid, cid, cline>0?cline:1, ccol>0?ccol:1, argc);
        if (p >= (int)sizeof(next_expr)) p = (int)sizeof(next_expr) - 1;
        for (ASTNode *a = args; a && p < (int)sizeof(next_expr) - 1; a = a->next) {
            int rem = (int)sizeof(next_expr) - p;
            if (a->var_type == TYPE_STRING && a->value[0] == '"')
                p += snprintf(next_expr + p, rem, ",%s", a->value);
            else if (a->is_variable && a->value[0] != '\0')
                p += snprintf(next_expr + p, rem, ",%s", a->value);
            else if ((a->var_type == TYPE_STRING || a->var_type == TYPE_OBJECT || a->var_type == TYPE_CHAR) && a->value[0] != '\0')
                p += snprintf(next_expr + p, rem, ",%s", a->value);
            else if (a->var_type == TYPE_BOOL && a->value[0] != '\0')
                p += snprintf(next_expr + p, rem, ",((%s)?\"true\":\"false\")", a->value);
            else if (a->value[0] == '\0')
                p += snprintf(next_expr + p, rem, ",\"\"");
            else
                p += snprintf(next_expr + p, rem, ",_ltng_ad((double)(%s))", a->value);
            if (p >= (int)sizeof(next_expr)) p = (int)sizeof(next_expr) - 1;
        }
        if (p < (int)sizeof(next_expr) - 1)
            p += snprintf(next_expr + p, (int)sizeof(next_expr) - p, "); free(__ct%d); __ctr%d; })", cid, cid);
        next_expr[sizeof(next_expr)-1]=0;
        if (p >= (int)sizeof(next_expr) - 1) {
            error_state_add(parser_errors, cline, ccol, "Chained call expression too deep/complex");
            lexer->current = saved; return 0;
        }
        strncpy(cur_expr, next_expr, sizeof(cur_expr)-1); cur_expr[sizeof(cur_expr)-1]=0;
    }

    Token after = lexer_peek(lexer);
    if (after.type != TOKEN_NEWLINE && after.type != TOKEN_EOF && after.type != TOKEN_RBRACE) {
        lexer->current = saved; return 0;
    }
    strncpy(cexpr, cur_expr, cexpr_size - 1); cexpr[cexpr_size - 1] = 0;
    if (out_type) {
        if (chain) *out_type = TYPE_STRING; /* chained indirect/dynamic result */
        else if (strcmp(call->name, "__ltng_indirect") == 0) *out_type = TYPE_STRING;
        else *out_type = infer_call_result_type(call);
    }
    if (out_call_cname && out_call_cname_sz > 0 && !chain) {
        strncpy(out_call_cname, call->name, out_call_cname_sz - 1);
        out_call_cname[out_call_cname_sz - 1] = '\0';
    }
    return 1;
}


static int parse_object_literal(Lexer *lexer, SymbolTable *table, const char *mangled, int is_const, int is_assignment) {
    /* assumes '{' consumed. is_assignment: 1 when used in set (not a new declaration). */
    ObjSchema schema;
    memset(&schema, 0, sizeof(schema));

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
        ObjField *f = NULL;
        if (schema.count < MAX_OBJ_FIELDS) f = &schema.fields[schema.count];
        if (f) {
            strncpy(f->name, fname.value, MAX_IDENTIFIER_LEN - 1);
            if (val.type == TOKEN_STRING) {
                lexer_next(lexer);
                f->type = TYPE_STRING;
                strncpy(f->value, val.value, MAX_STRING_LEN - 1);
            } else if (val.type == TOKEN_CHAR) {
                lexer_next(lexer);
                f->type = TYPE_STRING;
                strncpy(f->value, val.value, MAX_STRING_LEN - 1);
            } else if (val.type == TOKEN_INT_LIT) {
                lexer_next(lexer);
                f->type = TYPE_INT;
                strncpy(f->value, val.value, MAX_STRING_LEN - 1);
            } else if (val.type == TOKEN_FLOAT_LIT) {
                lexer_next(lexer);
                f->type = TYPE_FLOAT;
                strncpy(f->value, val.value, MAX_STRING_LEN - 1);
            } else if ((val.type == TOKEN_IDENT || token_is_name(val)) && is_bool_value(val.value)) {
                lexer_next(lexer);
                f->type = TYPE_BOOL;
                strncpy(f->value, (strcmp(val.value,"true")==0||strcmp(val.value,"t")==0) ? "true" : "false", MAX_STRING_LEN - 1);
            } else if (val.type == TOKEN_MINUS && (lexer->current+1 < lexer->count) &&
                       (lexer->tokens[lexer->current+1].type == TOKEN_INT_LIT || lexer->tokens[lexer->current+1].type == TOKEN_FLOAT_LIT)) {
                lexer_next(lexer); /* - */
                Token num = lexer_next(lexer);
                f->type = (num.type == TOKEN_FLOAT_LIT) ? TYPE_FLOAT : TYPE_INT;
                snprintf(f->value, MAX_STRING_LEN, "-%s", num.value);
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
    strncpy(schema.var, cname, MAX_IDENTIFIER_LEN - 1);
    schema.var[MAX_IDENTIFIER_LEN - 1] = '\0';
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
static void emit_chained_methods_on_array_var(Lexer *lexer, SymbolTable *table,
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
                char arg_expr[MAX_STRING_LEN];
                VarType arg_type = collect_expression(lexer, table, arg_expr, sizeof(arg_expr), 1, 0, NULL, NULL, NULL, NULL);
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
                char idx_expr[MAX_STRING_LEN];
                collect_expression(lexer, table, idx_expr, sizeof(idx_expr), 1, 1, NULL, NULL, NULL, NULL);
                if (lexer_peek(lexer).type != TOKEN_COMMA) {
                    error_state_add(parser_errors, lexer_peek(lexer).line, lexer_peek(lexer).col, "Expected ',' after insert index");
                    skip_to_newline(lexer); return;
                }
                lexer_next(lexer);
                char arg_expr[MAX_STRING_LEN];
                VarType arg_type = collect_expression(lexer, table, arg_expr, sizeof(arg_expr), 1, 0, NULL, NULL, NULL, NULL);
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
                strncpy(node->array_index, arg_expr, MAX_STRING_LEN - 1);
                node->array_index[MAX_STRING_LEN - 1] = '\0';
                node->var_type = arg_type; node->is_expr = 1; emit(node);
            } else if (strcmp(mname.value, "remove") == 0) {
                char idx_expr[MAX_STRING_LEN];
                collect_expression(lexer, table, idx_expr, sizeof(idx_expr), 1, 0, NULL, NULL, NULL, NULL);
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
                    char idx_expr[MAX_STRING_LEN];
                    collect_expression(lexer, table, idx_expr, sizeof(idx_expr), 1, 0, NULL, NULL, NULL, NULL);
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
                    char dummy[MAX_STRING_LEN];
                    collect_expression(lexer, table, dummy, sizeof(dummy), 1, 0, NULL, NULL, NULL, NULL);
                    if (lexer_peek(lexer).type == TOKEN_RPAREN) lexer_next(lexer);
                }
            }
        } else if (is_function_name_scoped(table, mname.value)) {
            /* Chained user-defined method on target */
            lexer_next(lexer); /* consume ( */
            ASTNode *recv = create_node(NODE_PARAM, "", "");
            if (symbol_table_has(table, target_rm)) {
                recv->is_variable = 1;
                strncpy(recv->value, target_rm, MAX_STRING_LEN - 1);
                recv->var_type = symbol_table_get(table, target_rm);
            } else if (symbol_table_has(table, target_src)) {
                recv->is_variable = 1;
                strncpy(recv->value, target_src, MAX_STRING_LEN - 1);
                recv->var_type = symbol_table_get(table, target_src);
            } else {
                recv->is_expr = 1;
                strncpy(recv->value, target_src, MAX_STRING_LEN - 1);
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


static void parse_let(Lexer *lexer, SymbolTable *table) {
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

    char mangled[MAX_IDENTIFIER_LEN];
    mangle_name(name.value, mangled, MAX_IDENTIFIER_LEN);

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
            char fval[MAX_STRING_LEN];
            if (fn && fn->capture_count > 0) {
                char expr[MAX_STRING_LEN];
                snprintf(expr, sizeof(expr), "\"%s\"", fn->name);
                for (int ci = 0; ci < fn->capture_count; ci++) {
                    char tmp[MAX_STRING_LEN];
                    snprintf(tmp, sizeof(tmp), "_ltng_add_sn(_ltng_add_sn(%s,\"\\001\"),_ltng_ptr_to_str((void*)%s))", expr, fn->captures[ci]);
                    strncpy(expr, tmp, sizeof(expr)-1); expr[sizeof(expr)-1]=0;
                }
                snprintf(fval, sizeof(fval), "%s", expr);
            } else {
                snprintf(fval, sizeof(fval), "\"%s\"", fn ? fn->name : "");
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
            char mangled_ref[MAX_IDENTIFIER_LEN]; mangle_name(fnref_tok.value, mangled_ref, sizeof(mangled_ref));
            int is_var = symbol_table_has(table, mangled_ref) || symbol_table_has(table, fnref_tok.value);
            char ref_cname[MAX_IDENTIFIER_LEN]; int ref_pc = -1;
            if (!is_var && resolve_func_ref_info(table, fnref_tok.value, ref_cname, sizeof(ref_cname), &ref_pc)) {
                lexer_next(lexer);
                symbol_table_add(table, mangled, TYPE_STRING);
                symbol_table_set_func_ref(table, mangled, ref_cname, ref_pc);
                const char *cname = symbol_table_get_cname(table, mangled);
                char fval[MAX_STRING_LEN];
                snprintf(fval, sizeof(fval), "\"%s\"", ref_cname);
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
            char sm[MAX_IDENTIFIER_LEN];
            mangle_name(src_tok.value, sm, MAX_IDENTIFIER_LEN);
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
                        char start_expr[MAX_STRING_LEN];
                        collect_expression(lexer, table, start_expr, sizeof(start_expr), 1, 1, NULL, NULL, NULL, NULL);
                        if (lexer_peek(lexer).type != TOKEN_COMMA) {
                            error_state_add(parser_errors, lexer_peek(lexer).line,
                                lexer_peek(lexer).col, "Expected ',' after slice start");
                            skip_to_newline(lexer);
                            return;
                        }
                        lexer_next(lexer); /* consume , */
                        char end_expr[MAX_STRING_LEN];
                        collect_expression(lexer, table, end_expr, sizeof(end_expr), 1, 0, NULL, NULL, NULL, NULL);
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
                        char slrange[MAX_STRING_LEN];
                        snprintf(slrange, sizeof(slrange), "%s,%s", start_expr, end_expr);
                        ASTNode *sl = create_node(NODE_ARRAY_SLICE, dcname, (char *)scname);
                        strncpy(sl->array_index, slrange, MAX_STRING_LEN - 1);
                        sl->array_index[MAX_STRING_LEN - 1] = '\0';
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
    Token value;

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
                case TYPE_INT: value.type = TOKEN_INT_LIT; strcpy(value.value, "0"); break;
                case TYPE_FLOAT: value.type = TOKEN_INT_LIT; strcpy(value.value, "0"); break;
                case TYPE_STRING: value.type = TOKEN_STRING; strcpy(value.value, ""); break;
                case TYPE_CHAR: value.type = TOKEN_STRING; strcpy(value.value, ""); break;
                case TYPE_BOOL: value.type = TOKEN_INT_LIT; strcpy(value.value, "false"); break;
                case TYPE_OBJECT: value.type = TOKEN_STRING; strcpy(value.value, ""); break;
                case TYPE_ARRAY: value.type = TOKEN_STRING; strcpy(value.value, ""); break;
                case TYPE_NULL:
                default: value.type = TOKEN_INT_LIT; strcpy(value.value, "0"); break;
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
            char inner_raw[MAX_STRING_LEN] = {0};
            int irpos = 0, depth = 1;
            int arg_start = lexer->current;
            while (lexer->current < lexer->count && depth > 0) {
                Token itok = lexer_peek(lexer);
                if (itok.type == TOKEN_LPAREN) depth++;
                else if (itok.type == TOKEN_RPAREN) { depth--; if (depth == 0) break; }
                else if (itok.type == TOKEN_NEWLINE || itok.type == TOKEN_EOF) break;
                itok = lexer_next(lexer);
                token_append_source(inner_raw, &irpos, MAX_STRING_LEN, itok);
            }
            while (irpos > 0 && (inner_raw[irpos-1] == ' ' || inner_raw[irpos-1] == '\t')) irpos--;
            inner_raw[irpos] = '\0';
            int arg_end = lexer->current;
            if (lexer->current < lexer->count && lexer_peek(lexer).type == TOKEN_RPAREN) {
                lexer_next(lexer); /* consume ) */
            }
            Token arg = lexer->tokens[arg_start];
            int arg_tok_count = arg_end - arg_start;
            char expr[MAX_STRING_LEN] = "0";
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
                    if (arg.type == TOKEN_STRING) snprintf(expr, sizeof(expr), "(long long)atoll(\"%s\")", arg.value);
                    else if (arg.type == TOKEN_INT_LIT || arg.type == TOKEN_FLOAT_LIT) snprintf(expr, sizeof(expr), "%lld", (long long)atoll(arg.value));
                    else if (arg.type == TOKEN_CHAR) snprintf(expr, sizeof(expr), "%lld", (long long)(unsigned char)arg.value[0]);
                    else if (arg.type == TOKEN_IDENT && is_bool_value(arg.value)) snprintf(expr, sizeof(expr), "%d", (strcmp(arg.value,"true")==0||strcmp(arg.value,"t")==0)?1:0);
                    else { char m[MAX_IDENTIFIER_LEN]; mangle_name(arg.value,m,MAX_IDENTIFIER_LEN); if(symbol_table_has(table,m)) snprintf(expr,sizeof(expr),"(long long)atoll(%s)", symbol_table_get_cname(table,m)); else snprintf(expr,sizeof(expr),"0"); }
                } else if (strcmp(conv.value, "tofloat") == 0 || strcmp(conv.value, "float") == 0) {
                    expr_type = TYPE_FLOAT;
                    if (arg.type == TOKEN_STRING) snprintf(expr, sizeof(expr), "(double)atof(\"%s\")", arg.value);
                    else if (arg.type == TOKEN_INT_LIT || arg.type == TOKEN_FLOAT_LIT) snprintf(expr, sizeof(expr), "%.17g", atof(arg.value));
                    else if (arg.type == TOKEN_IDENT && is_bool_value(arg.value)) snprintf(expr, sizeof(expr), "%d.0", (strcmp(arg.value,"true")==0||strcmp(arg.value,"t")==0)?1:0);
                    else { char m[MAX_IDENTIFIER_LEN]; mangle_name(arg.value,m,MAX_IDENTIFIER_LEN); if(symbol_table_has(table,m)) snprintf(expr,sizeof(expr),"(double)atof(%s)", symbol_table_get_cname(table,m)); else snprintf(expr,sizeof(expr),"0.0"); }
                } else if (strcmp(conv.value, "tobool") == 0 || strcmp(conv.value, "bool") == 0) {
                    if (arg.type == TOKEN_IDENT && !is_bool_value(arg.value)) {
                        handled = 0; /* runtime variable: handle below */
                    } else {
                        expr_type = TYPE_BOOL;
                        if (arg.type == TOKEN_STRING) {
                            int is_false = (arg.value[0]=='\0' || strcmp(arg.value,"0")==0 || strcmp(arg.value,"false")==0 || strcmp(arg.value,"f")==0);
                            snprintf(expr,sizeof(expr),"%s", is_false?"false":"true");
                        } else if (arg.type == TOKEN_INT_LIT || arg.type == TOKEN_FLOAT_LIT) {
                            snprintf(expr,sizeof(expr),"%s", (atof(arg.value)!=0.0)?"true":"false");
                        } else if (arg.type == TOKEN_IDENT && is_bool_value(arg.value)) {
                            snprintf(expr,sizeof(expr),"%s", (strcmp(arg.value,"true")==0||strcmp(arg.value,"t")==0)?"true":"false");
                        } else {
                            snprintf(expr,sizeof(expr),"false");
                        }
                    }
                } else if (strcmp(conv.value, "tostring") == 0 || strcmp(conv.value, "string") == 0) {
                    expr_type = TYPE_STRING;
                    if (arg.type == TOKEN_STRING || arg.type == TOKEN_CHAR) {
                        int p = 0;
                        expr[p++] = '"';
                        for (const char *sp = arg.value; *sp && p < (int)sizeof(expr)-3; sp++) {
                            unsigned char c = (unsigned char)*sp;
                            if (c=='\\') { expr[p++]='\\'; expr[p++]='\\'; }
                            else if (c=='"') { expr[p++]='\\'; expr[p++]='"'; }
                            else if (c=='\n') { expr[p++]='\\'; expr[p++]='n'; }
                            else if (c=='\t') { expr[p++]='\\'; expr[p++]='t'; }
                            else if (c=='\r') { expr[p++]='\\'; expr[p++]='r'; }
                            else expr[p++] = c;
                        }
                        expr[p++] = '"'; expr[p] = '\0';
                    } else if (arg.type == TOKEN_INT_LIT || arg.type == TOKEN_FLOAT_LIT) snprintf(expr,sizeof(expr),"\"%s\"", arg.value);
                    else if (arg.type == TOKEN_IDENT && is_bool_value(arg.value)) snprintf(expr,sizeof(expr),"\"%s\"", (strcmp(arg.value,"true")==0||strcmp(arg.value,"t")==0)?"true":"false");
                    else { char m[MAX_IDENTIFIER_LEN]; mangle_name(arg.value,m,MAX_IDENTIFIER_LEN); if(symbol_table_has(table,m)) snprintf(expr,sizeof(expr),"%s", symbol_table_get_cname(table,m)); else snprintf(expr,sizeof(expr),"\"\""); }
                } else { /* tochar/char */
                    expr_type = TYPE_CHAR;
                    if (arg.type == TOKEN_STRING) {
                        unsigned char ch = (unsigned char)(arg.value[0]?arg.value[0]:0);
                        if (arg.value[0]=='\\' && arg.value[1]) snprintf(expr,sizeof(expr),"\"\\%c\"", arg.value[1]);
                        else if (ch=='\n') snprintf(expr,sizeof(expr),"\"\\n\"");
                        else if (ch=='\t') snprintf(expr,sizeof(expr),"\"\\t\"");
                        else if (ch=='\r') snprintf(expr,sizeof(expr),"\"\\r\"");
                        else if (ch=='\\') snprintf(expr,sizeof(expr),"\"\\\\\"");
                        else if (ch=='\"') snprintf(expr,sizeof(expr),"\"\\\"\"");
                        else snprintf(expr,sizeof(expr),"\"%c\"", ch);
                    } else if (arg.type == TOKEN_INT_LIT) {
                        int iv = (int)atoi(arg.value);
                        if (iv=='\n') snprintf(expr,sizeof(expr),"\"\\n\"");
                        else if (iv=='\t') snprintf(expr,sizeof(expr),"\"\\t\"");
                        else if (iv=='\r') snprintf(expr,sizeof(expr),"\"\\r\"");
                        else if (iv=='\\') snprintf(expr,sizeof(expr),"\"\\\\\"");
                        else if (iv=='\"') snprintf(expr,sizeof(expr),"\"\\\"\"");
                        else if (iv>=32 && iv<=126) snprintf(expr,sizeof(expr),"\"%c\"", (char)iv);
                        else snprintf(expr,sizeof(expr), "({ char *_tc=_ltng_ds_new(\"\"); int _cv=%d; if(_cv=='\\n') _ltng_ds_set(&_tc,\"\\n\"); else if(_cv=='\\t') _ltng_ds_set(&_tc,\"\\t\"); else if(_cv=='\\r') _ltng_ds_set(&_tc,\"\\r\"); else if(_cv=='\\\\') _ltng_ds_set(&_tc,\"\\\\\"); else if(_cv=='\\\"') _ltng_ds_set(&_tc,\"\\\"\"); else _ltng_ds_setfmt(&_tc,\"%%c\",(char)_cv); char *_r=_ltng_as(_tc); free(_tc); _r; })", iv);
                    } else if (arg.type == TOKEN_CHAR) {
                        /* token value for char literal is the char content (possibly escaped like n or \\) */
                        if (arg.value[0]=='\\' && arg.value[1]) snprintf(expr,sizeof(expr),"\"\\%c\"", arg.value[1]);
                        else if (arg.value[0]=='\n') snprintf(expr,sizeof(expr),"\"\\n\"");
                        else if (arg.value[0]=='\t') snprintf(expr,sizeof(expr),"\"\\t\"");
                        else if (arg.value[0]=='\r') snprintf(expr,sizeof(expr),"\"\\r\"");
                        else if (arg.value[0]=='\\') snprintf(expr,sizeof(expr),"\"\\\\\"");
                        else if (arg.value[0]=='\"') snprintf(expr,sizeof(expr),"\"\\\"\"");
                        else snprintf(expr,sizeof(expr),"\"%c\"", arg.value[0]?arg.value[0]:'\0');
                    } else { char m[MAX_IDENTIFIER_LEN]; mangle_name(arg.value,m,MAX_IDENTIFIER_LEN); if(symbol_table_has(table,m)) { const char *cn=symbol_table_get_cname(table,m); snprintf(expr,sizeof(expr),"(_ltng_is_num_str(%s)?((char[]){(char)atoll(%s),0}):((char[]){(%s)[0]? (%s)[0]:0,0}))",cn,cn,cn,cn);} else snprintf(expr,sizeof(expr),"\"\""); }
                }
            }
            if (!handled) {
                /* Complex/runtime arg: compile conversion expression via general path, then adapt for typed storage */
                char conv_expr_src[MAX_STRING_LEN];
                snprintf(conv_expr_src, sizeof(conv_expr_src), "%s(%s)", conv.value, inner_raw);
                char compiled[MAX_STRING_LEN]; compiled[0] = '\0';
                VarType ctype = parse_expression_from_string(conv_expr_src, table, compiled, sizeof(compiled));
                if (compiled[0] == '\0') snprintf(compiled, sizeof(compiled), "0");
                if (strcmp(conv.value, "toint") == 0 || strcmp(conv.value, "int") == 0) {
                    expr_type = TYPE_INT;
                    if (ctype == TYPE_INT || ctype == TYPE_BOOL) snprintf(expr,sizeof(expr),"(long long)(%s)", compiled);
                    else snprintf(expr,sizeof(expr),"(long long)atoll(%s)", compiled);
                } else if (strcmp(conv.value, "tofloat") == 0 || strcmp(conv.value, "float") == 0) {
                    expr_type = TYPE_FLOAT;
                    if (ctype == TYPE_FLOAT) snprintf(expr,sizeof(expr),"(double)(%s)", compiled);
                    else if (ctype == TYPE_INT || ctype == TYPE_BOOL) snprintf(expr,sizeof(expr),"(double)(%s)", compiled);
                    else snprintf(expr,sizeof(expr),"(double)atof(%s)", compiled);
                } else if (strcmp(conv.value, "tobool") == 0 || strcmp(conv.value, "bool") == 0) {
                    expr_type = TYPE_BOOL;
                    if (ctype == TYPE_BOOL) snprintf(expr,sizeof(expr),"%s", compiled);
                    else snprintf(expr,sizeof(expr),"(((%s)[0]!='\\0'&&strcmp(%s,\"0\")!=0&&strcmp(%s,\"false\")!=0&&strcmp(%s,\"f\")!=0)?\"true\":\"false\")", compiled, compiled, compiled, compiled);
                } else if (strcmp(conv.value, "tochar") == 0 || strcmp(conv.value, "char") == 0) {
                    expr_type = TYPE_CHAR;
                    snprintf(expr,sizeof(expr),"%s", compiled);
                } else {
                    expr_type = TYPE_STRING;
                    snprintf(expr,sizeof(expr),"%s", compiled);
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
        char expr[MAX_STRING_LEN];
        VarType expr_type = collect_expression(lexer, table, expr, sizeof(expr), 0, 0, NULL, NULL, NULL, NULL);
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
            char expr[MAX_STRING_LEN];
            /* Skip try_parse_sole_call_expr for array.push/pop - let
               collect_expression handle them as inline C expressions */
            if (!(lexer->current + 2 < lexer->count &&
                  lexer->tokens[lexer->current + 1].type == TOKEN_DOT &&
                  lexer->tokens[lexer->current + 2].type == TOKEN_IDENT &&
                  (strcmp(lexer->tokens[lexer->current + 2].value, "push") == 0 ||
                   strcmp(lexer->tokens[lexer->current + 2].value, "pop") == 0))) {
                VarType call_type = TYPE_STRING;
                char call_cname[MAX_IDENTIFIER_LEN] = "";
                if (try_parse_sole_call_expr(lexer, table, expr, sizeof(expr), 1, &call_type, call_cname, sizeof(call_cname))) {
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
                        ObjSchema ds; memset(&ds, 0, sizeof(ds));
                        strncpy(ds.var, cname, MAX_IDENTIFIER_LEN - 1);
                        for (int i = 0; i < ret_sc->count && i < MAX_OBJ_FIELDS; i++) ds.fields[i] = ret_sc->fields[i];
                        ds.count = ret_sc->count;
                        obj_build_all(&ds);
                        obj_register(cname, &ds);
                        ASTNode *node = create_node(NODE_LET, cname, expr);
                        node->var_type = TYPE_OBJECT;
                        node->is_expr = 1;
                        node->is_default = 1;
                        emit(node);
                        /* copy returned object fields from function staging area */
                        for (int i = 0; i < ret_sc->count; i++) {
                            char df[MAX_IDENTIFIER_LEN*2+8], sf[MAX_IDENTIFIER_LEN*2+8];
                            obj_resolve_field(cname, ret_sc->fields[i].name, df, sizeof(df));
                            char stage_base[MAX_IDENTIFIER_LEN*2+16];
                            snprintf(stage_base, sizeof(stage_base), "__ltng_oret_%s", call_cname);
                            obj_resolve_field(stage_base, ret_sc->fields[i].name, sf, sizeof(sf));
                            ASTNode *sn = create_node(NODE_SET, df, sf);
                            sn->var_type = TYPE_STRING;
                            sn->is_expr = 1;
                            sn->is_variable = 1;
                            emit(sn);
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
            VarType expr_type = collect_expression(lexer, table, expr, sizeof(expr), 0, 0, NULL, NULL, NULL, NULL);
            symbol_table_add(table, mangled, expr_type);
            const char *cname = symbol_table_get_cname(table, mangled);
            ASTNode *node = create_node(NODE_LET, cname, expr);
            node->var_type = expr_type;
            node->is_expr = 1;
            emit(node);
            return;
        }
        Token typeof_val;
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

static void parse_const(Lexer *lexer, SymbolTable *table) {
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

    char mangled[MAX_IDENTIFIER_LEN];
    mangle_name(name.value, mangled, MAX_IDENTIFIER_LEN);

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
            char fval[MAX_STRING_LEN];
            if (fn && fn->capture_count > 0) {
                char expr[MAX_STRING_LEN];
                snprintf(expr, sizeof(expr), "\"%s\"", fn->name);
                for (int ci = 0; ci < fn->capture_count; ci++) {
                    char tmp[MAX_STRING_LEN];
                    snprintf(tmp, sizeof(tmp), "_ltng_add_sn(_ltng_add_sn(%s,\"\\001\"),_ltng_ptr_to_str((void*)%s))", expr, fn->captures[ci]);
                    strncpy(expr, tmp, sizeof(expr)-1); expr[sizeof(expr)-1]=0;
                }
                snprintf(fval, sizeof(fval), "%s", expr);
            } else {
                snprintf(fval, sizeof(fval), "\"%s\"", fn ? fn->name : "");
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
            char mangled_ref[MAX_IDENTIFIER_LEN]; mangle_name(fnref_tok.value, mangled_ref, sizeof(mangled_ref));
            int is_var = symbol_table_has(table, mangled_ref) || symbol_table_has(table, fnref_tok.value);
            char ref_cname[MAX_IDENTIFIER_LEN]; int ref_pc = -1;
            if (!is_var && resolve_func_ref_info(table, fnref_tok.value, ref_cname, sizeof(ref_cname), &ref_pc)) {
                lexer_next(lexer);
                symbol_table_add_locked(table, mangled, TYPE_STRING);
                symbol_table_set_func_ref(table, mangled, ref_cname, ref_pc);
                const char *cname = symbol_table_get_cname(table, mangled);
                char fval[MAX_STRING_LEN];
                snprintf(fval, sizeof(fval), "\"%s\"", ref_cname);
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
            char sm[MAX_IDENTIFIER_LEN];
            mangle_name(src_tok.value, sm, MAX_IDENTIFIER_LEN);
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
                        char start_expr[MAX_STRING_LEN];
                        collect_expression(lexer, table, start_expr, sizeof(start_expr), 1, 1, NULL, NULL, NULL, NULL);
                        if (lexer_peek(lexer).type != TOKEN_COMMA) {
                            error_state_add(parser_errors, lexer_peek(lexer).line, lexer_peek(lexer).col, "Expected ',' after slice start");
                            skip_to_newline(lexer); return;
                        }
                        lexer_next(lexer); /* consume , */
                        char end_expr[MAX_STRING_LEN];
                        collect_expression(lexer, table, end_expr, sizeof(end_expr), 1, 0, NULL, NULL, NULL, NULL);
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
                        char slrange[MAX_STRING_LEN];
                        snprintf(slrange, sizeof(slrange), "%s,%s", start_expr, end_expr);
                        ASTNode *sl = create_node(NODE_ARRAY_SLICE, dcname, (char *)scname);
                        strncpy(sl->array_index, slrange, MAX_STRING_LEN - 1);
                        sl->array_index[MAX_STRING_LEN - 1] = '\0';
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
        char expr[MAX_STRING_LEN];
        VarType expr_type = collect_expression(lexer, table, expr, sizeof(expr), 0, 0, NULL, NULL, NULL, NULL);
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
            char expr[MAX_STRING_LEN];
            /* Skip try_parse_sole_call_expr for array.push/pop - let
               collect_expression handle them as inline C expressions */
            if (!(lexer->current + 2 < lexer->count &&
                  lexer->tokens[lexer->current + 1].type == TOKEN_DOT &&
                  lexer->tokens[lexer->current + 2].type == TOKEN_IDENT &&
                  (strcmp(lexer->tokens[lexer->current + 2].value, "push") == 0 ||
                   strcmp(lexer->tokens[lexer->current + 2].value, "pop") == 0))) {
                VarType call_type = TYPE_STRING;
                char call_cname[MAX_IDENTIFIER_LEN] = "";
                if (try_parse_sole_call_expr(lexer, table, expr, sizeof(expr), 1, &call_type, call_cname, sizeof(call_cname))) {
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
                        
                        ObjSchema ds; memset(&ds, 0, sizeof(ds));
                        strncpy(ds.var, dcname, MAX_IDENTIFIER_LEN - 1);
                        for (int i = 0; i < ret_sc->count && i < MAX_OBJ_FIELDS; i++) ds.fields[i] = ret_sc->fields[i];
                        ds.count = ret_sc->count;
                        obj_build_all(&ds);
                        obj_register(dcname, &ds);
                        ASTNode *node = create_node(NODE_CONST, dcname, expr);
                        node->var_type = TYPE_OBJECT;
                        node->is_expr = 1;
                        node->is_default = 1;
                        emit(node);
                        for (int i = 0; i < ret_sc->count; i++) {
                            char df[MAX_IDENTIFIER_LEN*2+8], sf[MAX_IDENTIFIER_LEN*2+8];
                            obj_resolve_field(dcname, ret_sc->fields[i].name, df, sizeof(df));
                            char stage_base[MAX_IDENTIFIER_LEN*2+16];
                            snprintf(stage_base, sizeof(stage_base), "__ltng_oret_%s", call_cname);
                            obj_resolve_field(stage_base, ret_sc->fields[i].name, sf, sizeof(sf));
                            ASTNode *sn = create_node(NODE_SET, df, sf);
                            sn->var_type = TYPE_STRING;
                            sn->is_expr = 1;
                            sn->is_variable = 1;
                            emit(sn);
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
            VarType expr_type = collect_expression(lexer, table, expr, sizeof(expr), 0, 0, NULL, NULL, NULL, NULL);
            symbol_table_add_locked(table, mangled, expr_type);
            symbol_table_set_const(table, mangled, 1);
            ASTNode *node = create_node(NODE_CONST, mangled, expr);
            node->var_type = expr_type;
            node->is_expr = 1;
            emit(node);
            return;
        }
        Token typeof_val;
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


static void token_append_source(char *out, int *pos, int out_size, Token tok) {
    if (*pos >= out_size - 2) return;
    int n = 0;
    switch (tok.type) {
        case TOKEN_STRING:
            n = snprintf(out + *pos, out_size - *pos, "\"");
            if (n > 0) *pos += n;
            for (const char *p = tok.value; *p && *pos < out_size - 4; p++) {
                if (*p == '\\') { out[(*pos)++] = '\\'; out[(*pos)++] = '\\'; }
                else if (*p == '"') { out[(*pos)++] = '\\'; out[(*pos)++] = '"'; }
                else if (*p == '\n') { out[(*pos)++] = '\\'; out[(*pos)++] = 'n'; }
                else if (*p == '\t') { out[(*pos)++] = '\\'; out[(*pos)++] = 't'; }
                else out[(*pos)++] = *p;
            }
            n = snprintf(out + *pos, out_size - *pos, "\"");
            if (n > 0) *pos += n;
            break;
        case TOKEN_CHAR:
            n = snprintf(out + *pos, out_size - *pos, "'%s'", tok.value);
            if (n > 0) *pos += n;
            break;
        default:
            if (tok.value[0]) {
                n = snprintf(out + *pos, out_size - *pos, "%s", tok.value);
            } else {
                /* fallback by type */
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
                n = snprintf(out + *pos, out_size - *pos, "%s", sym);
            }
            if (n > 0) *pos += n;
            break;
    }
}

static const char *type_name_str(VarType t) {
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
static int check_set_type_immutable(SymbolTable *table, const char *mangled,
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

static void parse_set(Lexer *lexer, SymbolTable *table) {
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

    char mangled[MAX_IDENTIFIER_LEN];
    mangle_name(name.value, mangled, MAX_IDENTIFIER_LEN);

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
        char index_exprs[32][MAX_STRING_LEN];
        int index_count = 0;
        /* op_tok is first '[' already consumed */
        while (1) {
            if (index_count >= 32) {
                error_state_add(parser_errors, op_tok.line, op_tok.col,
                    "Too many array dimensions (max 32)");
                skip_to_newline(lexer);
                return;
            }
            collect_expression(lexer, table, index_exprs[index_count], MAX_STRING_LEN, 1, 0, NULL, NULL, NULL, NULL);
            index_count++;
            Token rb = lexer_next(lexer);
            if (rb.type != TOKEN_RBRACKET) {
                error_state_add(parser_errors, rb.line, rb.col,
                    "Expected ']' after array index");
                skip_to_newline(lexer);
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
            return;
        }
        /* Parse RHS expression */
        char rhs_expr[MAX_STRING_LEN];
        VarType rhs_type = collect_expression(lexer, table, rhs_expr, MAX_STRING_LEN, 0, 0, NULL, NULL, NULL, NULL);
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
                return;
            }
            char clenname[MAX_IDENTIFIER_LEN], carrname[MAX_IDENTIFIER_LEN], cidx[MAX_STRING_LEN];
            snprintf(clenname, sizeof(clenname), "%s_len", mangled);
            snprintf(carrname, sizeof(carrname), "%s_data", mangled);
            int cpos = snprintf(cidx, sizeof(cidx), "({ ");
            for (int d = 0; d < index_count; d++) {
                if (ccount == 1)
                    cpos += snprintf(cidx + cpos, sizeof(cidx) - cpos,
                        "int __ci%d = (int)(%s); __ci%d = __ci%d > 0 ? __ci%d - 1 : __ci%d + (%s); if (__ci%d < 0 || __ci%d >= %s) __ci%d = %s; ",
                        d, index_exprs[d], d, d, d, d, clenname, d, d, clenname, d, clenname);
                else
                    cpos += snprintf(cidx + cpos, sizeof(cidx) - cpos,
                        "int __ci%d = (int)(%s); __ci%d = __ci%d > 0 ? __ci%d - 1 : __ci%d + (%d); ",
                        d, index_exprs[d], d, d, d, d, cdims[d]);
            }
            cpos += snprintf(cidx + cpos, sizeof(cidx) - cpos, "int __cidx = __ci0");
            for (int d = 1; d < index_count; d++)
                cpos += snprintf(cidx + cpos, sizeof(cidx) - cpos, "; __cidx = __cidx * %d + __ci%d", cdims[d], d);
            snprintf(cidx + cpos, sizeof(cidx) - cpos, "; __cidx; })");
            if (!chave) free(cdims);

            char curexpr[MAX_STRING_LEN], combined[MAX_STRING_LEN];
            if (cet == TYPE_INT) {
                snprintf(curexpr, sizeof(curexpr),
                    "({ const char *__ltng_cur = _ltng_arr_get(%s, %s, %s); atoll(__ltng_cur ? __ltng_cur : \"\"); })",
                    carrname, clenname, cidx);
            } else if (cet == TYPE_FLOAT) {
                snprintf(curexpr, sizeof(curexpr),
                    "({ const char *__ltng_cur = _ltng_arr_get(%s, %s, %s); atof(__ltng_cur ? __ltng_cur : \"\"); })",
                    carrname, clenname, cidx);
            } else if (cet == TYPE_BOOL) {
                snprintf(curexpr, sizeof(curexpr),
                    "({ const char *__ltng_cur = _ltng_arr_get(%s, %s, %s); (__ltng_cur && (__ltng_cur[0]=='t' || __ltng_cur[0]=='T' || __ltng_cur[0]=='1')) ? 1 : 0; })",
                    carrname, clenname, cidx);
            } else {
                snprintf(curexpr, sizeof(curexpr), "_ltng_arr_get(%s, %s, %s)", carrname, clenname, cidx);
            }
            if (cet == TYPE_STRING && compound_op == '+') {
                snprintf(combined, sizeof(combined), "_ltng_add_sn(%s, %s)", curexpr, rhs_expr);
            } else if (compound_op == '/' || compound_op == '%') {
                if (cet == TYPE_FLOAT && compound_op == '%')
                    snprintf(combined, sizeof(combined),
                        "fmod(%s, (%s) == 0.0 ? 1.0 : (%s))", curexpr, rhs_expr, rhs_expr);
                else if (cet == TYPE_FLOAT)
                    snprintf(combined, sizeof(combined),
                        "((%s) == 0.0 ? 0.0 : (%s) / (%s))", rhs_expr, curexpr, rhs_expr);
                else if (compound_op == '%')
                    snprintf(combined, sizeof(combined),
                        "((%s) == 0 ? 0 : ((long long)(%s) %% (long long)(%s)))", rhs_expr, curexpr, rhs_expr);
                else
                    snprintf(combined, sizeof(combined),
                        "((%s) == 0 ? 0 : ((long long)(%s) / (long long)(%s)))", rhs_expr, curexpr, rhs_expr);
            } else {
                snprintf(combined, sizeof(combined), "(%s %c %s)", curexpr, compound_op, rhs_expr);
            }
            snprintf(rhs_expr, sizeof(rhs_expr), "%s", combined);
        }
        /* Homogeneous arrays keep their inferred element type: a single element
           assignment coerces the value instead of de-typing every element. Dynamic
           (untyped) arrays keep their raw value semantics. (Step 4) */
        {
            VarType cur_et = symbol_table_get_array_elem_type(table, mangled);
            if (cur_et == TYPE_INT || cur_et == TYPE_FLOAT || cur_et == TYPE_BOOL) {
                char coerced[MAX_STRING_LEN];
                int rhs_is_text = (rhs_type == TYPE_STRING || rhs_type == TYPE_CHAR ||
                                   rhs_type == TYPE_OBJECT || rhs_type == TYPE_ARRAY ||
                                   rhs_type == TYPE_NULL || rhs_type == TYPE_VOID);
                if (cur_et == TYPE_INT) {
                    if (rhs_is_text)
                        snprintf(coerced, sizeof(coerced),
                                 "({ const char *__ltng_rhs = (%s); atoll(__ltng_rhs ? __ltng_rhs : \"\"); })", rhs_expr);
                    else
                        snprintf(coerced, sizeof(coerced), "(long long)(%s)", rhs_expr);
                } else if (cur_et == TYPE_FLOAT) {
                    if (rhs_is_text)
                        snprintf(coerced, sizeof(coerced),
                                 "({ const char *__ltng_rhs = (%s); atof(__ltng_rhs ? __ltng_rhs : \"\"); })", rhs_expr);
                    else
                        snprintf(coerced, sizeof(coerced), "(double)(%s)", rhs_expr);
                } else { /* TYPE_BOOL */
                    if (rhs_is_text)
                        snprintf(coerced, sizeof(coerced),
                                 "({ const char *__ltng_rhs = (%s); (__ltng_rhs && (__ltng_rhs[0]=='t' || __ltng_rhs[0]=='T' || __ltng_rhs[0]=='1')) ? 1 : 0; })", rhs_expr);
                    else
                        snprintf(coerced, sizeof(coerced), "((%s) ? 1 : 0)", rhs_expr);
                }
                snprintf(rhs_expr, sizeof(rhs_expr), "%s", coerced);
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
        char lenname[MAX_IDENTIFIER_LEN];
        snprintf(lenname, sizeof(lenname), "%s_len", mangled);
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
            return;
        }

        /* Build index computation C code into array_index field */
        char idx_code[MAX_STRING_LEN];
        int ipos = 0;
        ipos += snprintf(idx_code + ipos, sizeof(idx_code) - ipos, "({ ");
        for (int d = 0; d < index_count; d++) {
            if (dim_count == 1) {
                ipos += snprintf(idx_code + ipos, sizeof(idx_code) - ipos,
                    "int __i%d = (int)(%s); __i%d = __i%d > 0 ? __i%d - 1 : __i%d + (%s); if (__i%d < 0 || __i%d >= %s) __i%d = %s; ",
                    d, index_exprs[d], d, d, d, d, lenname, d, d, lenname, d, lenname);
            } else {
                ipos += snprintf(idx_code + ipos, sizeof(idx_code) - ipos,
                    "int __i%d = (int)(%s); __i%d = __i%d > 0 ? __i%d - 1 : __i%d + (%d); ",
                    d, index_exprs[d], d, d, d, d, dims[d]);
            }
        }
        ipos += snprintf(idx_code + ipos, sizeof(idx_code) - ipos, "int __idx = __i0");
        for (int d = 1; d < index_count; d++) {
            ipos += snprintf(idx_code + ipos, sizeof(idx_code) - ipos,
                "; __idx = __idx * %d + __i%d", dims[d], d);
        }
        ipos += snprintf(idx_code + ipos, sizeof(idx_code) - ipos, "; __idx; })");
        if (!have_dims) free(dims);

        ASTNode *node = create_node(NODE_ARRAY_SET, cname, rhs_expr);
        strncpy(node->array_index, idx_code, MAX_STRING_LEN - 1);
        node->array_index[MAX_STRING_LEN - 1] = '\0';
        node->var_type = assign_type;
        node->is_expr = 1;
        emit(node);
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
        char expr_rhs_src[MAX_STRING_LEN];
        int rpos = 0;
        expr_rhs_src[0] = '\0';
        while (lexer->current < lexer->count) {
            Token rt = lexer->tokens[lexer->current];
            if (rt.type == TOKEN_NEWLINE || rt.type == TOKEN_EOF) break;
            if (rpos > 0 && rpos < MAX_STRING_LEN - 1) expr_rhs_src[rpos++] = ' ';
            token_append_source(expr_rhs_src, &rpos, MAX_STRING_LEN, rt);
            lexer->current++;
            if (rpos >= MAX_STRING_LEN - 2) break;
        }
        expr_rhs_src[rpos] = '\0';
        char full_expr[MAX_STRING_LEN];
        snprintf(full_expr, sizeof(full_expr), "%s %c (%s)", name.value, opchar, expr_rhs_src);
        char expr_out[MAX_STRING_LEN];
        VarType etype = parse_expression_from_string(full_expr, table, expr_out, sizeof(expr_out));
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
            char sm[MAX_IDENTIFIER_LEN];
            mangle_name(src_tok.value, sm, MAX_IDENTIFIER_LEN);
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
                        char start_expr[MAX_STRING_LEN];
                        collect_expression(lexer, table, start_expr, sizeof(start_expr), 1, 1, NULL, NULL, NULL, NULL);
                        if (lexer_peek(lexer).type != TOKEN_COMMA) {
                            error_state_add(parser_errors, lexer_peek(lexer).line,
                                lexer_peek(lexer).col, "Expected ',' after slice start");
                            skip_to_newline(lexer);
                            return;
                        }
                        lexer_next(lexer); /* consume , */
                        char end_expr[MAX_STRING_LEN];
                        collect_expression(lexer, table, end_expr, sizeof(end_expr), 1, 0, NULL, NULL, NULL, NULL);
                        if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                            error_state_add(parser_errors, lexer_peek(lexer).line,
                                lexer_peek(lexer).col, "Expected ')' after slice end");
                            skip_to_newline(lexer);
                            return;
                        }
                        lexer_next(lexer); /* consume ) */
                        const char *scname = symbol_table_get_cname(table, sm);
                        symbol_table_set_array_elem_type(table, mangled, symbol_table_get_array_elem_type(table, sm));
                        char slrange[MAX_STRING_LEN];
                        snprintf(slrange, sizeof(slrange), "%s,%s", start_expr, end_expr);
                        ASTNode *sl = create_node(NODE_ARRAY_SLICE, (char *)cname, (char *)scname);
                        strncpy(sl->array_index, slrange, MAX_STRING_LEN - 1);
                        sl->array_index[MAX_STRING_LEN - 1] = '\0';
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
            char fval[MAX_STRING_LEN];
            if (fn && fn->capture_count > 0) {
                char expr[MAX_STRING_LEN];
                snprintf(expr, sizeof(expr), "\"%s\"", fn->name);
                for (int ci = 0; ci < fn->capture_count; ci++) {
                    char tmp[MAX_STRING_LEN];
                    snprintf(tmp, sizeof(tmp), "_ltng_add_sn(_ltng_add_sn(%s,\"\\001\"),_ltng_ptr_to_str((void*)%s))", expr, fn->captures[ci]);
                    strncpy(expr, tmp, sizeof(expr)-1); expr[sizeof(expr)-1]=0;
                }
                snprintf(fval, sizeof(fval), "%s", expr);
            } else {
                snprintf(fval, sizeof(fval), "\"%s\"", fn ? fn->name : "");
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
            char mangled_ref[MAX_IDENTIFIER_LEN]; mangle_name(next.value, mangled_ref, sizeof(mangled_ref));
            int is_var = symbol_table_has(table, mangled_ref) || symbol_table_has(table, next.value);
            char ref_cname[MAX_IDENTIFIER_LEN]; int ref_pc = -1;
            if (!is_var && resolve_func_ref_info(table, next.value, ref_cname, sizeof(ref_cname), &ref_pc)) {
                lexer_next(lexer);
                if (!check_set_type_immutable(table, mangled, TYPE_STRING, set_tok.line, set_tok.col, lexer))
                    return;
                symbol_table_update(table, mangled, TYPE_STRING);
                symbol_table_set_func_ref(table, mangled, ref_cname, ref_pc);
                char fval[MAX_STRING_LEN];
                snprintf(fval, sizeof(fval), "\"%s\"", ref_cname);
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
            char expr[MAX_STRING_LEN];
            /* Skip try_parse_sole_call_expr for array.push/pop */
            if (!(lexer->current + 2 < lexer->count &&
                  lexer->tokens[lexer->current + 1].type == TOKEN_DOT &&
                  lexer->tokens[lexer->current + 2].type == TOKEN_IDENT &&
                  (strcmp(lexer->tokens[lexer->current + 2].value, "push") == 0 ||
                   strcmp(lexer->tokens[lexer->current + 2].value, "pop") == 0))) {
                VarType call_type = TYPE_STRING;
                char call_cname[MAX_IDENTIFIER_LEN] = "";
                if (try_parse_sole_call_expr(lexer, table, expr, sizeof(expr), 1, &call_type, call_cname, sizeof(call_cname))) {
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
                        
                        ObjSchema ds; memset(&ds, 0, sizeof(ds));
                        strncpy(ds.var, dcname, MAX_IDENTIFIER_LEN - 1);
                        for (int i = 0; i < ret_sc->count && i < MAX_OBJ_FIELDS; i++) ds.fields[i] = ret_sc->fields[i];
                        ds.count = ret_sc->count;
                        obj_build_all(&ds);
                        obj_register(dcname, &ds);
                        ASTNode *node = create_node(NODE_SET, dcname, expr);
                        node->var_type = TYPE_OBJECT;
                        node->is_expr = 1;
                        node->is_default = 1;
                        emit(node);
                        for (int i = 0; i < ret_sc->count; i++) {
                            char df[MAX_IDENTIFIER_LEN*2+8], sf[MAX_IDENTIFIER_LEN*2+8];
                            obj_resolve_field(dcname, ret_sc->fields[i].name, df, sizeof(df));
                            char stage_base[MAX_IDENTIFIER_LEN*2+16];
                            snprintf(stage_base, sizeof(stage_base), "__ltng_oret_%s", call_cname);
                            obj_resolve_field(stage_base, ret_sc->fields[i].name, sf, sizeof(sf));
                            ASTNode *sn = create_node(NODE_SET, df, sf);
                            sn->var_type = TYPE_STRING;
                            sn->is_expr = 1;
                            sn->is_variable = 1;
                            emit(sn);
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
            char expr[MAX_STRING_LEN];
            VarType expr_type = collect_expression(lexer, table, expr, sizeof(expr), 0, 0, NULL, NULL, NULL, NULL);
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

        Token typeof_val;
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
            char inner_raw[MAX_STRING_LEN] = {0};
            int irpos = 0, depth = 1;
            int arg_start = lexer->current;
            while (lexer->current < lexer->count && depth > 0) {
                Token itok = lexer_peek(lexer);
                if (itok.type == TOKEN_LPAREN) depth++;
                else if (itok.type == TOKEN_RPAREN) { depth--; if (depth == 0) break; }
                else if (itok.type == TOKEN_NEWLINE || itok.type == TOKEN_EOF) break;
                itok = lexer_next(lexer);
                token_append_source(inner_raw, &irpos, MAX_STRING_LEN, itok);
            }
            while (irpos > 0 && (inner_raw[irpos-1] == ' ' || inner_raw[irpos-1] == '\t')) irpos--;
            inner_raw[irpos] = '\0';
            int arg_end = lexer->current;
            if (lexer->current < lexer->count && lexer_peek(lexer).type == TOKEN_RPAREN) {
                lexer_next(lexer); /* consume ) */
            }
            Token arg = lexer->tokens[arg_start];
            int arg_tok_count = arg_end - arg_start;
            char expr[MAX_STRING_LEN] = "0";
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
                    if (arg.type == TOKEN_STRING) snprintf(expr, sizeof(expr), "(long long)atoll(\"%s\")", arg.value);
                    else if (arg.type == TOKEN_INT_LIT || arg.type == TOKEN_FLOAT_LIT) snprintf(expr, sizeof(expr), "%lld", (long long)atoll(arg.value));
                    else if (arg.type == TOKEN_CHAR) snprintf(expr, sizeof(expr), "%lld", (long long)(unsigned char)arg.value[0]);
                    else if (arg.type == TOKEN_IDENT && is_bool_value(arg.value)) snprintf(expr, sizeof(expr), "%d", (strcmp(arg.value,"true")==0||strcmp(arg.value,"t")==0)?1:0);
                    else { char m[MAX_IDENTIFIER_LEN]; mangle_name(arg.value,m,MAX_IDENTIFIER_LEN); if(symbol_table_has(table,m)) snprintf(expr,sizeof(expr),"(long long)atoll(%s)", symbol_table_get_cname(table,m)); else snprintf(expr,sizeof(expr),"0"); }
                } else if (strcmp(conv.value, "tofloat") == 0 || strcmp(conv.value, "float") == 0) {
                    expr_type = TYPE_FLOAT;
                    if (arg.type == TOKEN_STRING) snprintf(expr, sizeof(expr), "(double)atof(\"%s\")", arg.value);
                    else if (arg.type == TOKEN_INT_LIT || arg.type == TOKEN_FLOAT_LIT) snprintf(expr, sizeof(expr), "%.17g", atof(arg.value));
                    else if (arg.type == TOKEN_IDENT && is_bool_value(arg.value)) snprintf(expr, sizeof(expr), "%d.0", (strcmp(arg.value,"true")==0||strcmp(arg.value,"t")==0)?1:0);
                    else { char m[MAX_IDENTIFIER_LEN]; mangle_name(arg.value,m,MAX_IDENTIFIER_LEN); if(symbol_table_has(table,m)) snprintf(expr,sizeof(expr),"(double)atof(%s)", symbol_table_get_cname(table,m)); else snprintf(expr,sizeof(expr),"0.0"); }
                } else if (strcmp(conv.value, "tobool") == 0 || strcmp(conv.value, "bool") == 0) {
                    if (arg.type == TOKEN_IDENT && !is_bool_value(arg.value)) {
                        handled = 0;
                    } else {
                        expr_type = TYPE_BOOL;
                        if (arg.type == TOKEN_STRING) {
                            int is_false = (arg.value[0]=='\0' || strcmp(arg.value,"0")==0 || strcmp(arg.value,"false")==0 || strcmp(arg.value,"f")==0);
                            snprintf(expr,sizeof(expr),"%s", is_false?"false":"true");
                        } else if (arg.type == TOKEN_INT_LIT || arg.type == TOKEN_FLOAT_LIT) {
                            snprintf(expr,sizeof(expr),"%s", (atof(arg.value)!=0.0)?"true":"false");
                        } else if (arg.type == TOKEN_IDENT && is_bool_value(arg.value)) {
                            snprintf(expr,sizeof(expr),"%s", (strcmp(arg.value,"true")==0||strcmp(arg.value,"t")==0)?"true":"false");
                        } else {
                            snprintf(expr,sizeof(expr),"false");
                        }
                    }
                } else if (strcmp(conv.value, "tostring") == 0 || strcmp(conv.value, "string") == 0) {
                    expr_type = TYPE_STRING;
                    if (arg.type == TOKEN_STRING || arg.type == TOKEN_CHAR) {
                        int p = 0;
                        expr[p++] = '"';
                        for (const char *sp = arg.value; *sp && p < (int)sizeof(expr)-3; sp++) {
                            unsigned char c = (unsigned char)*sp;
                            if (c=='\\') { expr[p++]='\\'; expr[p++]='\\'; }
                            else if (c=='"') { expr[p++]='\\'; expr[p++]='"'; }
                            else if (c=='\n') { expr[p++]='\\'; expr[p++]='n'; }
                            else if (c=='\t') { expr[p++]='\\'; expr[p++]='t'; }
                            else if (c=='\r') { expr[p++]='\\'; expr[p++]='r'; }
                            else expr[p++] = c;
                        }
                        expr[p++] = '"'; expr[p] = '\0';
                    } else if (arg.type == TOKEN_INT_LIT || arg.type == TOKEN_FLOAT_LIT) snprintf(expr,sizeof(expr),"\"%s\"", arg.value);
                    else if (arg.type == TOKEN_IDENT && is_bool_value(arg.value)) snprintf(expr,sizeof(expr),"\"%s\"", (strcmp(arg.value,"true")==0||strcmp(arg.value,"t")==0)?"true":"false");
                    else { char m[MAX_IDENTIFIER_LEN]; mangle_name(arg.value,m,MAX_IDENTIFIER_LEN); if(symbol_table_has(table,m)) snprintf(expr,sizeof(expr),"%s", symbol_table_get_cname(table,m)); else snprintf(expr,sizeof(expr),"\"\""); }
                } else { /* tochar/char */
                    expr_type = TYPE_CHAR;
                    if (arg.type == TOKEN_STRING) {
                        unsigned char ch = (unsigned char)(arg.value[0]?arg.value[0]:0);
                        if (arg.value[0]=='\\' && arg.value[1]) snprintf(expr,sizeof(expr),"\"\\%c\"", arg.value[1]);
                        else if (ch=='\n') snprintf(expr,sizeof(expr),"\"\\n\"");
                        else if (ch=='\t') snprintf(expr,sizeof(expr),"\"\\t\"");
                        else if (ch=='\r') snprintf(expr,sizeof(expr),"\"\\r\"");
                        else if (ch=='\\') snprintf(expr,sizeof(expr),"\"\\\\\"");
                        else if (ch=='\"') snprintf(expr,sizeof(expr),"\"\\\"\"");
                        else snprintf(expr,sizeof(expr),"\"%c\"", ch);
                    } else if (arg.type == TOKEN_INT_LIT) {
                        int iv = (int)atoi(arg.value);
                        if (iv=='\n') snprintf(expr,sizeof(expr),"\"\\n\"");
                        else if (iv=='\t') snprintf(expr,sizeof(expr),"\"\\t\"");
                        else if (iv=='\r') snprintf(expr,sizeof(expr),"\"\\r\"");
                        else if (iv=='\\') snprintf(expr,sizeof(expr),"\"\\\\\"");
                        else if (iv=='\"') snprintf(expr,sizeof(expr),"\"\\\"\"");
                        else if (iv>=32 && iv<=126) snprintf(expr,sizeof(expr),"\"%c\"", (char)iv);
                        else snprintf(expr,sizeof(expr), "({ char *_tc=_ltng_ds_new(\"\"); int _cv=%d; if(_cv=='\\n') _ltng_ds_set(&_tc,\"\\n\"); else if(_cv=='\\t') _ltng_ds_set(&_tc,\"\\t\"); else if(_cv=='\\r') _ltng_ds_set(&_tc,\"\\r\"); else if(_cv=='\\\\') _ltng_ds_set(&_tc,\"\\\\\"); else if(_cv=='\\\"') _ltng_ds_set(&_tc,\"\\\"\"); else _ltng_ds_setfmt(&_tc,\"%%c\",(char)_cv); char *_r=_ltng_as(_tc); free(_tc); _r; })", iv);
                    } else if (arg.type == TOKEN_CHAR) {
                        /* token value for char literal is the char content (possibly escaped like n or \\) */
                        if (arg.value[0]=='\\' && arg.value[1]) snprintf(expr,sizeof(expr),"\"\\%c\"", arg.value[1]);
                        else if (arg.value[0]=='\n') snprintf(expr,sizeof(expr),"\"\\n\"");
                        else if (arg.value[0]=='\t') snprintf(expr,sizeof(expr),"\"\\t\"");
                        else if (arg.value[0]=='\r') snprintf(expr,sizeof(expr),"\"\\r\"");
                        else if (arg.value[0]=='\\') snprintf(expr,sizeof(expr),"\"\\\\\"");
                        else if (arg.value[0]=='\"') snprintf(expr,sizeof(expr),"\"\\\"\"");
                        else snprintf(expr,sizeof(expr),"\"%c\"", arg.value[0]?arg.value[0]:'\0');
                    } else { char m[MAX_IDENTIFIER_LEN]; mangle_name(arg.value,m,MAX_IDENTIFIER_LEN); if(symbol_table_has(table,m)) { const char *cn=symbol_table_get_cname(table,m); snprintf(expr,sizeof(expr),"(_ltng_is_num_str(%s)?((char[]){(char)atoll(%s),0}):((char[]){(%s)[0]? (%s)[0]:0,0}))",cn,cn,cn,cn);} else snprintf(expr,sizeof(expr),"\"\""); }
                }
            }
            if (!handled) {
                char conv_expr_src[MAX_STRING_LEN];
                snprintf(conv_expr_src, sizeof(conv_expr_src), "%s(%s)", conv.value, inner_raw);
                char compiled[MAX_STRING_LEN]; compiled[0] = '\0';
                VarType ctype = parse_expression_from_string(conv_expr_src, table, compiled, sizeof(compiled));
                if (compiled[0] == '\0') snprintf(compiled, sizeof(compiled), "0");
                if (strcmp(conv.value, "toint") == 0 || strcmp(conv.value, "int") == 0) {
                    expr_type = TYPE_INT;
                    if (ctype == TYPE_INT || ctype == TYPE_BOOL) snprintf(expr,sizeof(expr),"(long long)(%s)", compiled);
                    else snprintf(expr,sizeof(expr),"(long long)atoll(%s)", compiled);
                } else if (strcmp(conv.value, "tofloat") == 0 || strcmp(conv.value, "float") == 0) {
                    expr_type = TYPE_FLOAT;
                    if (ctype == TYPE_FLOAT) snprintf(expr,sizeof(expr),"(double)(%s)", compiled);
                    else if (ctype == TYPE_INT || ctype == TYPE_BOOL) snprintf(expr,sizeof(expr),"(double)(%s)", compiled);
                    else snprintf(expr,sizeof(expr),"(double)atof(%s)", compiled);
                } else if (strcmp(conv.value, "tobool") == 0 || strcmp(conv.value, "bool") == 0) {
                    expr_type = TYPE_BOOL;
                    if (ctype == TYPE_BOOL) snprintf(expr,sizeof(expr),"%s", compiled);
                    else snprintf(expr,sizeof(expr),"(((%s)[0]!='\\0'&&strcmp(%s,\"0\")!=0&&strcmp(%s,\"false\")!=0&&strcmp(%s,\"f\")!=0)?\"true\":\"false\")", compiled, compiled, compiled, compiled);
                } else if (strcmp(conv.value, "tochar") == 0 || strcmp(conv.value, "char") == 0) {
                    expr_type = TYPE_CHAR;
                    snprintf(expr,sizeof(expr),"%s", compiled);
                } else {
                    expr_type = TYPE_STRING;
                    snprintf(expr,sizeof(expr),"%s", compiled);
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
        char expr[MAX_STRING_LEN];
        VarType expr_type = collect_expression(lexer, table, expr, sizeof(expr), 0, 0, NULL, NULL, NULL, NULL);
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
            char expr[MAX_STRING_LEN];
            /* Skip try_parse_sole_call_expr for array.push/pop - let
               collect_expression handle them as inline C expressions */
            if (!(lexer->current + 2 < lexer->count &&
                  lexer->tokens[lexer->current + 1].type == TOKEN_DOT &&
                  lexer->tokens[lexer->current + 2].type == TOKEN_IDENT &&
                  (strcmp(lexer->tokens[lexer->current + 2].value, "push") == 0 ||
                   strcmp(lexer->tokens[lexer->current + 2].value, "pop") == 0))) {
                VarType call_type = TYPE_STRING;
                char call_cname[MAX_IDENTIFIER_LEN] = "";
                if (try_parse_sole_call_expr(lexer, table, expr, sizeof(expr), 1, &call_type, call_cname, sizeof(call_cname))) {
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
                        
                        ObjSchema ds; memset(&ds, 0, sizeof(ds));
                        strncpy(ds.var, dcname, MAX_IDENTIFIER_LEN - 1);
                        for (int i = 0; i < ret_sc->count && i < MAX_OBJ_FIELDS; i++) ds.fields[i] = ret_sc->fields[i];
                        ds.count = ret_sc->count;
                        obj_build_all(&ds);
                        obj_register(dcname, &ds);
                        ASTNode *node = create_node(NODE_SET, dcname, expr);
                        node->var_type = TYPE_OBJECT;
                        node->is_expr = 1;
                        node->is_default = 1;
                        emit(node);
                        for (int i = 0; i < ret_sc->count; i++) {
                            char df[MAX_IDENTIFIER_LEN*2+8], sf[MAX_IDENTIFIER_LEN*2+8];
                            obj_resolve_field(dcname, ret_sc->fields[i].name, df, sizeof(df));
                            char stage_base[MAX_IDENTIFIER_LEN*2+16];
                            snprintf(stage_base, sizeof(stage_base), "__ltng_oret_%s", call_cname);
                            obj_resolve_field(stage_base, ret_sc->fields[i].name, sf, sizeof(sf));
                            ASTNode *sn = create_node(NODE_SET, df, sf);
                            sn->var_type = TYPE_STRING;
                            sn->is_expr = 1;
                            sn->is_variable = 1;
                            emit(sn);
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
            VarType expr_type = collect_expression(lexer, table, expr, sizeof(expr), 0, 0, NULL, NULL, NULL, NULL);
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
        Token typeof_val;
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

static ASTNode *parse_switch(Lexer *lexer, SymbolTable *table) {
    Token kw = lexer_next(lexer);

    Token lp = lexer_next(lexer);
    if (lp.type != TOKEN_LPAREN) {
        error_state_add(parser_errors, lp.line, lp.col,
            "Expected '(' after switch");
        skip_to_newline(lexer);
        return NULL;
    }

    char cond[MAX_STRING_LEN];
    int csl=0,csc=0,cel=0,cec=0;
    VarType ctype = collect_expression(lexer, table, cond, sizeof(cond), 1, 0, &csl,&csc,&cel,&cec);

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
            char cexpr[MAX_STRING_LEN] = {0};
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
                    pos += snprintf(cexpr + pos, MAX_STRING_LEN - pos, "\"%s\"", ct.value);
                } else if (ct.type == TOKEN_CHAR) {
                    if (ct.value[0] != '\0') {
                        if (ctype == TYPE_STRING)
                            pos += snprintf(cexpr + pos, MAX_STRING_LEN - pos, "\"%s\"", ct.value);
                        else
                            pos += snprintf(cexpr + pos, MAX_STRING_LEN - pos, "'%s'", ct.value);
                    } else {
                        if (ctype == TYPE_STRING)
                            pos += snprintf(cexpr + pos, MAX_STRING_LEN - pos, "\"\"");
                        else
                            pos += snprintf(cexpr + pos, MAX_STRING_LEN - pos, "'\\0'");
                    }
                } else if (ct.type == TOKEN_IDENT) {
                    if (is_bool_value(ct.value)) {
                        pos += snprintf(cexpr + pos, MAX_STRING_LEN - pos, "%s",
                            (strcmp(ct.value, "true") == 0 || strcmp(ct.value, "t") == 0) ? "true" : "false");
                    } else {
                        char mangled[MAX_IDENTIFIER_LEN];
                        mangle_name(ct.value, mangled, MAX_IDENTIFIER_LEN);
                        if (symbol_table_has(table, mangled)) {
                            VarType vt = symbol_table_get(table, mangled);
                            if (vt == TYPE_STRING) pos += snprintf(cexpr + pos, MAX_STRING_LEN - pos, "%s", mangled);
                            else if (vt == TYPE_BOOL) pos += snprintf(cexpr + pos, MAX_STRING_LEN - pos, "(%s[0]=='t'||%s[0]=='T')?1:0", mangled, mangled);
                            else if (vt == TYPE_INT) pos += snprintf(cexpr + pos, MAX_STRING_LEN - pos, "atoll(%s)", mangled);
                            else if (vt == TYPE_FLOAT) pos += snprintf(cexpr + pos, MAX_STRING_LEN - pos, "atof(%s)", mangled);
                            else pos += snprintf(cexpr + pos, MAX_STRING_LEN - pos, "atoll(%s)", mangled);
                        } else if (is_c_reserved(ct.value)) {
                            pos += snprintf(cexpr + pos, MAX_STRING_LEN - pos, "%s", ct.value);
                        } else {
                            pos += snprintf(cexpr + pos, MAX_STRING_LEN - pos, "%s", ct.value);
                        }
                    }
                } else if (ct.type == TOKEN_INT_LIT || ct.type == TOKEN_FLOAT_LIT) {
                    pos += snprintf(cexpr + pos, MAX_STRING_LEN - pos, "%s", ct.value);
                }                 else if (ct.type == TOKEN_PLUS) pos += snprintf(cexpr + pos, MAX_STRING_LEN - pos, "+");
                else if (ct.type == TOKEN_MINUS) pos += snprintf(cexpr + pos, MAX_STRING_LEN - pos, "-");
                else if (ct.type == TOKEN_STAR) pos += snprintf(cexpr + pos, MAX_STRING_LEN - pos, "*");
                else if (ct.type == TOKEN_SLASH) pos += snprintf(cexpr + pos, MAX_STRING_LEN - pos, "/");
                else if (ct.type == TOKEN_PERCENT) pos += snprintf(cexpr + pos, MAX_STRING_LEN - pos, "%%");
                else if (ct.type == TOKEN_EQ_EQ) pos += snprintf(cexpr + pos, MAX_STRING_LEN - pos, "==");
                else if (ct.type == TOKEN_BANG_EQ) pos += snprintf(cexpr + pos, MAX_STRING_LEN - pos, "!=");
                else if (ct.type == TOKEN_LT_EQ) pos += snprintf(cexpr + pos, MAX_STRING_LEN - pos, "<=");
                else if (ct.type == TOKEN_GT_EQ) pos += snprintf(cexpr + pos, MAX_STRING_LEN - pos, ">=");
                else if (ct.type == TOKEN_LT) pos += snprintf(cexpr + pos, MAX_STRING_LEN - pos, "<");
                else if (ct.type == TOKEN_GT) pos += snprintf(cexpr + pos, MAX_STRING_LEN - pos, ">");
                else if (ct.type == TOKEN_AMP_AMP) pos += snprintf(cexpr + pos, MAX_STRING_LEN - pos, "&&");
                else if (ct.type == TOKEN_PIPE_PIPE) pos += snprintf(cexpr + pos, MAX_STRING_LEN - pos, "||");
                else if (ct.type == TOKEN_BANG) pos += snprintf(cexpr + pos, MAX_STRING_LEN - pos, "!");
                else if (ct.type == TOKEN_LPAREN) pos += snprintf(cexpr + pos, MAX_STRING_LEN - pos, "(");
                else if (ct.type == TOKEN_RPAREN) pos += snprintf(cexpr + pos, MAX_STRING_LEN - pos, ")");

                else if (ct.type == TOKEN_EQUALS) pos += snprintf(cexpr + pos, MAX_STRING_LEN - pos, "=");
                else pos += snprintf(cexpr + pos, MAX_STRING_LEN - pos, "%s", ct.value);
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
                if (_hp || _hs) normalize_string_expr(cexpr, sizeof(cexpr), 0, table);
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



static ASTNode *parse_switch(Lexer *lexer, SymbolTable *table);

static ASTNode *parse_if_core(Lexer *lexer, SymbolTable *table);
static ASTNode *parse_switch(Lexer *lexer, SymbolTable *table);


static void transform_inc_dec_stmt(Lexer *lexer, SymbolTable *table) {
    Token t = lexer_peek(lexer);
    if (t.type == TOKEN_PLUS_PLUS) {
        lexer_next(lexer);
        Token name = lexer_next(lexer);
        if (!token_is_name(name)) { skip_to_newline(lexer); return; }
        /* try to be permissive */
        char mangled[MAX_IDENTIFIER_LEN];
        mangle_name(name.value, mangled, MAX_IDENTIFIER_LEN);
        if (symbol_table_is_const(table, mangled)) { skip_to_newline(lexer); return; }
        if (!symbol_table_has(table, mangled)) {
            /* add if missing? maybe not - but try to proceed */
        }
        char full_expr[MAX_STRING_LEN];
        snprintf(full_expr, sizeof(full_expr), "%s + 1", name.value);
        char expr_out[MAX_STRING_LEN];
        VarType etype = parse_expression_from_string(full_expr, table, expr_out, sizeof(expr_out));
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
        char mangled[MAX_IDENTIFIER_LEN];
        mangle_name(name.value, mangled, MAX_IDENTIFIER_LEN);
        if (symbol_table_is_const(table, mangled)) { skip_to_newline(lexer); return; }
        char full_expr[MAX_STRING_LEN];
        snprintf(full_expr, sizeof(full_expr), "%s - 1", name.value);
        char expr_out[MAX_STRING_LEN];
        VarType etype = parse_expression_from_string(full_expr, table, expr_out, sizeof(expr_out));
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
            char mangled[MAX_IDENTIFIER_LEN];
            mangle_name(name.value, mangled, MAX_IDENTIFIER_LEN);
            if (!symbol_table_has(table, mangled) || symbol_table_is_const(table, mangled)) { lexer->current = saved; return; }
            char full_expr[MAX_STRING_LEN];
            snprintf(full_expr, sizeof(full_expr), "%s + 1", name.value);
            char expr_out[MAX_STRING_LEN];
            VarType etype = parse_expression_from_string(full_expr, table, expr_out, sizeof(expr_out));
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
            char mangled[MAX_IDENTIFIER_LEN];
            mangle_name(name.value, mangled, MAX_IDENTIFIER_LEN);
            if (!symbol_table_has(table, mangled) || symbol_table_is_const(table, mangled)) { lexer->current = saved; return; }
            char full_expr[MAX_STRING_LEN];
            snprintf(full_expr, sizeof(full_expr), "%s - 1", name.value);
            char expr_out[MAX_STRING_LEN];
            VarType etype = parse_expression_from_string(full_expr, table, expr_out, sizeof(expr_out));
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

static void parse_statement(Lexer *lexer, SymbolTable *table);


static ASTNode *parse_while(Lexer *lexer, SymbolTable *table) {
    Token kw = lexer_next(lexer);

    Token lp = lexer_next(lexer);
    if (lp.type != TOKEN_LPAREN) {
        error_state_add(parser_errors, lp.line, lp.col,
            "Expected '(' after while");
        skip_to_newline(lexer);
        return NULL;
    }

    char cond[MAX_STRING_LEN];
    int csl=0,csc=0,cel=0,cec=0;
    cond[0] = '\0';
    VarType ctype = TYPE_INT;
    if (lexer->current + 1 < lexer->count) {
        Token _ct = lexer->tokens[lexer->current];
        Token _nt = lexer->tokens[lexer->current + 1];
        if ((_ct.type == TOKEN_IDENT || token_is_name(_ct)) && _nt.type == TOKEN_RPAREN &&
            !is_bool_value(_ct.value) && strcmp(_ct.value, "typeof") != 0) {
            char _cm[MAX_IDENTIFIER_LEN]; mangle_name(_ct.value, _cm, MAX_IDENTIFIER_LEN);
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
                    snprintf(cond, sizeof(cond), "_ltng_str_truthy(%s)", _cn ? _cn : _cm);
                    ctype = TYPE_STRING;
                }
            }
        }
    }
    if (cond[0] == '\0') {
        ctype = collect_expression(lexer, table, cond, sizeof(cond), 1, 0, &csl,&csc,&cel,&cec);
        if (ctype == TYPE_STRING || ctype == TYPE_CHAR || ctype == TYPE_OBJECT || ctype == TYPE_NULL) {
            char *_tc = _ltng_cfmt("_ltng_str_truthy(%s)", cond);
            if(_tc){ strncpy(cond, _tc, sizeof(cond)-1); cond[sizeof(cond)-1] = '\0'; free(_tc); }
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

static ASTNode *parse_do_while(Lexer *lexer, SymbolTable *table) {
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

    char cond[MAX_STRING_LEN];
    int csl=0,csc=0,cel=0,cec=0;
    cond[0] = '\0';
    VarType ctype = TYPE_INT;
    if (lexer->current + 1 < lexer->count) {
        Token _ct = lexer->tokens[lexer->current];
        Token _nt = lexer->tokens[lexer->current + 1];
        if ((_ct.type == TOKEN_IDENT || token_is_name(_ct)) && _nt.type == TOKEN_RPAREN &&
            !is_bool_value(_ct.value) && strcmp(_ct.value, "typeof") != 0) {
            char _cm[MAX_IDENTIFIER_LEN]; mangle_name(_ct.value, _cm, MAX_IDENTIFIER_LEN);
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
                    snprintf(cond, sizeof(cond), "_ltng_str_truthy(%s)", _cn ? _cn : _cm);
                    ctype = TYPE_STRING;
                }
            }
        }
    }
    if (cond[0] == '\0') {
        ctype = collect_expression(lexer, table, cond, sizeof(cond), 1, 0, &csl,&csc,&cel,&cec);
        if (ctype == TYPE_STRING || ctype == TYPE_CHAR || ctype == TYPE_OBJECT || ctype == TYPE_NULL) {
            char *_tc = _ltng_cfmt("_ltng_str_truthy(%s)", cond);
            if(_tc){ strncpy(cond, _tc, sizeof(cond)-1); cond[sizeof(cond)-1] = '\0'; free(_tc); }
        }
    }
    strncpy(node->value, cond, MAX_STRING_LEN - 1);
    node->value[MAX_STRING_LEN - 1] = '\0';
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


static ASTNode *parse_for(Lexer *lexer, SymbolTable *table) {
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
        char am[MAX_IDENTIFIER_LEN];
        mangle_name(arr_tok.value, am, MAX_IDENTIFIER_LEN);
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
        char mangled[MAX_IDENTIFIER_LEN];
        mangle_name(var_tok.value, mangled, MAX_IDENTIFIER_LEN);
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
        strncpy(node->for_var, var_tok.value, MAX_IDENTIFIER_LEN - 1);
        node->for_var[MAX_IDENTIFIER_LEN - 1] = '\0';
        strncpy(node->for_start, acname, MAX_STRING_LEN - 1);
        node->for_start[MAX_STRING_LEN - 1] = '\0';
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
    char start_expr[MAX_STRING_LEN];
    int ssl=0,ssc=0,sel=0,sec=0;
    VarType start_type = collect_expression(lexer, table, start_expr, sizeof(start_expr), 1, 0, &ssl,&ssc,&sel,&sec);

    Token to_tok = lexer_next(lexer);
    if (to_tok.type != TOKEN_TO) {
        error_state_add(parser_errors, to_tok.line, to_tok.col,
            "Expected 'to' in for loop");
        skip_to_newline(lexer);
        return NULL;
    }

    /* End expression - stop at step or ) */
    char end_expr[MAX_STRING_LEN];
    int esl=0,esc=0,eel=0,eec=0;
    VarType end_type = collect_expression(lexer, table, end_expr, sizeof(end_expr), 1, 0, &esl,&esc,&eel,&eec);

    /* Optional step */
    char step_expr[MAX_STRING_LEN] = "1";
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
        collect_expression(lexer, table, step_expr, sizeof(step_expr), 1, 0, &stsl,&stsc,&stel,&stec);
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
    char mangled[MAX_IDENTIFIER_LEN];
    mangle_name(var_tok.value, mangled, MAX_IDENTIFIER_LEN);
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
    strncpy(node->for_var, var_tok.value, MAX_IDENTIFIER_LEN - 1);
    node->for_var[MAX_IDENTIFIER_LEN - 1] = '\0';
    strncpy(node->for_start, start_expr, MAX_STRING_LEN - 1);
    node->for_start[MAX_STRING_LEN - 1] = '\0';
    strncpy(node->for_end, end_expr, MAX_STRING_LEN - 1);
    node->for_end[MAX_STRING_LEN - 1] = '\0';
    strncpy(node->for_step, step_expr, MAX_STRING_LEN - 1);
    node->for_step[MAX_STRING_LEN - 1] = '\0';
    node->for_step_op = step_op;
    g_break_depth++;
    node->body = parse_block(lexer, loop_scope, 1);
    g_break_depth--;
    free(loop_scope);
    (void)kw;
    return node;
}

static ASTNode *parse_else_chain(Lexer *lexer, SymbolTable *table) {
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

static ASTNode *parse_if_core(Lexer *lexer, SymbolTable *table) {
    Token kw = lexer_next(lexer);

    Token lp = lexer_next(lexer);
    if (lp.type != TOKEN_LPAREN) {
        error_state_add(parser_errors, lp.line, lp.col,
            "Expected '(' after if");
        skip_to_newline(lexer);
        return NULL;
    }

    char cond[MAX_STRING_LEN];
    int csl=0,csc=0,cel=0,cec=0;
    cond[0] = '\0';
    VarType ctype = TYPE_INT;
    if (lexer->current + 1 < lexer->count) {
        Token _ct = lexer->tokens[lexer->current];
        Token _nt = lexer->tokens[lexer->current + 1];
        if ((_ct.type == TOKEN_IDENT || token_is_name(_ct)) && _nt.type == TOKEN_RPAREN &&
            !is_bool_value(_ct.value) && strcmp(_ct.value, "typeof") != 0) {
            char _cm[MAX_IDENTIFIER_LEN]; mangle_name(_ct.value, _cm, MAX_IDENTIFIER_LEN);
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
                    snprintf(cond, sizeof(cond), "_ltng_str_truthy(%s)", _cn ? _cn : _cm);
                    ctype = TYPE_STRING;
                }
            }
        }
    }
    if (cond[0] == '\0') {
        ctype = collect_expression(lexer, table, cond, sizeof(cond), 1, 0, &csl,&csc,&cel,&cec);
        if (ctype == TYPE_STRING || ctype == TYPE_CHAR || ctype == TYPE_OBJECT || ctype == TYPE_NULL) {
            char *_tc = _ltng_cfmt("_ltng_str_truthy(%s)", cond);
            if(_tc){ strncpy(cond, _tc, sizeof(cond)-1); cond[sizeof(cond)-1] = '\0'; free(_tc); }
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

static ASTNode *parse_block(Lexer *lexer, SymbolTable *table, int is_control) {
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

static void skip_newlines(Lexer *lexer) {
    while (lexer_peek(lexer).type == TOKEN_NEWLINE) lexer_next(lexer);
}

static void build_catch_schema(const char *cname) {
    ObjSchema schema;
    memset(&schema, 0, sizeof(schema));
    static const char *field_names[] = {
        "type", "message", "trace", "line", "column", "function", "value"
    };
    static const VarType field_types[] = {
        TYPE_STRING, TYPE_STRING, TYPE_STRING, TYPE_INT, TYPE_INT, TYPE_STRING, TYPE_STRING
    };
    int n = (int)(sizeof(field_names) / sizeof(field_names[0]));
    if (n > MAX_OBJ_FIELDS) n = MAX_OBJ_FIELDS;
    for (int i = 0; i < n; i++) {
        strncpy(schema.fields[i].name, field_names[i], MAX_IDENTIFIER_LEN - 1);
        schema.fields[i].name[MAX_IDENTIFIER_LEN - 1] = '\0';
        schema.fields[i].type = field_types[i];
        schema.fields[i].value[0] = '\0';
    }
    schema.count = n;
    strncpy(schema.var, cname, MAX_IDENTIFIER_LEN - 1);
    schema.var[MAX_IDENTIFIER_LEN - 1] = '\0';
    obj_build_all(&schema);
    obj_register(cname, &schema);
}

static ASTNode *parse_try(Lexer *lexer, SymbolTable *table) {
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

        char catch_var_src[MAX_IDENTIFIER_LEN];
        catch_var_src[0] = '\0';
        int has_var = 0;

        Token t = lexer_peek(lexer);
        if (t.type == TOKEN_LPAREN) {
            lexer_next(lexer); /* ( */
            Token var = lexer_next(lexer);
            if (!token_is_name(var) || is_ltng_reserved(var.value)) {
                error_state_add(parser_errors, var.line, var.col,
                    "Expected catch variable name");
            } else {
                strncpy(catch_var_src, var.value, MAX_IDENTIFIER_LEN - 1);
                catch_var_src[MAX_IDENTIFIER_LEN - 1] = '\0';
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
                strncpy(catch_var_src, var.value, MAX_IDENTIFIER_LEN - 1);
                catch_var_src[MAX_IDENTIFIER_LEN - 1] = '\0';
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
            char mangled[MAX_IDENTIFIER_LEN];
            mangle_name(catch_var_src, mangled, MAX_IDENTIFIER_LEN);
            /* Catch variables always receive a unique emitted C name so nested
               catches with the same source name cannot collide. */
            symbol_table_add_in_ex(catch_scope, mangled, TYPE_OBJECT, 1);
            const char *cname = symbol_table_get_cname(catch_scope, mangled);
            strncpy(node->name, cname, MAX_IDENTIFIER_LEN - 1);
            node->name[MAX_IDENTIFIER_LEN - 1] = '\0';
            strncpy(node->for_var, catch_var_src, MAX_IDENTIFIER_LEN - 1);
            node->for_var[MAX_IDENTIFIER_LEN - 1] = '\0';
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

static int ltng_ident_ends_with(const char *s, const char *suffix) {
    size_t ls = strlen(s), lf = strlen(suffix);
    if (lf > ls) return 0;
    return strcmp(s + (ls - lf), suffix) == 0;
}

static void parse_throw(Lexer *lexer, SymbolTable *table) {
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

    char type_expr[MAX_STRING_LEN];
    char msg_expr[MAX_STRING_LEN];
    snprintf(type_expr, sizeof(type_expr), "\"Error\"");
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

            char first[MAX_STRING_LEN];
            VarType ftype = collect_expression(lexer, table, first, sizeof(first), 1, 1,
                                               NULL, NULL, NULL, NULL);

            if (is_error_fn) {
                Token comma = lexer_next(lexer);
                if (comma.type != TOKEN_COMMA) {
                    error_state_add(parser_errors, comma.line, comma.col,
                        "Expected ',' after error type in error(type, message)");
                    skip_to_newline(lexer);
                    return;
                }
                char second[MAX_STRING_LEN];
                VarType stype = collect_expression(lexer, table, second, sizeof(second), 1, 0,
                                                   NULL, NULL, NULL, NULL);
                if (ftype != TYPE_STRING && ftype != TYPE_CHAR && ftype != TYPE_OBJECT) {
                    error_state_add(parser_errors, kw.line, kw.col,
                        "error(type, message) requires a string error type");
                }
                snprintf(type_expr, sizeof(type_expr), "%s", first[0] ? first : "\"Error\"");
                snprintf(msg_expr, sizeof(msg_expr), "%s", second[0] ? second : "\"\"");
                mtype = stype;
            } else {
                snprintf(type_expr, sizeof(type_expr), "\"%s\"", next.value);
                snprintf(msg_expr, sizeof(msg_expr), "%s", first[0] ? first : "\"\"");
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
        snprintf(msg_expr, sizeof(msg_expr), "\"%s\"", next.value);
        mtype = TYPE_STRING;
        handled = 1;
    }

    if (!handled) {
        mtype = collect_expression(lexer, table, msg_expr, sizeof(msg_expr), 0, 0,
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
static int try_parse_call_arg(Lexer *lexer, SymbolTable *table, char *cexpr, int cexpr_size, VarType *out_type) {
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
    build_call_cexpr(call, cexpr, cexpr_size);
    if (out_type) *out_type = infer_call_result_type(call);
    return 1;
}

/* Find a lifted function node by its C (mangled) name. */
static ASTNode *find_function_node_by_cname(const char *cname) {
    if (!g_program_ref) return NULL;
    for (ASTNode *fn = g_program_ref->functions; fn; fn = fn->next) {
        if (strcmp(fn->name, cname) == 0) return fn;
    }
    return NULL;
}

/* Return 1 if the function's first parameter is named "base".
   Such functions are "receiver" functions: called via UFCS as
   receiver.method(args), and the return value is written back to receiver. */
static int function_first_param_is_base(const char *cname) {
    ASTNode *fn = find_function_node_by_cname(cname);
    if (!fn || !fn->params) return 0;
    return strcmp(fn->params->for_var, "base") == 0;
}

/* Find a function AST node by its short source name (fn->for_var) or mangled C name. */
static ASTNode *find_function_ast_by_name(const char *name) {
    if (!g_program_ref) return NULL;
    for (ASTNode *fn = g_program_ref->functions; fn; fn = fn->next) {
        if (strcmp(fn->for_var, name) == 0 || strcmp(fn->name, name) == 0) return fn;
    }
    return NULL;
}

/* Resolve named function reference info (cname and param count). Falls back to
   scope/global function registration when AST node is not yet linked (e.g.
   self-reference inside function body during parsing). Returns 1 on success. */
static int resolve_func_ref_info(SymbolTable *table, const char *name, char *out_cname, int out_size, int *out_pc) {
    ASTNode *fndef = find_function_ast_by_name(name);
    if (fndef) {
        if (out_cname && out_size > 0) {
            strncpy(out_cname, fndef->name, out_size - 1);
            out_cname[out_size - 1] = '\0';
        }
        if (out_pc) {
            int pc = 0;
            for (ASTNode *p = fndef->params; p; p = p->next) pc++;
            *out_pc = pc;
        }
        return 1;
    }
    char cname[MAX_IDENTIFIER_LEN];
    if (!resolve_func_name(table, name, cname, sizeof(cname))) return 0;
    if (out_cname && out_size > 0) {
        strncpy(out_cname, cname, out_size - 1);
        out_cname[out_size - 1] = '\0';
    }
    if (out_pc) {
        int pc = -1;
        for (int i = 0; i < g_func_count; i++) {
            if (strcmp(g_funcs[i].name, cname) == 0) { pc = g_funcs[i].param_count; break; }
        }
        *out_pc = pc;
    }
    return 1;
}

static int is_string_literal_return(ASTNode *ret) {
    if (!ret || ret->var_type != TYPE_STRING) return 0;
    const char *v = ret->value;
    while (*v == ' ' || *v == '\t') v++;
    return v[0] == '"';
}

static int var_types_return_compatible_nodes(ASTNode *a, ASTNode *b) {
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

static void collect_return_nodes(ASTNode *node, ASTNode **nodes, int *count, int max) {
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
static int stmt_always_exits(ASTNode *node);

static int block_always_exits(ASTNode *block) {
    for (ASTNode *s = block; s; s = s->next) {
        if (stmt_always_exits(s)) return 1;
    }
    return 0;
}

static int stmt_always_exits(ASTNode *node) {
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
        default:
            return 0;
    }
}

static int return_type_matches_annotation(ASTNode *ret, VarType want) {    if (!ret) return 1;
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

static void validate_function_return_types(ASTNode *fn) {
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
static VarType analyze_function_returns(ASTNode *node, int *has_return) {
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
static int analyze_function_passthrough(ASTNode *node, int *pt_param, int *saw_return, int *disqualified) {
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

static int get_function_passthrough_param(const char *fname) {
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
static VarType infer_function_return_type(const char *fname) {
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

static int function_returns_string(const char *fname) {
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
static int find_local_func_binding(SymbolTable *table, const char *src, char *out, int out_size) {
    for (Scope *s = table; s && s->parent; s = s->parent) {
        for (int i = 0; i < s->func_count; i++) {
            if (strcmp(s->funcs[i].src, src) == 0) {
                strncpy(out, s->funcs[i].cname, out_size - 1);
                out[out_size - 1] = '\0';
                return 1;
            }
        }
    }
    return 0;
}

/* Re-evaluate a default expression captured by the signature pre-scan. Used
   when a call appears before the function body has been parsed. Errors are
   suppressed; on failure the expression falls back to an empty string. */
static void eval_default_tokens(Lexer *lexer, SymbolTable *table, int start, int end,
                                char *out, int out_size, VarType *out_type) {
    out[0] = '\0';
    if (out_type) *out_type = TYPE_STRING;
    if (start < 0 || end <= start || end > lexer->count) {
        snprintf(out, out_size, "\"\"");
        return;
    }
    int n = end - start;
    Lexer tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.token_capacity = n + 8;
    tmp.tokens = (Token *)calloc((size_t)tmp.token_capacity, sizeof(Token));
    if (!tmp.tokens) { snprintf(out, out_size, "\"\""); return; }
    for (int i = 0; i < n; i++) tmp.tokens[i] = lexer->tokens[start + i];
    for (int i = n; i < tmp.token_capacity; i++) {
        tmp.tokens[i].type = TOKEN_EOF;
        tmp.tokens[i].value[0] = '\0';
        tmp.tokens[i].line = n > 0 ? tmp.tokens[n - 1].line : 1;
        tmp.tokens[i].col = 1;
    }
    tmp.count = tmp.token_capacity;
    tmp.current = 0;
    ErrorState scratch;
    memset(&scratch, 0, sizeof(scratch));
    ErrorState *saved = parser_errors;
    parser_errors = &scratch;
    VarType t = collect_expression(&tmp, table, out, out_size, 1, 1, NULL, NULL, NULL, NULL);
    parser_errors = saved;
    if (scratch.count > 0) {
        snprintf(out, out_size, "\"\"");
        t = TYPE_STRING;
    }
    if (out_type) *out_type = t;
    free(tmp.tokens);
}

/* Select the overload of 'src' that best matches the supplied arguments.
   Returns NULL when no candidate matches. *group_count receives the number of
   top-level candidates with this name. */
static FuncInfo *resolve_overload(const char *src, ASTNode *args, int line, int col,
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

        int provided[MAX_FUNC_PARAMS];
        memset(provided, 0, sizeof(provided));
        int ok = 1;
        for (ASTNode *a = args; a && ok; a = a->next) {
            if (!a->for_var[0]) continue;
            int found = -1;
            for (int p = 0; p < fi->param_count && p < MAX_FUNC_PARAMS; p++) {
                if (strcmp(fi->param_names[p], a->for_var) == 0) { found = p; break; }
            }
            if (found < 0) { ok = 0; break; }
            if (provided[found]) { ok = 0; break; }
            provided[found] = 1;
        }
        if (!ok) continue;

        int pos = positional;
        int used_defaults = 0;
        for (int p = 0; p < fi->param_count; p++) {
            if (provided[p]) continue;
            if (pos > 0) { pos--; provided[p] = 1; }
            else if (fi->has_default[p]) { used_defaults++; }
            else { ok = 0; break; }
        }
        if (!ok) continue;
        int rest_used = 0;
        if (pos > 0) {
            if (fi->is_variadic) rest_used = 1;
            else { ok = 0; }
        }
        if (!ok) continue;

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
            /* Identical signatures are reported separately by the duplicate
               prescan check; do not add a cascading ambiguity error. */
            if (best && !(fi->param_count == best->param_count &&
                          fi->required_count == best->required_count &&
                          fi->is_variadic == best->is_variadic)) {
                ambiguous = 1;
            }
        }
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
static ASTNode *expand_call_args(Lexer *lexer, SymbolTable *table, FuncInfo *fi,
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
    if (pc > MAX_FUNC_PARAMS) pc = MAX_FUNC_PARAMS;

    ASTNode *param_node[MAX_FUNC_PARAMS] = {0};
    ASTNode *pn = fn_def ? fn_def->params : NULL;
    for (int p = 0; p < pc; p++) {
        param_node[p] = pn;
        if (pn) pn = pn->next;
    }

    ASTNode *chosen[MAX_FUNC_PARAMS] = {0};
    int provided[MAX_FUNC_PARAMS];
    memset(provided, 0, sizeof(provided));

    /* Map named arguments to parameters. */
    for (ASTNode *a = args; a; a = a->next) {
        if (!a->for_var[0]) continue;
        int found = -1;
        for (int p = 0; p < pc; p++) {
            const char *pname = param_node[p] ? param_node[p]->for_var :
                                (fi ? fi->param_names[p] : "");
            if (pname[0] && strcmp(pname, a->for_var) == 0) { found = p; break; }
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

    /* Collect positional arguments in source order. */
    ASTNode *pos[MAX_FUNC_PARAMS * 4];
    int pos_n = 0;
    for (ASTNode *a = args; a; a = a->next) {
        if (a->for_var[0]) continue;
        if (pos_n < (int)(sizeof(pos) / sizeof(pos[0]))) pos[pos_n++] = a;
    }
    int pos_i = 0;

    ASTNode *eff = NULL, *efflast = NULL;
    for (int p = 0; p < pc; p++) {
        ASTNode *outn = chosen[p];
        if (!outn) {
            if (pos_i < pos_n) {
                outn = pos[pos_i++];
            } else {
                int hasdef = param_node[p] ? param_node[p]->has_default :
                             (fi ? fi->has_default[p] : 0);
                if (hasdef) {
                    outn = create_node(NODE_PARAM, "", "");
                    outn->is_expr = 1;
                    outn->has_default = 1;
                    if (param_node[p]) {
                        snprintf(outn->value, MAX_STRING_LEN, "%s", param_node[p]->value);
                        outn->var_type = param_node[p]->var_type;
                    } else {
                        char defexpr[MAX_STRING_LEN];
                        VarType dvt = TYPE_STRING;
                        eval_default_tokens(lexer, table, fi->def_start[p], fi->def_end[p],
                                            defexpr, sizeof(defexpr), &dvt);
                        snprintf(outn->value, MAX_STRING_LEN, "%s", defexpr);
                        outn->var_type = dvt;
                    }
                } else {
                    char *msg = _ltng_cfmt("Missing required argument '%s' in call to '%s'", param_node[p] ? param_node[p]->for_var : (fi ? fi->param_names[p] : "?"), fname);
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

    /* Extra positional arguments become the variadic rest list. */
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
    return eff;
}

static ASTNode *parse_call_node(Lexer *lexer, SymbolTable *table, const char *fname_tok_val, int line, int col, ASTNode *first_arg) {
    /* assumes fname IDENT consumed, '(' consumed */
    ASTNode *call = create_node(NODE_CALL, "", "");
    call->line = line;
    call->col = col;
    char fm[MAX_IDENTIFIER_LEN];
    int indirect = 0;
    int indirect_dynamic = 0;
    int direct_overload = 0;
    int fr_pc = -1;
    char fr_cname[MAX_IDENTIFIER_LEN] = "";
    char local_cname[MAX_IDENTIFIER_LEN] = "";
    int has_local = find_local_func_binding(table, fname_tok_val, local_cname, sizeof(local_cname));
    int scoped = is_function_name_scoped(table, fname_tok_val);
    /* indirect call through tracked function-reference variable? */
    if (!scoped &&
        symbol_table_get_func_ref(table, fname_tok_val, fr_cname, sizeof(fr_cname), &fr_pc)) {
        indirect = 1;
        strncpy(fm, fr_cname, MAX_IDENTIFIER_LEN - 1);
        fm[MAX_IDENTIFIER_LEN - 1] = '\0';
    } else if (!scoped) {
        /* dynamic indirect call through plain variable holding function name */
        char mangled[MAX_IDENTIFIER_LEN];
        mangle_name(fname_tok_val, mangled, sizeof(mangled));
        if (symbol_table_has(table, mangled) || symbol_table_has(table, fname_tok_val)) {
            indirect_dynamic = 1;
            const char *vcname = symbol_table_get_cname(table, mangled);
            if (!vcname || !vcname[0]) vcname = symbol_table_get_cname(table, fname_tok_val);
            strncpy(call->for_var, vcname ? vcname : mangled, MAX_IDENTIFIER_LEN - 1);
            call->for_var[MAX_IDENTIFIER_LEN - 1] = '\0';
            strncpy(fm, "__ltng_indirect", MAX_IDENTIFIER_LEN - 1);
            fm[MAX_IDENTIFIER_LEN - 1] = '\0';
        } else {
            if (!resolve_func_name(table, fname_tok_val, fm, MAX_IDENTIFIER_LEN)) {
                mangle_name(fname_tok_val, fm, MAX_IDENTIFIER_LEN);
            }
        }
    } else if (has_local) {
        strncpy(fm, local_cname, MAX_IDENTIFIER_LEN - 1);
        fm[MAX_IDENTIFIER_LEN - 1] = '\0';
    } else {
        /* top-level call: overload is selected after the arguments are parsed */
        direct_overload = 1;
        mangle_name(fname_tok_val, fm, MAX_IDENTIFIER_LEN);
    }
    /* If calling a tracked function-ref that is actually a closure (has captures),
       we must dispatch through the variable's closure value (encoded). */
    if (indirect && !indirect_dynamic) {
        ASTNode *tfn = find_function_ast_by_name(fm);
        if (!tfn) tfn = find_function_ast_by_name(fr_cname);
        if (tfn && tfn->capture_count > 0) {
            char mangled_v[MAX_IDENTIFIER_LEN];
            mangle_name(fname_tok_val, mangled_v, sizeof(mangled_v));
            const char *vcname = symbol_table_get_cname(table, mangled_v);
            if (!vcname || !vcname[0]) vcname = symbol_table_get_cname(table, fname_tok_val);
            if (!vcname || !vcname[0]) vcname = mangled_v;
            strncpy(call->for_var, vcname, MAX_IDENTIFIER_LEN - 1);
            call->for_var[MAX_IDENTIFIER_LEN - 1] = '\0';
            strncpy(fm, "__ltng_indirect", MAX_IDENTIFIER_LEN - 1);
            fm[MAX_IDENTIFIER_LEN - 1] = '\0';
            indirect_dynamic = 1;
        }
    }
    if (!direct_overload) {
        strncpy(call->name, fm, MAX_IDENTIFIER_LEN - 1);
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
            char named_key[MAX_IDENTIFIER_LEN] = {0};
            if (at.type == TOKEN_IDENT) {
                int saved = lexer->current;
                Token idtok = lexer_next(lexer);
                Token eq = lexer_peek(lexer);
                if (eq.type == TOKEN_EQUALS) {
                    lexer_next(lexer); /* consume '=' */
                    named_arg = 1;
                    strncpy(named_key, idtok.value, MAX_IDENTIFIER_LEN - 1);
                } else {
                    lexer->current = saved;
                }
            }
            /* String literals passed directly */
            if (at.type == TOKEN_STRING && !named_arg) {
                lexer_next(lexer);
                ASTNode *an = create_node(NODE_PARAM, "", "");
                snprintf(an->value, MAX_STRING_LEN, "\"%s\"", at.value);
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
                    snprintf(an->value, MAX_STRING_LEN, "\"%s\"", fn ? fn->name : "");
                    an->var_type = TYPE_STRING;
                    an->is_expr = 1;
                    if (!args) args = alast = an; else { alast->next = an; alast = an; }
                } else {
                    /* named function ref not supported as expression here; fall through */
                    char aexpr[MAX_STRING_LEN];
                    VarType avt = collect_expression(lexer, table, aexpr, sizeof(aexpr), 1, 1, NULL, NULL, NULL, NULL);
                    ASTNode *an = create_node(NODE_PARAM, "", aexpr);
                    an->var_type = avt;
                    an->is_expr = 1;
                    if (!args) args = alast = an; else { alast->next = an; alast = an; }
                }
            } else if (at.type == TOKEN_CHAR && !named_arg) {
                lexer_next(lexer);
                ASTNode *an = create_node(NODE_PARAM, "", "");
                if (at.value[0]) snprintf(an->value, MAX_STRING_LEN, "'%s'", at.value);
                else strcpy(an->value, "'\\0'");
                an->var_type = TYPE_STRING;
                an->is_expr = 1;
                if (!args) args = alast = an; else { alast->next = an; alast = an; }
            } else if (!named_arg && at.type == TOKEN_IDENT && is_callable_ident(table, at.value)) {
                /* function call argument - pass result as string buffer */
                char cexpr[MAX_STRING_LEN];
                VarType carg_type = TYPE_STRING;
                if (try_parse_call_arg(lexer, table, cexpr, sizeof(cexpr), &carg_type)) {
                    ASTNode *an = create_node(NODE_PARAM, "", cexpr);
                    an->var_type = carg_type;
                    an->is_expr = 1;
                    an->is_variable = 1; /* pass pointer directly (call returns char* buffer) */
                    if (!args) args = alast = an; else { alast->next = an; alast = an; }
                } else {
                    /* complex expression involving call, or plain variable (since is_callable_ident now includes vars) */
                    VarType avt = collect_expression(lexer, table, cexpr, sizeof(cexpr), 1, 1, NULL, NULL, NULL, NULL);
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
                        strncpy(an->for_var, named_key, MAX_IDENTIFIER_LEN - 1);
                        has_named_arg = 1;
                    }
                    if (!args) args = alast = an; else { alast->next = an; alast = an; }
                }
            } else {
                /* Parse as expression (or named arg value) */
                char aexpr[MAX_STRING_LEN];
                VarType avt = collect_expression(lexer, table, aexpr, sizeof(aexpr), 1, 1, NULL, NULL, NULL, NULL);
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
                    strncpy(an->for_var, named_key, MAX_IDENTIFIER_LEN - 1);
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
            /* Pick the first candidate so codegen can continue after an error. */
            for (int i = 0; i < g_func_count; i++) {
                if (g_funcs[i].top_level && strcmp(g_funcs[i].src, fname_tok_val) == 0) {
                    fi = &g_funcs[i];
                    break;
                }
            }
        }
        if (fi) {
            strncpy(fm, fi->name, MAX_IDENTIFIER_LEN - 1);
            fm[MAX_IDENTIFIER_LEN - 1] = '\0';
            fn_def = find_function_node_by_cname(fi->name);
        }
        strncpy(call->name, fm, MAX_IDENTIFIER_LEN - 1);
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

static void record_return_obj_schema(const char *obj_base) {
    if (!obj_base || !obj_base[0] || !g_enclosing_cname[0]) return;
    if (!obj_lookup(obj_base)) return;
    FuncInfo *fi = find_funcinfo_by_cname(g_enclosing_cname);
    if (fi && fi->return_obj_base[0] == '\0') {
        strncpy(fi->return_obj_base, obj_base, MAX_IDENTIFIER_LEN - 1);
        fi->return_obj_base[MAX_IDENTIFIER_LEN - 1] = '\0';
    }
    ASTNode *fn = find_function_node_by_cname(g_enclosing_cname);
    if (fn && fn->return_obj_base[0] == '\0') {
        strncpy(fn->return_obj_base, obj_base, MAX_IDENTIFIER_LEN - 1);
        fn->return_obj_base[MAX_IDENTIFIER_LEN - 1] = '\0';
    }
}

static void parse_return(Lexer *lexer, SymbolTable *table) {
    Token kw = lexer_next(lexer);
    if (!g_in_function) {
        error_state_add(parser_errors, kw.line, kw.col,
            "'return' can only be used inside a function");
        skip_to_newline(lexer);
        return;
    }
    char expr[MAX_STRING_LEN];
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
                char fval[MAX_STRING_LEN];
                if (fn && fn->capture_count > 0) {
                    char expr[MAX_STRING_LEN];
                    snprintf(expr, sizeof(expr), "\"%s\"", fn->name);
                    for (int ci = 0; ci < fn->capture_count; ci++) {
                        char tmp[MAX_STRING_LEN];
                        snprintf(tmp, sizeof(tmp), "_ltng_add_sn(_ltng_add_sn(%s,\"\\001\"),_ltng_ptr_to_str((void*)%s))", expr, fn->captures[ci]);
                        strncpy(expr, tmp, sizeof(expr)-1); expr[sizeof(expr)-1]=0;
                    }
                    snprintf(fval, sizeof(fval), "%s", expr);
                } else {
                    snprintf(fval, sizeof(fval), "\"%s\"", fn ? fn->name : "");
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
                char mangled_ref[MAX_IDENTIFIER_LEN]; mangle_name(next.value, mangled_ref, sizeof(mangled_ref));
                int is_var = symbol_table_has(table, mangled_ref) || symbol_table_has(table, next.value);
                char ref_cname[MAX_IDENTIFIER_LEN]; int ref_pc = -1;
                if (!is_var && resolve_func_ref_info(table, next.value, ref_cname, sizeof(ref_cname), &ref_pc)) {
                    lexer_next(lexer);
                    char fval[MAX_STRING_LEN]; snprintf(fval, sizeof(fval), "\"%s\"", ref_cname);
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
                snprintf(expr, sizeof(expr), "\"%s\"", next.value);
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
                snprintf(expr, sizeof(expr), "%s", (strcmp(btok.value, "true") == 0 || strcmp(btok.value, "t") == 0) ? "1" : "0");
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
                char m[MAX_IDENTIFIER_LEN];
                mangle_name(id.value, m, sizeof(m));
                const char *cname = symbol_table_get_cname(table, m);
                strncpy(expr, cname, sizeof(expr) - 1);
                expr[sizeof(expr) - 1] = '\0';
                VarType vtype2 = TYPE_STRING;
                if (symbol_table_has(table, m)) vtype2 = symbol_table_get(table, m);
                /* Preserve legacy pass-through for dynamic/string/object values, but keep
                   typed numeric/bool returns for known-typed variables. Value-initialized
                   locals are dtype-unlocked by default, so infer from current known type
                   (excluding untyped/dynamic parameters). For parameters with default
                   values of known types, prefer that type for inference/return. */
                if (vtype2 == TYPE_ARRAY) {
                    etype = TYPE_ARRAY;
                    snprintf(expr, sizeof(expr), "_ltng_ah_pack(%s_data, %s_len, %s_cap)", cname, cname, cname);
                    if (g_enclosing_cname[0]) {
                        FuncInfo *efi = find_funcinfo_by_cname(g_enclosing_cname);
                        if (efi) { efi->return_type = TYPE_ARRAY; efi->has_return_type = 1; }
                    }
                } else if (vtype2 == TYPE_OBJECT) {
                    etype = TYPE_OBJECT;
                } else if ((vtype2 == TYPE_INT || vtype2 == TYPE_FLOAT || vtype2 == TYPE_BOOL) &&
                           (symbol_table_get_locked(table, m) || !symbol_table_is_param(table, m))) {
                    etype = vtype2;
                    if (vtype2 == TYPE_INT) snprintf(expr, sizeof(expr), "atoll(%s)", cname);
                    else if (vtype2 == TYPE_FLOAT) snprintf(expr, sizeof(expr), "atof(%s)", cname);
                    else snprintf(expr, sizeof(expr), "(%s[0]=='t'?1:0)", cname);
                } else {
                    /* Try param default type */
                    VarType pdt = TYPE_NULL;
                    if (g_enclosing_cname[0]) {
                        FuncInfo *efi = find_funcinfo_by_cname(g_enclosing_cname);
                        if (efi) {
                            for (int pi = 0; pi < efi->param_count && pi < MAX_FUNC_PARAMS; pi++) {
                                if (strcmp(efi->param_names[pi], id.value) == 0) {
                                    if (efi->has_default[pi]) pdt = efi->def_type[pi];
                                    break;
                                }
                            }
                        }
                    }
                    if (pdt == TYPE_INT || pdt == TYPE_FLOAT || pdt == TYPE_BOOL) {
                        etype = pdt;
                        if (pdt == TYPE_INT) snprintf(expr, sizeof(expr), "atoll(%s)", cname);
                        else if (pdt == TYPE_FLOAT) snprintf(expr, sizeof(expr), "atof(%s)", cname);
                        else snprintf(expr, sizeof(expr), "(%s[0]=='t'?1:0)", cname);
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
                if (etype == TYPE_STRING && symbol_table_is_param(table, m) && g_enclosing_cname[0]) {
                    int _is_pt = 1;
                    /* exclude if param has a concrete default type (we'd have taken pdt branch above) */
                    VarType _pdt2 = TYPE_NULL;
                    FuncInfo *_efi2 = find_funcinfo_by_cname(g_enclosing_cname);
                    if (_efi2) {
                        for (int _pi = 0; _pi < _efi2->param_count && _pi < MAX_FUNC_PARAMS; _pi++) {
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
            char sole_call_cname[MAX_IDENTIFIER_LEN] = "";
            VarType sole_call_type = TYPE_STRING;
            if (try_parse_sole_call_expr(lexer, table, expr, sizeof(expr), 1, &sole_call_type, sole_call_cname, sizeof(sole_call_cname))) {
                etype = sole_call_type;
                if (etype == TYPE_NULL || etype == TYPE_VOID) etype = TYPE_STRING;
                if (etype == TYPE_ARRAY && g_enclosing_cname[0]) {
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
            char temp_mangled[MAX_IDENTIFIER_LEN];
            snprintf(temp_mangled, sizeof(temp_mangled), "__ret_obj_%d", g_ret_obj_count++);
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
        etype = collect_expression(lexer, table, expr, sizeof(expr), 0, 0, NULL, NULL, NULL, NULL);
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

static void parse_exit(Lexer *lexer, SymbolTable *table) {
    Token kw = lexer_next(lexer);
    char expr[MAX_STRING_LEN];
    expr[0] = '\0';
    Token next = lexer_peek(lexer);
    if (next.type == TOKEN_NEWLINE || next.type == TOKEN_EOF || next.type == TOKEN_RBRACE) {
        error_state_add(parser_errors, kw.line, kw.col,
            "Expected integer expression after 'exit'");
        skip_to_newline(lexer);
        return;
    }
    VarType etype = collect_expression(lexer, table, expr, sizeof(expr), 0, 0, NULL, NULL, NULL, NULL);
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


static int is_ident_start_char(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int is_ident_char(char c) { return is_ident_start_char(c) || (c >= '0' && c <= '9'); }

static void add_capture_name(char names[][MAX_IDENTIFIER_LEN], int *count, int max, const char *name) {
    if (!name || !name[0] || *count >= max) return;
    /* skip internal/runtime names */
    if (strncmp(name, "_ltng", 5) == 0) return;
    if (strncmp(name, "_v", 2) == 0) { /* scoped var cname prefix - but we want to track by mangled/original; keep */ }
    for (int i = 0; i < *count; i++) if (strcmp(names[i], name) == 0) return;
    strncpy(names[*count], name, MAX_IDENTIFIER_LEN - 1);
    names[*count][MAX_IDENTIFIER_LEN - 1] = 0;
    (*count)++;
}

static void collect_idents_from_str(const char *s, char names[][MAX_IDENTIFIER_LEN], int *count, int max) {
    if (!s) return;
    for (int i = 0; s[i]; ) {
        if (is_ident_start_char(s[i])) {
            char buf[MAX_IDENTIFIER_LEN]; int j = 0;
            while (s[i] && is_ident_char(s[i]) && j < MAX_IDENTIFIER_LEN - 1) buf[j++] = s[i++];
            buf[j] = 0;
            add_capture_name(names, count, max, buf);
        } else if (s[i] == '"' ) {
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

static void collect_declared_locals(ASTNode *node, char names[][MAX_IDENTIFIER_LEN], int *count, int max, int depth) {
    if (!node || depth > 128) return;
    if (node->type == NODE_LET || node->type == NODE_CONST) {
        if (node->name[0]) add_capture_name(names, count, max, node->name);
    }
    /* don't descend into nested function bodies - they have separate scopes */
    if (node->type == NODE_FUNCTION) return;
    collect_declared_locals(node->body, names, count, max, depth + 1);
    collect_declared_locals(node->orelse, names, count, max, depth + 1);
    collect_declared_locals(node->else_body, names, count, max, depth + 1);
    collect_declared_locals(node->cases, names, count, max, depth + 1);
    collect_declared_locals(node->next, names, count, max, depth + 1);
}

static void collect_used_idents(ASTNode *node, char names[][MAX_IDENTIFIER_LEN], int *count, int max, int depth) {
    if (!node || depth > 128) return;
    /* don't descend into nested function bodies */
    if (node->type == NODE_FUNCTION) { collect_used_idents(node->next, names, count, max, depth); return; }
    /* NODE_READ stores variable name in node->name */
    if (node->type == NODE_READ && node->name[0]) add_capture_name(names, count, max, node->name);
    /* calls: name is callee (function) - not a variable capture unless indirect; args have values */
    if (node->type == NODE_CALL) {
        /* indirect call target var stored in for_var */
        if (strcmp(node->name, "__ltng_indirect") == 0 && node->for_var[0])
            add_capture_name(names, count, max, node->for_var);
    }
    /* scan expression-bearing string fields */
    collect_idents_from_str(node->value, names, count, max);
    collect_idents_from_str(node->for_start, names, count, max);
    collect_idents_from_str(node->for_end, names, count, max);
    collect_idents_from_str(node->for_step, names, count, max);
    collect_idents_from_str(node->array_index, names, count, max);
    /* SET/assignments target var in name - writing is not a capture read, but reading RHS is in value */
    /* for NODE_PARAM args, value may contain var refs */

    collect_used_idents(node->body, names, count, max, depth + 1);
    collect_used_idents(node->orelse, names, count, max, depth + 1);
    collect_used_idents(node->else_body, names, count, max, depth + 1);
    collect_used_idents(node->cases, names, count, max, depth + 1);
    collect_used_idents(node->args, names, count, max, depth + 1);
    collect_used_idents(node->params, names, count, max, depth + 1);
    collect_used_idents(node->next, names, count, max, depth + 1);
}

static int name_in_list(char names[][MAX_IDENTIFIER_LEN], int count, const char *name) {
    for (int i = 0; i < count; i++) if (strcmp(names[i], name) == 0) return 1;
    return 0;
}

static ASTNode *parse_function(Lexer *lexer, SymbolTable *table, const char *enclosing_cname) {
    Token kw = lexer_next(lexer); /* 'function' */
    int is_anon = 0;
    Token name = {0};
    if (lexer_peek(lexer).type == TOKEN_LPAREN) {
        /* anonymous function: function(params) { ... } */
        is_anon = 1;
        name.type = TOKEN_IDENT;
        name.line = kw.line;
        name.col = kw.col;
        snprintf(name.value, sizeof(name.value), "__anon_%d", g_anon_count++);
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

    char fname_short[MAX_IDENTIFIER_LEN];
    mangle_name(name.value, fname_short, MAX_IDENTIFIER_LEN);
    char cname[MAX_IDENTIFIER_LEN];
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
        strncpy(cname, sig->name, MAX_IDENTIFIER_LEN - 1);
        cname[MAX_IDENTIFIER_LEN - 1] = 0;
    } else if (enclosing_cname && enclosing_cname[0]) {
        snprintf(cname, sizeof(cname), "%s__%s", enclosing_cname, fname_short);
    } else {
        strncpy(cname, fname_short, MAX_IDENTIFIER_LEN - 1);
        cname[MAX_IDENTIFIER_LEN - 1] = 0;
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
        if (g_func_count < MAX_FUNCS) {
            FuncInfo *nf = &g_funcs[g_func_count++];
            memset(nf, 0, sizeof(*nf));
            nf->passthrough_param = -1;
            strncpy(nf->name, cname, MAX_IDENTIFIER_LEN - 1);
            nf->name[MAX_IDENTIFIER_LEN - 1] = 0;
            strncpy(nf->src, name.value, MAX_IDENTIFIER_LEN - 1);
            nf->src[MAX_IDENTIFIER_LEN - 1] = 0;
            nf->param_count = 0;
            nf->required_count = 0;
            nf->return_type = TYPE_STRING;
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
    char variadic_name[MAX_IDENTIFIER_LEN] = "";
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
                strncpy(variadic_name, rn.value, MAX_IDENTIFIER_LEN - 1);
                variadic_name[MAX_IDENTIFIER_LEN - 1] = '\0';
                char rm[MAX_IDENTIFIER_LEN];
                mangle_name(rn.value, rm, MAX_IDENTIFIER_LEN);
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
            char pm[MAX_IDENTIFIER_LEN];
            mangle_name(p.value, pm, MAX_IDENTIFIER_LEN);
            symbol_table_add_param(func_scope, pm, TYPE_FLOAT);
            ASTNode *pn = create_node(NODE_PARAM, pm, "");
            pn->var_type = TYPE_FLOAT;
            strncpy(pn->for_var, p.value, MAX_IDENTIFIER_LEN - 1);
            pn->has_default = 0;
            pn->value[0] = '\0';
            if (lexer_peek(lexer).type == TOKEN_EQUALS) {
                /* Default parameter value: name = expr */
                lexer_next(lexer); /* = */
                char defexpr[MAX_STRING_LEN];
                VarType dvt = collect_expression(lexer, table, defexpr, sizeof(defexpr), 1, 1, NULL, NULL, NULL, NULL);
                pn->has_default = 1;
                pn->var_type = dvt;
                snprintf(pn->value, MAX_STRING_LEN, "%s", defexpr);
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
                if (pi < MAX_FUNC_PARAMS) {
                    strncpy(fi->param_names[pi], pp->for_var, MAX_IDENTIFIER_LEN - 1);
                    fi->param_names[pi][MAX_IDENTIFIER_LEN - 1] = '\0';
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
    if (!is_anon) strncpy(fn->for_var, name.value, MAX_IDENTIFIER_LEN - 1);
    else fn->for_var[0] = '\0';
    fn->params = params;
    fn->has_return_type = has_return_type;
    fn->return_type = return_type;
    fn->is_variadic = variadic;
    if (variadic) {
        strncpy(fn->variadic_name, variadic_name, MAX_IDENTIFIER_LEN - 1);
        fn->variadic_name[MAX_IDENTIFIER_LEN - 1] = '\0';
    }
    if (sig) {
        sig->has_return_type = has_return_type;
        sig->return_type = return_type;
    }
    char saved_enc[MAX_IDENTIFIER_LEN];
    strncpy(saved_enc, g_enclosing_cname, MAX_IDENTIFIER_LEN - 1);
    saved_enc[MAX_IDENTIFIER_LEN - 1] = 0;
    strncpy(g_enclosing_cname, cname, MAX_IDENTIFIER_LEN - 1);
    g_enclosing_cname[MAX_IDENTIFIER_LEN - 1] = 0;
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
    strncpy(g_enclosing_cname, saved_enc, MAX_IDENTIFIER_LEN - 1);
    g_enclosing_cname[MAX_IDENTIFIER_LEN - 1] = 0;

    /* Compute captures for nested/anonymous functions */
    fn->capture_count = 0;
    if (is_anon || (enclosing_cname && enclosing_cname[0])) {
        char used[64][MAX_IDENTIFIER_LEN]; int used_n = 0;
        char locals[64][MAX_IDENTIFIER_LEN]; int locals_n = 0;
        /* params are locals */
        for (ASTNode *p = fn->params; p; p = p->next) {
            if (p->for_var[0]) add_capture_name(locals, &locals_n, 64, p->for_var);
            if (p->name[0]) add_capture_name(locals, &locals_n, 64, p->name);
        }
        if (fn->is_variadic && fn->variadic_name[0])
            add_capture_name(locals, &locals_n, 64, fn->variadic_name);
        collect_declared_locals(fn->body, locals, &locals_n, 64, 0);
        collect_used_idents(fn->body, used, &used_n, 64, 0);
        for (int i = 0; i < used_n && fn->capture_count < 8; i++) {
            const char *u = used[i];
            if (name_in_list(locals, locals_n, u)) continue;
            /* skip common keywords/builtins/literals that might appear as tokens in expressions */
            if (strcmp(u,"true")==0||strcmp(u,"false")==0||strcmp(u,"t")==0||strcmp(u,"f")==0) continue;
            if (strcmp(u,"null")==0||strcmp(u,"write")==0||strcmp(u,"read")==0) continue;
            if (strcmp(u,"int")==0||strcmp(u,"float")==0||strcmp(u,"string")==0||strcmp(u,"char")==0||strcmp(u,"bool")==0) continue;
            if (strcmp(u,"if")==0||strcmp(u,"else")==0||strcmp(u,"for")==0||strcmp(u,"while")==0||strcmp(u,"return")==0) continue;
            if (strcmp(u,"let")==0||strcmp(u,"const")==0||strcmp(u,"set")==0||strcmp(u,"function")==0) continue;
            if (strcmp(u,"try")==0||strcmp(u,"catch")==0||strcmp(u,"finally")==0||strcmp(u,"throw")==0) continue;
            if (strcmp(u,"break")==0||strcmp(u,"continue")==0||strcmp(u,"switch")==0||strcmp(u,"case")==0||strcmp(u,"default")==0) continue;
            if (strcmp(u,"and")==0||strcmp(u,"or")==0||strcmp(u,"not")==0||strcmp(u,"in")==0||strcmp(u,"from")==0||strcmp(u,"to")==0||strcmp(u,"step")==0||strcmp(u,"by")==0) continue;
            if (strcmp(u,"exit")==0||strcmp(u,"include")==0) continue;
            /* skip if it's a declared function name */
            if (is_function_name(u)) continue;
            /* skip numeric-like */
            if (u[0] >= '0' && u[0] <= '9') continue;
            /* check if this identifier exists in enclosing scope (as variable) */
            char mangled_u[MAX_IDENTIFIER_LEN];
            mangle_name(u, mangled_u, sizeof(mangled_u));
            int in_enclosing = 0;
            const char *cap_cname = NULL;
            if (table) {
                /* try by symbol name (mangled key) */
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
            if (!in_enclosing) continue;
            /* avoid duplicate captures */
            int dup = 0;
            for (int c = 0; c < fn->capture_count; c++) {
                if (strcmp(fn->captures[c], cap_cname) == 0) dup = 1;
            }
            if (dup) continue;
            strncpy(fn->captures[fn->capture_count], cap_cname, MAX_IDENTIFIER_LEN - 1);
            fn->captures[fn->capture_count][MAX_IDENTIFIER_LEN - 1] = 0;
            fn->capture_count++;
        }
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
            strncpy(fn->return_obj_base, fi2->return_obj_base, MAX_IDENTIFIER_LEN - 1);
            fn->return_obj_base[MAX_IDENTIFIER_LEN - 1] = '\0';
        }
        if (fi2 && fi2->return_obj_base[0] == '\0' && fn->return_obj_base[0] != '\0') {
            strncpy(fi2->return_obj_base, fn->return_obj_base, MAX_IDENTIFIER_LEN - 1);
            fi2->return_obj_base[MAX_IDENTIFIER_LEN - 1] = '\0';
        }
    }
    (void)kw;
    return fn;
}

static void parse_statement(Lexer *lexer, SymbolTable *table) {
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
            char fm[MAX_IDENTIFIER_LEN];
            int indirect_stmt = 0;
            int fr_pc = -1;
            if (!is_function_name_scoped(table, t.value) &&
                symbol_table_get_func_ref(table, t.value, fm, sizeof(fm), &fr_pc)) {
                indirect_stmt = 1;
            } else {
                if (!resolve_func_name(table, t.value, fm, MAX_IDENTIFIER_LEN)) {
                    mangle_name(t.value, fm, MAX_IDENTIFIER_LEN);
                }
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
                char cexpr[MAX_STRING_LEN];
                build_call_cexpr(call, cexpr, sizeof(cexpr));
                while (lexer_peek(lexer).type == TOKEN_LPAREN) {
                    if ((int)strlen(cexpr) > 2000) {
                        error_state_add(parser_errors, lexer_peek(lexer).line, lexer_peek(lexer).col,
                            "Chained call expression too deep/complex");
                        skip_to_newline(lexer); return;
                    }
                    int cline = lexer_peek(lexer).line, ccol = lexer_peek(lexer).col;
                    lexer_next(lexer);
                    ASTNode *args = NULL, *last = NULL;
                    if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                        while (1) {
                            char arg_expr[MAX_STRING_LEN];
                            VarType at = collect_expression(lexer, table, arg_expr, sizeof(arg_expr), 1, 1, NULL, NULL, NULL, NULL);
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
                    char next_expr[MAX_STRING_LEN]; int p = 0;
                    static int _stcid = 0; int cid = _stcid++;
                    p += snprintf(next_expr+p, sizeof(next_expr)-p, "({ char *__ct%d=_ltng_ds_fmt(\"%%s\",%s); char *__ctr%d=__ltng_call_indirect(__ct%d,_ltng_retbuf(),%d,%d,%d",
                        cid,cexpr,cid,cid,cline>0?cline:1,ccol>0?ccol:1,argc);
                    if (p >= (int)sizeof(next_expr)) p = (int)sizeof(next_expr) - 1;
                    for (ASTNode *a = args; a && p < (int)sizeof(next_expr) - 1; a = a->next) {
                        int rem = (int)sizeof(next_expr) - p;
                        if (a->var_type == TYPE_STRING && a->value[0] == '"') p += snprintf(next_expr+p, rem, ",%s", a->value);
                        else if (a->is_variable && a->value[0] != '\0') p += snprintf(next_expr+p, rem, ",%s", a->value);
                        else if ((a->var_type == TYPE_STRING || a->var_type == TYPE_OBJECT) && a->value[0] != '\0') p += snprintf(next_expr+p, rem, ",%s", a->value);
                        else if (a->value[0] == '\0') p += snprintf(next_expr+p, rem, ",\"\"");
                        else p += snprintf(next_expr+p, rem, ",_ltng_ad((double)(%s))", a->value);
                        if (p >= (int)sizeof(next_expr)) p = (int)sizeof(next_expr) - 1;
                    }
                    if (p < (int)sizeof(next_expr) - 1)
                        p += snprintf(next_expr+p, (int)sizeof(next_expr)-p, "); free(__ct%d); __ctr%d; })", cid, cid);
                    next_expr[sizeof(next_expr)-1]=0;
                    if (p >= (int)sizeof(next_expr) - 1) {
                        error_state_add(parser_errors, cline, ccol, "Chained call expression too deep/complex");
                        skip_to_newline(lexer); return;
                    }
                    strncpy(cexpr, next_expr, sizeof(cexpr)-1); cexpr[sizeof(cexpr)-1]=0;
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

                char rm[MAX_IDENTIFIER_LEN];
                mangle_name(t.value, rm, MAX_IDENTIFIER_LEN);
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

                char rm[MAX_IDENTIFIER_LEN];
                mangle_name(t.value, rm, MAX_IDENTIFIER_LEN);
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
                            char arg_expr[MAX_STRING_LEN];
                            VarType arg_type = collect_expression(lexer, table, arg_expr, sizeof(arg_expr), 1, 0, NULL, NULL, NULL, NULL);
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
                            char idx_expr[MAX_STRING_LEN];
                            collect_expression(lexer, table, idx_expr, sizeof(idx_expr), 1, 1, NULL, NULL, NULL, NULL);
                            if (lexer_peek(lexer).type != TOKEN_COMMA) {
                                error_state_add(parser_errors, lexer_peek(lexer).line,
                                    lexer_peek(lexer).col, "Expected ',' after insert index");
                                skip_to_newline(lexer);
                                return;
                            }
                            lexer_next(lexer); /* consume , */
                            char arg_expr[MAX_STRING_LEN];
                            VarType arg_type = collect_expression(lexer, table, arg_expr, sizeof(arg_expr), 1, 0, NULL, NULL, NULL, NULL);
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
                            strncpy(node->array_index, arg_expr, MAX_STRING_LEN - 1);
                            node->array_index[MAX_STRING_LEN - 1] = '\0';
                            node->var_type = arg_type;
                            node->is_expr = 1;
                            emit(node);
                        } else if (strcmp(mname.value, "remove") == 0) {
                            /* remove(index) */
                            char idx_expr[MAX_STRING_LEN];
                            collect_expression(lexer, table, idx_expr, sizeof(idx_expr), 1, 0, NULL, NULL, NULL, NULL);
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
                                char idx_expr[MAX_STRING_LEN];
                                collect_expression(lexer, table, idx_expr, sizeof(idx_expr), 1, 0, NULL, NULL, NULL, NULL);
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
                                char dummy[MAX_STRING_LEN];
                                collect_expression(lexer, table, dummy, sizeof(dummy), 1, 0, NULL, NULL, NULL, NULL);
                                if (lexer_peek(lexer).type == TOKEN_RPAREN) lexer_next(lexer);
                            }
                        }
                    } else if (is_function_name_scoped(table, mname.value)) {
                        /* User-defined method call on same base receiver: at this point '(' already consumed */
                        ASTNode *recv = create_node(NODE_PARAM, "", "");
                        char rm2[MAX_IDENTIFIER_LEN];
                        mangle_name(t.value, rm2, MAX_IDENTIFIER_LEN);
                        if (symbol_table_has(table, rm2)) {
                            recv->is_variable = 1;
                            strncpy(recv->value, rm2, MAX_STRING_LEN - 1);
                            recv->var_type = symbol_table_get(table, rm2);
                        } else if (symbol_table_has(table, t.value)) {
                            recv->is_variable = 1;
                            strncpy(recv->value, t.value, MAX_STRING_LEN - 1);
                            recv->var_type = symbol_table_get(table, t.value);
                        } else {
                            recv->is_expr = 1;
                            strncpy(recv->value, t.value, MAX_STRING_LEN - 1);
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
                    char rm[MAX_IDENTIFIER_LEN];
                    mangle_name(t.value, rm, MAX_IDENTIFIER_LEN);
                    if (symbol_table_has(table, rm)) {
                        recv->is_variable = 1;
                        strncpy(recv->value, rm, MAX_STRING_LEN - 1);
                        recv->var_type = symbol_table_get(table, rm);
                    } else if (symbol_table_has(table, t.value)) {
                        recv->is_variable = 1;
                        strncpy(recv->value, t.value, MAX_STRING_LEN - 1);
                        recv->var_type = symbol_table_get(table, t.value);
                    } else {
                        /* pass as expression/value - best effort */
                        recv->is_expr = 1;
                        strncpy(recv->value, t.value, MAX_STRING_LEN - 1);
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
                                    char rm3[MAX_IDENTIFIER_LEN]; mangle_name(t.value, rm3, MAX_IDENTIFIER_LEN);
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
static int skip_default_expr(Lexer *lexer, int start) {
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
static void prescan_functions(Lexer *lexer) {
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
        if (g_func_count >= MAX_FUNCS) break;

        FuncInfo *fi = &g_funcs[g_func_count];
        memset(fi, 0, sizeof(*fi));
        fi->passthrough_param = -1;
        fi->top_level = 1;
        strncpy(fi->src, nm.value, MAX_IDENTIFIER_LEN - 1);
        fi->src[MAX_IDENTIFIER_LEN - 1] = '\0';
        fi->line = nm.line;
        fi->col = nm.col;
        fi->return_type = TYPE_STRING;

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
            if (pc < MAX_FUNC_PARAMS) {
                int hasdef = 0, ds = -1, de = -1;
                strncpy(fi->param_names[pc], lexer->tokens[k].value, MAX_IDENTIFIER_LEN - 1);
                fi->param_names[pc][MAX_IDENTIFIER_LEN - 1] = '\0';
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
                if (!hasdef) req++;
            } else {
                /* Too many parameters to track; keep scanning to find the close. */
                while (k < lexer->count && lexer->tokens[k].type != TOKEN_RPAREN &&
                       lexer->tokens[k].type != TOKEN_COMMA &&
                       lexer->tokens[k].type != TOKEN_NEWLINE) k++;
            }
            pc++;
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
        char mangled[MAX_IDENTIFIER_LEN];
        mangle_name(g_funcs[a].src, mangled, sizeof(mangled));
        if (group <= 1) {
            strncpy(g_funcs[a].name, mangled, MAX_IDENTIFIER_LEN - 1);
            g_funcs[a].name[MAX_IDENTIFIER_LEN - 1] = '\0';
        } else {
            snprintf(g_funcs[a].name, MAX_IDENTIFIER_LEN, "%s__ov%d", mangled, idx);
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
    g_enclosing_cname[0] = 0;
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
                                     char *out, int out_size) {
    char buf[MAX_STRING_LEN];
    snprintf(buf, sizeof(buf), "%s\n", expr);
    Lexer *tmp_lexer = (Lexer *)malloc(sizeof(Lexer));
    if (!tmp_lexer) { out[0] = '\0'; return TYPE_INT; }
    lexer_init(tmp_lexer, buf);
    ErrorState tmp_errors;
    memset(&tmp_errors, 0, sizeof(tmp_errors));
    tmp_errors.filename = strdup("<expr>");
    lexer_tokenize(tmp_lexer, &tmp_errors);
    ErrorState *target = codegen_get_errors();
    if (!target) target = parser_errors;
    if (!target) target = &tmp_errors;
    ErrorState *saved = parser_errors;
    parser_errors = target;
    VarType result = collect_expression(tmp_lexer, table, out, out_size, 0, 0, NULL, NULL, NULL, NULL);
    parser_errors = saved;
    free(tmp_lexer);
    free(tmp_errors.filename);
    return result;
}

static void free_node_list(ASTNode *node) {
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
