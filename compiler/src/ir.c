#include "ltng.h"

static const char *node_type_name(NodeType t) {
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
        default: return "UNKNOWN";
    }
}

static const char *var_type_name_ir(VarType t) {
    switch (t) {
        case TYPE_INT: return "int";
        case TYPE_FLOAT: return "float";
        case TYPE_STRING: return "string";
        case TYPE_CHAR: return "char";
        case TYPE_BOOL: return "bool";
        case TYPE_OBJECT: return "object";
        case TYPE_NULL: return "null";
        case TYPE_ARRAY: return "array";
        default: return "unknown";
    }
}

static void ir_indent(FILE *out, int indent) {
    for (int i = 0; i < indent; i++) fputs("  ", out);
}

static void ir_print_escaped(FILE *out, const char *s) {
    fputc('"', out);
    for (const char *p = s ? s : ""; *p; p++) {
        if (*p == '\\') fputs("\\\\", out);
        else if (*p == '"') fputs("\\\"", out);
        else if (*p == '\n') fputs("\\n", out);
        else if (*p == '\t') fputs("\\t", out);
        else if (*p == '\r') fputs("\\r", out);
        else fputc(*p, out);
    }
    fputc('"', out);
}

static void ir_write_node(FILE *out, ASTNode *node, int indent);

static void ir_write_list(FILE *out, const char *label, ASTNode *list, int indent) {
    if (!list) return;
    ir_indent(out, indent);
    fprintf(out, "%s:\n", label);
    for (ASTNode *n = list; n; n = n->next) {
        ir_write_node(out, n, indent + 1);
    }
}

static void ir_write_node(FILE *out, ASTNode *node, int indent) {
    if (!node) return;
    ir_indent(out, indent);
    fprintf(out, "- node: %s\n", node_type_name(node->type));
    ir_indent(out, indent + 1); fprintf(out, "name: "); ir_print_escaped(out, node->name); fputc('\n', out);
    ir_indent(out, indent + 1); fprintf(out, "value: "); ir_print_escaped(out, node->value); fputc('\n', out);
    ir_indent(out, indent + 1); fprintf(out, "var_type: %s\n", var_type_name_ir(node->var_type));
    ir_indent(out, indent + 1); fprintf(out, "is_variable: %d\n", node->is_variable);
    ir_indent(out, indent + 1); fprintf(out, "is_expr: %d\n", node->is_expr);
    ir_indent(out, indent + 1); fprintf(out, "is_default: %d\n", node->is_default);
    ir_indent(out, indent + 1); fprintf(out, "is_base_assign: %d\n", node->is_base_assign);
    if (node->type == NODE_TRY) {
        ir_indent(out, indent + 1); fprintf(out, "has_catch: %d\n", node->has_catch);
        ir_indent(out, indent + 1); fprintf(out, "has_else: %d\n", node->has_else);
        ir_indent(out, indent + 1); fprintf(out, "has_finally: %d\n", node->has_finally);
    }
    if (node->for_var[0]) { ir_indent(out, indent + 1); fprintf(out, "for_var: "); ir_print_escaped(out, node->for_var); fputc('\n', out); }
    if (node->for_start[0]) { ir_indent(out, indent + 1); fprintf(out, "for_start: "); ir_print_escaped(out, node->for_start); fputc('\n', out); }
    if (node->for_end[0]) { ir_indent(out, indent + 1); fprintf(out, "for_end: "); ir_print_escaped(out, node->for_end); fputc('\n', out); }
    if (node->for_step[0]) { ir_indent(out, indent + 1); fprintf(out, "for_step: "); ir_print_escaped(out, node->for_step); fputc('\n', out); }
    if (node->for_step_op) { ir_indent(out, indent + 1); fprintf(out, "for_step_op: '%c'\n", node->for_step_op); }
    if (node->array_dim_count > 0) {
        ir_indent(out, indent + 1); fprintf(out, "array_dim_count: %d\n", node->array_dim_count);
        ir_indent(out, indent + 1); fprintf(out, "array_dims: [");
        for (int i = 0; i < node->array_dim_count; i++) {
            fprintf(out, "%d%s", node->array_dims ? node->array_dims[i] : 0, (i + 1 < node->array_dim_count) ? ", " : "");
        }
        fprintf(out, "]\n");
        ir_indent(out, indent + 1); fprintf(out, "array_elem_count: %d\n", node->array_elem_count);
    }
    if (node->array_index[0]) { ir_indent(out, indent + 1); fprintf(out, "array_index: "); ir_print_escaped(out, node->array_index); fputc('\n', out); }
    ir_write_list(out, "body", node->body, indent + 1);
    ir_write_list(out, "orelse", node->orelse, indent + 1);
    ir_write_list(out, "else_body", node->else_body, indent + 1);
    ir_write_list(out, "cases", node->cases, indent + 1);
    ir_write_list(out, "params", node->params, indent + 1);
    ir_write_list(out, "args", node->args, indent + 1);
}

void ir_write(Program *program, FILE *out) {
    if (!program || !out) return;
    fprintf(out, "LightningIR v1\n");
    fprintf(out, "functions:\n");
    for (ASTNode *fn = program->functions; fn; fn = fn->next) {
        ir_write_node(out, fn, 1);
    }
    fprintf(out, "statements:\n");
    for (ASTNode *st = program->head; st; st = st->next) {
        ir_write_node(out, st, 1);
    }
}
