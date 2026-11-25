#ifndef LTNG_H
#define LTNG_H

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Legacy fixed-limit macros removed: buffers are dynamic. */

typedef enum {
    TOKEN_WRITE,
    TOKEN_READ,
    TOKEN_LET,
    TOKEN_CONST,
    TOKEN_SET,
    TOKEN_LPAREN,
    TOKEN_RPAREN,
    TOKEN_EQUALS,
    TOKEN_STRING,
    TOKEN_CHAR,
    TOKEN_INT_LIT,
    TOKEN_FLOAT_LIT,
    TOKEN_IDENT,
    TOKEN_NEWLINE,
    TOKEN_EOF,
    TOKEN_ERROR,
    TOKEN_PLUS,
    TOKEN_MINUS,
    TOKEN_STAR,
    TOKEN_SLASH,
    TOKEN_PERCENT,
    TOKEN_PLUS_PLUS,
    TOKEN_MINUS_MINUS,
    TOKEN_PLUS_EQUALS,
    TOKEN_MINUS_EQUALS,
    TOKEN_STAR_EQUALS,
    TOKEN_SLASH_EQUALS,
    TOKEN_PERCENT_EQUALS,
    TOKEN_AMP_AMP,
    TOKEN_PIPE_PIPE,
    TOKEN_CARET_CARET,
    TOKEN_BANG,
    TOKEN_EQ_EQ,
    TOKEN_BANG_EQ,
    TOKEN_LT,
    TOKEN_GT,
    TOKEN_LT_EQ,
    TOKEN_GT_EQ,
    TOKEN_IF,
    TOKEN_ELSE,
    TOKEN_SWITCH,
    TOKEN_CASE,
    TOKEN_DEFAULT,
    TOKEN_BREAK,
    TOKEN_CONTINUE,
    TOKEN_WHILE,
    TOKEN_DO,
    TOKEN_FOR,
    TOKEN_FROM,
    TOKEN_TO,
    TOKEN_STEP,
    TOKEN_IN,
    TOKEN_FUNCTION,
    TOKEN_RETURN,
    TOKEN_EXIT,
    TOKEN_TRY,
    TOKEN_CATCH,
    TOKEN_FINALLY,
    TOKEN_THROW,
    TOKEN_COMMA,
    TOKEN_DOT,
    TOKEN_COLON,
    TOKEN_LBRACE,
    TOKEN_RBRACE,
    TOKEN_LBRACKET,
    TOKEN_RBRACKET,
    TOKEN_ARROW,
    TOKEN_ELLIPSIS,
    TOKEN_QUESTION
} TokenType;

typedef struct {
    TokenType type;
    char *value;
    int line;
    int col;
} Token;

Token token_create(TokenType type, const char *value, int line, int col);
void token_free(Token *t);
char *ltng_strdup(const char *s);
char *_ltng_cfmt(const char *fmt, ...);

typedef struct {
    Token *tokens;           /* dynamically allocated token array */
    int token_capacity;      /* current capacity of tokens array */
    int count;
    int current;
    char *source;
    int source_len;
    int pos;
    int line;
    int col;
} Lexer;

typedef enum {
    TYPE_INT,
    TYPE_FLOAT,
    TYPE_STRING,
    TYPE_CHAR,
    TYPE_BOOL,
    TYPE_OBJECT,
    TYPE_NULL,
    TYPE_ARRAY,
    TYPE_VOID
} VarType;

typedef enum {
    NODE_WRITE,
    NODE_READ,
    NODE_LET,
    NODE_CONST,
    NODE_SET,
    NODE_IF,
    NODE_ELSE,
    NODE_SWITCH,
    NODE_CASE,
    NODE_BREAK,
    NODE_CONTINUE,
    NODE_WHILE,
    NODE_DO_WHILE,
    NODE_FOR,
    NODE_FUNCTION,
    NODE_RETURN,
    NODE_EXIT,
    NODE_CALL,
    NODE_PARAM,
    NODE_PROGRAM,
    NODE_ARRAY_LIT,
    NODE_ARRAY_ACCESS,
    NODE_ARRAY_PUSH,
    NODE_ARRAY_POP,
    NODE_ARRAY_SET,
    NODE_ARRAY_INSERT,
    NODE_ARRAY_REMOVE,
    NODE_ARRAY_COPY,
    NODE_ARRAY_SLICE,
    NODE_ARRAY_SORT,
    NODE_ARRAY_REVERSE,
    NODE_BLOCK,
    NODE_TRY,
    NODE_THROW
} NodeType;

