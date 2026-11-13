#ifndef LTNG_H
#define LTNG_H

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define MAX_TOKENS 1024  /* initial token capacity */
#define MAX_STRING_LEN 4096
#define MAX_IDENTIFIER_LEN 256
#define MAX_VARS 256
#define MAX_ERRORS 256
#define MAX_SOURCE_LINES 4096

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
    TOKEN_COMMA,
    TOKEN_DOT,
    TOKEN_COLON,
    TOKEN_LBRACE,
    TOKEN_RBRACE,
    TOKEN_LBRACKET,
    TOKEN_RBRACKET
} TokenType;

typedef struct {
    TokenType type;
    char value[MAX_STRING_LEN];
    int line;
    int col;
} Token;

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
    TYPE_ARRAY
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
    NODE_ARRAY_REVERSE
} NodeType;

typedef struct ASTNode {
    NodeType type;
    char name[MAX_IDENTIFIER_LEN];
    char value[MAX_STRING_LEN];
    VarType var_type;
    int is_variable;
    int is_expr;
    int is_default;
    int is_base_assign; /* UFCS call whose return value is assigned to receiver (base param) */
    char for_var[MAX_IDENTIFIER_LEN];
    char for_start[MAX_STRING_LEN];
    char for_end[MAX_STRING_LEN];
    char for_step[MAX_STRING_LEN];
    char for_step_op; /* '+', '-', '*', '/' or 0 for default (+) */
    struct ASTNode *body;
    struct ASTNode *orelse;
    struct ASTNode *cases;
    struct ASTNode *params;  /* function parameters (NODE_PARAM/name chain) */
    struct ASTNode *args;    /* call arguments (expression nodes chain) */
    struct ASTNode *next;
    /* Array support */
    int array_dim_count;     /* number of dimensions (0 if not an array) */
    int *array_dims;         /* dynamically allocated size of each dimension */
    int array_elem_count;    /* total number of elements */
    char array_index[MAX_STRING_LEN]; /* for array element set: C index expression */
    struct ASTNode *index1;  /* first index expression */
    struct ASTNode *index2;  /* second index expression */
    struct ASTNode *index3;  /* third index expression */
} ASTNode;

typedef struct {
    char name[MAX_IDENTIFIER_LEN];   /* lookup key (mangled source name) */
    char cname[MAX_IDENTIFIER_LEN];  /* emitted C identifier */
    VarType type;
    int is_const;
    int is_locked;   /* dtype lock: once locked, dtype cannot change (even null->type) */
    int array_dim_count;  /* number of array dimensions (0 if not an array) */
    int *array_dims;      /* dynamically allocated size of each array dimension */
} VarEntry;

#define MAX_OBJ_FIELDS 32
typedef struct {
    char name[MAX_IDENTIFIER_LEN];   /* source field name */
    char value[MAX_STRING_LEN];      /* stringified value */
    VarType type;
} ObjField;

typedef struct {
    char var[MAX_IDENTIFIER_LEN];    /* mangled var name */
    ObjField fields[MAX_OBJ_FIELDS];
    int count;
    char default_val[MAX_STRING_LEN];
} ObjSchema;

#define MAX_FUNCBINDS 64
#define ARRAY_PUSH_RESERVE 16
typedef struct {
    char src[MAX_IDENTIFIER_LEN];   /* source name as written */
    char cname[MAX_IDENTIFIER_LEN]; /* lifted/global C (mangled) name */
    int param_count;
} FuncBind;

typedef struct Scope {
    VarEntry vars[MAX_VARS];
    int count;
    int disallow_decl;       /* 1 = let/const not allowed (control blocks) */
    FuncBind funcs[MAX_FUNCBINDS];
    int func_count;
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
    char message[MAX_STRING_LEN];
} CompileError;

typedef struct {
    CompileError errors[MAX_ERRORS];
    int count;
    char *source_lines[MAX_SOURCE_LINES];
    int total_lines;
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
void codegen(Program *program, SymbolTable *table, FILE *out, ErrorState *errors);
void codegen_free(Program *program);

int is_c_reserved(const char *name);
void mangle_name(const char *in, char *out, int out_size);

VarType parse_expression_from_string(const char *expr, SymbolTable *table,
                                     char *out, int out_size);
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
void symbol_table_set_array_dims(SymbolTable *table, const char *name,
                                 int dim_count, const int *dims);
int symbol_table_get_array_dims(SymbolTable *table, const char *name,
                                int **dims, int *dim_count);

void symbol_table_add_func(SymbolTable *table, const char *src, const char *cname, int param_count);
const char *resolve_func_name(SymbolTable *table, const char *src, char *out, int out_size);
int is_function_name_scoped(SymbolTable *table, const char *src);

void obj_register(const char *mangled_var, ObjSchema *schema);
ObjSchema *obj_lookup(const char *mangled_var);
const char *obj_resolve_field(const char *mangled_var, const char *field, char *out, int out_size);

#endif
