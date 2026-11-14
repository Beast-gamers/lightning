#include "ltng.h"

static ErrorState *parser_errors = NULL;
static int g_break_depth = 0;
static ASTNode *g_list = NULL;
static int g_stmt_line = 0;
static int g_stmt_col = 0;
static int g_expr_sl=0,g_expr_sc=0,g_expr_el=0,g_expr_ec=0;

#define MAX_FUNCS 128
typedef struct { char name[MAX_IDENTIFIER_LEN]; int param_count; } FuncInfo;
static FuncInfo g_funcs[MAX_FUNCS];
static int g_func_count = 0;
static int g_in_function = 0;
static int g_in_catch = 0;
static Program *g_program_ref = NULL;
static char g_enclosing_cname[MAX_IDENTIFIER_LEN] = "";
static int g_ret_obj_count = 0;

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
    /* avoid duplicates */
    for (int i = 0; i < g_func_count; i++) {
        if (strcmp(g_funcs[i].name, mangled) == 0) {
            if (param_count > 0) g_funcs[i].param_count = param_count;
            return;
        }
    }
    strncpy(g_funcs[g_func_count].name, mangled, MAX_IDENTIFIER_LEN - 1);
    g_funcs[g_func_count].name[MAX_IDENTIFIER_LEN - 1] = '\0';
    g_funcs[g_func_count].param_count = param_count;
    g_func_count++;
}