typedef struct ASTNode {
    NodeType type;
    char *name;
    char *value;
    VarType var_type;
    int is_variable;
    int is_expr;
    int is_default;
    int is_base_assign; /* UFCS call whose return value is assigned to receiver (base param) */
    int base_is_array;  /* NODE_FUNCTION: first 'base' param used as array */
    int has_catch;      /* NODE_TRY: catch clause present */
    int has_else;       /* NODE_TRY: else (success) clause present */
    int has_finally;    /* NODE_TRY: finally clause present */
    int has_default;    /* NODE_PARAM: default value expression supplied */
    int has_return_type;/* NODE_FUNCTION: explicit '-> type' annotation present */
    VarType return_type;/* NODE_FUNCTION: annotated return type */
    int is_variadic;    /* NODE_FUNCTION: has a trailing '...rest' parameter */
    int is_variadic_rest; /* NODE_PARAM: this param is the variadic rest collector */
    char *variadic_name; /* NODE_FUNCTION: source name of ...rest */
    int capture_count;  /* NODE_FUNCTION: number of captured variables from enclosing scope */
    char **captures; /* NODE_FUNCTION: cnames of captured variables */
    char *return_obj_base; /* NODE_FUNCTION: schema var for object return */
    char *for_var;
    char *for_start;
    char *for_end;
    char *for_step;
    char for_step_op; /* '+', '-', '*', '/' or 0 for default (+) */
    int line;
    int col;
    /* Source spans for expression/codegen mapping (best-effort) */
    int v_sl, v_sc, v_el, v_ec;       /* primary value/condition expression span */
    int idx_sl, idx_sc, idx_el, idx_ec; /* array index expression span */
    int fs_sl, fs_sc, fs_el, fs_ec;   /* for start expression span */
    int fe_sl, fe_sc, fe_el, fe_ec;   /* for end expression span */
    int fst_sl, fst_sc, fst_el, fst_ec; /* for step expression span */
    struct ASTNode *body;
    struct ASTNode *orelse;   /* NODE_TRY: catch body */
    struct ASTNode *else_body;/* NODE_TRY: else (success) body */
    struct ASTNode *cases;    /* NODE_TRY: finally body */
    struct ASTNode *params;  /* function parameters (NODE_PARAM/name chain) */
    struct ASTNode *args;    /* call arguments (expression nodes chain) */
    struct ASTNode *rest_args; /* NODE_CALL: variadic extra arguments */
    struct ASTNode *next;
    /* Array support */
    int array_dim_count;     /* number of dimensions (0 if not an array) */
    int *array_dims;         /* dynamically allocated size of each dimension */
    int array_elem_count;    /* total number of elements */
    char *array_index; /* for array element set: C index expression */
    VarType array_elem_type; /* inferred homogeneous element type for array literals/vars (TYPE_NULL = unknown/mixed) */
    struct ASTNode *index1;  /* first index expression */
    struct ASTNode *index2;  /* second index expression */
    struct ASTNode *index3;  /* third index expression */
    int passthrough_param;   /* NODE_RETURN: index of param passed through, or -1 */
} ASTNode;

typedef struct {
    char *name;   /* lookup key (mangled source name) */
    char *cname;  /* emitted C identifier */
    VarType type;
    int is_const;
    int is_locked;   /* dtype lock: once locked, dtype cannot change (even null->type) */
    int is_param;    /* declared as a function parameter (untyped params use dynamic typing) */
    int array_dim_count;  /* number of array dimensions (0 if not an array) */
    int *array_dims;      /* dynamically allocated size of each array dimension */
    VarType array_elem_type; /* inferred homogeneous element type (TYPE_NULL = unknown/mixed) */
    int is_func_ref;     /* variable holds a known function reference (cname) */
    char *func_ref_cname;
    int func_ref_param_count;
} VarEntry;

