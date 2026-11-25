#ifndef PARSER_INTERNAL_H
#define PARSER_INTERNAL_H

#include "ltng.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

/* ---- shared globals ---- */
extern ErrorState *parser_errors;
extern int g_break_depth;
extern ASTNode *g_list;
extern int g_stmt_line;
extern int g_stmt_col;
extern int g_expr_sl, g_expr_sc, g_expr_el, g_expr_ec;

typedef struct {
    char *name;
    char *src;
    int param_count;
    int required_count;
    int is_variadic;
    int has_return_type;
    VarType return_type;
    char *return_obj_base;
    int line, col;
    int defined;
    int top_level;
    char **param_names;
    int *has_default;
    VarType *def_type;
    int *def_start;
    int *def_end;
    int param_cap;
    int passthrough_param;
} FuncInfo;

extern FuncInfo *g_funcs;
extern int g_func_count;
extern int g_func_cap;
extern int g_in_function;
extern int g_in_catch;
extern Program *g_program_ref;
extern char *g_enclosing_cname;
extern int g_ret_obj_count;
extern int g_anon_count;

extern ObjSchema *g_objs;
extern int g_obj_count;
extern int g_obj_cap;
extern int g_scope_var_uid;

/* ---- shared helpers ---- */
char *_ltng_cfmt(const char *fmt, ...);
ASTNode *create_node(NodeType type, const char *name, const char *value);
void emit(ASTNode *node);
ASTNode *reverse_list(ASTNode *head);
int is_bool_value(const char *value);
const char *bool_to_str(const char *val);
VarType detect_type(const char *value);
int is_type_keyword(const char *name);
int is_return_type_keyword(const char *name);
int is_type_conversion(const char *name);
int is_ltng_reserved(const char *name);
int is_ltng_keyword(const char *name);
int token_is_name(Token t);
VarType type_keyword_to_vartype(const char *name);
VarType effective_param_default_type(const char *src_name, VarType sym_type);
void skip_to_newline(Lexer *lexer);
int is_operator(TokenType t);

void ensure_func_cap(void);
void funcinfo_init(FuncInfo *fi);
void funcinfo_ensure_params(FuncInfo *fi, int n);
void objschema_ensure(ObjSchema *s, int n);
void objschema_free(ObjSchema *s);
void objschema_deep_copy(ObjSchema *dst, const ObjSchema *src);

/* symbol table / obj / func helpers */
void obj_register(const char *mangled_var, ObjSchema *schema);
ObjSchema *obj_lookup(const char *mangled_var);
const char *obj_resolve_field(const char *mangled_var, const char *field, char *out, int out_size);
char *obj_resolve_field_alloc(const char *mangled_var, const char *field);
void obj_build_all(ObjSchema *s);
void register_function(const char *name, int param_count);
int is_function_name(const char *name);
int is_function_name_scoped(SymbolTable *table, const char *src);
int is_callable_ident(SymbolTable *table, const char *src);
char *resolve_func_name(SymbolTable *table, const char *src);
void scope_init(Scope *scope, Scope *parent, int disallow_decl);
int symbol_table_has_in_parents(SymbolTable *table, const char *name);
void symbol_table_add_in(SymbolTable *table, const char *name, VarType type);
void symbol_table_add_in_ex(SymbolTable *table, const char *name, VarType type, int unique_cname);
void symbol_table_add(SymbolTable *table, const char *name, VarType type);
void symbol_table_add_locked(SymbolTable *table, const char *name, VarType type);
void symbol_table_set_func_ref(SymbolTable *table, const char *name, const char *func_cname, int param_count);
int symbol_table_get_func_ref(SymbolTable *table, const char *name, char *out_cname, int out_size, int *out_param_count);

/* expression / parser cross-file */
VarType collect_expression(Lexer *lexer, SymbolTable *table,
                           char **outp, int *out_cap,
                           int stop_on_rparen, int stop_on_comma,
                           int *out_sl, int *out_sc, int *out_el, int *out_ec);
VarType collect_expression_ex(Lexer *lexer, SymbolTable *table,
                              char **outp, int *out_cap,
                              int stop_on_rparen, int stop_on_comma,
                              int *out_sl, int *out_sc, int *out_el, int *out_ec,
                              int stop_on_ternary_colon);