static int is_function_name(const char *name) {
    char mangled[MAX_IDENTIFIER_LEN];
    mangle_name(name, mangled, MAX_IDENTIFIER_LEN);
    for (int i = 0; i < g_func_count; i++) {
        if (strcmp(g_funcs[i].name, mangled) == 0) return 1;
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
        if (strcmp(g_funcs[i].name, mangled) == 0) {
            strncpy(out, mangled, out_size - 1);
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
    node->type = type;
    strncpy(node->name, name ? name : "", MAX_IDENTIFIER_LEN - 1);
    node->name[MAX_IDENTIFIER_LEN - 1] = '\0';
    strncpy(node->value, value ? value : "", MAX_STRING_LEN - 1);
    node->value[MAX_STRING_LEN - 1] = '\0';
    node->var_type = TYPE_STRING;
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

/* Return 1 for type keywords that can be used as conversion functions:
   int(expr), string(expr), bool(expr), tofloat(expr), tochar(expr). These are
   function calls, not type declarations, when followed by '('. */
static int is_type_conversion(const char *name) {
    return strcmp(name, "toint") == 0 ||
           strcmp(name, "tostring") == 0 ||
           strcmp(name, "tobool") == 0 ||
           strcmp(name, "tofloat") == 0 ||
           strcmp(name, "tochar") == 0;
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
    for (int i = 0; i < table->count; i++) {
        if (strcmp(table->vars[i].name, name) == 0) {
            table->vars[i].type = type;
            return;
        }
    }
    if (table->count >= MAX_VARS) {
        if (parser_errors) {
            error_state_add(parser_errors, 0, 0, "Too many variables");
        }
        return;
    }
    VarEntry *ve = &table->vars[table->count];
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
    ve->array_dim_count = 0;
    ve->array_dims = NULL;
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
           t == TOKEN_PLUS_PLUS || t == TOKEN_MINUS_MINUS;
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
    char tmp[2048];
    strncpy(tmp, expr, 2047);
    tmp[2047] = 0;
    char *start = tmp;
    char left[512] = {0}, right[512] = {0};
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
            found = 1;
        }
    }
    if (!found) return;
    char result[2048];
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
    const char *count_expr;
    if (right_is_num) {
        count_expr = right;
    } else {
        static char cntbuf[512];
        snprintf(cntbuf, sizeof(cntbuf), "atoi(%s)", right);
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
        snprintf(result, 2047, "({ char _ltm[512]; _ltng_repeat(_ltm,512,%s,(long long)(%s)); _ltm; })", right, left);
    } else if (right_is_num && (left[0] == '"' || (left[0] == '\'' && strlen(left) >= 3))) {
        /* "string" * number */
        snprintf(result, 2047, "({ char _ltm[512]; _ltng_repeat(_ltm,512,%s,(long long)(%s)); _ltm; })", left, right);
    } else {
        /* variable cases: assume left is string, right is count */
        snprintf(result, 2047, "({ char _ltm[512]; _ltng_repeat(_ltm,512,%s,(long long)(%s)); _ltm; })", left, count_expr);
    }
    strncpy(expr, result, size - 1);
    expr[size - 1] = 0;
}


static ASTNode *parse_call_node(Lexer *lexer, SymbolTable *table, const char *fname_tok_val, int line, int col, ASTNode *first_arg);
static void build_call_cexpr(ASTNode *call, char *cexpr, int cexpr_size);
static ASTNode *make_receiver_arg(SymbolTable *table, Token rtok);


static void sanitize_mod_atof(char *out, int out_size) {
    /* Cast atof(...) to (long long) when used with % to avoid double%int in C. */
    if (!strstr(out, "%")) return;
    char buf[4096];
    int bi = 0, i = 0, len = (int)strlen(out);
    while (i < len && bi < (int)sizeof(buf) - 8) {
        if (i + 5 <= len && out[i]=='a' && out[i+1]=='t' && out[i+2]=='o' && out[i+3]=='f' && out[i+4]=='(') {
            /* extract full atof(...) */
            char seg[512]; int si = 0;
            seg[si++] = out[i++]; seg[si++] = out[i++]; seg[si++] = out[i++]; seg[si++] = out[i++]; seg[si++] = out[i++];
            int depth = 1;
            while (i < len && depth > 0 && si < 500) {
                if (out[i] == '(') depth++;
                else if (out[i] == ')') depth--;
                seg[si++] = out[i++];
                if (depth == 0) break;
            }
            seg[si] = 0;
            int wrap = 0;
            /* check if next non-space is % */
            int j = i; while (j < len && (out[j]==' '||out[j]=='\t')) j++;
            if (j < len && out[j] == '%') wrap = 1;
            /* check if previous non-space in buf is % */
            int k = bi - 1; while (k >= 0 && (buf[k]==' '||buf[k]=='\t')) k--;
            if (k >= 0 && buf[k] == '%') wrap = 1;
            if (wrap) bi += snprintf(buf + bi, sizeof(buf) - bi, "((long long)%s)", seg);
            else bi += snprintf(buf + bi, sizeof(buf) - bi, "%s", seg);
            continue;
        }
        buf[bi++] = out[i++];
    }
    buf[bi] = 0;
    strncpy(out, buf, out_size - 1);
    out[out_size - 1] = 0;
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
    char result[4096] = {0};
    const char *src = out;
    char *dst = result;
    int replaced = 0;

    while (*src && (int)(dst - result) < 4095) {
        char left[256], right[256];

        if (is_add_operand(src, left, sizeof(left))) {
            const char *after_left = src;
            while (*after_left && (isalnum((unsigned char)*after_left) || *after_left == '_' ||
                                   *after_left == '(' || *after_left == ')' || *after_left == ',' ||
                                   *after_left == ' ' || *after_left == '\t')) after_left++;
            while (*after_left == ' ' || *after_left == '\t') after_left++;
            if (*after_left == '+') {
                const char *after_plus = after_left + 1;
                while (*after_plus == ' ' || *after_plus == '\t') after_plus++;
                if (is_add_operand(after_plus, right, sizeof(right))) {
                    const char *after_right = after_plus;
                    while (*after_right && (isalnum((unsigned char)*after_right) || *after_right == '_' ||
                                            *after_right == '(' || *after_right == ')' ||
                                            *after_right == ',' || *after_right == ' ' ||
                                            *after_right == '\t')) after_right++;
                    while (*after_right == ' ' || *after_right == '\t') after_right++;
                    /* Skip replacement if followed by another operator or closing paren */
                    int next_op = (*after_right == '*' || *after_right == '/' || *after_right == '%' ||
                                   *after_right == '+' || *after_right == '-' || *after_right == '^' ||
                                   *after_right == '&' || *after_right == '|' || *after_right == '<' ||
                                   *after_right == '>' || *after_right == '!' || *after_right == '=' ||
                                   *after_right == ')');
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
        return 1;
    }
    return 0;
}

static const char *type_name_str(VarType t);

/* Return 1 if a concat part is already a char* expression: a string literal,
   a simple identifier (all variables are char buffers in generated C), or a
   statement expression produced by transform_string_multiply (returns char*). */
static int part_is_stringy(const char *part) {
    if (part[0] == '"') return 1;
    if (part[0] == '_' || isalpha((unsigned char)part[0])) {
        const char *q = part;
        while (*q && (isalnum((unsigned char)*q) || *q == '_')) q++;
        if (*q == '\0') return 1;
    }
    if (strncmp(part, "({", 2) == 0 &&
        (strstr(part, "_ltng_repeat") != NULL || strstr(part, "_ltng_arr") != NULL)) return 1;
    return 0;
}

static void token_append_source(char *out, int *pos, int out_size, Token tok);

/* Find a function AST node by its short source name or mangled C name. */
static ASTNode *find_function_ast_by_name(const char *name);

/* Infer the return type of a user-defined function by scanning its return statements. */
static VarType infer_function_return_type(const char *fname);

static int parse_array_literal(Lexer *lexer, SymbolTable *table, ASTNode *node) {
    (void)table;
    /* The caller has already consumed the opening '['. Parse the remaining
       elements/nested arrays until the matching ']'. Arrays are stored as a
       flat, row-major list of values. Arrays may have any number of dimensions. */
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

        /* Element value. It may be a literal or an identifier. */
        lexer_next(lexer);
        char val[MAX_STRING_LEN] = "";
        if (t.type == TOKEN_CHAR) {
            snprintf(val, sizeof(val), "'%s'", t.value);
        } else {
            snprintf(val, sizeof(val), "%s", t.value);
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
    strncpy(node->value, values, MAX_STRING_LEN - 1);
    node->value[MAX_STRING_LEN - 1] = '\0';

    return 1;
}


static VarType collect_expression(Lexer *lexer, SymbolTable *table,
                                  char *out, int out_size,
                                  int stop_on_rparen, int stop_on_comma,
                                  int *out_sl, int *out_sc, int *out_el, int *out_ec) {
    int pos = 0;
    VarType result_type = TYPE_INT;
    int paren_depth = 0;
    int type_locked = 0;
    int arithmetic_used = 0;
    int has_string_concat = 0;
    int has_string_mult = 0;
    int array_access_used = 0;
    int sl=0, sc=0, el=0, ec=0, have_span=0;

    while (lexer->current < lexer->count) {
        Token tok = lexer_peek(lexer);
        if (tok.type == TOKEN_NEWLINE || tok.type == TOKEN_EOF) break;
        if (tok.type == TOKEN_LBRACE) break;
        if (tok.type == TOKEN_RBRACE && paren_depth == 0) break;
        if ((tok.type == TOKEN_RPAREN || tok.type == TOKEN_RBRACKET) && paren_depth == 0 && stop_on_rparen) break;
        if (tok.type == TOKEN_COMMA && stop_on_comma) break;
        if (tok.type == TOKEN_FROM || tok.type == TOKEN_TO || tok.type == TOKEN_STEP) break;

        tok = lexer_next(lexer);
        if (!have_span) { sl = tok.line; sc = tok.col; have_span = 1; }
        el = tok.line;
        ec = tok.col + (int)strlen(tok.value);
        if (tok.type == TOKEN_STRING) ec = tok.col + (int)strlen(tok.value) + 2;
        else if (tok.type == TOKEN_CHAR) ec = tok.col + (int)strlen(tok.value) + 2;

        switch (tok.type) {
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
                } else {
                    if (is_inc)
                        pos += snprintf(out + pos, out_size - pos,
                            "({ long long _n = (long long)(atoi(%s) + 1); snprintf(%s, sizeof(%s), \"%%lld\", _n); _n; })",
                            mangled, mangled, mangled);
                    else
                        pos += snprintf(out + pos, out_size - pos,
                            "({ long long _n = (long long)(atoi(%s) - 1); snprintf(%s, sizeof(%s), \"%%lld\", _n); _n; })",
                            mangled, mangled, mangled);
                }
                result_type = vt;
                break;
            }
            case TOKEN_INT_LIT:
                pos += snprintf(out + pos, out_size - pos, "%s", tok.value);
                break;
            case TOKEN_FLOAT_LIT:
                pos += snprintf(out + pos, out_size - pos, "%s", tok.value);
                if (!type_locked) result_type = TYPE_FLOAT;
                break;
            case TOKEN_STRING:
                if (arithmetic_used) {
                    pos += snprintf(out + pos, out_size - pos, "\"%s\"", tok.value);
                    if (!type_locked) result_type = TYPE_STRING;
                } else {
                    pos += snprintf(out + pos, out_size - pos, "\"%s\"", tok.value);
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
                /* Type conversion functions: toint/tofloat/tostring/tobool/tochar */
                if ((strcmp(tok.value, "toint") == 0 || strcmp(tok.value, "tostring") == 0 ||
                     strcmp(tok.value, "tobool") == 0 || strcmp(tok.value, "tofloat") == 0 ||
                     strcmp(tok.value, "tochar") == 0) && lexer_peek(lexer).type == TOKEN_LPAREN) {
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
                    if (ipos >= 2 && inner[0] == '"') {
                        int end_quote = -1;
                        for (int i = 1; i < ipos; i++) {
                            if (inner[i] == '\\' && i + 1 < ipos) i++;
                            else if (inner[i] == '"') { end_quote = i; break; }
                        }
                        if (end_quote == ipos - 1) is_simple_str_lit = 1;
                    }

                    if (strcmp(tok.value, "toint") == 0) {
                        /* toint(expr): preserve existing Lightning behavior */
                        if (is_simple_str_lit) {
                            pos += snprintf(out + pos, out_size - pos, "%s", inner);
                        } else if (is_num_lit) {
                            pos += snprintf(out + pos, out_size - pos, "\"%s\"", inner);
                        } else {
                            pos += snprintf(out + pos, out_size - pos, "%s", inner);
                        }
                        if (!type_locked) result_type = TYPE_STRING;
                    } else if (strcmp(tok.value, "tofloat") == 0) {
                        /* tofloat(expr): convert to float string/value representation */
                        if (is_num_lit) {
                            pos += snprintf(out + pos, out_size - pos, "\"%g\"", (double)atof(inner));
                        } else {
                            pos += snprintf(out + pos, out_size - pos, "_ltng_from_double(_ltng_retbuf(), (double)atof(%s))", inner);
                        }
                        if (!type_locked) result_type = TYPE_STRING;
                    } else if (strcmp(tok.value, "tostring") == 0) {
                        /* string(expr): convert bool literals and number literals
                           to quoted strings, else pass through */
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
                        } else {
                            pos += snprintf(out + pos, out_size - pos, "%s", inner);
                        }
                        if (!type_locked) result_type = TYPE_STRING;
                    } else if (strcmp(tok.value, "tobool") == 0) {
                        /* bool(expr): true unless null/empty/"0"/"false" */
                        if (ipos == 1 && inner[0] == '0') {
                            pos += snprintf(out + pos, out_size - pos, "\"false\"");
                        } else if (is_num_lit) {
                            pos += snprintf(out + pos, out_size - pos, "\"true\"");
                        } else if (is_simple_str_lit) {
                            if (ipos == 2) {
                                pos += snprintf(out + pos, out_size - pos, "\"false\"");
                            } else if (ipos == 3 && inner[1] == '0') {
                                pos += snprintf(out + pos, out_size - pos, "\"false\"");
                            } else if (ipos == 7 && memcmp(inner + 1, "false", 5) == 0) {
                                pos += snprintf(out + pos, out_size - pos, "\"false\"");
                            } else {
                                pos += snprintf(out + pos, out_size - pos, "\"true\"");
                            }
                        } else {
                            pos += snprintf(out + pos, out_size - pos,
                                "((%s)[0]!='\\0'&&strcmp(%s,\"0\")!=0&&strcmp(%s,\"false\")!=0?\"true\":\"false\")",
                                inner, inner, inner);
                        }
                        if (!type_locked) result_type = TYPE_STRING;
                    } else if (strcmp(tok.value, "tochar") == 0) {
                        /* tochar(expr): convert to single-character string */
                        if (is_simple_str_lit && ipos >= 3) {
                            /* take first character of string literal */
                            char ch = inner[1];
                            if (ch == '\\' && ipos >= 4) {
                                /* keep escape sequence as-is inside string literal */
                                char esc[8] = {0};
                                esc[0] = inner[1]; esc[1] = inner[2];
                                pos += snprintf(out + pos, out_size - pos, "\"%s\"", esc);
                            } else {
                                pos += snprintf(out + pos, out_size - pos, "\"%c\"", ch);
                            }
                        } else if (is_num_lit) {
                            int iv = (int)atoll(inner);
                            if (iv >= 0 && iv <= 127) {
                                if (iv == '\\' || iv == '"' || iv == '\n' || iv == '\t') {
                                    pos += snprintf(out + pos, out_size - pos, "_ltng_ad((double)(%d))", iv);
                                } else {
                                    pos += snprintf(out + pos, out_size - pos, "\"%c\"", iv);
                                }
                            } else {
                                pos += snprintf(out + pos, out_size - pos, "\"%c\"", (unsigned char)iv);
                            }
                        } else {
                            /* runtime: if numeric string use char code, else first char */
                            pos += snprintf(out + pos, out_size - pos,
                                "_ltng_as((_ltng_is_num_str(%s)?((char[]){(char)atoi(%s),0}):((char[]){(%s)[0]? (%s)[0]:0,0})))",
                                inner, inner, inner, inner);
                        }
                        if (!type_locked) result_type = TYPE_STRING;
                    }
                    break;
                }
                if (is_bool_value(tok.value)) {
                    pos += snprintf(out + pos, out_size - pos, "%s",
                        (strcmp(tok.value, "true") == 0 || strcmp(tok.value, "t") == 0) ? "1" : "0");
                } else if (is_type_keyword(tok.value)) {
                    pos += snprintf(out + pos, out_size - pos, "%s", tok.value);
                } else if (is_function_name_scoped(table, tok.value) && lexer_peek(lexer).type == TOKEN_LPAREN) {
                    lexer_next(lexer); /* consume ( */
                    ASTNode *call = parse_call_node(lexer, table, tok.value, tok.line, tok.col, NULL);
                    if (call->is_base_assign) {
                        error_state_add(parser_errors, tok.line, tok.col,
                            "function with 'base' parameter must be called with method syntax (receiver.method(args))");
                        skip_to_newline(lexer);
                        pos += snprintf(out + pos, out_size - pos, "0");
                        break;
                    }
                    pos += snprintf(out + pos, out_size - pos, "atof(%s(_ltng_retbuf(), %d, %d", call->name, call->line > 0 ? call->line : 1, call->col > 0 ? call->col : 1);
                    for (ASTNode *a = call->args; a; a = a->next) {
                        if (a->var_type == TYPE_STRING && a->value[0] == '"') {
                            pos += snprintf(out + pos, out_size - pos, ", %s", a->value);
                        } else if (a->is_variable && a->value[0] != '\0') {
                            pos += snprintf(out + pos, out_size - pos, ", %s", a->value);
                        } else if ((a->var_type == TYPE_STRING || a->var_type == TYPE_OBJECT) && a->value[0] != '\0') {
                            pos += snprintf(out + pos, out_size - pos, ", %s", a->value);
                        } else if (a->value[0] == '\0') {
                            pos += snprintf(out + pos, out_size - pos, ", \"\"");
                        } else {
                            pos += snprintf(out + pos, out_size - pos, ", _ltng_ad((double)(%s))", a->value);
                        }
                    }
                    pos += snprintf(out + pos, out_size - pos, "))");
                    if (!type_locked) result_type = TYPE_INT;
                } else if (lexer_peek(lexer).type == TOKEN_DOT) {
                    /* Check for array.push/pop - let TYPE_ARRAY case handle
                       these as inline C expressions */
                    if (lexer->current + 2 < lexer->count &&
                        lexer->tokens[lexer->current + 1].type == TOKEN_IDENT &&
                        (strcmp(lexer->tokens[lexer->current + 1].value, "push") == 0 ||
                         strcmp(lexer->tokens[lexer->current + 1].value, "pop") == 0)) {
                        /* Don't consume - TYPE_ARRAY case handles push/pop */
                        goto collect_ident_var;
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
                                "%s", tlenname);
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
                            error_state_add(parser_errors, mname.line, mname.col,
                                "method call with 'base' parameter cannot be used as an expression");
                            skip_to_newline(lexer);
                            pos += snprintf(out + pos, out_size - pos, "0");
                            break;
                        }
                        pos += snprintf(out + pos, out_size - pos, "atof(%s(_ltng_retbuf(), %d, %d", call->name, call->line > 0 ? call->line : 1, call->col > 0 ? call->col : 1);
                        for (ASTNode *a = call->args; a; a = a->next) {
                            if (a->var_type == TYPE_STRING && a->value[0] == '"') {
                                pos += snprintf(out + pos, out_size - pos, ", %s", a->value);
                            } else if (a->is_variable && a->value[0] != '\0') {
                                pos += snprintf(out + pos, out_size - pos, ", %s", a->value);
                            } else if ((a->var_type == TYPE_STRING || a->var_type == TYPE_OBJECT) && a->value[0] != '\0') {
                                pos += snprintf(out + pos, out_size - pos, ", %s", a->value);
                            } else if ((a->var_type == TYPE_STRING || a->var_type == TYPE_OBJECT) && a->value[0] != '\0') {
                                pos += snprintf(out + pos, out_size - pos, ", %s", a->value);
                            } else if (a->value[0] == '\0') {
                                pos += snprintf(out + pos, out_size - pos, ", \"\"");
                            } else {
                                pos += snprintf(out + pos, out_size - pos, ", _ltng_ad((double)(%s))", a->value);
                            }
                        }
                        pos += snprintf(out + pos, out_size - pos, "))");
                        if (!type_locked) result_type = TYPE_INT;
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
                        if (ftype == TYPE_INT || ftype == TYPE_BOOL || ftype == TYPE_STRING) {
                            if (ftype == TYPE_BOOL)
                                pos += snprintf(out + pos, out_size - pos, "(%s[0]=='t'?1:0)", fv);
                            else if (ftype == TYPE_STRING)
                                pos += snprintf(out + pos, out_size - pos, "%s", fv);
                            else
                                pos += snprintf(out + pos, out_size - pos, "atoi(%s)", fv);
                        } else if (ftype == TYPE_FLOAT) {
                            pos += snprintf(out + pos, out_size - pos, "atof(%s)", fv);
                            if (!type_locked) result_type = TYPE_INT;
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
                    VarType vt;
                    if (arg.type == TOKEN_INT_LIT) {
                        vt = TYPE_INT;
                    } else if (arg.type == TOKEN_FLOAT_LIT) {
                        vt = TYPE_FLOAT;
                    } else if (arg.type == TOKEN_STRING) {
                        vt = TYPE_STRING;
                    } else if (arg.type == TOKEN_CHAR) {
                        vt = TYPE_STRING;
                    } else if (arg.type == TOKEN_IDENT && is_bool_value(arg.value)) {
                        vt = TYPE_BOOL;
                    } else if (arg.type == TOKEN_IDENT || token_is_name(arg)) {
                        char mangled[MAX_IDENTIFIER_LEN];
                        mangle_name(arg.value, mangled, MAX_IDENTIFIER_LEN);
                        if (symbol_table_has(table, mangled)) {
                            vt = symbol_table_get(table, mangled);
                        } else {
                            vt = TYPE_STRING;
                        }
                    } else {
                        vt = TYPE_STRING;
                    }
                    const char *vtname = type_name_str(vt);
                    pos += snprintf(out + pos, out_size - pos, "\"%s\"", vtname);
                    if (!type_locked) result_type = TYPE_STRING;
                    continue;
                } else {
                collect_ident_var:
                    ; /* C99: declaration after label */
                    char mangled[MAX_IDENTIFIER_LEN];
                    mangle_name(tok.value, mangled, MAX_IDENTIFIER_LEN);
                    if (!symbol_table_has(table, mangled)) {
                        char errmsg[256];
                        snprintf(errmsg, sizeof(errmsg), "Variable '%s' not declared", tok.value);
                        error_state_add(parser_errors, tok.line, tok.col, errmsg);
                        pos += snprintf(out + pos, out_size - pos, "0");
                    } else {
                        VarType vtype = symbol_table_get(table, mangled);
                        const char *cname = symbol_table_get_cname(table, mangled);
                        /* in a string-building context, use the raw buffer
                           (array variables are handled by the index logic below) */
                        if (result_type == TYPE_STRING && !type_locked && vtype != TYPE_ARRAY) {
                            pos += snprintf(out + pos, out_size - pos, "%s", cname);
                        } else switch (vtype) {
                            case TYPE_INT:
                                pos += snprintf(out + pos, out_size - pos, "atoi(%s)", cname);
                                break;
                            case TYPE_FLOAT:
                                pos += snprintf(out + pos, out_size - pos, "atof(%s)", cname);
                                if (!type_locked) {
                                    if (result_type == TYPE_INT) result_type = TYPE_FLOAT;
                                }
                                break;
                            case TYPE_CHAR:
                            case TYPE_STRING:
                            case TYPE_OBJECT:
                                pos += snprintf(out + pos, out_size - pos, "%s", cname);
                                if (!type_locked) result_type = TYPE_STRING;
                                break;
                            case TYPE_BOOL:
                                pos += snprintf(out + pos, out_size - pos,
                                    "(%s[0]=='t'?1:0)", cname);
                                break;
                            case TYPE_NULL:
                            default:
                                pos += snprintf(out + pos, out_size - pos, "%s", cname);
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
                                                "%s", lenname);
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
                                                char errmsg[256];
                                                snprintf(errmsg, sizeof(errmsg),
                                                    "Expected '(' after '%s'", mname.value);
                                                error_state_add(parser_errors, mname.line, mname.col,
                                                    errmsg);
                                                skip_to_newline(lexer);
                                                pos += snprintf(out + pos, out_size - pos, "0");
                                                break;
                                            }
                                            lexer_next(lexer); /* consume ( */

                                            if (strcmp(mname.value, "push") == 0) {
                                                /* push(value) */
                                                char arg_expr[MAX_STRING_LEN];
                                                VarType arg_type = collect_expression(lexer, table, arg_expr, sizeof(arg_expr), 1, 0, NULL, NULL, NULL, NULL);
                                                if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                                                    error_state_add(parser_errors, lexer_peek(lexer).line,
                                                        lexer_peek(lexer).col, "Expected ')' after push argument");
                                                    skip_to_newline(lexer);
                                                    pos += snprintf(out + pos, out_size - pos, "0");
                                                    break;
                                                }
                                                lexer_next(lexer); /* consume ) */
                                                if (arg_type == TYPE_INT) {
                                                    pos += snprintf(out + pos, out_size - pos,
                                                        "({ char __apv[256]; snprintf(__apv, 256, \"%%lld\", (long long)(%s)); _ltng_arr_push(&%s, &%s_len, &%s_cap, __apv); char *__r = _ltng_retbuf(); snprintf(__r, 256, \"%%s\", __apv); __r; })",
                                                        arg_expr, arrname, lenname, capname);
                                                } else if (arg_type == TYPE_FLOAT) {
                                                    pos += snprintf(out + pos, out_size - pos,
                                                        "({ char __apv[256]; snprintf(__apv, 256, \"%%g\", (double)(%s)); _ltng_arr_push(&%s, &%s_len, &%s_cap, __apv); char *__r = _ltng_retbuf(); snprintf(__r, 256, \"%%s\", __apv); __r; })",
                                                        arg_expr, arrname, lenname, capname);
                                                } else {
                                                    pos += snprintf(out + pos, out_size - pos,
                                                        "({ char __apv[256]; snprintf(__apv, 256, \"%%s\", %s); _ltng_arr_push(&%s, &%s_len, &%s_cap, __apv); char *__r = _ltng_retbuf(); snprintf(__r, 256, \"%%s\", __apv); __r; })",
                                                        arg_expr, arrname, lenname, capname);
                                                }
                                                result_type = TYPE_STRING;
                                            } else if (strcmp(mname.value, "pop") == 0) {
                                                /* pop() or pop(index) */
                                                if (lexer_peek(lexer).type == TOKEN_RPAREN) {
                                                    /* pop() - no args */
                                                    lexer_next(lexer); /* consume ) */
                                                    pos += snprintf(out + pos, out_size - pos,
                                                        "({ char *__r = _ltng_arr_pop(%s, &%s); __r; })",
                                                        arrname, lenname);
                                                    result_type = TYPE_STRING;
                                                } else {
                                                    /* pop(index) */
                                                    char idx_expr[MAX_STRING_LEN];
                                                    collect_expression(lexer, table, idx_expr, sizeof(idx_expr), 1, 0, NULL, NULL, NULL, NULL);
                                                    if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                                                        error_state_add(parser_errors, lexer_peek(lexer).line,
                                                            lexer_peek(lexer).col, "Expected ')' after pop index");
                                                        skip_to_newline(lexer);
                                                        pos += snprintf(out + pos, out_size - pos, "0");
                                                        break;
                                                    }
                                                    lexer_next(lexer); /* consume ) */
                                                    pos += snprintf(out + pos, out_size - pos,
                                                        "({ char *__r = _ltng_arr_pop_index(%s, &%s, (int)(%s)); __r; })",
                                                        arrname, lenname, idx_expr);
                                                    result_type = TYPE_STRING;
                                                }
                                            } else if (strcmp(mname.value, "insert") == 0) {
                                                /* insert(index, value) */
                                                char idx_expr[MAX_STRING_LEN];
                                                collect_expression(lexer, table, idx_expr, sizeof(idx_expr), 1, 0, NULL, NULL, NULL, NULL);
                                                if (lexer_peek(lexer).type != TOKEN_COMMA) {
                                                    error_state_add(parser_errors, lexer_peek(lexer).line,
                                                        lexer_peek(lexer).col, "Expected ',' after insert index");
                                                    skip_to_newline(lexer);
                                                    pos += snprintf(out + pos, out_size - pos, "0");
                                                    break;
                                                }
                                                lexer_next(lexer); /* consume , */
                                                char arg_expr[MAX_STRING_LEN];
                                                VarType arg_type = collect_expression(lexer, table, arg_expr, sizeof(arg_expr), 1, 0, NULL, NULL, NULL, NULL);
                                                if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                                                    error_state_add(parser_errors, lexer_peek(lexer).line,
                                                        lexer_peek(lexer).col, "Expected ')' after insert argument");
                                                    skip_to_newline(lexer);
                                                    pos += snprintf(out + pos, out_size - pos, "0");
                                                    break;
                                                }
                                                lexer_next(lexer); /* consume ) */
                                                if (arg_type == TYPE_INT) {
                                                    pos += snprintf(out + pos, out_size - pos,
                                                        "({ char __aiv[256]; snprintf(__aiv, 256, \"%%lld\", (long long)(%s)); _ltng_arr_insert(&%s, &%s_len, &%s_cap, (int)(%s), __aiv); char *__r = _ltng_retbuf(); snprintf(__r, 256, \"%%s\", __aiv); __r; })",
                                                        arg_expr, arrname, lenname, capname, idx_expr);
                                                } else if (arg_type == TYPE_FLOAT) {
                                                    pos += snprintf(out + pos, out_size - pos,
                                                        "({ char __aiv[256]; snprintf(__aiv, 256, \"%%g\", (double)(%s)); _ltng_arr_insert(&%s, &%s_len, &%s_cap, (int)(%s), __aiv); char *__r = _ltng_retbuf(); snprintf(__r, 256, \"%%s\", __aiv); __r; })",
                                                        arg_expr, arrname, lenname, capname, idx_expr);
                                                } else {
                                                    pos += snprintf(out + pos, out_size - pos,
                                                        "({ char __aiv[256]; snprintf(__aiv, 256, \"%%s\", %s); _ltng_arr_insert(&%s, &%s_len, &%s_cap, (int)(%s), __aiv); char *__r = _ltng_retbuf(); snprintf(__r, 256, \"%%s\", __aiv); __r; })",
                                                        arg_expr, arrname, lenname, capname, idx_expr);
                                                }
                                                result_type = TYPE_STRING;
                                            } else if (strcmp(mname.value, "remove") == 0) {
                                                /* remove(index) */
                                                char idx_expr[MAX_STRING_LEN];
                                                collect_expression(lexer, table, idx_expr, sizeof(idx_expr), 1, 0, NULL, NULL, NULL, NULL);
                                                if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                                                    error_state_add(parser_errors, lexer_peek(lexer).line,
                                                        lexer_peek(lexer).col, "Expected ')' after remove index");
                                                    skip_to_newline(lexer);
                                                    pos += snprintf(out + pos, out_size - pos, "0");
                                                    break;
                                                }
                                                lexer_next(lexer); /* consume ) */
                                                pos += snprintf(out + pos, out_size - pos,
                                                    "({ char *__r = _ltng_arr_pop_index(%s, &%s, (int)(%s)); __r; })",
                                                    arrname, lenname, idx_expr);
                                                result_type = TYPE_STRING;
                                            } else if (strcmp(mname.value, "contains") == 0) {
                                                /* contains(value) */
                                                char arg_expr[MAX_STRING_LEN];
                                                VarType arg_type = collect_expression(lexer, table, arg_expr, sizeof(arg_expr), 1, 0, NULL, NULL, NULL, NULL);
                                                if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                                                    error_state_add(parser_errors, lexer_peek(lexer).line,
                                                        lexer_peek(lexer).col, "Expected ')' after contains argument");
                                                    skip_to_newline(lexer);
                                                    pos += snprintf(out + pos, out_size - pos, "0");
                                                    break;
                                                }
                                                lexer_next(lexer); /* consume ) */
                                                if (arg_type == TYPE_INT) {
                                                    pos += snprintf(out + pos, out_size - pos,
                                                        "({ char __cv[256]; snprintf(__cv, 256, \"%%lld\", (long long)(%s)); char *__r = _ltng_retbuf(); snprintf(__r, 256, \"%%s\", _ltng_arr_contains(%s, %s_len, __cv) ? \"true\" : \"false\"); __r; })",
                                                        arg_expr, arrname, lenname);
                                                } else if (arg_type == TYPE_FLOAT) {
                                                    pos += snprintf(out + pos, out_size - pos,
                                                        "({ char __cv[256]; snprintf(__cv, 256, \"%%g\", (double)(%s)); char *__r = _ltng_retbuf(); snprintf(__r, 256, \"%%s\", _ltng_arr_contains(%s, %s_len, __cv) ? \"true\" : \"false\"); __r; })",
                                                        arg_expr, arrname, lenname);
                                                } else {
                                                    pos += snprintf(out + pos, out_size - pos,
                                                        "({ char __cv[256]; snprintf(__cv, 256, \"%%s\", %s); char *__r = _ltng_retbuf(); snprintf(__r, 256, \"%%s\", _ltng_arr_contains(%s, %s_len, __cv) ? \"true\" : \"false\"); __r; })",
                                                        arg_expr, arrname, lenname);
                                                }
                                                result_type = TYPE_STRING;
                                            } else if (strcmp(mname.value, "index_of") == 0) {
                                                /* index_of(value) */
                                                char arg_expr[MAX_STRING_LEN];
                                                VarType arg_type = collect_expression(lexer, table, arg_expr, sizeof(arg_expr), 1, 0, NULL, NULL, NULL, NULL);
                                                if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                                                    error_state_add(parser_errors, lexer_peek(lexer).line,
                                                        lexer_peek(lexer).col, "Expected ')' after index_of argument");
                                                    skip_to_newline(lexer);
                                                    pos += snprintf(out + pos, out_size - pos, "0");
                                                    break;
                                                }
                                                lexer_next(lexer); /* consume ) */
                                                if (arg_type == TYPE_INT) {
                                                    pos += snprintf(out + pos, out_size - pos,
                                                        "({ char __iv[256]; snprintf(__iv, 256, \"%%lld\", (long long)(%s)); char *__r = _ltng_retbuf(); snprintf(__r, 256, \"%%d\", _ltng_arr_index_of(%s, %s_len, __iv)); __r; })",
                                                        arg_expr, arrname, lenname);
                                                } else if (arg_type == TYPE_FLOAT) {
                                                    pos += snprintf(out + pos, out_size - pos,
                                                        "({ char __iv[256]; snprintf(__iv, 256, \"%%g\", (double)(%s)); char *__r = _ltng_retbuf(); snprintf(__r, 256, \"%%d\", _ltng_arr_index_of(%s, %s_len, __iv)); __r; })",
                                                        arg_expr, arrname, lenname);
                                                } else {
                                                    pos += snprintf(out + pos, out_size - pos,
                                                        "({ char __iv[256]; snprintf(__iv, 256, \"%%s\", %s); char *__r = _ltng_retbuf(); snprintf(__r, 256, \"%%d\", _ltng_arr_index_of(%s, %s_len, __iv)); __r; })",
                                                        arg_expr, arrname, lenname);
                                                }
                                                result_type = TYPE_STRING;
                                            } else if (strcmp(mname.value, "sort") == 0) {
                                                /* sort() */
                                                if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                                                    error_state_add(parser_errors, lexer_peek(lexer).line,
                                                        lexer_peek(lexer).col, "Expected ')' after sort");
                                                    skip_to_newline(lexer);
                                                    pos += snprintf(out + pos, out_size - pos, "0");
                                                    break;
                                                }
                                                lexer_next(lexer); /* consume ) */
                                                pos += snprintf(out + pos, out_size - pos,
                                                    "({ _ltng_arr_sort(%s, %s_len); char *__r = _ltng_retbuf(); snprintf(__r, 256, \"true\"); __r; })",
                                                    arrname, lenname);
                                                result_type = TYPE_STRING;
                                            } else if (strcmp(mname.value, "reverse") == 0) {
                                                /* reverse() */
                                                if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                                                    error_state_add(parser_errors, lexer_peek(lexer).line,
                                                        lexer_peek(lexer).col, "Expected ')' after reverse");
                                                    skip_to_newline(lexer);
                                                    pos += snprintf(out + pos, out_size - pos, "0");
                                                    break;
                                                }
                                                lexer_next(lexer); /* consume ) */
                                                pos += snprintf(out + pos, out_size - pos,
                                                    "({ _ltng_arr_reverse(%s, %s_len); char *__r = _ltng_retbuf(); snprintf(__r, 256, \"true\"); __r; })",
                                                    arrname, lenname);
                                                result_type = TYPE_STRING;
                                            }
                                            return result_type; /* skip postfix/array-access handling */
                                        }

                                        /* copy() and slice() are handled in let/const declarations,
                                           not as bare expressions. Rewind so the caller reports an error. */
                                        if ((mname.type == TOKEN_IDENT || token_is_name(mname)) &&
                                            (strcmp(mname.value, "copy") == 0 ||
                                             strcmp(mname.value, "slice") == 0)) {
                                            lexer->current = saved_dot;
                                            break;
                                        }

                                        /* Not a recognized array operation - rewind to the dot */
                                        lexer->current = saved_dot;
                                    }
                                }
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
                                    if (vt == TYPE_INT) {
                                        pos += snprintf(out + pos, out_size - pos,
                                            "({ long long _t = (long long)atoi(%s); snprintf(%s, sizeof(%s), \"%%lld\", _t + 1); _t; })",
                                            mangled2, mangled2, mangled2);
                                    } else if (vt == TYPE_FLOAT) {
                                        pos += snprintf(out + pos, out_size - pos,
                                            "({ double _t = atof(%s); snprintf(%s, sizeof(%s), \"%%g\", _t + 1.0); _t; })",
                                            mangled2, mangled2, mangled2);
                                    } else {
                                        pos += snprintf(out + pos, out_size - pos,
                                            "({ snprintf(%s, sizeof(%s), \"%%s\", %s); %s; })",
                                            mangled2, mangled2, mangled2, mangled2);
                                    }
                                } else { /* MINUS_MINUS */
                                    VarType vt = symbol_table_get(table, mangled2);
                                    if (vt == TYPE_INT) {
                                        pos += snprintf(out + pos, out_size - pos,
                                            "({ long long _t = (long long)atoi(%s); snprintf(%s, sizeof(%s), \"%%lld\", _t - 1); _t; })",
                                            mangled2, mangled2, mangled2);
                                    } else if (vt == TYPE_FLOAT) {
                                        pos += snprintf(out + pos, out_size - pos,
                                            "({ double _t = atof(%s); snprintf(%s, sizeof(%s), \"%%g\", _t - 1.0); _t; })",
                                            mangled2, mangled2, mangled2);
                                    } else {
                                        pos += snprintf(out + pos, out_size - pos,
                                            "({ snprintf(%s, sizeof(%s), \"%%s\", %s); %s; })",
                                            mangled2, mangled2, mangled2, mangled2);
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
                            char index_exprs[3][MAX_STRING_LEN];
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
                                    char errmsg[256];
                                    snprintf(errmsg, sizeof(errmsg),
                                        "Array '%s' has %d dimension(s) but %d index(es) were given",
                                        tok.value, dim_count, index_count);
                                    error_state_add(parser_errors, tok.line, tok.col, errmsg);
                                    pos += snprintf(out + pos, out_size - pos, "0");
                                } else {
                                    char arrname[MAX_IDENTIFIER_LEN];
                                    snprintf(arrname, sizeof(arrname), "%s_data", mangled);
                                    char lenname[MAX_IDENTIFIER_LEN];
                                    snprintf(lenname, sizeof(lenname), "%s_len", mangled);

                                    pos += snprintf(out + pos, out_size - pos, "({ ");
                                    for (int d = 0; d < index_count; d++) {
                                        pos += snprintf(out + pos, out_size - pos,
                                            "int __i%d = (int)(%s); __i%d = __i%d > 0 ? __i%d - 1 : __i%d + (%d); ",
                                            d, index_exprs[d], d, d, d, d, dims[d]);
                                    }
                                    /* Compute linear index: __idx = __i0; __idx = __idx * d1 + __i1; ... */
                                    pos += snprintf(out + pos, out_size - pos, "int __idx = __i0");
                                    for (int d = 1; d < index_count; d++) {
                                        pos += snprintf(out + pos, out_size - pos,
                                            "; __idx = __idx * %d + __i%d", dims[d], d);
                                    }
                                    pos += snprintf(out + pos, out_size - pos,
                                        "; _ltng_arr_get(%s, %s, __idx); })",
                                        arrname, lenname);
                                    result_type = TYPE_STRING;
                                    array_access_used = 1;
                                }
                                if (!have_dims) free(dims);
                            } else if (!index_error) {
                                pos += snprintf(out + pos, out_size - pos, "0");
                            }
                        } else {
                            char errmsg[256];
                            snprintf(errmsg, sizeof(errmsg), "Variable '%s' is not an array", tok.value);
                            error_state_add(parser_errors, tok.line, tok.col, errmsg);
                            pos += snprintf(out + pos, out_size - pos, "0");
                            /* Consume remaining index brackets so parsing stays in sync. */
                            while (lexer_peek(lexer).type == TOKEN_LBRACKET) {
                                lexer_next(lexer);
                                skip_to_newline(lexer);
                            }
                        }
                    }
                }
                }
                break;
            case TOKEN_LPAREN:
                pos += snprintf(out + pos, out_size - pos, "(");
                paren_depth++;
                break;
            case TOKEN_RPAREN:
                pos += snprintf(out + pos, out_size - pos, ")");
                if (paren_depth > 0) paren_depth--;
                break;
            case TOKEN_PLUS:
            case TOKEN_MINUS:
            case TOKEN_STAR:
            case TOKEN_SLASH:
            case TOKEN_PERCENT:
                if (result_type == TYPE_STRING) {
                    if (tok.type == TOKEN_PLUS) {
                        pos += snprintf(out + pos, out_size - pos, " + ");
                        arithmetic_used = 1;
                        has_string_concat = 1;
                    } else if (tok.type == TOKEN_STAR) {
                        pos += snprintf(out + pos, out_size - pos, " * ");
                        arithmetic_used = 1;
                        has_string_mult = 1;
                    } else {
                        error_state_add(parser_errors, tok.line, tok.col,
                            "Cannot use arithmetic on a string");
                        pos += snprintf(out + pos, out_size - pos, "0");
                    }
                } else {
                    pos += snprintf(out + pos, out_size - pos, "%s",
                        tok.type == TOKEN_PLUS ? "+" :
                        tok.type == TOKEN_MINUS ? "-" :
                        tok.type == TOKEN_STAR ? "*" :
                        tok.type == TOKEN_SLASH ? "/" : "%");
                    arithmetic_used = 1;
                }
                break;
            case TOKEN_AMP_AMP:
            case TOKEN_PIPE_PIPE: {
                Token next_tok = lexer_peek(lexer);
                if (result_type == TYPE_INT && token_is_int(next_tok, table)) {
                    pos += snprintf(out + pos, out_size - pos, "%s",
                        (tok.type == TOKEN_AMP_AMP) ? " & " : " | ");
                } else {
                    pos += snprintf(out + pos, out_size - pos, "%s",
                        (tok.type == TOKEN_AMP_AMP) ? " && " : " || ");
                }
                result_type = TYPE_INT;
                type_locked = 1;
                break;
            }
            case TOKEN_CARET_CARET:
                pos += snprintf(out + pos, out_size - pos, " ^ ");
                result_type = TYPE_INT;
                type_locked = 1;
                break;
            case TOKEN_BANG: {
                Token next_tok = lexer_peek(lexer);
                if (token_is_int(next_tok, table)) {
                    pos += snprintf(out + pos, out_size - pos, "~");
                } else {
                    pos += snprintf(out + pos, out_size - pos, "!");
                }
                result_type = TYPE_INT;
                type_locked = 1;
                break;
            }
            case TOKEN_EQ_EQ:   pos += snprintf(out + pos, out_size - pos, "=="); result_type = TYPE_INT; type_locked = 1; break;
            case TOKEN_BANG_EQ: pos += snprintf(out + pos, out_size - pos, "!="); result_type = TYPE_INT; type_locked = 1; break;
            case TOKEN_LT:      pos += snprintf(out + pos, out_size - pos, "<");  result_type = TYPE_INT; type_locked = 1; break;
            case TOKEN_GT:      pos += snprintf(out + pos, out_size - pos, ">");  result_type = TYPE_INT; type_locked = 1; break;
            case TOKEN_LT_EQ:   pos += snprintf(out + pos, out_size - pos, "<="); result_type = TYPE_INT; type_locked = 1; break;
            case TOKEN_GT_EQ:   pos += snprintf(out + pos, out_size - pos, ">="); result_type = TYPE_INT; type_locked = 1; break;
            case TOKEN_RETURN: case TOKEN_BREAK: case TOKEN_CONTINUE: case TOKEN_FUNCTION:
            case TOKEN_SWITCH: case TOKEN_CASE: case TOKEN_DEFAULT:
            case TOKEN_FROM: case TOKEN_TO: case TOKEN_STEP: {
                /* keyword-as-identifier in expressions (C-reserved compat) */
                char mangled[MAX_IDENTIFIER_LEN];
                mangle_name(tok.value, mangled, MAX_IDENTIFIER_LEN);
                if (!symbol_table_has(table, mangled)) {
                    char errmsg[256];
                    snprintf(errmsg, sizeof(errmsg), "Variable '%s' not declared", tok.value);
                    error_state_add(parser_errors, tok.line, tok.col, errmsg);
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
            default: break;
        }
        if (pos >= out_size - 1) break;
    }
    out[pos] = '\0';
    sanitize_mod_atof(out, out_size);
    /* Replace atof(X)+atof(Y) / chained add patterns with runtime string/number add.
       When a replacement happens, result_type becomes STRING (_ltng_add_sn returns char*). */
    if (!has_string_concat && !has_string_mult && result_type != TYPE_STRING) {
        while (optimize_add_concat(out, out_size, pos)) {
            result_type = TYPE_STRING;
        }
    }
    if (has_string_concat || has_string_mult || (result_type == TYPE_STRING && (strstr(out, "+") != NULL || strstr(out, "*") != NULL))) {
        /* Handle multiplication with higher precedence - split by + at top level first */
        if (strstr(out, "+") != NULL) {
            /* split by + */
            char tmp[2048];
            strncpy(tmp, out, 2047);
            tmp[2047] = 0;
            char parts[32][512];
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
                    strncpy(parts[nparts++], s, 511);
                    parts[nparts-1][511] = 0;
                    start = p + 1;
                }
            }
            {
                char *s = start;
                while (*s == ' ' || *s == '\t') s++;
                char *e = s + strlen(s) - 1;
                while (e >= s && (*e == ' ' || *e == '\t')) { *e = 0; e--; }
                strncpy(parts[nparts++], s, 511);
                parts[nparts-1][511] = 0;
            }
            /* transform each part */
            for (int i = 0; i < nparts; i++) {
                transform_string_multiply(parts[i], 512);
            }
            /* If no actual concatenation (single part), skip wrapping */
            if (nparts <= 1) {
                /* No concatenation needed, keep original expression */
            } else if (array_access_used) {
                /* Array elements are string buffers; use the runtime
                   numeric-or-string add so `arr[1] + arr[2]` adds numbers
                   and concatenates strings. */
                char cparts[32][600];
                for (int i = 0; i < nparts; i++) {
                    const char *part = parts[i];
                    if (!part_is_stringy(part)) {
                        int has_dot = (strchr(part, '.') != NULL);
                        snprintf(cparts[i], sizeof(cparts[i]),
                                 "({ char __ltcn[64]; snprintf(__ltcn,64,\"%s\",(%s)(%s)); __ltcn; })",
                                 has_dot ? "%g" : "%lld",
                                 has_dot ? "double" : "long long",
                                 part);
                    } else {
                        strncpy(cparts[i], part, sizeof(cparts[i]) - 1);
                        cparts[i][sizeof(cparts[i]) - 1] = 0;
                    }
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
            /* concatenate */
            char fmt[1024] = {0};
            for (int i = 0; i < nparts; i++) strcat(fmt, "%s");
            char result[2048] = {0};
            snprintf(result, 2047, "({ char _ltc[512]; snprintf(_ltc,512,\"");
            for (int i = 0; i < nparts; i++) strcat(result, "%s");
            strcat(result, "\", ");
            for (int i = 0; i < nparts; i++) {
                const char *part = parts[i];
                if (!part_is_stringy(part)) {
                    int has_dot = (strchr(part, '.') != NULL);
                    char conv[560];
                    snprintf(conv, sizeof(conv), "({ char __ltcn[64]; snprintf(__ltcn,64,\"%s\",(%s)(%s)); __ltcn; })",
                             has_dot ? "%g" : "%lld",
                             has_dot ? "double" : "long long",
                             part);
                    strcat(result, conv);
                } else {
                    strcat(result, part);
                }
                if (i < nparts - 1) strcat(result, ",");
            }
            strcat(result, "); _ltc; })");
            strncpy(out, result, out_size - 1);
            out[out_size - 1] = 0;
            result_type = TYPE_STRING;
            }
        } else if (strstr(out, "*") != NULL) {
            transform_string_multiply(out, out_size);
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
    }
    return "string";
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
                if (strncmp(vname, "typeof(", 7) == 0 && vname[vlen - 1] == ')') {
                    vname[vlen - 1] = '\0';
                    memmove(vname, vname + 7, strlen(vname + 7) + 1);
                    
                    /* typeof(toint("5")) → "int", typeof(tofloat("5")) → "float",
                       typeof(tostring("x")) → "string", typeof(tochar("x")) → "char",
                       typeof(tobool("t")) → "bool" */
                    if (strncmp(vname, "toint(", 6) == 0) {
                        rlen += snprintf(resolved + rlen, MAX_STRING_LEN - rlen, "int");
                    } else if (strncmp(vname, "tofloat(", 8) == 0) {
                        rlen += snprintf(resolved + rlen, MAX_STRING_LEN - rlen, "float");
                    } else if (strncmp(vname, "tostring(", 9) == 0) {
                        rlen += snprintf(resolved + rlen, MAX_STRING_LEN - rlen, "string");
                    } else if (strncmp(vname, "tochar(", 7) == 0) {
                        rlen += snprintf(resolved + rlen, MAX_STRING_LEN - rlen, "char");
                    } else if (strncmp(vname, "tobool(", 7) == 0) {
                        rlen += snprintf(resolved + rlen, MAX_STRING_LEN - rlen, "bool");
                    } else {
                        /* typeof(func(...)) for user-defined functions */
                        const char *p = vname;
                        while (*p && (isalnum((unsigned char)*p) || *p == '_')) p++;
                        if (p > vname && *p == '(') {
                            char fname[MAX_IDENTIFIER_LEN];
                            int flen = (int)(p - vname);
                            if (flen >= MAX_IDENTIFIER_LEN) flen = MAX_IDENTIFIER_LEN - 1;
                            memcpy(fname, vname, flen);
                            fname[flen] = '\0';
                            if (find_function_ast_by_name(fname)) {
                                VarType ftype = infer_function_return_type(fname);
                                rlen += snprintf(resolved + rlen, MAX_STRING_LEN - rlen, "%s", var_type_name(ftype));
                            } else {
                                rlen += snprintf(resolved + rlen, MAX_STRING_LEN - rlen, "string");
                            }
                        } else {
                            /* Nested typeof or variable lookup */
                            int is_nested = (strncmp(vname, "typeof(", 7) == 0);
                            if (is_nested) {
                                while (strncmp(vname, "typeof(", 7) == 0) {
                                    int nl = (int)strlen(vname);
                                    if (vname[nl - 1] != ')') break;
                                    vname[nl - 1] = '\0';
                                    memmove(vname, vname + 7, strlen(vname + 7) + 1);
                                }
                                char mvname[MAX_IDENTIFIER_LEN];
                                mangle_name(vname, mvname, MAX_IDENTIFIER_LEN);
                                if (!symbol_table_has(table, mvname) && !is_type_keyword(vname) && !is_bool_value(vname)) {
                                    char errmsg[256];
                                    snprintf(errmsg, sizeof(errmsg),
                                        "Variable '%s' not declared (used in typeof)", vname);
                                    error_state_add(parser_errors, line, col, errmsg);
                                }
                                const char *tname = "string";
                                int tlen = (int)strlen(tname);
                                if (rlen + tlen < MAX_STRING_LEN) {
                                    memcpy(resolved + rlen, tname, tlen);
                                    rlen += tlen;
                                }
                            } else {
                                char mvname[MAX_IDENTIFIER_LEN];
                                mangle_name(vname, mvname, MAX_IDENTIFIER_LEN);
                                if (!symbol_table_has(table, mvname) && !is_type_keyword(vname) && !is_bool_value(vname)) {
                                    char errmsg[256];
                                    snprintf(errmsg, sizeof(errmsg),
                                        "Variable '%s' not declared (used in typeof)", vname);
                                    error_state_add(parser_errors, line, col, errmsg);
                                } else {
                                    const char *tname = "unknown";
                                    if (symbol_table_has(table, mvname)) {
                                        tname = var_type_name(symbol_table_get(table, mvname));
                                    } else if (is_bool_value(vname)) {
                                        tname = "bool";
                                    } else if (is_type_keyword(vname)) {
                                        tname = vname;
                                    }
                                    int tlen = (int)strlen(tname);
                                    if (rlen + tlen < MAX_STRING_LEN) {
                                        memcpy(resolved + rlen, tname, tlen);
                                        rlen += tlen;
                                    }
                                }
                            }
                        }
                    }
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
                            char errmsg[256];
                            snprintf(errmsg, sizeof(errmsg),
                                "Variable '%s' not declared (used in string interpolation)", vname);
                            error_state_add(parser_errors, line, col, errmsg);
                        }
                        int voff = (int)(vstart - 2 - tok->value);
                        int vtotal = (int)(sp - (vstart - 2) + 1);
                        for (int ci = voff; ci < voff + vtotal && rlen < MAX_STRING_LEN - 1; ci++) {
                            resolved[rlen++] = tok->value[ci];
                        }
                    } else {
                        /* Check for type conversion calls inside interpolation:
                           $[toint("10")], $[tostring("x")], $[tobool("t")]
                           all return string results */
                        if (strncmp(vname, "toint(", 6) == 0 && vname[strlen(vname) - 1] == ')') {
                            char inner[MAX_STRING_LEN];
                            int ilen = (int)strlen(vname) - 7; /* strip "toint(" and ")" */
                            memcpy(inner, vname + 6, ilen);
                            inner[ilen] = '\0';
                            /* Trim whitespace */
                            while (ilen > 0 && (inner[ilen-1] == ' ' || inner[ilen-1] == '\t')) ilen--;
                            inner[ilen] = '\0';
                            if (ilen >= 2 && inner[0] == '"' && inner[ilen-1] == '"') {
                                /* String literal - strip quotes */
                                char clean[MAX_STRING_LEN];
                                int cpos = 0;
                                for (int i = 1; i < ilen - 1 && cpos < MAX_STRING_LEN - 1; i++) {
                                    if (inner[i] == '\\' && i + 1 < ilen - 1) {
                                        clean[cpos++] = inner[i];
                                        clean[cpos++] = inner[++i];
                                    } else {
                                        clean[cpos++] = inner[i];
                                    }
                                }
                                clean[cpos] = '\0';
                                rlen += snprintf(resolved + rlen, MAX_STRING_LEN - rlen, "\"%s\"", clean);
                            } else {
                                /* Variable or number - pass through as string */
                                rlen += snprintf(resolved + rlen, MAX_STRING_LEN - rlen, "\"%s\"", inner);
                            }
                        } else if (strncmp(vname, "tostring(", 9) == 0 && vname[strlen(vname) - 1] == ')') {
                            char inner[MAX_STRING_LEN];
                            int ilen = (int)strlen(vname) - 10; /* strip "tostring(" and ")" */
                            memcpy(inner, vname + 9, ilen);
                            inner[ilen] = '\0';
                            while (ilen > 0 && (inner[ilen-1] == ' ' || inner[ilen-1] == '\t')) ilen--;
                            inner[ilen] = '\0';
                            if (ilen == 4 && memcmp(inner, "true", 4) == 0) {
                                rlen += snprintf(resolved + rlen, MAX_STRING_LEN - rlen, "\"true\"");
                            } else if (ilen == 5 && memcmp(inner, "false", 5) == 0) {
                                rlen += snprintf(resolved + rlen, MAX_STRING_LEN - rlen, "\"false\"");
                            } else {
                                rlen += snprintf(resolved + rlen, MAX_STRING_LEN - rlen, "\"%s\"", inner);
                            }
                        } else if (strncmp(vname, "tobool(", 7) == 0 && vname[strlen(vname) - 1] == ')') {
                            char inner[MAX_STRING_LEN];
                            int ilen = (int)strlen(vname) - 8; /* strip "tobool(" and ")" */
                            memcpy(inner, vname + 7, ilen);
                            inner[ilen] = '\0';
                            while (ilen > 0 && (inner[ilen-1] == ' ' || inner[ilen-1] == '\t')) ilen--;
                            inner[ilen] = '\0';
                            if (ilen == 1 && inner[0] == '0') {
                                rlen += snprintf(resolved + rlen, MAX_STRING_LEN - rlen, "\"false\"");
                            } else if (ilen >= 1 && inner[0] >= '1' && inner[0] <= '9') {
                                rlen += snprintf(resolved + rlen, MAX_STRING_LEN - rlen, "\"true\"");
                            } else if (ilen >= 2 && inner[0] == '"') {
                                if (ilen == 2 || (ilen == 3 && inner[1] == '0') ||
                                    (ilen == 7 && memcmp(inner + 1, "false", 5) == 0)) {
                                    rlen += snprintf(resolved + rlen, MAX_STRING_LEN - rlen, "\"false\"");
                                } else {
                                    rlen += snprintf(resolved + rlen, MAX_STRING_LEN - rlen, "\"true\"");
                                }
                            } else {
                                rlen += snprintf(resolved + rlen, MAX_STRING_LEN - rlen, "\"true\"");
                            }
                        } else {
                            /* General expression - parse and use result */
                            char check[MAX_STRING_LEN];
                            (void)parse_expression_from_string(vname, table, check, sizeof(check));
                            rlen += snprintf(resolved + rlen, MAX_STRING_LEN - rlen, "%s", check);
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
       typeof(tostring(...)) → string, typeof(tobool(...)) → bool */
    if (arg.type == TOKEN_IDENT && is_type_conversion(arg.value) &&
        lexer_peek(lexer).type == TOKEN_LPAREN) {
        lexer_next(lexer); /* consume ( */
        int depth = 1;
        while (lexer->current < lexer->count && depth > 0) {
            Token itok = lexer_peek(lexer);
            if (itok.type == TOKEN_LPAREN) depth++;
            else if (itok.type == TOKEN_RPAREN) { depth--; if (depth == 0) break; }
            else if (itok.type == TOKEN_NEWLINE || itok.type == TOKEN_EOF) break;
            lexer_next(lexer);
        }
        if (lexer->current < lexer->count && lexer_peek(lexer).type == TOKEN_RPAREN) {
            lexer_next(lexer); /* consume inner ) */
        }
        Token close = lexer_next(lexer); /* consume outer ) */
        if (close.type != TOKEN_RPAREN) {
            error_state_add(parser_errors, arg.line, arg.col,
                "Expected ')' after typeof argument");
            lexer->current = saved_pos;
            return 0;
        }
        VarType cvt;
        if (strcmp(arg.value, "toint") == 0) cvt = TYPE_INT;
        else if (strcmp(arg.value, "tofloat") == 0) cvt = TYPE_FLOAT;
        else if (strcmp(arg.value, "tostring") == 0) cvt = TYPE_STRING;
        else if (strcmp(arg.value, "tochar") == 0) cvt = TYPE_CHAR;
        else cvt = TYPE_BOOL; /* tobool */
        result->type = TOKEN_STRING;
        strncpy(result->value, var_type_name(cvt), MAX_STRING_LEN - 1);
        result->value[MAX_STRING_LEN - 1] = '\0';
        result->line = typeof_tok.line;
        result->col = typeof_tok.col;
        return 1;
    }

    /* User-defined function call inside typeof: typeof(add(1, 2)) */
    if (arg.type == TOKEN_IDENT && lexer_peek(lexer).type == TOKEN_LPAREN) {
        /* Check if this is a known function */
        if (find_function_ast_by_name(arg.value)) {
            lexer_next(lexer); /* consume ( */
            int depth = 1;
            while (lexer->current < lexer->count && depth > 0) {
                Token itok = lexer_peek(lexer);
                if (itok.type == TOKEN_LPAREN) depth++;
                else if (itok.type == TOKEN_RPAREN) { depth--; if (depth == 0) break; }
                else if (itok.type == TOKEN_NEWLINE || itok.type == TOKEN_EOF) break;
                lexer_next(lexer);
            }
            if (lexer->current < lexer->count && lexer_peek(lexer).type == TOKEN_RPAREN) {
                lexer_next(lexer); /* consume inner ) */
            }
            Token close = lexer_next(lexer); /* consume outer ) */
            if (close.type != TOKEN_RPAREN) {
                error_state_add(parser_errors, arg.line, arg.col,
                    "Expected ')' after typeof argument");
                lexer->current = saved_pos;
                return 0;
            }
            VarType ftype = infer_function_return_type(arg.value);
            result->type = TOKEN_STRING;
            strncpy(result->value, var_type_name(ftype), MAX_STRING_LEN - 1);
            result->value[MAX_STRING_LEN - 1] = '\0';
            result->line = typeof_tok.line;
            result->col = typeof_tok.col;
            return 1;
        }
    }

    /* Simple single-token argument followed by ) */
    Token close = lexer_next(lexer);
    if (close.type != TOKEN_RPAREN) {
        error_state_add(parser_errors, arg.line, arg.col,
            "Expected ')' after typeof argument");
        lexer->current = saved_pos;
        return 0;
    }

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
        vtype = symbol_table_get(table, mangled);
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
    } else if (arg.type == TOKEN_IDENT && is_type_conversion(arg.value) &&
               lexer_peek(lexer).type == TOKEN_LPAREN) {
        /* Type conversion call inside typeof: typeof(int(...)), typeof(string(...)),
           typeof(bool(...)) - all return strings */
        lexer_next(lexer); /* consume ( */
        int depth = 1;
        while (lexer->current < lexer->count && depth > 0) {
            Token itok = lexer_peek(lexer);
            if (itok.type == TOKEN_LPAREN) depth++;
            else if (itok.type == TOKEN_RPAREN) { depth--; if (depth == 0) break; }
            else if (itok.type == TOKEN_NEWLINE || itok.type == TOKEN_EOF) break;
            lexer_next(lexer);
        }
        if (lexer->current < lexer->count && lexer_peek(lexer).type == TOKEN_RPAREN) {
            lexer_next(lexer); /* consume ) */
        }
        result->type = TOKEN_STRING;
        strncpy(result->value, "string", MAX_STRING_LEN - 1);
        result->value[MAX_STRING_LEN - 1] = '\0';
        result->line = typeof_tok.line;
        result->col = typeof_tok.col;
        return 1;
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
                char errmsg[256];
                snprintf(errmsg, sizeof(errmsg), "Variable '%s' not declared", first.value);
                error_state_add(parser_errors, first.line, first.col, errmsg);
                skip_to_newline(lexer);
                return;
            }
            VarType vt = symbol_table_get(table, rm);
            if (vt != TYPE_ARRAY) {
                char errmsg[256];
                snprintf(errmsg, sizeof(errmsg), "Variable '%s' is not an array", first.value);
                error_state_add(parser_errors, first.line, first.col, errmsg);
                skip_to_newline(lexer);
                return;
            }
            char arrname[MAX_IDENTIFIER_LEN];
            snprintf(arrname, sizeof(arrname), "%s_data", rm);
            char lenname[MAX_IDENTIFIER_LEN];
            snprintf(lenname, sizeof(lenname), "%s_len", rm);
            char capname[MAX_IDENTIFIER_LEN];
            snprintf(capname, sizeof(capname), "%s_cap", rm);
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
                    "({ char __apv[256]; snprintf(__apv, 256, \"%s\", %s); _ltng_arr_push(&%s, &%s, &%s, __apv); char *__r = _ltng_retbuf(); snprintf(__r, 256, \"%%s\", __apv); __r; })",
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
                    "({ char __apv[256]; snprintf(__apv, 256, \"%s\", %s); _ltng_arr_insert(&%s, &%s, &%s, (int)(%s), __apv); char *__r = _ltng_retbuf(); snprintf(__r, 256, \"%%s\", __apv); __r; })",
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
                        "({ char __cv[256]; snprintf(__cv, 256, \"%%lld\", (long long)(%s)); char *__r = _ltng_retbuf(); snprintf(__r, 256, \"%%s\", _ltng_arr_contains(%s, %s, __cv) ? \"true\" : \"false\"); __r; })",
                        arg_expr, arrname, lenname);
                } else if (arg_type == TYPE_FLOAT) {
                    snprintf(expr, sizeof(expr),
                        "({ char __cv[256]; snprintf(__cv, 256, \"%%g\", (double)(%s)); char *__r = _ltng_retbuf(); snprintf(__r, 256, \"%%s\", _ltng_arr_contains(%s, %s, __cv) ? \"true\" : \"false\"); __r; })",
                        arg_expr, arrname, lenname);
                } else {
                    snprintf(expr, sizeof(expr),
                        "({ char __cv[256]; snprintf(__cv, 256, \"%%s\", %s); char *__r = _ltng_retbuf(); snprintf(__r, 256, \"%%s\", _ltng_arr_contains(%s, %s, __cv) ? \"true\" : \"false\"); __r; })",
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
                        "({ char __iv[256]; snprintf(__iv, 256, \"%%lld\", (long long)(%s)); char *__r = _ltng_retbuf(); snprintf(__r, 256, \"%%d\", _ltng_arr_index_of(%s, %s, __iv)); __r; })",
                        arg_expr, arrname, lenname);
                } else if (arg_type == TYPE_FLOAT) {
                    snprintf(expr, sizeof(expr),
                        "({ char __iv[256]; snprintf(__iv, 256, \"%%g\", (double)(%s)); char *__r = _ltng_retbuf(); snprintf(__r, 256, \"%%d\", _ltng_arr_index_of(%s, %s, __iv)); __r; })",
                        arg_expr, arrname, lenname);
                } else {
                    snprintf(expr, sizeof(expr),
                        "({ char __iv[256]; snprintf(__iv, 256, \"%%s\", %s); char *__r = _ltng_retbuf(); snprintf(__r, 256, \"%%d\", _ltng_arr_index_of(%s, %s, __iv)); __r; })",
                        arg_expr, arrname, lenname);
                }
                ASTNode *node = create_node(NODE_WRITE, "", expr);
                node->is_variable = 0;
                node->is_expr = 1;
                node->var_type = TYPE_STRING;
                emit(node);
                return;
            } else if (strcmp(third_tok.value, "sort") == 0) {
                if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                    error_state_add(parser_errors, lexer_peek(lexer).line,
                        lexer_peek(lexer).col, "Expected ')' after sort");
                    skip_to_newline(lexer);
                    return;
                }
                lexer_next(lexer); /* consume sort's ) */
                lexer_next(lexer); /* consume write's ) */
                char expr[MAX_STRING_LEN];
                snprintf(expr, sizeof(expr),
                    "({ _ltng_arr_sort(%s, %s_len); char *__r = _ltng_retbuf(); snprintf(__r, 256, \"true\"); __r; })",
                    arrname, lenname);
                ASTNode *node = create_node(NODE_WRITE, "", expr);
                node->is_variable = 0;
                node->is_expr = 1;
                node->var_type = TYPE_STRING;
                emit(node);
                return;
            } else if (strcmp(third_tok.value, "reverse") == 0) {
                if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                    error_state_add(parser_errors, lexer_peek(lexer).line,
                        lexer_peek(lexer).col, "Expected ')' after reverse");
                    skip_to_newline(lexer);
                    return;
                }
                lexer_next(lexer); /* consume reverse's ) */
                lexer_next(lexer); /* consume write's ) */
                char expr[MAX_STRING_LEN];
                snprintf(expr, sizeof(expr),
                    "({ _ltng_arr_reverse(%s, %s_len); char *__r = _ltng_retbuf(); snprintf(__r, 256, \"true\"); __r; })",
                    arrname, lenname);
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
    }

    Token second = lexer->tokens[lexer->current + 1];
    int is_conv_call = (first.type == TOKEN_IDENT && is_type_conversion(first.value) &&
                        second.type == TOKEN_LPAREN);

    if (first.type == TOKEN_LPAREN || first.type == TOKEN_MINUS || first.type == TOKEN_BANG ||
        first.type == TOKEN_PLUS_PLUS || first.type == TOKEN_MINUS_MINUS ||
        has_operator_ahead(lexer) || is_conv_call) {
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
                        if (strncmp(vname, "typeof(", 7) == 0 && vname[vlen - 1] == ')') {
                            vname[vlen - 1] = '\0';
                            memmove(vname, vname + 7, strlen(vname + 7) + 1);
                            
                            /* typeof(toint("5")) → "int", typeof(tostring("x")) → "string",
                               typeof(tobool("t")) → "bool" */
                            if (strncmp(vname, "toint(", 6) == 0) {
                                rlen += snprintf(resolved + rlen, MAX_STRING_LEN - rlen, "int");
                            } else if (strncmp(vname, "tostring(", 9) == 0) {
                                rlen += snprintf(resolved + rlen, MAX_STRING_LEN - rlen, "string");
                            } else if (strncmp(vname, "tobool(", 7) == 0) {
                                rlen += snprintf(resolved + rlen, MAX_STRING_LEN - rlen, "bool");
                            } else {
                                /* typeof(func(...)) for user-defined functions */
                                const char *p = vname;
                                while (*p && (isalnum((unsigned char)*p) || *p == '_')) p++;
                                if (p > vname && *p == '(') {
                                    char fname[MAX_IDENTIFIER_LEN];
                                    int flen = (int)(p - vname);
                                    if (flen >= MAX_IDENTIFIER_LEN) flen = MAX_IDENTIFIER_LEN - 1;
                                    memcpy(fname, vname, flen);
                                    fname[flen] = '\0';
                                    if (find_function_ast_by_name(fname)) {
                                        VarType ftype = infer_function_return_type(fname);
                                        rlen += snprintf(resolved + rlen, MAX_STRING_LEN - rlen, "%s", var_type_name(ftype));
                                    } else {
                                        rlen += snprintf(resolved + rlen, MAX_STRING_LEN - rlen, "string");
                                    }
                                } else {
                                    /* Nested typeof or variable lookup */
                                    int is_nested = (strncmp(vname, "typeof(", 7) == 0);
                                    if (is_nested) {
                                        while (strncmp(vname, "typeof(", 7) == 0) {
                                            int nl = (int)strlen(vname);
                                            if (vname[nl - 1] != ')') break;
                                            vname[nl - 1] = '\0';
                                            memmove(vname, vname + 7, strlen(vname + 7) + 1);
                                        }
                                        char mvname[MAX_IDENTIFIER_LEN];
                                        mangle_name(vname, mvname, MAX_IDENTIFIER_LEN);
                                        if (!symbol_table_has(table, mvname) && !is_type_keyword(vname) && !is_bool_value(vname)) {
                                            char errmsg[256];
                                            snprintf(errmsg, sizeof(errmsg),
                                                "Variable '%s' not declared (used in typeof)", vname);
                                            error_state_add(parser_errors, arg.line, arg.col, errmsg);
                                        }
                                        const char *tname = "string";
                                        int tlen = (int)strlen(tname);
                                        if (rlen + tlen < MAX_STRING_LEN) {
                                            memcpy(resolved + rlen, tname, tlen);
                                            rlen += tlen;
                                        }
                                    } else {
                                        char mvname[MAX_IDENTIFIER_LEN];
                                        mangle_name(vname, mvname, MAX_IDENTIFIER_LEN);
                                        if (!symbol_table_has(table, mvname) && !is_type_keyword(vname) && !is_bool_value(vname)) {
                                            char errmsg[256];
                                            snprintf(errmsg, sizeof(errmsg),
                                                "Variable '%s' not declared (used in typeof)", vname);
                                            error_state_add(parser_errors, arg.line, arg.col, errmsg);
                                        } else {
                                            const char *tname = "unknown";
                                            if (symbol_table_has(table, mvname)) {
                                                tname = var_type_name(symbol_table_get(table, mvname));
                                            } else if (is_bool_value(vname)) {
                                                tname = "bool";
                                            } else if (is_type_keyword(vname)) {
                                                tname = vname;
                                            }
                                            int tlen = (int)strlen(tname);
                                            if (rlen + tlen < MAX_STRING_LEN) {
                                                memcpy(resolved + rlen, tname, tlen);
                                                rlen += tlen;
                                            }
                                        }
                                    }
                                }
                            }
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
                                    char errmsg[256];
                                    snprintf(errmsg, sizeof(errmsg),
                                        "Variable '%s' not declared (used in string interpolation)", vname);
                                    error_state_add(parser_errors, arg.line, arg.col, errmsg);
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
                } else if (is_function_name_scoped(table, arg.value) && lexer_peek(lexer).type == TOKEN_LPAREN) {
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
                    error_state_add(parser_errors, mname.line, mname.col,
                        "method call with 'base' parameter cannot be used as an expression");
                    skip_to_newline(lexer);
                    return;
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
static void build_call_cexpr(ASTNode *call, char *cexpr, int cexpr_size) {
    int cpos = 0;
    int cline = call->line > 0 ? call->line : 1;
    int ccol = call->col > 0 ? call->col : 1;
    cpos += snprintf(cexpr + cpos, cexpr_size - cpos, "%s(_ltng_retbuf(), %d, %d", call->name, cline, ccol);
    for (ASTNode *a = call->args; a; a = a->next) {
        if (a->var_type == TYPE_STRING && a->value[0] == '"') {
            cpos += snprintf(cexpr + cpos, cexpr_size - cpos, ", %s", a->value);
        } else if (a->is_variable && a->value[0] != '\0') {
            cpos += snprintf(cexpr + cpos, cexpr_size - cpos, ", %s", a->value);
        } else if ((a->var_type == TYPE_STRING || a->var_type == TYPE_OBJECT) && a->value[0] != '\0') {
            cpos += snprintf(cexpr + cpos, cexpr_size - cpos, ", %s", a->value);
            } else if ((a->var_type == TYPE_STRING || a->var_type == TYPE_OBJECT) && a->value[0] != '\0') {
                cpos += snprintf(cexpr + cpos, cexpr_size - cpos, ", %s", a->value);
            } else if (a->value[0] == '\0') {
                cpos += snprintf(cexpr + cpos, cexpr_size - cpos, ", \"\"");
            } else {
                cpos += snprintf(cexpr + cpos, cexpr_size - cpos, ", _ltng_ad((double)(%s))", a->value);
            }
    }
    cpos += snprintf(cexpr + cpos, cexpr_size - cpos, ")");
    cexpr[cexpr_size - 1] = '\0';
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

static int try_parse_sole_call_expr(Lexer *lexer, SymbolTable *table, char *cexpr, int cexpr_size, int allow_base_assign) {
    Token first = lexer_peek(lexer);
    int saved = lexer->current;
    if (first.type == TOKEN_IDENT && !is_function_name_scoped(table, first.value)) {
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
                ASTNode *call = parse_call_node(lexer, table, mname.value, mname.line, mname.col, recv);
                if (call->is_base_assign && !allow_base_assign) {
                    lexer->current = saved; return 0;
                }
                Token after = lexer_peek(lexer);
                if (after.type != TOKEN_NEWLINE && after.type != TOKEN_EOF && after.type != TOKEN_RBRACE) {
                    lexer->current = saved; return 0;
                }
                if (allow_base_assign) call->is_base_assign = 0;
                build_call_cexpr(call, cexpr, cexpr_size);
                return 1;
            }
        }
        lexer->current = saved;
    }
    if (first.type == TOKEN_IDENT && is_function_name_scoped(table, first.value)) {
        Token fname = lexer_next(lexer);
        if (lexer_peek(lexer).type != TOKEN_LPAREN) { lexer->current = saved; return 0; }
        lexer_next(lexer); /* consume ( */
        ASTNode *call = parse_call_node(lexer, table, fname.value, fname.line, fname.col, NULL);
        if (call->is_base_assign && !allow_base_assign) {
            lexer->current = saved; return 0;
        }
        Token after = lexer_peek(lexer);
        if (after.type != TOKEN_NEWLINE && after.type != TOKEN_EOF && after.type != TOKEN_RBRACE) {
            lexer->current = saved; return 0;
        }
        if (allow_base_assign) call->is_base_assign = 0;
        build_call_cexpr(call, cexpr, cexpr_size);
        return 1;
    }
    lexer->current = saved;
    return 0;
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
        symbol_table_add_locked(table, mangled, TYPE_OBJECT);
        if (is_const) symbol_table_set_const(table, mangled, 1);
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
        symbol_table_add_locked(table, mangled, TYPE_ARRAY);
        symbol_table_set_array_dims(table, mangled, arr->array_dim_count, arr->array_dims);
        const char *cname = symbol_table_get_cname(table, mangled);
        ASTNode *node = create_node(NODE_LET, cname, arr->value);
        node->var_type = TYPE_ARRAY;
        node->array_dim_count = arr->array_dim_count;
        node->array_dims = malloc(arr->array_dim_count * sizeof(int));
        for (int d = 0; d < arr->array_dim_count; d++) {
            node->array_dims[d] = arr->array_dims[d];
        }
        node->array_elem_count = arr->array_elem_count;
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
                    symbol_table_add_locked(table, mangled, TYPE_ARRAY);
                    const char *dcname = symbol_table_get_cname(table, mangled);
                    ASTNode *node = create_node(NODE_LET, dcname, "");
                    node->var_type = TYPE_ARRAY;
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
                        symbol_table_add_locked(table, mangled, TYPE_ARRAY);
                        const char *dcname = symbol_table_get_cname(table, mangled);
                        ASTNode *node = create_node(NODE_LET, dcname, "");
                        node->var_type = TYPE_ARRAY;
                        node->is_expr = 0;
                        emit(node);
                        ASTNode *cp = create_node(NODE_ARRAY_COPY, dcname, (char *)scname);
                        cp->is_expr = 1;
                        emit(cp);
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
                        symbol_table_add_locked(table, mangled, TYPE_ARRAY);
                        const char *dcname = symbol_table_get_cname(table, mangled);
                        ASTNode *node = create_node(NODE_LET, dcname, "");
                        node->var_type = TYPE_ARRAY;
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

    if (next.type == TOKEN_IDENT && is_type_keyword(next.value) && !is_type_conv_call) {
        Token type_tok = lexer_next(lexer);
        vtype = type_keyword_to_vartype(type_tok.value);

        Token peek_val = lexer_peek(lexer);
        if (peek_val.type == TOKEN_NEWLINE || peek_val.type == TOKEN_EOF) {
            /* Type declaration without value - init to default */
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
            Token arg = lexer_peek(lexer);
            char expr[MAX_STRING_LEN] = "0";
            VarType expr_type = TYPE_STRING;
            int handled = 0;
            /* Simple argument: single literal or identifier */
            if ((arg.type == TOKEN_STRING || arg.type == TOKEN_INT_LIT || arg.type == TOKEN_FLOAT_LIT ||
                 arg.type == TOKEN_CHAR || arg.type == TOKEN_IDENT) &&
                lexer->tokens[lexer->current + 1].type == TOKEN_RPAREN) {
                lexer_next(lexer); /* consume arg */
                lexer_next(lexer); /* consume ) */
                handled = 1;
                if (strcmp(conv.value, "toint") == 0 || strcmp(conv.value, "int") == 0) {
                    expr_type = TYPE_INT;
                    if (arg.type == TOKEN_STRING) snprintf(expr, sizeof(expr), "(long long)atoll(\"%s\")", arg.value);
                    else if (arg.type == TOKEN_INT_LIT || arg.type == TOKEN_FLOAT_LIT) snprintf(expr, sizeof(expr), "%lld", (long long)atoll(arg.value));
                    else if (arg.type == TOKEN_CHAR) snprintf(expr, sizeof(expr), "%lld", (long long)(unsigned char)arg.value[0]);
                    else if (arg.type == TOKEN_IDENT && is_bool_value(arg.value)) snprintf(expr, sizeof(expr), "%d", (strcmp(arg.value,"true")==0||strcmp(arg.value,"t")==0)?1:0);
                    else { /* ident var */ char m[MAX_IDENTIFIER_LEN]; mangle_name(arg.value,m,MAX_IDENTIFIER_LEN); if(symbol_table_has(table,m)) snprintf(expr,sizeof(expr),"(long long)atoll(%s)", symbol_table_get_cname(table,m)); else snprintf(expr,sizeof(expr),"0"); }
                } else if (strcmp(conv.value, "tofloat") == 0 || strcmp(conv.value, "float") == 0) {
                    expr_type = TYPE_FLOAT;
                    if (arg.type == TOKEN_STRING) snprintf(expr, sizeof(expr), "(double)atof(\"%s\")", arg.value);
                    else if (arg.type == TOKEN_INT_LIT || arg.type == TOKEN_FLOAT_LIT) snprintf(expr, sizeof(expr), "%.17g", atof(arg.value));
                    else if (arg.type == TOKEN_IDENT && is_bool_value(arg.value)) snprintf(expr, sizeof(expr), "%d.0", (strcmp(arg.value,"true")==0||strcmp(arg.value,"t")==0)?1:0);
                    else { char m[MAX_IDENTIFIER_LEN]; mangle_name(arg.value,m,MAX_IDENTIFIER_LEN); if(symbol_table_has(table,m)) snprintf(expr,sizeof(expr),"(double)atof(%s)", symbol_table_get_cname(table,m)); else snprintf(expr,sizeof(expr),"0.0"); }
                } else if (strcmp(conv.value, "tobool") == 0 || strcmp(conv.value, "bool") == 0) {
                    if (arg.type == TOKEN_IDENT && !is_bool_value(arg.value)) {
                        handled = 0; /* runtime variable: fall back to legacy */
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
                    if (arg.type == TOKEN_STRING) snprintf(expr,sizeof(expr),"\"%s\"", arg.value);
                    else if (arg.type == TOKEN_CHAR) snprintf(expr,sizeof(expr),"\"%s\"", arg.value);
                    else if (arg.type == TOKEN_INT_LIT || arg.type == TOKEN_FLOAT_LIT) snprintf(expr,sizeof(expr),"\"%s\"", arg.value);
                    else if (arg.type == TOKEN_IDENT && is_bool_value(arg.value)) snprintf(expr,sizeof(expr),"\"%s\"", (strcmp(arg.value,"true")==0||strcmp(arg.value,"t")==0)?"true":"false");
                    else { char m[MAX_IDENTIFIER_LEN]; mangle_name(arg.value,m,MAX_IDENTIFIER_LEN); if(symbol_table_has(table,m)) snprintf(expr,sizeof(expr),"%s", symbol_table_get_cname(table,m)); else snprintf(expr,sizeof(expr),"\"\""); }
                } else { /* tochar/char */
                    expr_type = TYPE_STRING; /* char stored as string buffer */
                    if (arg.type == TOKEN_STRING) {
                        if (arg.value[0]=='\\' && arg.value[1]) snprintf(expr,sizeof(expr),"\"%c%c\"", arg.value[0], arg.value[1]);
                        else snprintf(expr,sizeof(expr),"\"%c\"", arg.value[0]?arg.value[0]:'\0');
                    } else if (arg.type == TOKEN_INT_LIT) snprintf(expr,sizeof(expr),"\"%c\"", (unsigned char)atoi(arg.value));
                    else if (arg.type == TOKEN_CHAR) snprintf(expr,sizeof(expr),"\"%s\"", arg.value);
                    else { char m[MAX_IDENTIFIER_LEN]; mangle_name(arg.value,m,MAX_IDENTIFIER_LEN); if(symbol_table_has(table,m)) { const char *cn=symbol_table_get_cname(table,m); snprintf(expr,sizeof(expr),"(_ltng_is_num_str(%s)?((char[]){(char)atoi(%s),0}):((char[]){(%s)[0]? (%s)[0]:0,0}))",cn,cn,cn,cn);} else snprintf(expr,sizeof(expr),"\"\""); }
                }
            }
            if (handled) {
                /* For bool, emit as string literal expr to be safe with storage */
                if (strcmp(conv.value, "tobool") == 0 || strcmp(conv.value, "bool") == 0) {
                    /* store "true"/"false" */
                }
                symbol_table_add_locked(table, mangled, (strcmp(conv.value,"tobool")==0||strcmp(conv.value,"bool")==0)?TYPE_BOOL:expr_type);
                const char *cname = symbol_table_get_cname(table, mangled);
                ASTNode *node = create_node(NODE_LET, cname, expr);
                node->var_type = (strcmp(conv.value,"tobool")==0||strcmp(conv.value,"bool")==0)?TYPE_BOOL:expr_type;
                /* bool/string literal results are non-expr values in storage form */
                if (node->var_type == TYPE_BOOL || node->var_type == TYPE_STRING) node->is_expr = 0;
                else node->is_expr = 1;
                /* for string/char exprs that are C expressions, mark expr */
                if (node->var_type == TYPE_STRING && expr[0] != '"') node->is_expr = 1;
                emit(node);
                return;
            } else {
                /* Complex arg: fall back to legacy behavior */
                lexer->current -= 2; /* rewind conv and ( */
            }
        }
        char expr[MAX_STRING_LEN];
        VarType expr_type = collect_expression(lexer, table, expr, sizeof(expr), 0, 0, NULL, NULL, NULL, NULL);
        symbol_table_add_locked(table, mangled, expr_type);
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
                if (try_parse_sole_call_expr(lexer, table, expr, sizeof(expr), 1)) {
                    symbol_table_add_locked(table, mangled, TYPE_STRING);
                    const char *cname = symbol_table_get_cname(table, mangled);
                    ASTNode *node = create_node(NODE_LET, cname, expr);
                    node->var_type = TYPE_STRING;
                    node->is_expr = 1;
                    emit(node);
                    return;
                }
            }
            VarType expr_type = collect_expression(lexer, table, expr, sizeof(expr), 0, 0, NULL, NULL, NULL, NULL);
            symbol_table_add_locked(table, mangled, expr_type);
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

    symbol_table_add_locked(table, mangled, vtype);
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
        symbol_table_set_const(table, mangled, 1);
        ASTNode *node = create_node(NODE_CONST, mangled, arr->value);
        node->var_type = TYPE_ARRAY;
        node->array_dim_count = arr->array_dim_count;
        node->array_dims = malloc(arr->array_dim_count * sizeof(int));
        for (int d = 0; d < arr->array_dim_count; d++) {
            node->array_dims[d] = arr->array_dims[d];
        }
        node->array_elem_count = arr->array_elem_count;
        node->is_expr = 0;
        emit(node);
        return;
    }

    Token next = lexer_peek(lexer);
    VarType vtype;
    Token value;

    int is_type_conv_call = (next.type == TOKEN_IDENT && is_type_conversion(next.value) &&
                             lexer_peek(lexer).type == TOKEN_LPAREN);

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
                if (try_parse_sole_call_expr(lexer, table, expr, sizeof(expr), 1)) {
                    symbol_table_add_locked(table, mangled, TYPE_STRING);
                    symbol_table_set_const(table, mangled, 1);
                    ASTNode *node = create_node(NODE_CONST, mangled, expr);
                    node->var_type = TYPE_STRING;
                    node->is_expr = 1;
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
            char msg[256];
            snprintf(msg, sizeof(msg),
                "Cannot change type of locked variable '%s' from %s to %s",
                mangled, type_name_str(existing), type_name_str(new_type));
            error_state_add(parser_errors, line, col, msg);
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
        /* Expect '=' */
        Token eq = lexer_next(lexer);
        if (eq.type != TOKEN_EQUALS) {
            error_state_add(parser_errors, eq.line, eq.col,
                "Expected '=' after array index in set statement");
            skip_to_newline(lexer);
            return;
        }
        /* Parse RHS expression */
        char rhs_expr[MAX_STRING_LEN];
        VarType rhs_type = collect_expression(lexer, table, rhs_expr, MAX_STRING_LEN, 0, 0, NULL, NULL, NULL, NULL);

        /* Get array dimensions */
        int dim_count = 0;
        int *dims = NULL;
        int have_dims = symbol_table_get_array_dims(table, mangled, &dims, &dim_count);
        if (!have_dims || dim_count <= 0) {
            dim_count = 1;
            dims = malloc(sizeof(int));
            if (dims) dims[0] = 0;
            have_dims = 0;
        }
        if (index_count != dim_count) {
            char errmsg[256];
            snprintf(errmsg, sizeof(errmsg),
                "Array '%s' has %d dimension(s) but %d index(es) were given",
                name.value, dim_count, index_count);
            error_state_add(parser_errors, name.line, name.col, errmsg);
            if (!have_dims) free(dims);
            return;
        }

        /* Build index computation C code into array_index field */
        char idx_code[MAX_STRING_LEN];
        int ipos = 0;
        ipos += snprintf(idx_code + ipos, sizeof(idx_code) - ipos, "({ ");
        for (int d = 0; d < index_count; d++) {
            ipos += snprintf(idx_code + ipos, sizeof(idx_code) - ipos,
                "int __i%d = (int)(%s); __i%d = __i%d > 0 ? __i%d - 1 : __i%d + (%d); ",
                d, index_exprs[d], d, d, d, d, dims[d]);
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
        node->var_type = rhs_type;
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
        snprintf(full_expr, sizeof(full_expr), "%s %c %s", name.value, opchar, expr_rhs_src);
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
            ASTNode *node = create_node(NODE_SET, cname, arr->value);
            node->var_type = TYPE_ARRAY;
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
                        ASTNode *cp = create_node(NODE_ARRAY_COPY, (char *)cname, (char *)scname);
                        cp->is_expr = 1;
                        emit(cp);
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
                        char slrange[MAX_STRING_LEN];
                        snprintf(slrange, sizeof(slrange), "%s,%s", start_expr, end_expr);
                        ASTNode *sl = create_node(NODE_ARRAY_SLICE, (char *)cname, (char *)scname);
                        strncpy(sl->array_index, slrange, MAX_STRING_LEN - 1);
                        sl->array_index[MAX_STRING_LEN - 1] = '\0';
                        sl->is_expr = 1;
                        emit(sl);
                        return;
                    }
                }
            }
        }
    }

    Token next = lexer_peek(lexer);
    VarType vtype;
    Token value;

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
                if (try_parse_sole_call_expr(lexer, table, expr, sizeof(expr), 1)) {
                    vtype = TYPE_STRING;
                    if (!check_set_type_immutable(table, mangled, vtype, type_tok.line, type_tok.col, lexer))
                        return;
                    symbol_table_update(table, mangled, vtype);
                    ASTNode *node = create_node(NODE_SET, cname, expr);
                    node->var_type = vtype;
                    node->is_expr = 1;
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
            Token arg = lexer_peek(lexer);
            char expr[MAX_STRING_LEN] = "0";
            VarType expr_type = TYPE_STRING;
            int handled = 0;
            if ((arg.type == TOKEN_STRING || arg.type == TOKEN_INT_LIT || arg.type == TOKEN_FLOAT_LIT ||
                 arg.type == TOKEN_CHAR || arg.type == TOKEN_IDENT) &&
                lexer->tokens[lexer->current + 1].type == TOKEN_RPAREN) {
                lexer_next(lexer); /* arg */
                lexer_next(lexer); /* ) */
                handled = 1;
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
                        handled = 0; /* runtime variable: fall back to legacy */
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
                    if (arg.type == TOKEN_STRING) snprintf(expr,sizeof(expr),"\"%s\"", arg.value);
                    else if (arg.type == TOKEN_CHAR) snprintf(expr,sizeof(expr),"\"%s\"", arg.value);
                    else if (arg.type == TOKEN_INT_LIT || arg.type == TOKEN_FLOAT_LIT) snprintf(expr,sizeof(expr),"\"%s\"", arg.value);
                    else if (arg.type == TOKEN_IDENT && is_bool_value(arg.value)) snprintf(expr,sizeof(expr),"\"%s\"", (strcmp(arg.value,"true")==0||strcmp(arg.value,"t")==0)?"true":"false");
                    else { char m[MAX_IDENTIFIER_LEN]; mangle_name(arg.value,m,MAX_IDENTIFIER_LEN); if(symbol_table_has(table,m)) snprintf(expr,sizeof(expr),"%s", symbol_table_get_cname(table,m)); else snprintf(expr,sizeof(expr),"\"\""); }
                } else { /* tochar/char */
                    expr_type = TYPE_STRING;
                    if (arg.type == TOKEN_STRING) {
                        if (arg.value[0]=='\\' && arg.value[1]) snprintf(expr,sizeof(expr),"\"%c%c\"", arg.value[0], arg.value[1]);
                        else snprintf(expr,sizeof(expr),"\"%c\"", arg.value[0]?arg.value[0]:'\0');
                    } else if (arg.type == TOKEN_INT_LIT) snprintf(expr,sizeof(expr),"\"%c\"", (unsigned char)atoi(arg.value));
                    else if (arg.type == TOKEN_CHAR) snprintf(expr,sizeof(expr),"\"%s\"", arg.value);
                    else { char m[MAX_IDENTIFIER_LEN]; mangle_name(arg.value,m,MAX_IDENTIFIER_LEN); if(symbol_table_has(table,m)) { const char *cn=symbol_table_get_cname(table,m); snprintf(expr,sizeof(expr),"(_ltng_is_num_str(%s)?((char[]){(char)atoi(%s),0}):((char[]){(%s)[0]? (%s)[0]:0,0}))",cn,cn,cn,cn);} else snprintf(expr,sizeof(expr),"\"\""); }
                }
            }
            if (handled) {
                if (!check_set_type_immutable(table, mangled, expr_type, set_tok.line, set_tok.col, lexer))
                    return;
                symbol_table_update(table, mangled, expr_type);
                ASTNode *node = create_node(NODE_SET, cname, expr);
                node->var_type = expr_type;
                if (node->var_type == TYPE_BOOL || (node->var_type == TYPE_STRING && expr[0] == '"')) node->is_expr = 0;
                else node->is_expr = 1;
                emit(node);
                return;
            } else {
                lexer->current -= 2; /* rewind conv and ( */
            }
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
                if (try_parse_sole_call_expr(lexer, table, expr, sizeof(expr), 1)) {
                    vtype = TYPE_STRING;
                    if (!check_set_type_immutable(table, mangled, vtype, set_tok.line, set_tok.col, lexer))
                        return;
                    symbol_table_update(table, mangled, vtype);
                    ASTNode *node = create_node(NODE_SET, cname, expr);
                    node->var_type = vtype;
                    node->is_expr = 1;
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
                            else if (vt == TYPE_INT) pos += snprintf(cexpr + pos, MAX_STRING_LEN - pos, "atoi(%s)", mangled);
                            else if (vt == TYPE_FLOAT) pos += snprintf(cexpr + pos, MAX_STRING_LEN - pos, "atof(%s)", mangled);
                            else pos += snprintf(cexpr + pos, MAX_STRING_LEN - pos, "atoi(%s)", mangled);
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
ASTNode *cnode = create_node(NODE_CASE, "", cexpr);
            cnode->is_default = 0;
            cnode->var_type = ctype;

            Token lb2 = lexer_next(lexer);
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
    VarType ctype = collect_expression(lexer, table, cond, sizeof(cond), 1, 0, &csl,&csc,&cel,&cec);

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
    VarType ctype = collect_expression(lexer, table, cond, sizeof(cond), 1, 0, &csl,&csc,&cel,&cec);
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
        if (lb.type != TOKEN_LBRACE) {
            error_state_add(parser_errors, lb.line, lb.col,
                "Expected '{' after for-in header");
            skip_to_newline(lexer);
            return NULL;
        }
        char mangled[MAX_IDENTIFIER_LEN];
        mangle_name(var_tok.value, mangled, MAX_IDENTIFIER_LEN);
        Scope *loop_scope = (Scope *)malloc(sizeof(Scope));
        if (!loop_scope) {
            error_state_add(parser_errors, var_tok.line, var_tok.col, "Memory allocation failed");
            return NULL;
        }
        scope_init(loop_scope, table, 0);
        symbol_table_add_local(loop_scope, mangled, TYPE_STRING);
        const char *cname = symbol_table_get_cname(loop_scope, mangled);
        const char *acname = symbol_table_get_cname(table, am);
        ASTNode *node = create_node(NODE_FOR, cname, "");
    node->line = kw.line; node->col = kw.col;
        node->var_type = TYPE_STRING;
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
    VarType ctype = collect_expression(lexer, table, cond, sizeof(cond), 1, 0, &csl,&csc,&cel,&cec);

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
   Returns 0 if not a sole call arg (lexer restored). */
static int try_parse_call_arg(Lexer *lexer, SymbolTable *table, char *cexpr, int cexpr_size) {
    Token first = lexer_peek(lexer);
    if (first.type != TOKEN_IDENT || !is_function_name_scoped(table, first.value)) return 0;
    int saved = lexer->current;
    Token fname = lexer_next(lexer);
    if (lexer_peek(lexer).type != TOKEN_LPAREN) { lexer->current = saved; return 0; }
    lexer_next(lexer); /* consume ( */
    ASTNode *call = parse_call_node(lexer, table, fname.value, fname.line, fname.col, NULL);
    if (call->is_base_assign) {
        lexer->current = saved;
        return 0;
    }
    Token after = lexer_peek(lexer);
    if (after.type != TOKEN_COMMA && after.type != TOKEN_RPAREN) {
        lexer->current = saved;
        return 0;
    }
    int cpos = 0;
    int cline = call->line > 0 ? call->line : 1;
    int ccol = call->col > 0 ? call->col : 1;
    cpos += snprintf(cexpr + cpos, cexpr_size - cpos, "%s(_ltng_retbuf(), %d, %d", call->name, cline, ccol);
    for (ASTNode *a = call->args; a; a = a->next) {
        if (a->var_type == TYPE_STRING && a->value[0] == '"') {
            cpos += snprintf(cexpr + cpos, cexpr_size - cpos, ", %s", a->value);
        } else if (a->is_variable && a->value[0] != '\0') {
            cpos += snprintf(cexpr + cpos, cexpr_size - cpos, ", %s", a->value);
        } else if ((a->var_type == TYPE_STRING || a->var_type == TYPE_OBJECT) && a->value[0] != '\0') {
            cpos += snprintf(cexpr + cpos, cexpr_size - cpos, ", %s", a->value);
            } else if ((a->var_type == TYPE_STRING || a->var_type == TYPE_OBJECT) && a->value[0] != '\0') {
                cpos += snprintf(cexpr + cpos, cexpr_size - cpos, ", %s", a->value);
            } else if (a->value[0] == '\0') {
                cpos += snprintf(cexpr + cpos, cexpr_size - cpos, ", \"\"");
            } else {
                cpos += snprintf(cexpr + cpos, cexpr_size - cpos, ", _ltng_ad((double)(%s))", a->value);
            }
    }
    cpos += snprintf(cexpr + cpos, cexpr_size - cpos, ")");
    cexpr[cexpr_size - 1] = '\0';
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

/* Recursively scan an AST subtree for NODE_RETURN nodes and tally their var_types.
   Returns the dominant return type, or TYPE_STRING if mixed/none. */
static VarType analyze_function_returns(ASTNode *node, int *has_return) {
    if (!node) return TYPE_STRING;

    if (node->type == NODE_RETURN) {
        *has_return = 1;
        return node->var_type;
    }

    VarType result = TYPE_STRING;
    VarType child;
    int child_has_return = 0;

    child = analyze_function_returns(node->body, &child_has_return);
    if (child_has_return) {
        *has_return = 1;
        result = (result == TYPE_STRING || result == child) ? child : TYPE_STRING;
    }

    child = analyze_function_returns(node->orelse, &child_has_return);
    if (child_has_return) {
        *has_return = 1;
        result = (result == TYPE_STRING || result == child) ? child : TYPE_STRING;
    }

    child = analyze_function_returns(node->cases, &child_has_return);
    if (child_has_return) {
        *has_return = 1;
        result = (result == TYPE_STRING || result == child) ? child : TYPE_STRING;
    }

    child = analyze_function_returns(node->else_body, &child_has_return);
    if (child_has_return) {
        *has_return = 1;
        result = (result == TYPE_STRING || result == child) ? child : TYPE_STRING;
    }

    child = analyze_function_returns(node->next, &child_has_return);
    if (child_has_return) {
        *has_return = 1;
        result = (result == TYPE_STRING || result == child) ? child : TYPE_STRING;
    }

    return result;
}

/* Infer the return type of a user-defined function by scanning its return statements.
   Returns TYPE_STRING if the function is not found or has mixed/no returns. */
static VarType infer_function_return_type(const char *fname) {
    ASTNode *fn = find_function_ast_by_name(fname);
    if (!fn) return TYPE_STRING;

    int has_return = 0;
    return analyze_function_returns(fn->body, &has_return);
}

static ASTNode *parse_call_node(Lexer *lexer, SymbolTable *table, const char *fname_tok_val, int line, int col, ASTNode *first_arg) {
    /* assumes fname IDENT consumed, '(' consumed */
    ASTNode *call = create_node(NODE_CALL, "", "");
    call->line = line;
    call->col = col;
    char fm[MAX_IDENTIFIER_LEN];
    if (!resolve_func_name(table, fname_tok_val, fm, MAX_IDENTIFIER_LEN)) {
        mangle_name(fname_tok_val, fm, MAX_IDENTIFIER_LEN);
    }
    strncpy(call->name, fm, MAX_IDENTIFIER_LEN - 1);
    if (function_first_param_is_base(fm)) {
        call->is_base_assign = 1;
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
            } else if (at.type == TOKEN_CHAR && !named_arg) {
                lexer_next(lexer);
                ASTNode *an = create_node(NODE_PARAM, "", "");
                if (at.value[0]) snprintf(an->value, MAX_STRING_LEN, "'%s'", at.value);
                else strcpy(an->value, "'\\0'");
                an->var_type = TYPE_STRING;
                an->is_expr = 1;
                if (!args) args = alast = an; else { alast->next = an; alast = an; }
            } else if (!named_arg && at.type == TOKEN_IDENT && is_function_name_scoped(table, at.value)) {
                /* function call argument - pass result as string buffer */
                char cexpr[MAX_STRING_LEN];
                if (try_parse_call_arg(lexer, table, cexpr, sizeof(cexpr))) {
                    ASTNode *an = create_node(NODE_PARAM, "", cexpr);
                    an->var_type = TYPE_STRING;
                    an->is_expr = 1;
                    an->is_variable = 1; /* pass pointer directly */
                    if (!args) args = alast = an; else { alast->next = an; alast = an; }
                } else {
                    /* complex expression involving call */
                    VarType avt = collect_expression(lexer, table, cexpr, sizeof(cexpr), 1, 1, NULL, NULL, NULL, NULL);
                    ASTNode *an = create_node(NODE_PARAM, "", cexpr);
                    an->var_type = avt;
                    an->is_expr = 1;
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

    /* Fill in default values for parameters that were not supplied by the caller. */
    ASTNode *fn_def = find_function_ast_by_name(fname_tok_val);
    if (fn_def && fn_def->params) {
        int param_count = 0;
        for (ASTNode *p = fn_def->params; p; p = p->next) param_count++;
        ASTNode *eff = NULL;
        ASTNode *efflast = NULL;
        if (has_named_arg) {
            /* Validate named args: unknown or duplicate parameter names. */
            for (ASTNode *a = args; a; a = a->next) {
                if (a->for_var[0] == '\0') continue;
                int found = 0;
                for (ASTNode *p = fn_def->params; p; p = p->next) {
                    if (strcmp(a->for_var, p->for_var) == 0) { found = 1; break; }
                }
                if (!found) {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "Unknown argument '%s' in call to '%s'", a->for_var, fname_tok_val);
                    error_state_add(parser_errors, line, col, msg);
                }
                int dup = 0;
                for (ASTNode *b = args; b != a; b = b->next) {
                    if (b->for_var[0] && strcmp(b->for_var, a->for_var) == 0) { dup = 1; break; }
                }
                if (dup) {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "Duplicate argument '%s' in call to '%s'", a->for_var, fname_tok_val);
                    error_state_add(parser_errors, line, col, msg);
                }
            }
            /* Build positional list in order (do not mutate original arg links). */
            ASTNode *pos_list[64]; int pos_n = 0;
            for (ASTNode *a = args; a; a = a->next) {
                if (a->for_var[0] == '\0' && pos_n < 64) pos_list[pos_n++] = a;
            }
            ASTNode *out_list[64]; int out_n = 0;
            int pos_i = 0;
            for (ASTNode *p = fn_def->params; p; p = p->next) {
                ASTNode *outn = NULL;
                /* named arg for this param? */
                for (ASTNode *a = args; a; a = a->next) {
                    if (a->for_var[0] && strcmp(a->for_var, p->for_var) == 0) { outn = a; break; }
                }
                if (outn && pos_i < pos_n) {
                    /* positional would also try to fill; check if too many positionals overall later */
                }
                if (!outn) {
                    if (pos_i < pos_n) { outn = pos_list[pos_i++]; }
                    else if (p->has_default) {
                        outn = create_node(NODE_PARAM, "", p->value);
                        outn->var_type = p->var_type;
                        outn->is_expr = 1;
                        outn->has_default = 1;
                    }
                }
                if (outn) {
                    if (out_n < 64) out_list[out_n++] = outn;
                } else {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "Missing required argument '%s' in call to '%s'", p->for_var, fname_tok_val);
                    error_state_add(parser_errors, line, col, msg);
                    /* create placeholder to keep arity consistent in emitted code */
                    outn = create_node(NODE_PARAM, "", "\"\"");
                    outn->var_type = TYPE_STRING;
                    outn->is_expr = 1;
                    if (out_n < 64) out_list[out_n++] = outn;
                }
            }
            if (pos_i < pos_n) {
                char msg[256];
                snprintf(msg, sizeof(msg), "Too many arguments in call to '%s'", fname_tok_val);
                error_state_add(parser_errors, line, col, msg);
            }
            for (int i = 0; i < out_n; i++) {
                out_list[i]->next = NULL;
                if (!eff) eff = efflast = out_list[i]; else { efflast->next = out_list[i]; efflast = out_list[i]; }
            }
        } else {
            /* Count supplied positional args */
            int supplied = 0;
            for (ASTNode *a = args; a; a = a->next) supplied++;
            if (supplied > param_count) {
                char msg[256];
                snprintf(msg, sizeof(msg), "Too many arguments in call to '%s'", fname_tok_val);
                error_state_add(parser_errors, line, col, msg);
            }
            ASTNode *src = args;
            for (ASTNode *p = fn_def->params; p; p = p->next) {
                ASTNode *a = src;
                if (a) src = a->next; else a = NULL;
                ASTNode *outn = NULL;
                if (a) {
                    outn = a;
                } else if (p->has_default) {
                    outn = create_node(NODE_PARAM, "", p->value);
                    outn->var_type = p->var_type;
                    outn->is_expr = 1;
                    outn->has_default = 1;
                } else {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "Missing required argument '%s' in call to '%s'", p->for_var, fname_tok_val);
                    error_state_add(parser_errors, line, col, msg);
                    outn = create_node(NODE_PARAM, "", "\"\"");
                    outn->var_type = TYPE_STRING;
                    outn->is_expr = 1;
                }
                outn->next = NULL;
                if (!eff) eff = efflast = outn; else { efflast->next = outn; efflast = outn; }
            }
        }
        call->args = eff;
    } else {
        call->args = args;
    }
    return call;
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
        /* sole variable - pass through as string to preserve value */
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
                etype = TYPE_STRING;
                ASTNode *node = create_node(NODE_RETURN, "", expr);
                node->var_type = etype;
                node->is_expr = 1;
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
            if (try_parse_sole_call_expr(lexer, table, expr, sizeof(expr), 0)) {
                etype = TYPE_STRING;
                ASTNode *node = create_node(NODE_RETURN, "", expr);
                node->var_type = etype;
                node->is_expr = 1;
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
            node->var_type = TYPE_STRING;
            node->is_expr = 1;
            emit(node);
            return;
        }
        int _esl=0,_esc=0,_eel=0,_eec=0;
        etype = collect_expression(lexer, table, expr, sizeof(expr), 0, 0, NULL, NULL, NULL, NULL);
        _esl=g_expr_sl; _esc=g_expr_sc; _eel=g_expr_el; _eec=g_expr_ec;
        ASTNode *node = create_node(NODE_RETURN, "", expr);
        node->v_sl=_esl; node->v_sc=_esc; node->v_el=_eel; node->v_ec=_eec;
        node->line = kw.line; node->col = kw.col;
        node->var_type = etype;
        node->is_expr = 1;
        emit(node);
        return;
    }
    ASTNode *node = create_node(NODE_RETURN, "", expr);
    node->line = kw.line; node->col = kw.col;
    node->var_type = etype;
    node->is_expr = 1;
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