typedef struct {
    char *name;   /* source field name */
    char *value;  /* stringified value */
    VarType type;
} ObjField;

typedef struct {
    char *var;    /* mangled var name */
    ObjField *fields;
    int count;
    int cap;
    char *default_val;
} ObjSchema;
#define ARRAY_PUSH_RESERVE 16
typedef struct {
    char *src;   /* source name as written */
    char *cname; /* lifted/global C (mangled) name */
    int param_count;
} FuncBind;

typedef struct Scope {
    VarEntry *vars;
    int count;
    int vars_cap;
    int disallow_decl;       /* 1 = let/const not allowed (control blocks) */
    FuncBind *funcs;
    int func_count;
    int funcs_cap;
    struct Scope *parent;
} Scope;

typedef Scope SymbolTable; /* global/root scope */

typedef struct {
    ASTNode *head;
    ASTNode *functions;
    SymbolTable symbol_table;
    int count;
} Program;

typedef struct {
    int line;
    int col;
    char *message;
} CompileError;

typedef struct {
    CompileError *errors;
    int count;
    int errors_cap;
    char **source_lines;
    int total_lines;
    int source_lines_cap;
    char *filename;
    int check_only;
} ErrorState;

void error_state_init(ErrorState *state, char *source, const char *filename, int check_only);
void error_state_add(ErrorState *state, int line, int col, const char *msg);
void error_state_print(ErrorState *state);
int error_state_has_errors(ErrorState *state);

void lexer_init(Lexer *lexer, char *source);
void lexer_tokenize(Lexer *lexer, ErrorState *errors);
void lexer_free(Lexer *lexer);
Token lexer_next(Lexer *lexer);
Token lexer_peek(Lexer *lexer);

Program *parse(Lexer *lexer, ErrorState *errors);
typedef struct {
    FILE *file;
    const char *src_file;
    int enabled;
} CodegenLog;

void codegen_set_log(CodegenLog *log);
void codegen(Program *program, SymbolTable *table, FILE *out, ErrorState *errors);
void codegen_free(Program *program);

int is_c_reserved(const char *name);
void mangle_name(const char *in, char *out, int out_size);
char *mangle_name_alloc(const char *in);

VarType parse_expression_from_string(const char *expr, SymbolTable *table,
                                     char **out, int *out_cap);
ErrorState *codegen_get_errors(void);
int symbol_table_has(SymbolTable *table, const char *name);
VarType symbol_table_get(SymbolTable *table, const char *name);
const char *symbol_table_get_cname(SymbolTable *table, const char *name);
int symbol_table_is_const(SymbolTable *table, const char *name);
void scope_init(Scope *scope, Scope *parent, int disallow_decl);
void symbol_table_add_local(SymbolTable *table, const char *name, VarType type);
void symbol_table_add_param(SymbolTable *table, const char *name, VarType type);
void symbol_table_update(SymbolTable *table, const char *name, VarType type);
int symbol_table_has_local(SymbolTable *table, const char *name);
int symbol_table_get_locked(SymbolTable *table, const char *name);
void symbol_table_set_locked(SymbolTable *table, const char *name, int locked);
int symbol_table_is_param(SymbolTable *table, const char *name);
void symbol_table_set_param(SymbolTable *table, const char *name, int is_param);
void symbol_table_set_array_dims(SymbolTable *table, const char *name,
                                 int dim_count, const int *dims);
int symbol_table_get_array_dims(SymbolTable *table, const char *name,
                                int **dims, int *dim_count);
void symbol_table_set_array_elem_type(SymbolTable *table, const char *name, VarType elem_type);
VarType symbol_table_get_array_elem_type(SymbolTable *table, const char *name);

void symbol_table_add_func(SymbolTable *table, const char *src, const char *cname, int param_count);
char *resolve_func_name(SymbolTable *table, const char *src);
int is_function_name_scoped(SymbolTable *table, const char *src);

void obj_register(const char *mangled_var, ObjSchema *schema);
ObjSchema *obj_lookup(const char *mangled_var);
const char *obj_resolve_field(const char *mangled_var, const char *field, char *out, int out_size);
char *obj_resolve_field_alloc(const char *mangled_var, const char *field);

void ir_write(Program *program, FILE *out);

#endif