ASTNode *parse_function(Lexer *lexer, SymbolTable *table, const char *enclosing_cname);
ASTNode *parse_block(Lexer *lexer, SymbolTable *table, int is_control);
ASTNode *parse_try(Lexer *lexer, SymbolTable *table);
void parse_throw(Lexer *lexer, SymbolTable *table);
void parse_statement(Lexer *lexer, SymbolTable *table);
ASTNode *parse_call_node(Lexer *lexer, SymbolTable *table, const char *fname_tok_val, int line, int col, ASTNode *first_arg);
int try_parse_typeof(Lexer *lexer, SymbolTable *table, Token *result);
int try_parse_sole_call_expr(Lexer *lexer, SymbolTable *table, char *cexpr, int cexpr_size, int allow_base_assign, VarType *out_type, char *out_call_cname, int out_call_cname_sz);
int parse_object_literal(Lexer *lexer, SymbolTable *table, const char *mangled, int is_const, int is_assignment);
int parse_array_literal(Lexer *lexer, SymbolTable *table, ASTNode *node);
ASTNode *find_function_ast_by_name(const char *name);
ASTNode *find_function_node_by_cname(const char *cname);
int resolve_func_ref_info(SymbolTable *table, const char *name, char **out_cname, int *out_pc);
VarType infer_function_return_type(const char *fname);
VarType infer_call_result_type(ASTNode *call);
int function_returns_string(const char *fname);

