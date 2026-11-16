static int parse_array_literal(Lexer *lexer, SymbolTable *table, ASTNode *node) {
    /* The caller has already consumed the opening '['. Parse the remaining
       elements/nested arrays until the matching ']'. Arrays may have up to
       three dimensions and are stored as a flat, row-major list of values. */
    int depth = 1;                 /* nesting level, outer '[' already consumed */
    int max_depth = 1;             /* deepest nesting level seen */
    int elem_count = 0;
    char values[4096] = "";
    int vpos = 0;

    int elem_counts[4] = {0, 0, 0, 0};       /* values in current group at each depth */
    int max_elem_counts[4] = {0, 0, 0, 0};   /* max values in a group at each depth */
    int child_counts[4] = {0, 0, 0, 0};      /* direct child groups in current group */
    int max_child_counts[4] = {0, 0, 0, 0};  /* max direct child groups in a group */
    int too_many_dims = 0;

    while (lexer->current < lexer->count) {
        Token t = lexer_peek(lexer);

        if (t.type == TOKEN_NEWLINE || t.type == TOKEN_EOF) break;

        if (t.type == TOKEN_LBRACKET) {
            lexer_next(lexer);
            if (depth >= 3) {
                if (!too_many_dims) {
                    error_state_add(parser_errors, t.line, t.col,
                        "Arrays support at most 3 dimensions");
                    too_many_dims = 1;
                }
                /* Still track nesting so the matching ']' is consumed. */
                depth++;
                continue;
            }
            /* Starting a child group of the current group. */
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
            if (depth <= 3) {
                if (elem_counts[depth] > max_elem_counts[depth]) {
                    max_elem_counts[depth] = elem_counts[depth];
                }
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
        if (depth <= 3) {
            elem_counts[depth]++;
            if (elem_counts[depth] > max_elem_counts[depth]) {
                max_elem_counts[depth] = elem_counts[depth];
            }
        }
    }

    if (too_many_dims) return 0;

    int dim_count = max_depth;
    if (dim_count < 1) dim_count = 1;
    if (dim_count > 3) dim_count = 3;

    int dims[3] = {0, 0, 0};
    if (dim_count == 1) {
        dims[0] = elem_count;
    } else if (dim_count == 2) {
        dims[0] = max_child_counts[1];
        dims[1] = max_elem_counts[2];
    } else {
        dims[0] = max_child_counts[1];
        dims[1] = max_child_counts[2];
        dims[2] = max_elem_counts[3];
    }

    node->array_dim_count = dim_count;
    node->array_dims[0] = dims[0];
    node->array_dims[1] = dims[1];
    node->array_dims[2] = dims[2];
    node->array_elem_count = elem_count;
    strncpy(node->value, values, MAX_STRING_LEN - 1);
    node->value[MAX_STRING_LEN - 1] = '\0';

    return 1;
}