static ASTNode *parse_function(Lexer *lexer, SymbolTable *table, const char *enclosing_cname) {
    Token kw = lexer_next(lexer);
    Token name = lexer_next(lexer);
    if (!token_is_name(name)) {
        error_state_add(parser_errors, name.line, name.col,
            "Expected function name after 'function'");
        skip_to_newline(lexer);
        return NULL;
    }
    if (is_ltng_reserved(name.value) || is_ltng_keyword(name.value)) {
        error_state_add(parser_errors, name.line, name.col,
            "Cannot use reserved word as function name");
        skip_to_newline(lexer);
        return NULL;
    }

    char fname_short[MAX_IDENTIFIER_LEN];
    mangle_name(name.value, fname_short, MAX_IDENTIFIER_LEN);
    char cname[MAX_IDENTIFIER_LEN];
    if (enclosing_cname && enclosing_cname[0]) {
        snprintf(cname, sizeof(cname), "%s__%s", enclosing_cname, fname_short);
    } else {
        strncpy(cname, fname_short, MAX_IDENTIFIER_LEN - 1);
        cname[MAX_IDENTIFIER_LEN - 1] = 0;
    }
    /* bind short name in enclosing scope (so calls inside it resolve) */
    if (table) symbol_table_add_func(table, name.value, cname, 0);
    /* register lifted C name globally for codegen/prototypes */
    {
        if (g_func_count < MAX_FUNCS) {
            strncpy(g_funcs[g_func_count].name, cname, MAX_IDENTIFIER_LEN - 1);
            g_funcs[g_func_count].name[MAX_IDENTIFIER_LEN - 1] = 0;
            g_funcs[g_func_count].param_count = 0;
            g_func_count++;
        }
    }

    Token lp = lexer_next(lexer);
    if (lp.type != TOKEN_LPAREN) {
        error_state_add(parser_errors, lp.line, lp.col,
            "Expected '(' after function name");
        skip_to_newline(lexer);
        return NULL;
    }

    /* Parse parameters */
    ASTNode *params = NULL;
    ASTNode *plast = NULL;
    int pcount = 0;
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
    /* update param count on the lifted registration and local binding */
    for (int i = 0; i < g_func_count; i++) {
        if (strcmp(g_funcs[i].name, cname) == 0) { g_funcs[i].param_count = pcount; break; }
    }
    if (table) symbol_table_add_func(table, name.value, cname, pcount);

    Token rp = lexer_next(lexer);
    if (rp.type != TOKEN_RPAREN) {
        error_state_add(parser_errors, rp.line, rp.col,
            "Expected ')' after function parameters");
        skip_to_newline(lexer);
        free(func_scope);
        return NULL;
    }

    Token lb = lexer_next(lexer);
    if (lb.type != TOKEN_LBRACE) {
        error_state_add(parser_errors, lb.line, lb.col,
            "Expected '{' after function header");
        skip_to_newline(lexer);
        free(func_scope);
        return NULL;
    }

    ASTNode *fn = create_node(NODE_FUNCTION, cname, "");
    fn->line = kw.line; fn->col = kw.col;
    strncpy(fn->for_var, name.value, MAX_IDENTIFIER_LEN - 1);
    fn->params = params;
    char saved_enc[MAX_IDENTIFIER_LEN];
    strncpy(saved_enc, g_enclosing_cname, MAX_IDENTIFIER_LEN - 1);
    saved_enc[MAX_IDENTIFIER_LEN - 1] = 0;
    strncpy(g_enclosing_cname, cname, MAX_IDENTIFIER_LEN - 1);
    g_enclosing_cname[MAX_IDENTIFIER_LEN - 1] = 0;
    g_in_function++;
    fn->body = parse_block(lexer, func_scope, 0);
    g_in_function--;
    strncpy(g_enclosing_cname, saved_enc, MAX_IDENTIFIER_LEN - 1);
    g_enclosing_cname[MAX_IDENTIFIER_LEN - 1] = 0;
    free(func_scope);
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
        if (t2.type == TOKEN_LPAREN && is_function_name_scoped(table, t.value)) {
            /* function call statement: name(args) */
            char fm[MAX_IDENTIFIER_LEN];
            if (!resolve_func_name(table, t.value, fm, MAX_IDENTIFIER_LEN)) {
                mangle_name(t.value, fm, MAX_IDENTIFIER_LEN);
            }
            if (function_first_param_is_base(fm)) {
                error_state_add(parser_errors, t.line, t.col,
                    "function with 'base' parameter must be called with method syntax (receiver.method(args))");
                skip_to_newline(lexer);
                return;
            }
            lexer->current = saved;
            lexer_next(lexer); /* consume fname */
            lexer_next(lexer); /* ( */
            ASTNode *call = create_node(NODE_CALL, "", "");
            call->line = t.line; call->col = t.col;
            strncpy(call->name, fm, MAX_IDENTIFIER_LEN - 1);
            /* parse args */
            ASTNode *args = NULL, *alast = NULL;
            if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                while (1) {
                    char aexpr[MAX_STRING_LEN];
                    VarType at = collect_expression(lexer, table, aexpr, sizeof(aexpr), 1, 1, NULL, NULL, NULL, NULL);
                    int esl=g_expr_sl,esc=g_expr_sc,eel=g_expr_el,eec=g_expr_ec;
                    ASTNode *an = create_node(NODE_PARAM, "", aexpr);
                    an->v_sl=esl; an->v_sc=esc; an->v_el=eel; an->v_ec=eec;
                    an->var_type = at;
                    an->is_expr = 1;
                    if ((at == TYPE_STRING || at == TYPE_OBJECT) && aexpr[0] != '"' && aexpr[0] != '\0') {
                        int simple = 1;
                        for (char *p = aexpr; *p; p++) {
                            if (*p==' '||*p=='\t'||*p=='('||*p==')'||*p=='+'||*p=='*'||*p=='-'||*p=='/'||*p=='%'||*p=='='||*p=='!'||*p=='<'||*p=='>'||*p=='&'||*p=='|'||*p=='?') { simple = 0; break; }
                        }
                        if (simple) an->is_variable = 1;
                    }
                    if (!args) args = alast = an; else { alast->next = an; alast = an; }
                    if (lexer_peek(lexer).type == TOKEN_COMMA) { lexer_next(lexer); continue; }
                    break;
                }
            }
            if (lexer_peek(lexer).type == TOKEN_RPAREN) lexer_next(lexer);
            call->args = args;
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
                    char errmsg[256];
                    snprintf(errmsg, sizeof(errmsg), "Expected '(' after '%s'", mname.value);
                    error_state_add(parser_errors, mname.line, mname.col, errmsg);
                    skip_to_newline(lexer);
                    return;
                }
                lexer_next(lexer); /* consume ( */
                if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                    char errmsg[256];
                    snprintf(errmsg, sizeof(errmsg),
                        "Expected ')' with no arguments for '%s()'", mname.value);
                    error_state_add(parser_errors, lexer_peek(lexer).line,
                        lexer_peek(lexer).col, errmsg);
                    skip_to_newline(lexer);
                    return;
                }
                lexer_next(lexer); /* consume ) */

                char rm[MAX_IDENTIFIER_LEN];
                mangle_name(t.value, rm, MAX_IDENTIFIER_LEN);
                if (!symbol_table_has(table, rm)) {
                    char errmsg[256];
                    snprintf(errmsg, sizeof(errmsg), "Variable '%s' not declared", t.value);
                    error_state_add(parser_errors, t.line, t.col, errmsg);
                    return;
                }
                VarType vt = symbol_table_get(table, rm);
                if (vt == TYPE_NULL) {
                    char errmsg[256];
                    snprintf(errmsg, sizeof(errmsg),
                        "Cannot lock/unlock variable '%s' with null dtype", t.value);
                    error_state_add(parser_errors, t.line, t.col, errmsg);
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
               These generate AST nodes handled by codegen. */
            if ((mname.type == TOKEN_IDENT || token_is_name(mname)) &&
                (strcmp(mname.value, "push") == 0 ||
                 strcmp(mname.value, "pop") == 0 ||
                 strcmp(mname.value, "insert") == 0 ||
                 strcmp(mname.value, "remove") == 0 ||
                 strcmp(mname.value, "sort") == 0 ||
                 strcmp(mname.value, "reverse") == 0 ||
                 strcmp(mname.value, "contains") == 0 ||
                 strcmp(mname.value, "index_of") == 0)) {
                lexer_next(lexer); /* consume ( */

                char rm[MAX_IDENTIFIER_LEN];
                mangle_name(t.value, rm, MAX_IDENTIFIER_LEN);
                if (!symbol_table_has(table, rm)) {
                    char errmsg[256];
                    snprintf(errmsg, sizeof(errmsg), "Variable '%s' not declared", t.value);
                    error_state_add(parser_errors, t.line, t.col, errmsg);
                    skip_to_newline(lexer);
                    return;
                }
                VarType vt = symbol_table_get(table, rm);
                if (vt != TYPE_ARRAY) {
                    char errmsg[256];
                    snprintf(errmsg, sizeof(errmsg), "Variable '%s' is not an array", t.value);
                    error_state_add(parser_errors, t.line, t.col, errmsg);
                    skip_to_newline(lexer);
                    return;
                }

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
                    ASTNode *node = create_node(NODE_ARRAY_PUSH, rm, arg_expr);
                    node->var_type = arg_type;
                    node->is_expr = 1;
                    emit(node);
                    return;
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
                    ASTNode *node = create_node(NODE_ARRAY_INSERT, rm, idx_expr);
                    strncpy(node->array_index, arg_expr, MAX_STRING_LEN - 1);
                    node->array_index[MAX_STRING_LEN - 1] = '\0';
                    node->var_type = arg_type;
                    node->is_expr = 1;
                    emit(node);
                    return;
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
                    return;
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
                    return;
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
                    return;
                } else if (strcmp(mname.value, "pop") == 0) {
                    /* pop() or pop(index) */
                    if (lexer_peek(lexer).type == TOKEN_RPAREN) {
                        /* pop() - no args */
                        lexer_next(lexer); /* consume ) */
                        ASTNode *node = create_node(NODE_ARRAY_POP, rm, "");
                        node->var_type = TYPE_STRING;
                        node->is_expr = 1;
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
                        lexer_next(lexer); /* consume ) */
                        ASTNode *node = create_node(NODE_ARRAY_POP, rm, idx_expr);
                        node->var_type = TYPE_STRING;
                        node->is_expr = 1;
                        emit(node);
                        return;
                    }
                } else {
                    /* contains / index_of used as statements - result discarded,
                       treat as no-op but consume tokens for sync */
                    if (lexer_peek(lexer).type == TOKEN_RPAREN) {
                        lexer_next(lexer); /* consume ) */
                    } else {
                        char dummy[MAX_STRING_LEN];
                        collect_expression(lexer, table, dummy, sizeof(dummy), 1, 0, NULL, NULL, NULL, NULL);
                        if (lexer_peek(lexer).type == TOKEN_RPAREN) lexer_next(lexer);
                    }
                    return;
                }
            }
            if ((mname.type == TOKEN_IDENT || token_is_name(mname)) &&
                lexer_peek(lexer).type == TOKEN_LPAREN &&
                is_function_name_scoped(table, mname.value)) {
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
                return;
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

    /* Pre-scan to register top-level function names for forward references */
    {
        int saved = lexer->current;
        int depth = 0;
        for (int i = 0; i < lexer->count; i++) {
            TokenType tt = lexer->tokens[i].type;
            if (tt == TOKEN_LBRACE) depth++;
            else if (tt == TOKEN_RBRACE) { if (depth > 0) depth--; }
            else if (tt == TOKEN_FUNCTION && depth == 0 && i + 1 < lexer->count &&
                     lexer->tokens[i+1].type == TOKEN_IDENT) {
                if (!is_function_name(lexer->tokens[i+1].value)) {
                    register_function(lexer->tokens[i+1].value, 0);
                }
            }
        }
        lexer->current = saved;
    }

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