/* auto prototypes */
int expr_is_string_value(const char *expr);
void trim_c_expr(char *s);
void normalize_value_expr(char *buf, int buf_size, int array_access_used, SymbolTable *table);
int split_top_level(const char *s, const char *op, char *left, int lsz, char *right, int rsz);
void normalize_or_expr(char *buf, int buf_size, int array_access_used, SymbolTable *table);
void normalize_mixed_expr(char *buf, int buf_size, int array_access_used, SymbolTable *table);
const char * var_type_name(VarType t);
VarType infer_expression_text_type(const char *expr, SymbolTable *table);
void append_resolved_text(char *resolved, int *rlen, const char *text);
int resolve_typeof_interpolation(const char *vname, SymbolTable *table, int line, int col, char *resolved, int *rlen);
void resolve_string_interpolation(Token *tok, SymbolTable *table, int line, int col);
void parse_write(Lexer *lexer, SymbolTable *table);
void parse_read(Lexer *lexer, SymbolTable *table);
FuncInfo * find_funcinfo_by_cname(const char *cname);
void append_arg_expr(char *out, int *pos, int out_size, ASTNode *a);
void build_call_cexpr(ASTNode *call, char *cexpr, int cexpr_size);
int append_immediate_call_chains(Lexer *lexer, SymbolTable *table, char *cexpr, int cexpr_size);
ASTNode * make_receiver_arg(SymbolTable *table, Token rtok);
void emit_chained_methods_on_array_var(Lexer *lexer, SymbolTable *table, const char *target_src, const char *target_rm);
void parse_let(Lexer *lexer, SymbolTable *table);
void parse_const(Lexer *lexer, SymbolTable *table);
void token_append_source(char *out, int *pos, int out_size, Token tok);
const char * type_name_str(VarType t);
int check_set_type_immutable(SymbolTable *table, const char *mangled, VarType new_type, int line, int col, Lexer *lexer);
void parse_set(Lexer *lexer, SymbolTable *table);
ASTNode * parse_switch(Lexer *lexer, SymbolTable *table);
void transform_inc_dec_stmt(Lexer *lexer, SymbolTable *table);
ASTNode * parse_while(Lexer *lexer, SymbolTable *table);
ASTNode * parse_do_while(Lexer *lexer, SymbolTable *table);
ASTNode * parse_for(Lexer *lexer, SymbolTable *table);
ASTNode * parse_else_chain(Lexer *lexer, SymbolTable *table);
ASTNode * parse_if_core(Lexer *lexer, SymbolTable *table);
void skip_newlines(Lexer *lexer);
void build_catch_schema(const char *cname);
int ltng_ident_ends_with(const char *s, const char *suffix);
int try_parse_call_arg(Lexer *lexer, SymbolTable *table, char *cexpr, int cexpr_size, VarType *out_type);
int function_first_param_is_base(const char *cname);
int is_string_literal_return(ASTNode *ret);
int var_types_return_compatible_nodes(ASTNode *a, ASTNode *b);
void collect_return_nodes(ASTNode *node, ASTNode **nodes, int *count, int max);
int block_always_exits(ASTNode *block);
int stmt_always_exits(ASTNode *node);
int return_type_matches_annotation(ASTNode *ret, VarType want);
void validate_function_return_types(ASTNode *fn);
VarType analyze_function_returns(ASTNode *node, int *has_return);
int analyze_function_passthrough(ASTNode *node, int *pt_param, int *saw_return, int *disqualified);
int get_function_passthrough_param(const char *fname);
int find_local_func_binding(SymbolTable *table, const char *src, char *out, int out_size);
void eval_default_tokens(Lexer *lexer, SymbolTable *table, int start, int end, char **out, int *out_cap, VarType *out_type);
FuncInfo * resolve_overload(const char *src, ASTNode *args, int line, int col, int *group_count);
ASTNode * expand_call_args(Lexer *lexer, SymbolTable *table, FuncInfo *fi, ASTNode *fn_def, ASTNode *args, const char *fname, int line, int col, ASTNode **out_rest);
void record_return_obj_schema(const char *obj_base);
void parse_return(Lexer *lexer, SymbolTable *table);
void parse_exit(Lexer *lexer, SymbolTable *table);
int is_ident_start_char(char c);
int is_ident_char(char c);
void add_capture_name(char ***names, int *count, int *cap, const char *name);
void collect_idents_from_str(const char *s, char ***names, int *count, int *cap);
void collect_declared_locals(ASTNode *node, char ***names, int *count, int *cap, int depth);
void collect_used_idents(ASTNode *node, char ***names, int *count, int *cap, int depth);
int name_in_list(char **names, int count, const char *name);
int skip_default_expr(Lexer *lexer, int start);
void prescan_functions(Lexer *lexer);
Program * parse(Lexer *lexer, ErrorState *errors);
VarType parse_expression_from_string(const char *expr, SymbolTable *table, char **out, int *out_cap);
void free_node_list(ASTNode *node);
void codegen_free(Program *program);
void symbol_table_add_func(SymbolTable *table, const char *src, const char *cname, int param_count);
int is_c_reserved(const char *name);
void mangle_name(const char *in, char *out, int out_size);
char *mangle_name_alloc(const char *in);
void symbol_table_add_local(SymbolTable *table, const char *name, VarType type);
void symbol_table_add_param(SymbolTable *table, const char *name, VarType type);
void symbol_table_update(SymbolTable *table, const char *name, VarType type);
VarType symbol_table_get(SymbolTable *table, const char *name);
const char * symbol_table_get_cname(SymbolTable *table, const char *name);
int symbol_table_has(SymbolTable *table, const char *name);
int symbol_table_has_local(SymbolTable *table, const char *name);
void symbol_table_set_const(SymbolTable *table, const char *name, int is_const);
int symbol_table_is_const(SymbolTable *table, const char *name);
int symbol_table_get_locked(SymbolTable *table, const char *name);
void symbol_table_set_locked(SymbolTable *table, const char *name, int locked);
int symbol_table_is_param(SymbolTable *table, const char *name);
void symbol_table_set_param(SymbolTable *table, const char *name, int is_param);
void symbol_table_set_array_dims(SymbolTable *table, const char *name, int dim_count, const int *dims);
int symbol_table_get_array_dims(SymbolTable *table, const char *name, int **dims, int *dim_count);
void symbol_table_set_array_elem_type(SymbolTable *table, const char *name, VarType elem_type);
VarType symbol_table_get_array_elem_type(SymbolTable *table, const char *name);
int token_is_int(Token tok, SymbolTable *table);
int has_operator_ahead(Lexer *lexer);
void transform_string_multiply(char *expr, int size);
void sanitize_mod_atof(char *out, int out_size);
void normalize_string_expr(char *buf, int buf_size, int array_access_used, SymbolTable *table);
int is_add_operand(const char *p, char *content, int content_size);
int optimize_add_concat(char *out, int out_size, int pos);
int part_is_stringy(const char *part);

#endif
