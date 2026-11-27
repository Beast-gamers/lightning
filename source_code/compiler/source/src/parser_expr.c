#include "parser_internal.h"

#define SNPRINTF_FULL(buf,...) do{ char *_sf=_ltng_cfmt(__VA_ARGS__); if(_sf){ size_t _sn=strlen(_sf)+1; if(_sn<65536){ char *_sg=(char*)realloc(_sf,65536); if(_sg){_sf=_sg; _sn=65536;} } free(buf); buf=_sf; } }while(0)
#define EXPR_APPEND(buf,cap,pos,...) do{ int _sa_n; for(;;){ _sa_n=snprintf((buf)+(pos),(cap)-(pos),__VA_ARGS__); if(_sa_n<0){(buf)[pos]=0;break;} if(_sa_n>=(cap)-(pos)){int _sa_nc=(cap)*2; if(_sa_nc<(pos)+_sa_n+1)_sa_nc=(pos)+_sa_n+1; char*_sa_nb=(char*)realloc((buf),_sa_nc); if(!_sa_nb)break; (buf)=_sa_nb; (cap)=_sa_nc; continue;} (pos)+=_sa_n; break;} }while(0)

#define BUF_SET(bufp,cap,...) do{ char *_sf=_ltng_cfmt(__VA_ARGS__); if(_sf){ free(*(bufp)); *(bufp)=_sf; *(cap)=(int)strlen(_sf)+1; if(*(cap)<65536){char*_sg=(char*)realloc(*(bufp),65536); if(_sg){*(bufp)=_sg; *(cap)=65536;} } } }while(0)
static void _expr_ensure(char **p, int *cap, int need) {
    if (*cap > need) return;
    int nc = *cap * 2;
    if (nc <= need) nc = need + 1;
    char *nb = (char*)realloc(*p, nc);
    if (!nb) return;
    *p = nb; *cap = nc;
}

int parse_array_literal(Lexer *lexer, SymbolTable *table, ASTNode *node) {
    /* The caller has already consumed the opening '['. Parse the remaining
       elements/nested arrays until the matching ']'. Arrays are stored as a
       flat, row-major list of values. Arrays may have any number of dimensions.
       Element encoding in node->value (pipe-separated): a leading '\x01' marks
       a variable/C-expression element (its C name follows); otherwise the text
       is pushed as a string literal value. */
    int depth = 1;                 /* nesting level, outer '[' already consumed */
    int max_depth = 1;             /* deepest nesting level seen */
    int elem_count = 0;
    int vcap = 4096;
    char *values = (char*)calloc(1, (size_t)vcap);
    int vpos = 0;
    if (!values) return -1;

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
        char *val = (char*)malloc(65536);
        if (!val) { error_state_add(parser_errors, t.line, t.col, "Out of memory"); continue; }
        val[0] = 0;
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
                SNPRINTF_FULL(val, "'%s'", t1.value);
                this_elem_type = TYPE_CHAR;
            } else if (t1.type == TOKEN_INT_LIT) {
                SNPRINTF_FULL(val, "%s", t1.value);
                this_elem_type = TYPE_INT;
            } else if (t1.type == TOKEN_FLOAT_LIT) {
                SNPRINTF_FULL(val, "%s", t1.value);
                this_elem_type = TYPE_FLOAT;
            } else if (t1.type == TOKEN_STRING) {
                SNPRINTF_FULL(val, "%s", t1.value);
                this_elem_type = TYPE_STRING;
            } else if (t1.type == TOKEN_IDENT && !is_bool_value(t1.value) && !is_type_keyword(t1.value)) {
                char *m = mangle_name_alloc(t1.value);
                if (symbol_table_has(table, m) || symbol_table_has(table, t1.value)) {
                    const char *cname = symbol_table_has(table, m) ? symbol_table_get_cname(table, m) : symbol_table_get_cname(table, t1.value);
                    SNPRINTF_FULL(val, "%c%s", '\x01', cname);
                } else {
                    SNPRINTF_FULL(val, "%s", t1.value);
                }
                free(m);
                /* variable reference element: runtime type unknown */
                this_elem_type = TYPE_NULL;
            } else if (t1.type == TOKEN_IDENT && is_bool_value(t1.value)) {
                if (strcmp(t1.value, "true") == 0 || strcmp(t1.value, "t") == 0)
                    SNPRINTF_FULL(val, "true");
                else
                    SNPRINTF_FULL(val, "false");
                this_elem_type = TYPE_BOOL;
            } else {
                SNPRINTF_FULL(val, "%s", t1.value);
                this_elem_type = TYPE_NULL;
            }
        } else {
            /* Parse as expression element (supports conversion calls, arithmetic, calls, etc.) */
            char *expr = (char*)malloc(65536);
            if (!expr) { free(val); error_state_add(parser_errors, t1.line, t1.col, "Out of memory"); continue; }
            expr[0] = 0;
            VarType etype = collect_expression(lexer, table, &expr, &(int){65536}, 1, 1, NULL, NULL, NULL, NULL);
            if (expr[0] == '"') {
                /* C string literal result: store raw content (codegen will quote/escape) */
                size_t elen = strlen(expr);
                if (elen >= 2 && expr[elen-1] == '"') {
                    expr[elen-1] = '\0';
                    SNPRINTF_FULL(val, "%s", expr + 1);
                } else {
                    SNPRINTF_FULL(val, "%c%s", '\x01', expr);
                }
                this_elem_type = TYPE_STRING;
            } else if (etype == TYPE_STRING || etype == TYPE_OBJECT || etype == TYPE_ARRAY || etype == TYPE_CHAR) {
                SNPRINTF_FULL(val, "%c%s", '\x01', expr);
                this_elem_type = TYPE_STRING;
            } else if (etype == TYPE_BOOL) {
                char *wrapped = _ltng_cfmt("((%s)?\"true\":\"false\")", expr);
                if (wrapped) { SNPRINTF_FULL(val, "%c%s", '\x01', wrapped); free(wrapped); }
                this_elem_type = TYPE_BOOL;
            } else {
                char *wrapped = (etype == TYPE_FLOAT)
                    ? _ltng_cfmt("_ltng_ad((double)(%s))", expr)
                    : _ltng_cfmt("_ltng_ad((double)(long long)(%s))", expr);
                if (wrapped) { SNPRINTF_FULL(val, "%c%s", '\x01', wrapped); free(wrapped); }
                this_elem_type = (etype == TYPE_FLOAT) ? TYPE_FLOAT : TYPE_INT;
            }
            free(expr);
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
        { int need = vpos + vlen + 2; if (need >= vcap) { int nc = vcap; while (nc <= need) nc *= 2; char *nv = (char*)realloc(values, (size_t)nc); if (nv) { values = nv; vcap = nc; } }
          if (vpos + vlen + 1 < vcap) {
            if (vpos > 0) values[vpos++] = '|';
            memcpy(values + vpos, val, vlen);
            vpos += vlen;
          }
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
    { free(node->value); node->value = ltng_strdup(values); }
    free(values);

    return 1;
}



int expr_is_string_value(const char *expr) {
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
    size_t _ibsz = (size_t)(pend - p); char *inner_buf = (char*)malloc(_ibsz + 1); if(!inner_buf) return 0;
    size_t ilen = (size_t)(pend - p);
    /* ilen already bounded by alloc */
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
        { free(inner_buf); return 0; }
    if (strncmp(p, "(long long)", 11) == 0 || strncmp(p, "(int)", 5) == 0 ||
        strncmp(p, "(double)", 8) == 0 || strncmp(p, "(float)", 7) == 0)
        { free(inner_buf); return 0; }
    if (strncmp(p, "_ltng_from_double(", 18) == 0 || strncmp(p, "_ltng_from_int(", 15) == 0 ||
        strncmp(p, "_ltng_ad(", 9) == 0)
        { free(inner_buf); return 0; }
    /* Bool numeric forms */
    if (strstr(p, "[0]=='t'") != NULL || strstr(p, "[0]=='T'") != NULL) return 0;
    /* Known string-producing helpers/exprs (not generic buffers like _ltng_retbuf/_ltng_ad) */
    if (strstr(p, "_ltng_add_sn(") || strstr(p, "_ltng_repeat(") ||
        strstr(p, "_ltng_as(") || strstr(p, "_ltng_concat(") ||
        strstr(p, "_ltng_oret_"))
        { free(inner_buf); return 1; }
    /* Statement expression that builds strings */
    if (strncmp(p, "({", 2) == 0) {
        if (strstr(p, "_ltng_add_sn") || strstr(p, "_ltng_repeat") ||
            strstr(p, "_ltng_as(") || strstr(p, "_ltng_concat") ||
            strstr(p, "_ltng_oret_"))
            return 1;
        if (strstr(p, "char _ltc") || strstr(p, "char _ltm"))
            return 1;
        { free(inner_buf); return 0; }
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
        { free(inner_buf); return 0; }
    /* Contains C string literals => string expression */
    if (strchr(p, '"') != NULL) return 1;
    /* Other specific string helpers */
    if (strstr(p, "_ltng_add_sn") || strstr(p, "_ltng_repeat") || strstr(p, "_ltng_oret_"))
        { free(inner_buf); return 1; }
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
    { free(inner_buf); return 0; }
}
void trim_c_expr(char *s) {
    char *p = s;
    while (*p == ' ' || *p == '\t' || *p == '\n') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t len = strlen(s);
    while (len > 0 && (s[len-1] == ' ' || s[len-1] == '\t' || s[len-1] == '\n')) { s[--len] = '\0'; }
}

/* Normalize a value subexpression that may be a string concat/repeat. */
void normalize_value_expr(char **bufp, int *cap, int array_access_used, SymbolTable *table) {
    trim_c_expr(*bufp);
    if (!(*bufp)[0]) return;
    int has_plus = 0, has_star = 0, in = 0; char qc = 0;
    for (const char *p = *bufp; *p; p++) {
        if (in) { if (*p == '\\' && p[1]) { p++; continue; } if (*p == qc) in = 0; continue; }
        if (*p == '"' || *p == '\'') { in = 1; qc = *p; continue; }
        if (*p == '+') has_plus = 1;
        if (*p == '*') has_star = 1;
    }
    if (has_plus || has_star) {
        if (expr_is_string_value(*bufp) ||
            strchr(*bufp, '"') || strstr(*bufp, "_ltng_add_sn") || strstr(*bufp, "_ltng_repeat") ||
            strstr(*bufp, "_ltng_as(") || strstr(*bufp, "_ltng_ad(")) {
            normalize_string_expr(bufp, cap, array_access_used, table);
        }
    }
}

int split_top_level(const char *s, const char *op, char **leftp, int *lcap, char **rightp, int *rcap) {
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
            if (oplen == 1) {
                if (op[0] == '<' && s[i+1] == '=') continue;
                if (op[0] == '>' && s[i+1] == '=') continue;
                if (op[0] == '!' && s[i+1] == '=') continue;
                if (op[0] == '=' && s[i+1] == '=') continue;
                if (op[0] == '&' && s[i+1] == '&') continue;
                if (op[0] == '|' && s[i+1] == '|') continue;
            }
            found = i;
        }
    }
    if (found < 0) return 0;
    int llen = found;
    _expr_ensure(leftp, lcap, llen + 1);
    memcpy(*leftp, s, llen); (*leftp)[llen] = '\0';
    int rlen = (int)strlen(s + found + oplen);
    _expr_ensure(rightp, rcap, rlen + 1);
    memcpy(*rightp, s + found + oplen, rlen + 1);
    trim_c_expr(*leftp); trim_c_expr(*rightp);
    return 1;
}

void normalize_mixed_expr(char **bufp, int *cap, int array_access_used, SymbolTable *table);

void normalize_or_expr(char **bufp, int *cap, int array_access_used, SymbolTable *table) {
    int left_cap=4096,right_cap=4096;
    char *left = (char*)malloc(left_cap); char *right = (char*)malloc(right_cap);
    if(!left||!right){free(left);free(right);return;} left[0]=0; right[0]=0;
    if (split_top_level(*bufp, "||", &left, &left_cap, &right, &right_cap)) {
        normalize_mixed_expr(&left, &left_cap, array_access_used, table);
        normalize_mixed_expr(&right, &right_cap, array_access_used, table);
        BUF_SET(bufp, cap, "(%s) || (%s)", left, right);
        free(left); free(right);
        return;
    }
    /* try && */
    if (split_top_level(*bufp, "&&", &left, &left_cap, &right, &right_cap)) {
        normalize_mixed_expr(&left, &left_cap, array_access_used, table);
        normalize_mixed_expr(&right, &right_cap, array_access_used, table);
        BUF_SET(bufp, cap, "(%s) && (%s)", left, right);
        free(left); free(right);
        return;
    }
    /* comparisons: == != <= >= < > (check two-char first) */
    const char *ops[] = {"==", "!=", "<=", ">=", "<", ">", NULL};
    for (int i = 0; ops[i]; i++) {
        if (split_top_level(*bufp, ops[i], &left, &left_cap, &right, &right_cap)) {
            int is_str_cmp = 0;
            if (strcmp(ops[i], "==") == 0 || strcmp(ops[i], "!=") == 0) {
                char *ltmp = ltng_strdup(left ? left : ""); char *rtmp = ltng_strdup(right ? right : "");
                if(!ltmp||!rtmp){free(ltmp);free(rtmp);} else {
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
                free(ltmp); free(rtmp); }
            }
            if (is_str_cmp) {
                /* But if it's actually a numeric-conversion helper vs numeric literal/value, compare numerically */
                {
                    char *lt = ltng_strdup(left ? left : ""); char *rt = ltng_strdup(right ? right : "");
                    if(lt&&rt){ trim_c_expr(lt); trim_c_expr(rt);
                    int lnumh = (strstr(lt,"_ltng_from_double(")||strstr(lt,"_ltng_from_int(")||strstr(lt,"_ltng_ad(")||strstr(lt,"_LTNG_FMOD_D(")||strstr(lt,"_LTNG_DIV_D(")||strstr(lt,"atof(")||strstr(lt,"atoi(")||strstr(lt,"atoll("));
                    int rnumh = (strstr(rt,"_ltng_from_double(")||strstr(rt,"_ltng_from_int(")||strstr(rt,"_ltng_ad(")||strstr(rt,"_LTNG_FMOD_D(")||strstr(rt,"_LTNG_DIV_D(")||strstr(rt,"atof(")||strstr(rt,"atoi(")||strstr(rt,"atoll("));
                    const char *lq2=lt; while(*lq2==' '||*lq2=='\t') lq2++; const char *rq2=rt; while(*rq2==' '||*rq2=='\t') rq2++;
                    int lnl = (*lq2=='-'||*lq2=='+'||isdigit((unsigned char)*lq2));
                    int rnl = (*rq2=='-'||*rq2=='+'||isdigit((unsigned char)*rq2));
                    if ((lnumh && (rnumh || rnl)) || (rnumh && (lnumh || lnl))) {
                        normalize_value_expr(&left, &left_cap, array_access_used, table);
                        normalize_value_expr(&right, &right_cap, array_access_used, table);
                        BUF_SET(bufp, cap, "(%s) %s (%s)", left, ops[i], right);
                        free(lt); free(rt); free(left); free(right);
                        return;
                    }
                    } free(lt); free(rt);
                }
                /* Normalize both sides as string expressions (handle + / * concat/repeat) */
                /* In string comparison context, + / * on bare-buffer operands are string ops */
                {
                    int hp=0,hs=0,in=0; char qc=0;
                    for(const char *p=left;*p;p++){ if(in){if(*p=='\\'&&p[1]){p++;continue;} if(*p==qc)in=0; continue;} if(*p=='"'||*p=='\''){in=1;qc=*p;continue;} if(*p=='+')hp=1; if(*p=='*')hs=1; }
                    if(hp||hs) normalize_string_expr(&left,&left_cap,array_access_used, table);
                }
                {
                    int hp=0,hs=0,in=0; char qc=0;
                    for(const char *p=right;*p;p++){ if(in){if(*p=='\\'&&p[1]){p++;continue;} if(*p==qc)in=0; continue;} if(*p=='"'||*p=='\''){in=1;qc=*p;continue;} if(*p=='+')hp=1; if(*p=='*')hs=1; }
                    if(hp||hs) normalize_string_expr(&right,&right_cap,array_access_used, table);
                }
                normalize_value_expr(&left, &left_cap, array_access_used, table);
                normalize_value_expr(&right, &right_cap, array_access_used, table);
                /* Ensure numeric literals are quoted for strcmp */
                {
                    char *lq = ltng_strdup(left ? left : ""); char *rq = ltng_strdup(right ? right : "");
                    if(!lq||!rq){free(lq);free(rq);} else {
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
                    if (ln) { char *_t=_ltng_cfmt("\"%s\"", lp); if(_t){free(lq); lq=_t;} } else { char *_t=ltng_strdup(left?left:""); if(_t){free(lq); lq=_t;} }
                    if (rn) { char *_t=_ltng_cfmt("\"%s\"", rp); if(_t){free(rq); rq=_t;} } else { char *_t=ltng_strdup(right?right:""); if(_t){free(rq); rq=_t;} }
                    {
                        int lnumh = (strstr(left,"_ltng_from_double(")||strstr(left,"_ltng_from_int(")||strstr(left,"_ltng_ad(")||strstr(left,"_LTNG_FMOD_D(")||strstr(left,"_LTNG_DIV_D(")||strstr(left,"atof(")||strstr(left,"atoi(")||strstr(left,"atoll("));
                        int rnumh = (strstr(right,"_ltng_from_double(")||strstr(right,"_ltng_from_int(")||strstr(right,"_ltng_ad(")||strstr(right,"_LTNG_FMOD_D(")||strstr(right,"_LTNG_DIV_D(")||strstr(right,"atof(")||strstr(right,"atoi(")||strstr(right,"atoll("));
                        /* Numeric expression compared with quoted numeric string literal: compare numerically */
                        int lq_qnumlit = 0, rq_qnumlit = 0;
                        {
                            const char *lpq = lq; while (*lpq==' '||*lpq=='\t') lpq++;
                            if (*lpq=='"') { const char *p=lpq+1; if(*p=='+'||*p=='-') p++; int hd=0,ok=1,dot=0; while(*p && *p!='"'){ if(isdigit((unsigned char)*p)) hd=1; else if(*p=='.'&&!dot) dot=1; else {ok=0;break;} p++; } if(*p=='"' && hd && ok && p[1]=='\0') lq_qnumlit=1; }
                            const char *rpq = rq; while (*rpq==' '||*rpq=='\t') rpq++;
                            if (*rpq=='"') { const char *p=rpq+1; if(*p=='+'||*p=='-') p++; int hd=0,ok=1,dot=0; while(*p && *p!='"'){ if(isdigit((unsigned char)*p)) hd=1; else if(*p=='.'&&!dot) dot=1; else {ok=0;break;} p++; } if(*p=='"' && hd && ok && p[1]=='\0') rq_qnumlit=1; }
                        }
                        if ((lnumh && (rnumh || rn || rq_qnumlit)) || (rnumh && (lnumh || ln || lq_qnumlit))) {
                            /* if side is quoted numeric literal string, unquote for numeric compare */
                            char *lcmp = ltng_strdup(left?left:""); char *rcmp = ltng_strdup(right?right:"");
                            if(lcmp&&rcmp){
                            if (lq_qnumlit) { const char *p=lq; while(*p==' '||*p=='\t') p++; if(*p=='"'){ size_t n=strlen(p); if(n>=2 && p[n-1]=='"'){ char *_nc=_ltng_cfmt("%.*s",(int)(n-2),p+1); if(_nc){free(lcmp);lcmp=_nc;} } } }
                            if (rq_qnumlit) { const char *p=rq; while(*p==' '||*p=='\t') p++; if(*p=='"'){ size_t n=strlen(p); if(n>=2 && p[n-1]=='"'){ char *_nc=_ltng_cfmt("%.*s",(int)(n-2),p+1); if(_nc){free(rcmp);rcmp=_nc;} } } }
                            BUF_SET(bufp, cap, "(%s) %s (%s)", lcmp, ops[i], rcmp);
                            }
                            free(lcmp); free(rcmp);
                        } else {
                            /* Ensure numeric helper expressions become string expressions for strcmp */
                            char *lsc = ltng_strdup(lq?lq:""); char *rsc = ltng_strdup(rq?rq:"");
                            if(!lsc||!rsc){
                                free(lsc); free(rsc);
                                BUF_SET(bufp, cap, "strcmp(%s, %s) %s 0", lq, rq, ops[i]);
                            } else {
                                if (lnumh) {
                                    int is_f = (strstr(left,"atof(")||strstr(left,"_ltng_from_double(")||strstr(left,"_ltng_ad(")||strstr(left,"_LTNG_FMOD_D(")||strstr(left,"_LTNG_DIV_D("));
                                    if (is_f)
                                        { char *_t=_ltng_cfmt("({ _ltng_retbuf_fmt(\"%%g\",(double)(%s)); })", left); if(_t){free(lsc); lsc=_t;} }
                                    else
                                        { char *_t=_ltng_cfmt("({ _ltng_retbuf_fmt(\"%%lld\",(long long)(%s)); })", left); if(_t){free(lsc); lsc=_t;} }
                                }
                                if (rnumh) {
                                    int is_f = (strstr(right,"atof(")||strstr(right,"_ltng_from_double(")||strstr(right,"_ltng_ad(")||strstr(right,"_LTNG_FMOD_D(")||strstr(right,"_LTNG_DIV_D("));
                                    if (is_f)
                                        { char *_t=_ltng_cfmt("({ _ltng_retbuf_fmt(\"%%g\",(double)(%s)); })", right); if(_t){free(rsc); rsc=_t;} }
                                    else
                                        { char *_t=_ltng_cfmt("({ _ltng_retbuf_fmt(\"%%lld\",(long long)(%s)); })", right); if(_t){free(rsc); rsc=_t;} }
                                }
                                if (strcmp(ops[i], "==") == 0)
                                    BUF_SET(bufp, cap, "(strcmp(%s, %s) == 0)", lsc, rsc);
                                else
                                    BUF_SET(bufp, cap, "(strcmp(%s, %s) != 0)", lsc, rsc);
                                free(lsc); free(rsc);
                            }
                        }
                        /* fallback: numeric helper compared to numeric string literal should be numeric */
                        {
                                                        if ((strstr(lq,"_ltng_from_double(")||strstr(lq,"_ltng_from_int(")||strstr(lq,"_ltng_ad(")) && rq[0]=='"') {
                                /* rq is "num" */
                                char *qe2=strchr(rq+1,'"'); size_t _nl = qe2 ? (size_t)(qe2-(rq+1)) : strlen(rq+1); char *num = (char*)malloc(_nl+1); if(num){ memcpy(num,rq+1,_nl); num[_nl]=0;
                                int ok=1; char *nn=num; if(*nn=='-'||*nn=='+') nn++; if(!*nn) ok=0; for(char*t=nn;*t;t++){ if(!isdigit((unsigned char)*t)&&*t!='.') {ok=0;break;} }
                                if(ok) BUF_SET(bufp, cap, "(%s) %s (%s)", left, ops[i], num);
                                free(num); }
                            } else if ((strstr(rq,"_ltng_from_double(")||strstr(rq,"_ltng_from_int(")||strstr(rq,"_ltng_ad(")) && lq[0]=='"') {
                                char *qe3=strchr(lq+1,'"'); size_t _nl2 = qe3 ? (size_t)(qe3-(lq+1)) : strlen(lq+1); char *num = (char*)malloc(_nl2+1); if(num){ memcpy(num,lq+1,_nl2); num[_nl2]=0;
                                int ok=1; char *nn=num; if(*nn=='-'||*nn=='+') nn++; if(!*nn) ok=0; for(char*t=nn;*t;t++){ if(!isdigit((unsigned char)*t)&&*t!='.') {ok=0;break;} }
                                if(ok) BUF_SET(bufp, cap, "(%s) %s (%s)", num, ops[i], right);
                                free(num); }
                            } else {
                                /* bool/int ternary expr compared with quoted "0"/"1" numeric literal: compare numerically */
                                int lq_qnum = (lq[0]=='"' && (strcmp(lq,"\"0\"")==0 || strcmp(lq,"\"1\"")==0));
                                int rq_qnum = (rq[0]=='"' && (strcmp(rq,"\"0\"")==0 || strcmp(rq,"\"1\"")==0));
                                int l_tern = (strstr(lq,"?1:0")!=NULL || strstr(lq,"? 1 : 0")!=NULL);
                                int r_tern = (strstr(rq,"?1:0")!=NULL || strstr(rq,"? 1 : 0")!=NULL);
                                if (lq_qnum && r_tern) BUF_SET(bufp, cap, "(%s) %s (%s)", lq[1]=='1'?"1":"0", ops[i], rq);
                                else if (rq_qnum && l_tern) BUF_SET(bufp, cap, "(%s) %s (%s)", lq, ops[i], rq[1]=='1'?"1":"0");
                            }
                        }
                    }
                    free(lq); free(rq); }
                }
            } else {
                normalize_value_expr(&left, &left_cap, array_access_used, table);
                normalize_value_expr(&right, &right_cap, array_access_used, table);
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
                                size_t n = (size_t)((end-1) - inner);
                                char *tmp = (char*)malloc(n+1);
                                if(tmp){ memcpy(tmp, inner, n); tmp[n] = 0;
                                memmove(s, tmp, strlen(tmp)+1); free(tmp); }
                            }
                        } else if (strncmp(p, "_ltng_from_int(_ltng_retbuf(), ", 32) == 0) {
                            const char *inner = p + 32;
                            const char *end = s + strlen(s);
                            if (end > p && *(end-1) == ')') {
                                size_t n = (size_t)((end-1) - inner);
                                char *tmp = (char*)malloc(n+1);
                                if(tmp){ memcpy(tmp, inner, n); tmp[n] = 0;
                                memmove(s, tmp, strlen(tmp)+1); free(tmp); }
                            }
                        } else if (strncmp(p, "_ltng_ad(", 9) == 0) {
                            const char *inner = p + 9;
                            const char *end = s + strlen(s);
                            if (end > p && *(end-1) == ')') {
                                size_t n = (size_t)((end-1) - inner);
                                char *tmp = (char*)malloc(n+1);
                                if(tmp){ memcpy(tmp, inner, n); tmp[n] = 0;
                                memmove(s, tmp, strlen(tmp)+1); free(tmp); }
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
                        char *lq = (char*)malloc(4096); char *rq = (char*)malloc(4096);
                        if(lq&&rq){
                        if (ln) { char *_t=_ltng_cfmt("\"%s\"", left); if(_t){free(lq); lq=_t;} } else { char *_t=ltng_strdup(left?left:""); if(_t){free(lq); lq=_t;} }
                        if (rn) { char *_t=_ltng_cfmt("\"%s\"", right); if(_t){free(rq); rq=_t;} } else { char *_t=ltng_strdup(right?right:""); if(_t){free(rq); rq=_t;} }
                        if (strcmp(ops[i], "==") == 0)
                            BUF_SET(bufp, cap, "(strcmp(%s, %s) == 0)", lq, rq);
                        else
                            BUF_SET(bufp, cap, "(strcmp(%s, %s) != 0)", lq, rq);
                        }
                        free(lq); free(rq);
                        free(left); free(right);
                        return;
                    }
                }
                BUF_SET(bufp, cap, "(%s) %s (%s)", left, ops[i], right);
            }
            free(left); free(right);
            return;
        }
    }
    /* No top-level logical/comparison op: treat as value expr */
    normalize_value_expr(bufp, cap, array_access_used, table);
    free(left); free(right);
}

void normalize_mixed_expr(char **bufp, int *cap, int array_access_used, SymbolTable *table) {
    trim_c_expr(*bufp);
    normalize_or_expr(bufp, cap, array_access_used, table);
    /* Fix: strcmp(numeric_helper, "num") / strcmp("num", numeric_helper) -> numeric compare */
    for (int pass = 0; pass < 4; pass++) {
        char *s = *bufp;
        char *p = strstr(s, "strcmp(");
        if (!p) break;
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
        char *endparen = q;
        char *after = endparen + 1;
        while (*after == ' ' || *after == '\t') after++;
        int is_eq = 0;
        if (strncmp(after, "== 0", 4) == 0) is_eq = 1;
        else if (strncmp(after, "!= 0", 4) == 0) { /* is_ne */ }
        else break;
        char *endcmp = after + 4;
        int l1 = (int)(comma - a1);
        char *leftarg = (char*)malloc(l1 + 1); if (!leftarg) break;
        memcpy(leftarg, a1, l1); leftarg[l1] = 0;
        char *a2 = comma + 1; while (*a2 == ' ' || *a2 == '\t') a2++;
        int l2 = (int)(endparen - a2);
        char *rightarg = (char*)malloc(l2 + 1); if (!rightarg) { free(leftarg); break; }
        memcpy(rightarg, a2, l2); rightarg[l2] = 0;
        trim_c_expr(leftarg); trim_c_expr(rightarg);
        int lnumh = (strstr(leftarg,"_ltng_from_double(")||strstr(leftarg,"_ltng_from_int(")||strstr(leftarg,"_ltng_ad("));
        int rnumh = (strstr(rightarg,"_ltng_from_double(")||strstr(rightarg,"_ltng_from_int(")||strstr(rightarg,"_ltng_ad("));
        char *numside = NULL; int have = 0; char *helperside = NULL; int helper_left = 0;
        if (lnumh && rightarg[0]=='"') {
            char *qe = strchr(rightarg+1,'"'); size_t nl = qe ? (size_t)(qe - (rightarg+1)) : strlen(rightarg+1);
            numside = (char*)malloc(nl+1); if(numside){ memcpy(numside,rightarg+1,nl); numside[nl]=0; helperside=leftarg; helper_left=1; have=1; }
        } else if (rnumh && leftarg[0]=='"') {
            char *qe = strchr(leftarg+1,'"'); size_t nl = qe ? (size_t)(qe - (leftarg+1)) : strlen(leftarg+1);
            numside = (char*)malloc(nl+1); if(numside){ memcpy(numside,leftarg+1,nl); numside[nl]=0; helperside=rightarg; helper_left=0; have=1; }
        }
        if (have && numside) {
            int ok=1; char *nn=numside; if(*nn=='-'||*nn=='+') nn++; if(!*nn) ok=0; for(char*t=nn;*t;t++){ if(!isdigit((unsigned char)*t)&&*t!='.') {ok=0;break;} }
            if (ok) {
                char *h = ltng_strdup(helperside); if (!h) h = ltng_strdup("");
                trim_c_expr(h);
                char *repl = helper_left
                    ? _ltng_cfmt("((double)atof(%s) %s (%s))", h, is_eq?"==":"!=", numside)
                    : _ltng_cfmt("((%s) %s (double)atof(%s))", numside, is_eq?"==":"!=", h);
                if (repl) {
                    int prefix = (int)(p - *bufp);
                    char *newbuf = _ltng_cfmt("%.*s%s%s", prefix, *bufp, repl, endcmp);
                    if (newbuf) { BUF_SET(bufp, cap, "%s", newbuf); free(newbuf); }
                    free(repl);
                }
                free(h);
                free(leftarg); free(rightarg); free(numside);
                continue;
            }
        }
        free(leftarg); free(rightarg); free(numside);
        break;
    }
}


VarType collect_expression(Lexer *lexer, SymbolTable *table,
                                  char **outp, int *out_cap,
                                  int stop_on_rparen, int stop_on_comma,
                                  int *out_sl, int *out_sc, int *out_el, int *out_ec) {
    return collect_expression_ex(lexer, table, outp, out_cap,
                                 stop_on_rparen, stop_on_comma,
                                 out_sl, out_sc, out_el, out_ec, 0);
}

VarType collect_expression_ex(Lexer *lexer, SymbolTable *table,
                                     char **outp, int *out_cap,
                                     int stop_on_rparen, int stop_on_comma,
                                     int *out_sl, int *out_sc, int *out_el, int *out_ec,
                                     int stop_on_ternary_colon) {
    char *out = *outp;
    int out_size = *out_cap;
    int pos = 0;
#define CE_SYNC() do { *outp = out; *out_cap = out_size; } while(0)
#define CE_GROW(need) do {     int _need = (need);     if (pos + _need + 1 > out_size) {         int _nc = out_size * 2;         if (_nc < pos + _need + 1) _nc = pos + _need + 1;         char *_nb = (char*)realloc(out, _nc);         if (!_nb) { CE_SYNC(); return result_type; }         out = _nb; out_size = _nc; CE_SYNC();     } } while(0)
#define CE_SNPRINTF_ADD(...) do { \
    for(;;) { CE_GROW(256); int _sn = snprintf(out + pos, out_size - pos, __VA_ARGS__); \
        if (_sn < 0) { out[pos]=0; break; } \
        if (_sn >= out_size - pos) { CE_GROW(_sn + 1); continue; } \
        pos += _sn; break; } \
} while(0)
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
                    int clen = pos - cstart;
                    if (clen < 0) clen = 0;
                    int cond_buf_cap = clen + 1; if (cond_buf_cap < 65536) cond_buf_cap = 65536;
                    char *cond_buf = (char*)malloc(cond_buf_cap);
                    if (!cond_buf) { error_state_add(parser_errors, tok.line, tok.col, "Out of memory"); return TYPE_NULL; }
                    memcpy(cond_buf, out + cstart, clen);
                    cond_buf[clen] = '\0';
                    VarType cond_type = result_type;

                    /* Post-process condition (subset of collect_expression end processing) */
                    sanitize_mod_atof(cond_buf, cond_buf_cap);
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
                            normalize_mixed_expr(&cond_buf, &cond_buf_cap, array_access_used, table);
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
                                char *vname = ltng_strdup(cs ? cs : "");
                                if (strncmp(cs, "atof(", 5) == 0) {
                                    const char *p2c = cs + 5; int n = 0;
                                    while (isalnum((unsigned char)*p2c) || *p2c == '_') { vname[n++] = *p2c++; }
                                    vname[n] = '\0';
                                } else if (strncmp(cs, "atoll(", 6) == 0) {
                                    const char *p2c = cs + 6; int n = 0;
                                    while (isalnum((unsigned char)*p2c) || *p2c == '_') { vname[n++] = *p2c++; }
                                    vname[n] = '\0';
                                }
                                if (vname[0]) {
                                    BUF_SET(&cond_buf, &cond_buf_cap, "_ltng_str_truthy(%s)", vname);
                                }
                                free(vname);
                            }
                        }
                        if (need_truthy && !(has_dynamic_numeric && cond_type != TYPE_STRING && cond_type != TYPE_CHAR && cond_type != TYPE_OBJECT && cond_type != TYPE_NULL)) {
                            /* already handled above for dynamic numeric case, or handle string/char/object/null */
                        }
                        if (!need_truthy) { /* nothing */ }
                        else if (cond_type == TYPE_STRING || cond_type == TYPE_CHAR || cond_type == TYPE_OBJECT || cond_type == TYPE_NULL) {
                            char *wrapped = _ltng_cfmt("_ltng_str_truthy(%s)", cond_buf);
                            if (wrapped) { SNPRINTF_FULL(cond_buf, "%s", wrapped); free(wrapped); }
                        }
                    }

                    /* Collect true branch (stops at matching ':') */
                    int true_buf_cap = 65536;
                    char *true_buf = (char*)malloc(true_buf_cap);
                    if (!true_buf) { free(cond_buf); error_state_add(parser_errors, lexer_peek(lexer).line, lexer_peek(lexer).col, "Out of memory"); return TYPE_NULL; }
                    true_buf[0] = '\0';
                    VarType true_type = collect_expression_ex(lexer, table, &true_buf, &true_buf_cap,
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
                    int false_buf_cap = 65536;
                    char *false_buf = (char*)malloc(false_buf_cap);
                    if (!false_buf) { free(cond_buf); free(true_buf); error_state_add(parser_errors, tok.line, tok.col, "Out of memory"); return TYPE_NULL; }
                    false_buf[0] = '\0';
                    VarType false_type = collect_expression_ex(lexer, table, &false_buf, &false_buf_cap,
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
                                const char *p2i = inner; size_t n = 0; size_t vncap = 64;
                                char *vname = (char*)malloc(vncap);
                                if (!vname) { /* skip */ }
                                while (vname && (isalnum((unsigned char)*p2i) || *p2i == '_')) { if (n+1>=vncap){vncap*=2; char *nv=(char*)realloc(vname,vncap); if(!nv){free(vname);vname=NULL;break;} vname=nv;} vname[n++] = *p2i++; }
                                if (vname) vname[n] = '\0';
                                int close_ok = 0;
                                if (off == 14) { /* (double)atoll(name) */ if (*p2i==')' && *(p2i+1)==')' && *(p2i+2)=='\0') close_ok = 1; }
                                else { if (*p2i==')' && *(p2i+1)=='\0') close_ok = 1; }
                                if (vname && n > 0 && close_ok) {
                                    char *mangled_v = mangle_name_alloc(vname);
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
                                        if (bi == 0) { SNPRINTF_FULL(true_buf, "%s", cname_v); bufs[0]=true_buf; }
                                        else { SNPRINTF_FULL(false_buf, "%s", cname_v); bufs[1]=false_buf; }
                                        *types[bi] = TYPE_STRING; /* treat as dynamic/string */
                                    }
                                    free(mangled_v);
                                }
                                free(vname);
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
                    pos = cstart;
                    CE_SNPRINTF_ADD( "(%s?(%s):(%s))", cond_buf, true_buf, false_buf);
                    if (pos >= out_size) { CE_GROW(1); if (pos >= out_size) pos = out_size - 1; }
                    out[pos] = '\0';
                    free(cond_buf); free(true_buf); free(false_buf);
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
                        if (out_sl) *out_sl = sl;
                        if (out_sc) *out_sc = sc;
                        if (out_el) *out_el = el;
                        if (out_ec) *out_ec = ec;
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
                char *mangled = mangle_name_alloc(name.value);
                if (!symbol_table_has(table, mangled)) {
                    error_state_add(parser_errors, name.line, name.col,
                        "Variable not declared");
                    free(mangled);
                    break;
                }
                VarType vt = symbol_table_get(table, mangled);
                if (vt == TYPE_FLOAT) {
                    if (is_inc)
                        CE_SNPRINTF_ADD(
                            "({ double _n = (double)(atof(%s) + 1.0); snprintf(%s, sizeof(%s), \"%%g\", _n); _n; })",
                            mangled, mangled, mangled);
                    else
                        CE_SNPRINTF_ADD(
                            "({ double _n = (double)(atof(%s) - 1.0); snprintf(%s, sizeof(%s), \"%%g\", _n); _n; })",
                            mangled, mangled, mangled);
                    if (!type_locked) result_type = TYPE_FLOAT;
                } else if (vt == TYPE_BOOL) {
                    if (is_inc)
                        CE_SNPRINTF_ADD(
                            "({ long long _n = (long long)((%s[0]=='t'||%s[0]=='1'?1:0) + 1); snprintf(%s, sizeof(%s), \"%%lld\", _n); _n; })",
                            mangled, mangled, mangled, mangled);
                    else
                        CE_SNPRINTF_ADD(
                            "({ long long _n = (long long)((%s[0]=='t'||%s[0]=='1'?1:0) - 1); snprintf(%s, sizeof(%s), \"%%lld\", _n); _n; })",
                            mangled, mangled, mangled, mangled);
                    if (!type_locked) result_type = TYPE_INT;
                } else {
                    if (is_inc)
                        CE_SNPRINTF_ADD(
                            "({ long long _n = (long long)(atoll(%s) + 1); snprintf(%s, sizeof(%s), \"%%lld\", _n); _n; })",
                            mangled, mangled, mangled);
                    else
                        CE_SNPRINTF_ADD(
                            "({ long long _n = (long long)(atoll(%s) - 1); snprintf(%s, sizeof(%s), \"%%lld\", _n); _n; })",
                            mangled, mangled, mangled);
                    if (!type_locked) result_type = TYPE_INT;
                }
                free(mangled);
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
                        int _llen = _mop; char *_left = (char*)malloc(_llen + 1); if(_left){
                        memcpy(_left, out, _llen); _left[_llen] = 0;
                        pos = 0;
                        CE_SNPRINTF_ADD( "_LTNG_FMOD_D(%s, %s, %d, %d)", _left, tok.value, pending_divmod_line, pending_divmod_col); free(_left); }
                        if (!type_locked) result_type = TYPE_FLOAT;
                    } else {
                        CE_SNPRINTF_ADD( "(%s(%s,%d,%d))",
                            (result_type == TYPE_FLOAT ? (pending_divmod_is_mod?"_LTNG_NZMOD_D":"_LTNG_NZDIV_D") : (pending_divmod_is_mod?"_LTNG_NZMOD_LL":"_LTNG_NZDIV_LL")),
                            tok.value, pending_divmod_line, pending_divmod_col);
                    }
                } else {
                    CE_SNPRINTF_ADD( "%s", tok.value);
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
                        int _llen = pending_mod_op_pos; char *_left = (char*)malloc(_llen + 1); if(_left){
                        memcpy(_left, out, _llen); _left[_llen] = 0;
                        pos = 0;
                        CE_SNPRINTF_ADD( "_LTNG_FMOD_D(%s, %s, %d, %d)", _left, tok.value, pending_divmod_line, pending_divmod_col); free(_left); }
                        if (!type_locked) result_type = TYPE_FLOAT;
                    } else {
                        CE_SNPRINTF_ADD( "(%s(%s,%d,%d))",
                            pending_divmod_is_mod ? "_LTNG_NZMOD_D" : "_LTNG_NZDIV_D",
                            tok.value, pending_divmod_line, pending_divmod_col);
                    }
                } else {
                    CE_SNPRINTF_ADD( "%s", tok.value);
                }
                if (!type_locked) result_type = TYPE_FLOAT;
                pending_divmod = 0; pending_mod_op_pos = -1;
                break;
            case TOKEN_STRING:
                if (pending_divmod) {
                    error_state_add(parser_errors, tok.line, tok.col,
                        "Division or modulo requires numeric operands");
                    pending_divmod = 0; pending_mod_op_pos = -1;
                }
                {
                    CE_GROW(1); if (1) out[pos++] = '"';
                    for (const char *sp = tok.value; *sp && pos < out_size - 2; sp++) {
                        unsigned char c = (unsigned char)*sp;
                        if (c == '\\') { out[pos++]='\\'; out[pos++]='\\'; }
                        else if (c == '"') { out[pos++]='\\'; out[pos++]='"'; }
                        else if (c == '\n') { out[pos++]='\\'; out[pos++]='n'; }
                        else if (c == '\t') { out[pos++]='\\'; out[pos++]='t'; }
                        else if (c == '\r') { out[pos++]='\\'; out[pos++]='r'; }
                        else { out[pos++] = c; }
                    }
                    CE_GROW(1); if (1) out[pos++] = '"';
                    out[pos] = '\0';
                    if (!type_locked) result_type = TYPE_STRING;
                }
                break;
            case TOKEN_CHAR:
                if (pending_divmod) {
                    error_state_add(parser_errors, tok.line, tok.col,
                        "Division or modulo requires numeric operands");
                    pending_divmod = 0; pending_mod_op_pos = -1;
                }
                if (tok.value[0] != '\0') {
                    CE_SNPRINTF_ADD( "\"%s\"", tok.value);
                } else {
                    CE_SNPRINTF_ADD( "\"\"" );
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
                    int inner_cap = 65536;
                    char *inner = (char*)malloc(inner_cap);
                    if (!inner) { error_state_add(parser_errors, tok.line, tok.col, "Out of memory"); break; }
                    int ipos = 0;
                    int depth = 1;
                    while (lexer->current < lexer->count && depth > 0) {
                        Token itok = lexer_peek(lexer);
                        if (itok.type == TOKEN_LPAREN) depth++;
                        else if (itok.type == TOKEN_RPAREN) { depth--; if (depth == 0) break; }
                        else if (itok.type == TOKEN_NEWLINE || itok.type == TOKEN_EOF) break;
                        itok = lexer_next(lexer);
                        token_append_source(&inner, &ipos, &inner_cap, itok);
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
                    int inner_c_cap = 65536;
                    char *inner_c = (char*)malloc(inner_c_cap); if (!inner_c) { error_state_add(parser_errors, tok.line, tok.col, "Out of memory"); break; } inner_c[0] = '\0';
                    const char *inner_expr = inner;
                    VarType inner_ctype = TYPE_STRING;
                    int inner_compiled = 0;
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
                            char *mname = mangle_name_alloc(inner);
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
                            free(mname);
                        } else {
                            /* Complex expression: compile */
                            inner_ctype = parse_expression_from_string(inner, table, &inner_c, &inner_c_cap);
                            if (inner_c[0] != '\0') { inner_expr = inner_c; inner_compiled = 1; }
                            else { inner_ctype = TYPE_STRING; }
                        }
                    }

                    /* helper: inner is a runtime C numeric value only when compiled to numeric expr */
                    int inner_is_cnum = inner_compiled && (inner_ctype == TYPE_INT || inner_ctype == TYPE_FLOAT || inner_ctype == TYPE_BOOL);
                    if (_ck_int) {
                        if (is_simple_str_lit) {
                            int sval_cap = (lit_end-lit_start+1); if (sval_cap < 64) sval_cap = 64;
                            char *sval = (char*)malloc(sval_cap);
                            int sp=0;
                            if (sval) { for (int i=lit_start;i<lit_end && sp<sval_cap-1;i++){ if(inner[i]=='\\' && i+1<lit_end) sval[sp++]=inner[++i]; else sval[sp++]=inner[i]; } sval[sp]='\0'; }
                            sval[sp]='\0';
                            CE_SNPRINTF_ADD( "%lld", (long long)atoll(sval));
                        } else if (is_num_lit) {
                            CE_SNPRINTF_ADD( "%lld", (long long)atoll(inner));
                        } else if (is_bool_value(inner)) {
                            CE_SNPRINTF_ADD( "%d", (strcmp(inner,"true")==0||strcmp(inner,"t")==0)?1:0);
                        } else if (inner_is_cnum) {
                            CE_SNPRINTF_ADD( "(long long)(%s)", inner_expr);
                        } else {
                            CE_SNPRINTF_ADD( "(long long)atoll(%s)", inner_expr);
                        }
                        if (!type_locked) {
                            if (result_type == TYPE_FLOAT) { /* keep float promotion */ }
                            else result_type = TYPE_INT;
                        }
                    } else if (_ck_float) {
                        if (is_simple_str_lit) {
                            int sval_cap = (lit_end-lit_start+1); if (sval_cap < 64) sval_cap = 64;
                            char *sval = (char*)malloc(sval_cap);
                            int sp=0;
                            if (sval) { for (int i=lit_start;i<lit_end && sp<sval_cap-1;i++){ if(inner[i]=='\\' && i+1<lit_end) sval[sp++]=inner[++i]; else sval[sp++]=inner[i]; } sval[sp]='\0'; }
                            sval[sp]='\0';
                            CE_SNPRINTF_ADD( "%.17g", atof(sval));
                        } else if (is_num_lit) {
                            CE_SNPRINTF_ADD( "%.17g", atof(inner));
                        } else if (is_bool_value(inner)) {
                            CE_SNPRINTF_ADD( "%s", (strcmp(inner,"true")==0||strcmp(inner,"t")==0)?"1.0":"0.0");
                        } else if (inner_is_cnum) {
                            CE_SNPRINTF_ADD( "(double)(%s)", inner_expr);
                        } else {
                            CE_SNPRINTF_ADD( "(double)atof(%s)", inner_expr);
                        }
                        if (!type_locked) result_type = TYPE_FLOAT;
                    } else if (_ck_str) {
                        if (ipos == 4 && memcmp(inner, "true", 4) == 0) {
                            CE_SNPRINTF_ADD( "\"true\"");
                        } else if (ipos == 5 && memcmp(inner, "false", 5) == 0) {
                            CE_SNPRINTF_ADD( "\"false\"");
                        } else if (ipos == 1 && inner[0] == 't') {
                            CE_SNPRINTF_ADD( "\"true\"");
                        } else if (ipos == 1 && inner[0] == 'f') {
                            CE_SNPRINTF_ADD( "\"false\"");
                        } else if (is_num_lit) {
                            CE_SNPRINTF_ADD( "\"%s\"", inner);
                        } else if (inner_is_cnum && inner_ctype == TYPE_BOOL) {
                            CE_SNPRINTF_ADD( "((%s)?\"true\":\"false\")", inner_expr);
                        } else if (inner_is_cnum && inner_ctype == TYPE_INT) {
                            CE_SNPRINTF_ADD( "_ltng_from_int(_ltng_retbuf(), (long long)(%s))", inner_expr);
                        } else if (inner_is_cnum && inner_ctype == TYPE_FLOAT) {
                            CE_SNPRINTF_ADD( "_ltng_from_double(_ltng_retbuf(), (double)(%s))", inner_expr);
                        } else {
                            CE_SNPRINTF_ADD( "%s", inner_expr);
                        }
                        if (!type_locked) result_type = TYPE_STRING;
                    } else if (_ck_bool) {
                        if (ipos == 1 && inner[0] == '0') {
                            CE_SNPRINTF_ADD( "0");
                        } else if (is_num_lit) {
                            CE_SNPRINTF_ADD( "%d", (atof(inner)!=0.0)?1:0);
                        } else if (is_simple_str_lit) {
                            int bv = 1;
                            int slen = lit_end - lit_start;
                            if (slen == 0) bv = 0;
                            else if (slen == 1 && inner[lit_start] == '0') bv = 0;
                            else if (slen == 1 && inner[lit_start] == 'f') bv = 0;
                            else if (slen == 5 && memcmp(inner + lit_start, "false", 5) == 0) bv = 0;
                            CE_SNPRINTF_ADD( "%d", bv);
                        } else if (inner_is_cnum && inner_ctype == TYPE_BOOL) {
                            CE_SNPRINTF_ADD( "((%s)?1:0)", inner_expr);
                        } else if (inner_is_cnum) {
                            CE_SNPRINTF_ADD( "((%s)!=0?1:0)", inner_expr);
                        } else {
                            CE_SNPRINTF_ADD(
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
                                CE_SNPRINTF_ADD( "\"%s\"", esc); free(esc); }
                            } else {
                                CE_SNPRINTF_ADD( "\"%c\"", ch);
                            }
                        } else if (is_num_lit) {
                            int iv = (int)atoll(inner);
                            if (iv >= 0 && iv <= 127 && iv != '\\' && iv != '"' && iv != '\n' && iv != '\t' && iv != '\r' && iv != '\0') {
                                CE_SNPRINTF_ADD( "\"%c\"", iv);
                            } else {
                                CE_SNPRINTF_ADD( "({ char *_tc=_ltng_ds_new(\"\"); int _cv=(int)(%d); if(_cv=='\\n') _ltng_ds_set(&_tc,\"\\n\"); else if(_cv=='\\t') _ltng_ds_set(&_tc,\"\\t\"); else if(_cv=='\\r') _ltng_ds_set(&_tc,\"\\r\"); else if(_cv=='\\\\') _ltng_ds_set(&_tc,\"\\\\\"); else if(_cv=='\\\"') _ltng_ds_set(&_tc,\"\\\"\"); else _ltng_ds_setfmt(&_tc,\"%%c\",(char)_cv); char *_r=_ltng_as(_tc); free(_tc); _r; })", iv);
                            }
                        } else if (inner_is_cnum) {
                            CE_SNPRINTF_ADD(
                                "({ char *_tc=_ltng_ds_fmt(\"%%c\",(char)(int)(%s)); char *_r=_ltng_as(_tc); free(_tc); _r; })",
                                inner_expr);
                        } else {
                            CE_SNPRINTF_ADD(
                                "_ltng_as((_ltng_is_num_str(%s)?((char[]){(char)atoll(%s),0}):((char[]){(%s)[0]? (%s)[0]:0,0})))",
                                inner_expr, inner_expr, inner_expr, inner_expr);
                        }
                        if (!type_locked) result_type = TYPE_CHAR;
                    }
                    continue;
                }
                if (is_bool_value(tok.value)) {
                    CE_SNPRINTF_ADD( "%s",
                        (strcmp(tok.value, "true") == 0 || strcmp(tok.value, "t") == 0) ? "1" : "0");
                    if (!type_locked && !arithmetic_used && !comparison_or_logical) result_type = TYPE_BOOL;
                    else if (!type_locked && (arithmetic_used || comparison_or_logical) && result_type != TYPE_FLOAT) result_type = TYPE_INT;
                } else if (is_type_keyword(tok.value)) {
                    CE_SNPRINTF_ADD( "%s", tok.value);
                } else if (is_callable_ident(table, tok.value) && lexer_peek(lexer).type == TOKEN_LPAREN) {
                    lexer_next(lexer); /* consume ( */
                    ASTNode *call = parse_call_node(lexer, table, tok.value, tok.line, tok.col, NULL);
                    if (call->is_base_assign) {
                        error_state_add(parser_errors, tok.line, tok.col,
                            "function with 'base' parameter must be called with method syntax (receiver.method(args))");
                        skip_to_newline(lexer);
                        CE_SNPRINTF_ADD( "0");
                        break;
                    }
                    char *cexpr = (char*)malloc(65536);
                    if (!cexpr) { error_state_add(parser_errors, tok.line, tok.col, "Out of memory"); break; }
                    cexpr[0] = 0;
                    build_call_cexpr(call, &cexpr, &(int){65536});
                    int chained_call = append_immediate_call_chains(lexer, table, &cexpr, &(int){65536});
                    {
                        VarType crt = chained_call ? TYPE_STRING : infer_call_result_type(call);
                        if (chained_call) {
                            CE_SNPRINTF_ADD( "%s", cexpr);
                            if (!type_locked) result_type = TYPE_STRING;
                        } else if (!type_locked && crt == TYPE_STRING && function_returns_string(call->name)) {
                            CE_SNPRINTF_ADD( "%s", cexpr);
                            result_type = TYPE_STRING;
                        } else if (crt == TYPE_INT) {
                            CE_SNPRINTF_ADD( "(double)atoll(%s)", cexpr);
                            if (!type_locked) result_type = TYPE_INT;
                        } else if (crt == TYPE_FLOAT || crt == TYPE_NULL || crt == TYPE_VOID) {
                            /* for passthrough-specialized non-string types we got concrete type; NULL/VOID means unknown */
                            if (crt == TYPE_NULL || crt == TYPE_VOID) {
                                CE_SNPRINTF_ADD( "atof(%s)", cexpr);
                                if (!type_locked) result_type = TYPE_FLOAT;
                            } else {
                                CE_SNPRINTF_ADD( "atof(%s)", cexpr);
                                if (!type_locked) result_type = TYPE_FLOAT;
                            }
                        } else if (crt == TYPE_STRING) {
                            CE_SNPRINTF_ADD( "%s", cexpr);
                            if (!type_locked) result_type = TYPE_STRING;
                        } else if (crt == TYPE_BOOL) {
                            CE_SNPRINTF_ADD( "(%s[0]=='t'||%s[0]=='T'||%s[0]=='1'?1:0)", cexpr, cexpr, cexpr);
                            if (!type_locked) result_type = TYPE_BOOL;
                        } else if (crt == TYPE_CHAR) {
                            CE_SNPRINTF_ADD( "%s", cexpr);
                            if (!type_locked) result_type = TYPE_STRING;
                        } else if (crt == TYPE_ARRAY) {
                            CE_SNPRINTF_ADD(
                                "({ char *__ahv=_ltng_ds_new(%s); _ltng_ah *__ah=_ltng_ah_unpack(__ahv); char *__r=_ltng_ds_new(\"\"); if(__ah){ for(int _i=0;_i<__ah->len;_i++){ if(_i) _ltng_ds_append(&__r,\", \"); _ltng_ds_append(&__r,__ah->d[_i]?__ah->d[_i]:\"\"); } _ltng_ah_free(__ah); } free(__ahv); __r; })",
                                cexpr);
                            if (!type_locked) result_type = TYPE_STRING;
                        } else {
                            CE_SNPRINTF_ADD( "atof(%s)", cexpr);
                            if (!type_locked) result_type = TYPE_FLOAT;
                        }
                    }
                } else if (lexer_peek(lexer).type == TOKEN_LPAREN && strcmp(tok.value, "typeof") != 0) {
                    char *mangled = mangle_name_alloc(tok.value);
                    if (symbol_table_has(table, mangled) || symbol_table_has(table, tok.value)) {
                        /* dynamic indirect call through variable */
                        lexer_next(lexer); /* consume ( */
                        ASTNode *call = parse_call_node(lexer, table, tok.value, tok.line, tok.col, NULL);
                        if (call->is_base_assign) {
                            error_state_add(parser_errors, tok.line, tok.col,
                                "function with 'base' parameter must be called with method syntax (receiver.method(args))");
                            skip_to_newline(lexer);
                            CE_SNPRINTF_ADD( "0");
                            free(mangled);
                            break;
                        }
                        char *cexpr = (char*)malloc(65536);
                        if (!cexpr) { error_state_add(parser_errors, tok.line, tok.col, "Out of memory"); free(mangled); break; }
                        cexpr[0] = 0;
                        build_call_cexpr(call, &cexpr, &(int){65536});
                        int chained_call = append_immediate_call_chains(lexer, table, &cexpr, &(int){65536});
                        {
                            if (chained_call) {
                                CE_SNPRINTF_ADD( "%s", cexpr);
                                if (!type_locked) result_type = TYPE_STRING;
                            } else {
                            char *fr_cname2 = (char*)calloc(1, 65536); int fr_pc2 = -1;
                            VarType irt = TYPE_NULL;
                            if (fr_cname2 && symbol_table_get_func_ref(table, tok.value, &fr_cname2, &fr_pc2) && fr_cname2[0]) {
                                irt = infer_function_return_type(fr_cname2);
                            }
                            if (irt == TYPE_INT) {
                                CE_SNPRINTF_ADD( "(double)atoll(%s)", cexpr);
                                if (!type_locked) result_type = TYPE_INT;
                            } else if (irt == TYPE_FLOAT || irt == TYPE_VOID || irt == TYPE_NULL) {
                                /* unknown/void/float: in numeric context treat as numeric */
                                if (irt == TYPE_NULL && type_locked && result_type == TYPE_STRING) {
                                    CE_SNPRINTF_ADD( "%s", cexpr);
                                } else if (irt == TYPE_NULL && !type_locked && result_type == TYPE_STRING) {
                                    CE_SNPRINTF_ADD( "%s", cexpr);
                                } else {
                                    CE_SNPRINTF_ADD( "atof(%s)", cexpr);
                                    if (!type_locked) result_type = TYPE_FLOAT;
                                }
                            } else if (irt == TYPE_STRING) {
                                CE_SNPRINTF_ADD( "%s", cexpr);
                                if (!type_locked) result_type = TYPE_STRING;
                            } else if (irt == TYPE_ARRAY) {
                                CE_SNPRINTF_ADD(
                                    "({ char *__ahv=_ltng_ds_new(%s); _ltng_ah *__ah=_ltng_ah_unpack(__ahv); char *__r=_ltng_ds_new(\"\"); if(__ah){ for(int _i=0;_i<__ah->len;_i++){ if(_i) _ltng_ds_append(&__r,\", \"); _ltng_ds_append(&__r,__ah->d[_i]?__ah->d[_i]:\"\"); } _ltng_ah_free(__ah); } free(__ahv); __r; })",
                                    cexpr);
                                if (!type_locked) result_type = TYPE_STRING;
                            } else {
                                CE_SNPRINTF_ADD( "atof(%s)", cexpr);
                                if (!type_locked) result_type = TYPE_FLOAT;
                            }
                            free(fr_cname2);
                            }
                        }
                        free(mangled);
                    } else {
                        free(mangled);
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
                            char *_am_mangled = mangle_name_alloc(tok.value);
                            int _am_is_arr = symbol_table_has(table, _am_mangled) &&
                                symbol_table_get(table, _am_mangled) == TYPE_ARRAY;
                            free(_am_mangled);
                            if (_am_is_arr) {
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
                        char *tmangled = mangle_name_alloc(tok.value);
                        if (symbol_table_has(table, tmangled) &&
                            symbol_table_get(table, tmangled) == TYPE_ARRAY &&
                            strcmp(mname.value, "length") == 0 &&
                            lexer_peek(lexer).type != TOKEN_LPAREN) {
                            const char *tcname = symbol_table_get_cname(table, tmangled);
                            char *tlenname = _ltng_cfmt("%s_len", tcname ? tcname : "");
                            CE_SNPRINTF_ADD(
                                "(int)(%s)", tlenname ? tlenname : "0");
                            free(tlenname);
                            free(tmangled);
                            result_type = TYPE_INT;
                            break;
                        }
                        free(tmangled);
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
                        char *cexpr = (char*)malloc(65536);
                        if (!cexpr) { error_state_add(parser_errors, tok.line, tok.col, "Out of memory"); break; }
                        cexpr[0] = 0;
                        build_call_cexpr(call, &cexpr, &(int){65536});
                        int chained_call = append_immediate_call_chains(lexer, table, &cexpr, &(int){65536});
                        {
                            VarType crt = chained_call ? TYPE_STRING : infer_call_result_type(call);
                            if (chained_call) {
                                CE_SNPRINTF_ADD( "%s", cexpr);
                                if (!type_locked) result_type = TYPE_STRING;
                            } else if (!type_locked && crt == TYPE_STRING && function_returns_string(mname.value)) {
                                CE_SNPRINTF_ADD( "%s", cexpr);
                                result_type = TYPE_STRING;
                            } else if (crt == TYPE_INT) {
                                CE_SNPRINTF_ADD( "(double)atoll(%s)", cexpr);
                                if (!type_locked) result_type = TYPE_INT;
                            } else if (crt == TYPE_FLOAT || crt == TYPE_NULL || crt == TYPE_VOID) {
                                CE_SNPRINTF_ADD( "atof(%s)", cexpr);
                                if (!type_locked) result_type = TYPE_FLOAT;
                            } else if (crt == TYPE_STRING) {
                                CE_SNPRINTF_ADD( "%s", cexpr);
                                if (!type_locked) result_type = TYPE_STRING;
                            } else if (crt == TYPE_BOOL) {
                                CE_SNPRINTF_ADD( "(%s[0]=='t'||%s[0]=='T'||%s[0]=='1'?1:0)", cexpr, cexpr, cexpr);
                                if (!type_locked) result_type = TYPE_BOOL;
                            } else if (crt == TYPE_CHAR) {
                                CE_SNPRINTF_ADD( "%s", cexpr);
                                if (!type_locked) result_type = TYPE_STRING;
                            } else if (crt == TYPE_ARRAY) {
                                CE_SNPRINTF_ADD(
                                    "({ char *__ahv=_ltng_ds_new(%s); _ltng_ah *__ah=_ltng_ah_unpack(__ahv); char *__r=_ltng_ds_new(\"\"); if(__ah){ for(int _i=0;_i<__ah->len;_i++){ if(_i) _ltng_ds_append(&__r,\", \"); _ltng_ds_append(&__r,__ah->d[_i]?__ah->d[_i]:\"\"); } _ltng_ah_free(__ah); } free(__ahv); __r; })",
                                    cexpr);
                                if (!type_locked) result_type = TYPE_STRING;
                            } else {
                                CE_SNPRINTF_ADD( "atof(%s)", cexpr);
                                if (!type_locked) result_type = TYPE_FLOAT;
                            }
                        }
                    } else if (mname.type == TOKEN_IDENT || token_is_name(mname)) {
                        /* property access: obj.field */
                        char *mangled = mangle_name_alloc(tok.value);
                        const char *field_base = symbol_table_get_cname(table, mangled);
                        if (!field_base || !field_base[0]) field_base = mangled;
                        char *fv = obj_resolve_field_alloc(field_base, mname.value);
                        VarType ftype = TYPE_STRING;
                        ObjSchema *osc = obj_lookup(field_base);
                        if (!osc) osc = obj_lookup(mangled);
                        if (osc) {
                            for (int oi = 0; oi < osc->count; oi++) {
                                if (strcmp(osc->fields[oi].name, mname.value) == 0) { ftype = osc->fields[oi].type; break; }
                            }
                        }
                        if (ftype == TYPE_BOOL) {
                            CE_SNPRINTF_ADD( "(%s[0]=='t'?1:0)", fv);
                            if (!type_locked) result_type = TYPE_BOOL;
                        } else if (ftype == TYPE_STRING) {
                            CE_SNPRINTF_ADD( "%s", fv);
                            if (!type_locked) result_type = TYPE_STRING;
                        } else if (ftype == TYPE_INT) {
                            CE_SNPRINTF_ADD( "atoll(%s)", fv);
                            if (!type_locked) result_type = TYPE_INT;
                        } else if (ftype == TYPE_FLOAT) {
                            CE_SNPRINTF_ADD( "atof(%s)", fv);
                            if (!type_locked) result_type = TYPE_FLOAT;
                        } else {
                            CE_SNPRINTF_ADD( "%s", fv);
                            if (!type_locked) result_type = TYPE_STRING;
                        }
                        free(fv);
                        free(mangled);
                    } else {
                        lexer->current = saved_m;
                        goto collect_ident_var;
                    }
                } else if (strcmp(tok.value, "typeof") == 0) {
                    /* typeof was already consumed by lexer_next; parse typeof(arg) */
                    lexer->current--; /* unconsume typeof for try_parse_typeof */
                    Token tres = {0};
                    if (try_parse_typeof(lexer, table, &tres)) {
                        CE_SNPRINTF_ADD( "\"%s\"", tres.value);
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
                        CE_SNPRINTF_ADD( "0");
                        continue;
                    }
                    VarType vt = TYPE_STRING;
                    if (arg.type == TOKEN_INT_LIT) vt = TYPE_INT;
                    else if (arg.type == TOKEN_FLOAT_LIT) vt = TYPE_FLOAT;
                    else if (arg.type == TOKEN_STRING) vt = TYPE_STRING;
                    else if (arg.type == TOKEN_CHAR) vt = TYPE_STRING;
                    else if ((arg.type == TOKEN_IDENT || token_is_name(arg)) && is_bool_value(arg.value)) vt = TYPE_BOOL;
                    else if (arg.type == TOKEN_IDENT || token_is_name(arg)) {
                        char *mangled = mangle_name_alloc(arg.value);
                        if (symbol_table_has(table, mangled)) vt = symbol_table_get(table, mangled);
                        free(mangled);
                    }
                    CE_SNPRINTF_ADD( "\"%s\"", type_name_str(vt));
                    if (!type_locked) result_type = TYPE_STRING;
                    continue;
                } else {
                collect_ident_var:
                    ; /* C99: declaration after label */
                    pending_divmod = _pd_save; pending_divmod_is_mod = _pd_mod_save;
                    pending_divmod_line = _pd_l_save; pending_divmod_col = _pd_c_save;
                    pending_mod_op_pos = _pd_mop_save;
                    char *mangled = mangle_name_alloc(tok.value);
                    if (!symbol_table_has(table, mangled)) {
                        ASTNode *fndef = find_function_ast_by_name(tok.value);
                        if (fndef) {
                            CE_SNPRINTF_ADD( "\"%s\"", fndef->name);
                            if (!type_locked) result_type = TYPE_STRING;
                        } else {
                            char *errmsg = _ltng_cfmt("Variable '%s' not declared", tok.value);
                            error_state_add(parser_errors, tok.line, tok.col, errmsg ? errmsg : "");
                            free(errmsg);
                            CE_SNPRINTF_ADD( "0");
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
                            CE_SNPRINTF_ADD( "%s", cname);
                        } else switch (_emit_type) {
                            case TYPE_INT: {
                                char *_opnd = _ltng_cfmt("atoll(%s)", cname);
                                if (pending_divmod) {
                                    int _is_fmod = (result_type == TYPE_FLOAT && pending_divmod_is_mod);
                                    if (_is_fmod && pending_mod_op_pos >= 0 && pending_mod_op_pos < pos && out[pending_mod_op_pos] == '%') {
                                        int _llen = pending_mod_op_pos; char *_left = (char*)malloc(_llen + 1); if(_left){
                                        memcpy(_left, out, _llen); _left[_llen] = 0;
                                        pos = 0;
                                        CE_SNPRINTF_ADD( "_LTNG_FMOD_D(%s, %s, %d, %d)", _left, _opnd, pending_divmod_line, pending_divmod_col); free(_left); }
                                    } else {
                                        CE_SNPRINTF_ADD( "(%s(%s,%d,%d))", (result_type == TYPE_FLOAT ? (pending_divmod_is_mod?"_LTNG_NZMOD_D":"_LTNG_NZDIV_D") : (pending_divmod_is_mod?"_LTNG_NZMOD_LL":"_LTNG_NZDIV_LL")), _opnd, pending_divmod_line, pending_divmod_col);
                                    }
                                CE_GROW(4096);
                                } else CE_SNPRINTF_ADD( "%s", _opnd);
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
                                        int _llen = pending_mod_op_pos; char *_left = (char*)malloc(_llen + 1); if(_left){
                                        memcpy(_left, out, _llen); _left[_llen] = 0;
                                        pos = 0;
                                        CE_SNPRINTF_ADD( "_LTNG_FMOD_D(%s, %s, %d, %d)", _left, _opnd, pending_divmod_line, pending_divmod_col); free(_left); }
                                    } else {
                                        CE_SNPRINTF_ADD( "(%s(%s,%d,%d))", (pending_divmod_is_mod?"_LTNG_NZMOD_D":"_LTNG_NZDIV_D"), _opnd, pending_divmod_line, pending_divmod_col);
                                    }
                                CE_GROW(4096);
                                } else CE_SNPRINTF_ADD( "%s", _opnd);
                                if (!type_locked) { if (result_type == TYPE_INT) result_type = TYPE_FLOAT; }
                                /* Untyped/dynamic parameters (no known default type) need runtime add semantics.
                                   Parameters with known default types are treated as typed by default. */
                                if (!_has_default_type && symbol_table_is_param(table, mangled) && !symbol_table_get_locked(table, mangled)) has_dynamic_numeric = 1;
                                free(_opnd); pending_divmod = 0; pending_mod_op_pos = -1;
                                break; }
                            case TYPE_CHAR:
                            case TYPE_STRING:
                            case TYPE_OBJECT:
                                CE_SNPRINTF_ADD( "%s", cname);
                                if (!type_locked) result_type = TYPE_STRING;
                                pending_divmod = 0; pending_mod_op_pos = -1;
                                break;
                            case TYPE_BOOL: {
                                char *_opnd = _ltng_cfmt("(%s[0]=='t'?1:0)", cname);
                                if (pending_divmod) {
                                    int _is_fmod = (result_type == TYPE_FLOAT && pending_divmod_is_mod);
                                    if (_is_fmod && pending_mod_op_pos >= 0 && pending_mod_op_pos < pos && out[pending_mod_op_pos] == '%') {
                                        int _llen = pending_mod_op_pos; char *_left = (char*)malloc(_llen + 1); if(_left){
                                        memcpy(_left, out, _llen); _left[_llen] = 0;
                                        pos = 0;
                                        CE_SNPRINTF_ADD( "_LTNG_FMOD_D(%s, %s, %d, %d)", _left, _opnd, pending_divmod_line, pending_divmod_col); free(_left); }
                                    } else {
                                        CE_SNPRINTF_ADD( "(%s(%s,%d,%d))", (result_type == TYPE_FLOAT ? (pending_divmod_is_mod?"_LTNG_NZMOD_D":"_LTNG_NZDIV_D") : (pending_divmod_is_mod?"_LTNG_NZMOD_LL":"_LTNG_NZDIV_LL")), _opnd, pending_divmod_line, pending_divmod_col);
                                    }
                                CE_GROW(4096);
                                } else CE_SNPRINTF_ADD( "%s", _opnd);
                                if (!type_locked) result_type = TYPE_BOOL;
                                free(_opnd); pending_divmod = 0; pending_mod_op_pos = -1;
                                break; }
                            case TYPE_NULL:
                            default:
                                CE_SNPRINTF_ADD( "%s", cname);
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

                                        char *arrname = _ltng_cfmt("%s_data", cname);
                                        char *lenname = _ltng_cfmt("%s_len", cname);
                                        char *capname = _ltng_cfmt("%s_cap", cname);

                                        /* .length property (no parens) */
                                        if ((mname.type == TOKEN_IDENT || token_is_name(mname)) &&
                                            strcmp(mname.value, "length") == 0 &&
                                            lexer_peek(lexer).type != TOKEN_LPAREN) {
                                            CE_SNPRINTF_ADD(
                                                "(int)(%s)", lenname);
                                            result_type = TYPE_INT;
                                            free(arrname); free(lenname); free(capname);
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
                                                CE_SNPRINTF_ADD( "0");
                                                break;
                                            }
                                            /* Array method call(s) with chaining support.
                                               Value-consumed context: operate on a temp copy to keep
                                               the original receiver intact, return method result. */
                                            int _chain_prefix_cap = 16384;
                                            char *_chain_prefix = (char*)malloc(_chain_prefix_cap); if(!_chain_prefix){CE_SNPRINTF_ADD("0");free(arrname); free(lenname); free(capname); goto _arr_method_result_done;} _chain_prefix[0]=0;
                                            int _cp = 0;
                                            int _chain_step = 0;
                                            int _final_expr_cap = 4096;
                                            char *_final_expr = (char*)calloc(1,_final_expr_cap);
                                            if(!_final_expr){ CE_SNPRINTF_ADD("0"); free(arrname); free(lenname); free(capname); goto _arr_method_result_done; }
                                            int _chain_err = 0;
                                            Token _cm = mname;
                                            /* Copy original array into temp working array */
                                            EXPR_APPEND(_chain_prefix, _chain_prefix_cap, _cp,
                                                "char **__td=NULL; int __tl=0; int __tc=0; _ltng_arr_copy(%s, &__td, &__tl, &__tc, %s); ",
                                                arrname, lenname);
                                            const char *_wa = "__td"; /* working array data */
                                            const char *_wl = "__tl"; /* working array len */
                                            const char *_wc = "__tc"; /* working array cap */
                                            for (;;) {
                                                _chain_step++;
                                                if (lexer_peek(lexer).type != TOKEN_LPAREN) {
                                                    error_state_add(parser_errors, _cm.line, _cm.col, "Expected '(' after array method name");
                                                    CE_GROW(4096);
                                                    skip_to_newline(lexer); CE_SNPRINTF_ADD( "0"); _chain_err = 1; break;
                                                }
                                                lexer_next(lexer); /* consume ( */

                                                if (strcmp(_cm.value, "push") == 0) {
                                                    char *arg_expr = (char*)malloc(65536);
                                                    if (!arg_expr) { _chain_err = 1; break; }
                                                    arg_expr[0] = 0;
                                                    VarType arg_type = collect_expression(lexer, table, &arg_expr, &(int){65536}, 1, 0, NULL, NULL, NULL, NULL);
                                                    if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                                                        error_state_add(parser_errors, lexer_peek(lexer).line, lexer_peek(lexer).col, "Expected ')' after push argument");
                                                        CE_GROW(4096);
                                                        skip_to_newline(lexer); CE_SNPRINTF_ADD( "0"); free(arg_expr); _chain_err = 1; break;
                                                    }
                                                    lexer_next(lexer);
                                                    {
                                                        const char *_fmt = (arg_type==TYPE_INT)?"%lld":(arg_type==TYPE_FLOAT)?"%g":"%s";
                                                        const char *_cast = (arg_type==TYPE_INT)?"(long long)":(arg_type==TYPE_FLOAT)?"(double)":"";
                                                        EXPR_APPEND(_chain_prefix, _chain_prefix_cap, _cp,
                                                            "char *__apv%d = _ltng_ds_fmt(\"%s\",%s(%s)); _ltng_arr_push(&%s,&%s,&%s,__apv%d); char *__apr%d=_ltng_retbuf_fmt(\"%%s\",__apv%d); free(__apv%d); ",
                                                            _chain_step, _fmt, _cast, arg_expr, _wa, _wl, _wc, _chain_step, _chain_step, _chain_step, _chain_step);
                                                    }
                                                    BUF_SET(&_final_expr, &_final_expr_cap, "__apr%d", _chain_step);
                                                    free(arg_expr);
                                                    result_type = TYPE_STRING;
                                                } else if (strcmp(_cm.value, "pop") == 0) {
                                                    if (lexer_peek(lexer).type == TOKEN_RPAREN) {
                                                        lexer_next(lexer);
                                                        EXPR_APPEND(_chain_prefix, _chain_prefix_cap, _cp,
                                                            "char *__pv%d = _ltng_arr_pop(%s, &%s); ", _chain_step, _wa, _wl);
                                                        BUF_SET(&_final_expr, &_final_expr_cap, "__pv%d", _chain_step);
                                                    } else {
                                                        char *idx_expr = (char*)malloc(65536);
                                                        if (!idx_expr) { _chain_err = 1; break; }
                                                        idx_expr[0] = 0;
                                                        collect_expression(lexer, table, &idx_expr, &(int){65536}, 1, 0, NULL, NULL, NULL, NULL);
                                                        if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                                                            error_state_add(parser_errors, lexer_peek(lexer).line, lexer_peek(lexer).col, "Expected ')' after pop index");
                                                            CE_GROW(4096);
                                                            skip_to_newline(lexer); CE_SNPRINTF_ADD( "0"); free(idx_expr); _chain_err = 1; break;
                                                        }
                                                        lexer_next(lexer);
                                                        EXPR_APPEND(_chain_prefix, _chain_prefix_cap, _cp,
                                                            "char *__pv%d = _ltng_arr_pop_index(%s, &%s, (int)(%s)); ",
                                                            _chain_step, _wa, _wl, idx_expr);
                                                        BUF_SET(&_final_expr, &_final_expr_cap, "__pv%d", _chain_step);
                                                        free(idx_expr);
                                                    }
                                                    result_type = TYPE_STRING;
                                                } else if (strcmp(_cm.value, "insert") == 0) {
                                                    char *idx_expr = (char*)malloc(65536);
                                                    if (!idx_expr) { _chain_err = 1; break; }
                                                    idx_expr[0] = 0;
                                                    collect_expression(lexer, table, &idx_expr, &(int){65536}, 1, 1, NULL, NULL, NULL, NULL);
                                                    if (lexer_peek(lexer).type != TOKEN_COMMA) {
                                                        error_state_add(parser_errors, lexer_peek(lexer).line, lexer_peek(lexer).col, "Expected ',' after insert index");
                                                        CE_GROW(4096);
                                                        skip_to_newline(lexer); CE_SNPRINTF_ADD( "0"); free(idx_expr); _chain_err = 1; break;
                                                    }
                                                    lexer_next(lexer);
                                                    char *arg_expr = (char*)malloc(65536);
                                                    if (!arg_expr) { free(idx_expr); _chain_err = 1; break; }
                                                    arg_expr[0] = 0;
                                                    VarType arg_type = collect_expression(lexer, table, &arg_expr, &(int){65536}, 1, 0, NULL, NULL, NULL, NULL);
                                                    if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                                                        error_state_add(parser_errors, lexer_peek(lexer).line, lexer_peek(lexer).col, "Expected ')' after insert argument");
                                                        CE_GROW(4096);
                                                        skip_to_newline(lexer); CE_SNPRINTF_ADD( "0"); free(arg_expr); free(idx_expr); _chain_err = 1; break;
                                                    }
                                                    lexer_next(lexer);
                                                    {
                                                        const char *_fmt = (arg_type==TYPE_INT)?"%lld":(arg_type==TYPE_FLOAT)?"%g":"%s";
                                                        const char *_cast = (arg_type==TYPE_INT)?"(long long)":(arg_type==TYPE_FLOAT)?"(double)":"";
                                                        EXPR_APPEND(_chain_prefix, _chain_prefix_cap, _cp,
                                                            "char *__aiv%d = _ltng_ds_fmt(\"%s\",%s(%s)); _ltng_arr_insert(&%s,&%s,&%s,(int)(%s),__aiv%d); char *__air%d=_ltng_retbuf_fmt(\"%%s\",__aiv%d); free(__aiv%d); ",
                                                            _chain_step, _fmt, _cast, arg_expr, _wa, _wl, _wc, idx_expr, _chain_step, _chain_step, _chain_step, _chain_step);
                                                    }
                                                    BUF_SET(&_final_expr, &_final_expr_cap, "__air%d", _chain_step);
                                                    free(arg_expr); free(idx_expr);
                                                    result_type = TYPE_STRING;
                                                } else if (strcmp(_cm.value, "remove") == 0) {
                                                    char *idx_expr = (char*)malloc(65536);
                                                    if (!idx_expr) { _chain_err = 1; break; }
                                                    idx_expr[0] = 0;
                                                    collect_expression(lexer, table, &idx_expr, &(int){65536}, 1, 0, NULL, NULL, NULL, NULL);
                                                    if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                                                        error_state_add(parser_errors, lexer_peek(lexer).line, lexer_peek(lexer).col, "Expected ')' after remove index");
                                                        CE_GROW(4096);
                                                        skip_to_newline(lexer); CE_SNPRINTF_ADD( "0"); free(idx_expr); _chain_err = 1; break;
                                                    }
                                                    lexer_next(lexer);
                                                    EXPR_APPEND(_chain_prefix, _chain_prefix_cap, _cp,
                                                        "char *__rv%d = _ltng_arr_pop_index(%s, &%s, (int)(%s)); ",
                                                        _chain_step, _wa, _wl, idx_expr);
                                                    BUF_SET(&_final_expr, &_final_expr_cap, "__rv%d", _chain_step);
                                                    free(idx_expr);
                                                    result_type = TYPE_STRING;
                                                } else if (strcmp(_cm.value, "contains") == 0) {
                                                    char *arg_expr = (char*)malloc(65536);
                                                    if (!arg_expr) { _chain_err = 1; break; }
                                                    arg_expr[0] = 0;
                                                    VarType arg_type = collect_expression(lexer, table, &arg_expr, &(int){65536}, 1, 0, NULL, NULL, NULL, NULL);
                                                    if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                                                        error_state_add(parser_errors, lexer_peek(lexer).line, lexer_peek(lexer).col, "Expected ')' after contains argument");
                                                        CE_GROW(4096);
                                                        skip_to_newline(lexer); CE_SNPRINTF_ADD( "0"); free(arg_expr); _chain_err = 1; break;
                                                    }
                                                    lexer_next(lexer);
                                                    {
                                                        const char *_fmt = (arg_type==TYPE_INT)?"%lld":(arg_type==TYPE_FLOAT)?"%g":"%s";
                                                        const char *_cast = (arg_type==TYPE_INT)?"(long long)":(arg_type==TYPE_FLOAT)?"(double)":"";
                                                        EXPR_APPEND(_chain_prefix, _chain_prefix_cap, _cp,
                                                            "char *__cv%d = _ltng_ds_fmt(\"%s\",%s(%s)); int __cr%d=_ltng_arr_contains(%s,%s,__cv%d); free(__cv%d); ",
                                                            _chain_step, _fmt, _cast, arg_expr, _chain_step, _wa, _wl, _chain_step, _chain_step);
                                                    }
                                                    BUF_SET(&_final_expr, &_final_expr_cap,
                                                        "({ _ltng_retbuf_fmt(\"%%s\",__cr%d?\"true\":\"false\"); })", _chain_step);
                                                    free(arg_expr);
                                                    result_type = TYPE_STRING;
                                                } else if (strcmp(_cm.value, "index_of") == 0) {
                                                    char *arg_expr = (char*)malloc(65536);
                                                    if (!arg_expr) { _chain_err = 1; break; }
                                                    arg_expr[0] = 0;
                                                    VarType arg_type = collect_expression(lexer, table, &arg_expr, &(int){65536}, 1, 0, NULL, NULL, NULL, NULL);
                                                    if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                                                        error_state_add(parser_errors, lexer_peek(lexer).line, lexer_peek(lexer).col, "Expected ')' after index_of argument");
                                                        CE_GROW(4096);
                                                        skip_to_newline(lexer); CE_SNPRINTF_ADD( "0"); free(arg_expr); _chain_err = 1; break;
                                                    }
                                                    lexer_next(lexer);
                                                    {
                                                        const char *_fmt = (arg_type==TYPE_INT)?"%lld":(arg_type==TYPE_FLOAT)?"%g":"%s";
                                                        const char *_cast = (arg_type==TYPE_INT)?"(long long)":(arg_type==TYPE_FLOAT)?"(double)":"";
                                                        EXPR_APPEND(_chain_prefix, _chain_prefix_cap, _cp,
                                                            "char *__iv%d = _ltng_ds_fmt(\"%s\",%s(%s)); int __ir%d=_ltng_arr_index_of(%s,%s,__iv%d); free(__iv%d); ",
                                                            _chain_step, _fmt, _cast, arg_expr, _chain_step, _wa, _wl, _chain_step, _chain_step);
                                                    }
                                                    BUF_SET(&_final_expr, &_final_expr_cap,
                                                        "({ _ltng_retbuf_fmt(\"%%d\",__ir%d); })", _chain_step);
                                                    free(arg_expr);
                                                    result_type = TYPE_STRING;
                                                } else if (strcmp(_cm.value, "sort") == 0) {
                                                    if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                                                        error_state_add(parser_errors, lexer_peek(lexer).line, lexer_peek(lexer).col, "Expected ')' after sort");
                                                        CE_GROW(4096);
                                                        skip_to_newline(lexer); CE_SNPRINTF_ADD( "0"); _chain_err = 1; break;
                                                    }
                                                    lexer_next(lexer);
                                                    EXPR_APPEND(_chain_prefix, _chain_prefix_cap, _cp,
                                                        "_ltng_arr_sort(%s, %s); ", _wa, _wl);
                                                    BUF_SET(&_final_expr, &_final_expr_cap,
                                                        "({ _ltng_retbuf_fmt(\"true\"); })");
                                                    result_type = TYPE_STRING;
                                                } else if (strcmp(_cm.value, "reverse") == 0) {
                                                    if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                                                        error_state_add(parser_errors, lexer_peek(lexer).line, lexer_peek(lexer).col, "Expected ')' after reverse");
                                                        CE_GROW(4096);
                                                        skip_to_newline(lexer); CE_SNPRINTF_ADD( "0"); _chain_err = 1; break;
                                                    }
                                                    lexer_next(lexer);
                                                    EXPR_APPEND(_chain_prefix, _chain_prefix_cap, _cp,
                                                        "_ltng_arr_reverse(%s, %s); ", _wa, _wl);
                                                    BUF_SET(&_final_expr, &_final_expr_cap,
                                                        "({ _ltng_retbuf_fmt(\"true\"); })");
                                                    result_type = TYPE_STRING;
                                                } else {
                                                    error_state_add(parser_errors, _cm.line, _cm.col, "Unknown array method");
                                                    CE_SNPRINTF_ADD( "0");
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
                                                        continue;
                                                    }
                                                    lexer->current = saved_cdot; /* not a chainable array method */
                                                }
                                                break;
                                            }
                                            if (!_chain_err) {
                                                /* Capture final result, free temp array copy, return result */
                                                CE_SNPRINTF_ADD(
                                                    "({ %schar *__fres = (char *)(%s); for(int _fi=0;_fi<__tl;_fi++){if(__td[_fi])free(__td[_fi]);} free(__td); __fres; })",
                                                    _chain_prefix, _final_expr);
                                                /* result_type already set by last method */
                                            } else {
                                                /* On error, still emit cleanup if possible */
                                                CE_SNPRINTF_ADD(
                                                    "({ %sfor(int _fi=0;_fi<__tl;_fi++){if(__td[_fi])free(__td[_fi]);} free(__td); (char *)\"0\"; })",
                                                    _chain_prefix);
                                            }
                                            free(arrname); free(lenname); free(capname); goto _arr_method_result_done;
                                        }

                                        /* copy()/slice() in general expression: create temp array,
                                           allow chaining, return final scalar or formatted array. */
                                        if ((mname.type == TOKEN_IDENT || token_is_name(mname)) &&
                                            (strcmp(mname.value, "copy") == 0 ||
                                             strcmp(mname.value, "slice") == 0)) {
                                            if (lexer_peek(lexer).type != TOKEN_LPAREN) {
                                                error_state_add(parser_errors, mname.line, mname.col, "Expected '(' after array method");
                                                skip_to_newline(lexer);
                                                CE_SNPRINTF_ADD( "0");
                                                free(arrname); free(lenname); free(capname); goto _arr_method_result_done;
                                            }
                                            lexer_next(lexer); /* consume ( */
                                            int _chain_prefix_cap = 16384;
                                            char *_chain_prefix = (char*)malloc(_chain_prefix_cap); if(!_chain_prefix){CE_SNPRINTF_ADD("0");free(arrname); free(lenname); free(capname); goto _arr_method_result_done;} _chain_prefix[0]=0; int _cp = 0;
                                            EXPR_APPEND(_chain_prefix, _chain_prefix_cap, _cp,
                                                "char **__td=NULL; int __tl=0; int __tc=0; ");
                                            if (strcmp(mname.value, "copy") == 0) {
                                                if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                                                    error_state_add(parser_errors, lexer_peek(lexer).line, lexer_peek(lexer).col, "Expected ')' after copy()");
                                                    CE_GROW(4096);
                                                    skip_to_newline(lexer); CE_SNPRINTF_ADD( "0"); free(arrname); free(lenname); free(capname); goto _arr_method_result_done;
                                                }
                                                lexer_next(lexer); /* consume ) */
                                                EXPR_APPEND(_chain_prefix, _chain_prefix_cap, _cp,
                                                    "_ltng_arr_copy(%s, &__td, &__tl, &__tc, %s); ",
                                                    arrname, lenname);
                                            } else { /* slice */
                                                char *s0 = (char*)malloc(65536); char *s1 = (char*)malloc(65536);
                                                CE_GROW(4096);
                                                if (!s0 || !s1) { free(s0); free(s1); CE_SNPRINTF_ADD( "0"); free(arrname); free(lenname); free(capname); goto _arr_method_result_done; }
                                                s0[0]=0; s1[0]=0;
                                                collect_expression(lexer, table, &s0, &(int){65536}, 1, 1, NULL, NULL, NULL, NULL);
                                                if (lexer_peek(lexer).type != TOKEN_COMMA) {
                                                    error_state_add(parser_errors, lexer_peek(lexer).line, lexer_peek(lexer).col, "Expected ',' after slice start");
                                                    CE_GROW(4096);
                                                    skip_to_newline(lexer); free(s0); free(s1); CE_SNPRINTF_ADD( "0"); free(arrname); free(lenname); free(capname); goto _arr_method_result_done;
                                                }
                                                lexer_next(lexer); /* , */
                                                collect_expression(lexer, table, &s1, &(int){65536}, 1, 0, NULL, NULL, NULL, NULL);
                                                if (lexer_peek(lexer).type != TOKEN_RPAREN) {
                                                    error_state_add(parser_errors, lexer_peek(lexer).line, lexer_peek(lexer).col, "Expected ')' after slice end");
                                                    CE_GROW(4096);
                                                    skip_to_newline(lexer); free(s0); free(s1); CE_SNPRINTF_ADD( "0"); free(arrname); free(lenname); free(capname); goto _arr_method_result_done;
                                                }
                                                lexer_next(lexer); /* ) */
                                                EXPR_APPEND(_chain_prefix, _chain_prefix_cap, _cp,
                                                    "_ltng_arr_slice(%s, %s, (int)(%s), (int)(%s), &__td, &__tl, &__tc); ",
                                                    arrname, lenname, s0, s1);
                                                free(s0); free(s1);
                                            }
                                            const char *_wa = "__td", *_wl = "__tl", *_wc = "__tc";
                                            int _final_expr_cap = 4096;
                                            char *_final_expr = (char*)calloc(1,_final_expr_cap);
                                            if(!_final_expr){ CE_SNPRINTF_ADD("0"); free(arrname); free(lenname); free(capname); goto _arr_method_result_done; }
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
                                                    char *arg_expr = (char*)malloc(65536); if(!arg_expr){_chain_err=1;break;} arg_expr[0]=0;
                                                    VarType at = collect_expression(lexer, table, &arg_expr, &(int){65536}, 1, 0, NULL, NULL, NULL, NULL);
                                                    if (lexer_peek(lexer).type != TOKEN_RPAREN) { free(arg_expr); _chain_err=1; break; } lexer_next(lexer);
                                                    const char *fmt = (at==TYPE_INT)?"%lld":(at==TYPE_FLOAT)?"%g":"%s";
                                                    const char *cast = (at==TYPE_INT)?"(long long)":(at==TYPE_FLOAT)?"(double)":"";
                                                    EXPR_APPEND(_chain_prefix, _chain_prefix_cap, _cp, "char *__apv%d=_ltng_ds_fmt(\"%s\",%s(%s)); _ltng_arr_push(&%s,&%s,&%s,__apv%d); char *__apr%d=_ltng_retbuf_fmt(\"%%s\",__apv%d); free(__apv%d); ", _chain_step,fmt,cast,arg_expr,_wa,_wl,_wc,_chain_step,_chain_step,_chain_step,_chain_step);
                                                    BUF_SET(&_final_expr, &_final_expr_cap,"__apr%d",_chain_step);
                                                    free(arg_expr);
                                                } else if (strcmp(_cm.value, "pop") == 0) {
                                                    if (lexer_peek(lexer).type == TOKEN_RPAREN) { lexer_next(lexer); EXPR_APPEND(_chain_prefix, _chain_prefix_cap, _cp,"char *__pv%d=_ltng_arr_pop(%s,&%s); ",_chain_step,_wa,_wl); BUF_SET(&_final_expr, &_final_expr_cap,"__pv%d",_chain_step); }
                                                    else { char *ie = (char*)malloc(65536); if(!ie){_chain_err=1;break;} ie[0]=0; collect_expression(lexer,table,&ie, &(int){65536},1,0,NULL,NULL,NULL,NULL); if(lexer_peek(lexer).type!=TOKEN_RPAREN){free(ie);_chain_err=1;break;} lexer_next(lexer); EXPR_APPEND(_chain_prefix, _chain_prefix_cap, _cp,"char *__pv%d=_ltng_arr_pop_index(%s,&%s,(int)(%s)); ",_chain_step,_wa,_wl,ie); BUF_SET(&_final_expr, &_final_expr_cap,"__pv%d",_chain_step); free(ie); }
                                                } else if (strcmp(_cm.value, "insert") == 0) {
                                                    char *ie = (char*)malloc(65536); if(!ie){_chain_err=1;break;} ie[0]=0; collect_expression(lexer,table,&ie, &(int){65536},1,1,NULL,NULL,NULL,NULL);
                                                    if(lexer_peek(lexer).type!=TOKEN_COMMA){free(ie);_chain_err=1;break;} lexer_next(lexer);
                                                    char *ae = (char*)malloc(65536); if(!ae){free(ie);_chain_err=1;break;} ae[0]=0; VarType at=collect_expression(lexer,table,&ae, &(int){65536},1,0,NULL,NULL,NULL,NULL);
                                                    if(lexer_peek(lexer).type!=TOKEN_RPAREN){free(ae);free(ie);_chain_err=1;break;} lexer_next(lexer);
                                                    const char *fmt = (at==TYPE_INT)?"%lld":(at==TYPE_FLOAT)?"%g":"%s";
                                                    const char *cast = (at==TYPE_INT)?"(long long)":(at==TYPE_FLOAT)?"(double)":"";
                                                    EXPR_APPEND(_chain_prefix, _chain_prefix_cap, _cp,"char *__aiv%d=_ltng_ds_fmt(\"%s\",%s(%s)); _ltng_arr_insert(&%s,&%s,&%s,(int)(%s),__aiv%d); char *__air%d=_ltng_retbuf_fmt(\"%%s\",__aiv%d); free(__aiv%d); ",_chain_step,fmt,cast,ae,_wa,_wl,_wc,ie,_chain_step,_chain_step,_chain_step,_chain_step);
                                                    BUF_SET(&_final_expr, &_final_expr_cap,"__air%d",_chain_step);
                                                    free(ae); free(ie);
                                                } else if (strcmp(_cm.value, "remove") == 0) {
                                                    char *ie = (char*)malloc(65536); if(!ie){_chain_err=1;break;} ie[0]=0; collect_expression(lexer,table,&ie, &(int){65536},1,0,NULL,NULL,NULL,NULL);
                                                    if(lexer_peek(lexer).type!=TOKEN_RPAREN){free(ie);_chain_err=1;break;} lexer_next(lexer);
                                                    EXPR_APPEND(_chain_prefix, _chain_prefix_cap, _cp,"char *__rv%d=_ltng_arr_pop_index(%s,&%s,(int)(%s)); ",_chain_step,_wa,_wl,ie);
                                                    BUF_SET(&_final_expr, &_final_expr_cap,"__rv%d",_chain_step);
                                                    free(ie);
                                                } else if (strcmp(_cm.value, "sort") == 0) {
                                                    if(lexer_peek(lexer).type!=TOKEN_RPAREN){_chain_err=1;break;} lexer_next(lexer);
                                                    EXPR_APPEND(_chain_prefix, _chain_prefix_cap, _cp,"_ltng_arr_sort(%s,%s); ",_wa,_wl);
                                                    BUF_SET(&_final_expr, &_final_expr_cap,"({ _ltng_retbuf_fmt(\"true\"); })");
                                                } else if (strcmp(_cm.value, "reverse") == 0) {
                                                    if(lexer_peek(lexer).type!=TOKEN_RPAREN){_chain_err=1;break;} lexer_next(lexer);
                                                    EXPR_APPEND(_chain_prefix, _chain_prefix_cap, _cp,"_ltng_arr_reverse(%s,%s); ",_wa,_wl);
                                                    BUF_SET(&_final_expr, &_final_expr_cap,"({ _ltng_retbuf_fmt(\"true\"); })");
                                                } else if (strcmp(_cm.value, "contains") == 0) {
                                                    char *ae = (char*)malloc(65536); if(!ae){_chain_err=1;break;} ae[0]=0; VarType at=collect_expression(lexer,table,&ae, &(int){65536},1,0,NULL,NULL,NULL,NULL);
                                                    if(lexer_peek(lexer).type!=TOKEN_RPAREN){free(ae);_chain_err=1;break;} lexer_next(lexer);
                                                    const char *fmt = (at==TYPE_INT)?"%lld":(at==TYPE_FLOAT)?"%g":"%s";
                                                    const char *cast = (at==TYPE_INT)?"(long long)":(at==TYPE_FLOAT)?"(double)":"";
                                                    EXPR_APPEND(_chain_prefix, _chain_prefix_cap, _cp,"char *__cv%d=_ltng_ds_fmt(\"%s\",%s(%s)); int __cr%d=_ltng_arr_contains(%s,%s,__cv%d); free(__cv%d); ",_chain_step,fmt,cast,ae,_chain_step,_wa,_wl,_chain_step,_chain_step);
                                                    BUF_SET(&_final_expr, &_final_expr_cap,"({ _ltng_retbuf_fmt(\"%%s\",__cr%d?\"true\":\"false\"); })",_chain_step);
                                                    free(ae);
                                                } else if (strcmp(_cm.value, "index_of") == 0) {
                                                    char *ae = (char*)malloc(65536); if(!ae){_chain_err=1;break;} ae[0]=0; VarType at=collect_expression(lexer,table,&ae, &(int){65536},1,0,NULL,NULL,NULL,NULL);
                                                    if(lexer_peek(lexer).type!=TOKEN_RPAREN){free(ae);_chain_err=1;break;} lexer_next(lexer);
                                                    const char *fmt = (at==TYPE_INT)?"%lld":(at==TYPE_FLOAT)?"%g":"%s";
                                                    const char *cast = (at==TYPE_INT)?"(long long)":(at==TYPE_FLOAT)?"(double)":"";
                                                    EXPR_APPEND(_chain_prefix, _chain_prefix_cap, _cp,"char *__iv%d=_ltng_ds_fmt(\"%s\",%s(%s)); int __ir%d=_ltng_arr_index_of(%s,%s,__iv%d); free(__iv%d); ",_chain_step,fmt,cast,ae,_chain_step,_wa,_wl,_chain_step,_chain_step);
                                                    BUF_SET(&_final_expr, &_final_expr_cap,"({ _ltng_retbuf_fmt(\"%%d\",__ir%d); })",_chain_step);
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
                                            if (_chain_err) { CE_SNPRINTF_ADD("0"); free(arrname); free(lenname); free(capname); goto _arr_method_result_done; }
                                            if (!_has_scalar_result) {
                                                /* No chain - return array formatted as string */
                                                BUF_SET(&_final_expr, &_final_expr_cap,
                                                    "({ char *__r=_ltng_ds_new(\"\"); for(int _fi=0;_fi<__tl;_fi++){ if(_fi) _ltng_ds_append(&__r,\", \"); _ltng_ds_append(&__r,__td[_fi]?__td[_fi]:\"\"); } __r; })");
                                            }
                                            CE_SNPRINTF_ADD(
                                                "({ %schar *__fres=(char*)(%s); for(int _fi=0;_i<__tl;_i++){if(__td[_fi])free(__td[_fi]);} free(__td); __fres; })",
                                                _chain_prefix, _final_expr);
                                            result_type = TYPE_STRING;
                                            free(arrname); free(lenname); free(capname); goto _arr_method_result_done;
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
                                CE_SNPRINTF_ADD(
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
                            char *mangled2 = mangle_name_alloc(tok.value);
                            if (symbol_table_has(table, mangled2) && !symbol_table_is_const(table, mangled2)) {
                                /* Rewind output: replace variable conversion with statement expression */
                                pos = saved_pos;
                                if (nxt.type == TOKEN_PLUS_PLUS) {
                                    VarType vt = symbol_table_get(table, mangled2);
                                    if (vt == TYPE_FLOAT) {
                                        CE_SNPRINTF_ADD(
                                            "({ double _t = atof(%s); snprintf(%s, sizeof(%s), \"%%g\", _t + 1.0); _t; })",
                                            mangled2, mangled2, mangled2);
                                        if (!type_locked) result_type = TYPE_FLOAT;
                                    } else if (vt == TYPE_BOOL) {
                                        CE_SNPRINTF_ADD(
                                            "({ long long _t = (long long)(%s[0]=='t'||%s[0]=='1'?1:0); snprintf(%s, sizeof(%s), \"%%lld\", _t + 1); _t; })",
                                            mangled2, mangled2, mangled2, mangled2);
                                        if (!type_locked) result_type = TYPE_INT;
                                    } else {
                                        CE_SNPRINTF_ADD(
                                            "({ long long _t = (long long)atoll(%s); snprintf(%s, sizeof(%s), \"%%lld\", _t + 1); _t; })",
                                            mangled2, mangled2, mangled2);
                                        if (!type_locked) result_type = TYPE_INT;
                                    }
                                } else { /* MINUS_MINUS */
                                    VarType vt = symbol_table_get(table, mangled2);
                                    if (vt == TYPE_FLOAT) {
                                        CE_SNPRINTF_ADD(
                                            "({ double _t = atof(%s); snprintf(%s, sizeof(%s), \"%%g\", _t - 1.0); _t; })",
                                            mangled2, mangled2, mangled2);
                                        if (!type_locked) result_type = TYPE_FLOAT;
                                    } else if (vt == TYPE_BOOL) {
                                        CE_SNPRINTF_ADD(
                                            "({ long long _t = (long long)(%s[0]=='t'||%s[0]=='1'?1:0); snprintf(%s, sizeof(%s), \"%%lld\", _t - 1); _t; })",
                                            mangled2, mangled2, mangled2, mangled2);
                                        if (!type_locked) result_type = TYPE_INT;
                                    } else {
                                        CE_SNPRINTF_ADD(
                                            "({ long long _t = (long long)atoll(%s); snprintf(%s, sizeof(%s), \"%%lld\", _t - 1); _t; })",
                                            mangled2, mangled2, mangled2);
                                        if (!type_locked) result_type = TYPE_INT;
                                    }
                                }
                                free(mangled2);
                                continue;
                            } else {
                                pos = saved_pos;
                                CE_SNPRINTF_ADD(
                                    "({ snprintf(%s, sizeof(%s), \"%%s\", %s); %s; })",
                                    mangled2, mangled2, mangled2, mangled2);
                                free(mangled2);
                                continue;
                            }
                        }
                    }
                }
                /* Array access: x[1], m[1][1], t[1][1][1] (1-based, negative wraps) */
                {
                    Token nxt = lexer_peek(lexer);
                    if (nxt.type == TOKEN_LBRACKET) {
                        char *mangled = mangle_name_alloc(tok.value);
                        VarType vt = symbol_table_get(table, mangled);

                        if (vt == TYPE_ARRAY) {
                            int idx_cap_ie = 4, index_count = 0;
                            char **index_exprs = (char**)malloc(idx_cap_ie * sizeof(char*));
                            if (!index_exprs) { error_state_add(parser_errors, nxt.line, nxt.col, "Out of memory"); skip_to_newline(lexer); return TYPE_NULL; }
                            int index_error = 0;

                            while (nxt.type == TOKEN_LBRACKET) {
                                lexer_next(lexer); /* consume [ */
                                if (index_count >= idx_cap_ie) {
                                    int nc = idx_cap_ie * 2; char **nb = (char**)realloc(index_exprs, nc*sizeof(char*));
                                    if (!nb) { index_error = 1; break; }
                                    index_exprs = nb; idx_cap_ie = nc;
                                }
                                index_exprs[index_count] = (char*)malloc(65536);
                                if (!index_exprs[index_count]) { index_error = 1; break; }
                                index_exprs[index_count][0] = 0;
                                {
                                    Token _idx_peek = lexer_peek(lexer);
                                    if (_idx_peek.type == TOKEN_STRING || _idx_peek.type == TOKEN_CHAR) {
                                        error_state_add(parser_errors, _idx_peek.line, _idx_peek.col,
                                            "Array index must be numeric");
                                    }
                                }
                                VarType _idx_t = collect_expression(lexer, table, &index_exprs[index_count], &(int){65536}, 1, 0, NULL, NULL, NULL, NULL);
                                if (_idx_t == TYPE_ARRAY) {
                                    error_state_add(parser_errors, nxt.line, nxt.col,
                                        "Array index must be numeric");
                                }
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
                                    CE_SNPRINTF_ADD( "0");
                                } else {
                                    char *arrname = _ltng_cfmt("%s_data", mangled);
                                    char *lenname = _ltng_cfmt("%s_len", mangled);

                                    CE_SNPRINTF_ADD( "({ ");
                                    for (int d = 0; d < index_count; d++) {
                                        /* For 1-D arrays use the live length so negative indices see
                                           pushes/pops. Static dimensions remain the shape for N-D. */
                                        if (dim_count == 1) {
                                            /* Live length plus a non-negative OOB sentinel avoids
                                               double-adjustment in the runtime getter. */
                                            CE_SNPRINTF_ADD(
                                                "int __i%d = (int)(%s); __i%d = __i%d > 0 ? __i%d - 1 : __i%d + (%s); if (__i%d < 0 || __i%d >= %s) __i%d = %s; ",
                                                d, index_exprs[d], d, d, d, d, lenname, d, d, lenname, d, lenname);
                                        } else {
                                            CE_SNPRINTF_ADD(
                                                "int __i%d = (int)(%s); __i%d = __i%d > 0 ? __i%d - 1 : __i%d + (%d); ",
                                                d, index_exprs[d], d, d, d, d, dims[d]);
                                        }
                                    }
                                    /* Compute linear index: __idx = __i0; __idx = __idx * d1 + __i1; ... */
                                    CE_SNPRINTF_ADD( "int __idx = __i0");
                                    for (int d = 1; d < index_count; d++) {
                                        CE_SNPRINTF_ADD(
                                            "; __idx = __idx * %d + __i%d", dims[d], d);
                                    }
                                    VarType _aet = symbol_table_get_array_elem_type(table, mangled);
                                    /* If we're already in a string-building context, keep string form.
                                       Otherwise, emit typed access for known homogeneous element types
                                       to support numeric/bool semantics, typeof, and comparisons. */
                                    int _in_str_ctx = (result_type == TYPE_STRING && !type_locked);
                                    if (!_in_str_ctx && _aet == TYPE_INT) {
                                        CE_SNPRINTF_ADD(
                                            "; const char *__av = _ltng_arr_get(%s, %s, __idx); atoll(__av ? __av : \"\"); })",
                                            arrname, lenname);
                                        if (!type_locked) result_type = TYPE_INT;
                                    } else if (!_in_str_ctx && _aet == TYPE_FLOAT) {
                                        CE_SNPRINTF_ADD(
                                            "; const char *__av = _ltng_arr_get(%s, %s, __idx); atof(__av ? __av : \"\"); })",
                                            arrname, lenname);
                                        if (!type_locked) {
                                            if (result_type == TYPE_INT) result_type = TYPE_FLOAT;
                                            else result_type = TYPE_FLOAT;
                                        }
                                    } else if (!_in_str_ctx && _aet == TYPE_BOOL) {
                                        CE_SNPRINTF_ADD(
                                            "; const char *__av = _ltng_arr_get(%s, %s, __idx); (__av && (__av[0]=='t' || __av[0]=='1')) ? 1 : 0; })",
                                            arrname, lenname);
                                        if (!type_locked) result_type = TYPE_BOOL;
                                    } else {
                                        CE_SNPRINTF_ADD(
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
                                    free(arrname); free(lenname);
                                }
                                if (!have_dims) free(dims);
                                for(int _ie=0;_ie<index_count;_ie++) free(index_exprs[_ie]);
                                free(index_exprs);
                            } else if (!index_error) {
                                CE_SNPRINTF_ADD( "0");
                                for(int _ie=0;_ie<index_count;_ie++) free(index_exprs[_ie]);
                                free(index_exprs);
                            } else {
                                for(int _ie=0;_ie<index_count;_ie++) free(index_exprs[_ie]);
                                free(index_exprs);
                            }
                        } else {
                            char *errmsg = _ltng_cfmt("Variable '%s' is not an array", tok.value);
                            error_state_add(parser_errors, tok.line, tok.col, errmsg ? errmsg : "");
                            free(errmsg);
                            CE_SNPRINTF_ADD( "0");
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
                CE_SNPRINTF_ADD( "(");
                paren_depth++;
                if (paren_depth < 64) {
                    depth_content_start[paren_depth] = pos;
                    depth_in_str_concat[paren_depth] = (has_string_concat || result_type == TYPE_STRING) ? 1 : 0;
                }
                break;
            case TOKEN_RPAREN:
                CE_SNPRINTF_ADD( ")");
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
                                char *_rm = mangle_name_alloc(_rtok.value);
                                VarType _rvt = TYPE_NULL;
                                if (symbol_table_has(table, _rm)) _rvt = symbol_table_get(table, _rm);
                                if (_rvt == TYPE_INT || _rvt == TYPE_FLOAT || _rvt == TYPE_BOOL) _right_numeric = 1;
                                free(_rm);
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
                                    if (strstr(_tail, "atoll(") || strstr(_tail, "atof(")) _left_num = 1;
                                    free(_tail); } }
                                }
                                _numeric_add = _left_num;
                            }
                        }
                        if (_numeric_add) {
                            CE_SNPRINTF_ADD( " + ");
                            arithmetic_used = 1;
                            result_type = TYPE_INT;
                        } else {
                            CE_SNPRINTF_ADD( " + ");
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
                                char *_rm = mangle_name_alloc(_rtok.value);
                                VarType _rvt = TYPE_NULL;
                                if (symbol_table_has(table, _rm)) _rvt = symbol_table_get(table, _rm);
                                int _rn = (_rvt == TYPE_INT || _rvt == TYPE_FLOAT || _rvt == TYPE_BOOL);
                                free(_rm);
                                if (_rn) {
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
                                        if (strstr(_tail, "atoll(") || strstr(_tail, "atof(")) _left_num = 1;
                                        free(_tail); }
                                    }
                                    _numeric_mul = _left_num;
                                }
                            }
                        }
                        if (_numeric_mul) {
                            CE_SNPRINTF_ADD( " * ");
                            arithmetic_used = 1;
                            /* stay in numeric subexpr mode - result_type becomes numeric context */
                            result_type = TYPE_INT;
                        } else {
                            CE_SNPRINTF_ADD( " * ");
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
                            CE_SNPRINTF_ADD( "%s", _ops);
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
                            CE_SNPRINTF_ADD( "0");
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
                        CE_SNPRINTF_ADD( "%s", _ops);
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
                    CE_SNPRINTF_ADD( "%s",
                        (tok.type == TOKEN_AMP_AMP) ? " & " : " | ");
                } else {
                    CE_SNPRINTF_ADD( "%s",
                        (tok.type == TOKEN_AMP_AMP) ? " && " : " || ");
                }
                result_type = TYPE_INT;
                comparison_or_logical = 1;
                break;
            }
            case TOKEN_CARET_CARET:
                CE_SNPRINTF_ADD( " ^ ");
                result_type = TYPE_INT;
                comparison_or_logical = 1;
                break;
            case TOKEN_BANG: {
                Token next_tok = lexer_peek(lexer);
                if (token_is_int(next_tok, table)) {
                    CE_SNPRINTF_ADD( "~");
                } else {
                    CE_SNPRINTF_ADD( "!");
                }
                result_type = TYPE_INT;
                comparison_or_logical = 1;
                break;
            }
            case TOKEN_EQ_EQ:   CE_SNPRINTF_ADD( "=="); result_type = TYPE_INT; comparison_or_logical = 1; break;
            case TOKEN_BANG_EQ: CE_SNPRINTF_ADD( "!="); result_type = TYPE_INT; comparison_or_logical = 1; break;
            case TOKEN_LT:      CE_SNPRINTF_ADD( "<");  result_type = TYPE_INT; comparison_or_logical = 1; break;
            case TOKEN_GT:      CE_SNPRINTF_ADD( ">");  result_type = TYPE_INT; comparison_or_logical = 1; break;
            case TOKEN_LT_EQ:   CE_SNPRINTF_ADD( "<="); result_type = TYPE_INT; comparison_or_logical = 1; break;
            case TOKEN_GT_EQ:   CE_SNPRINTF_ADD( ">="); result_type = TYPE_INT; comparison_or_logical = 1; break;
            case TOKEN_RETURN: case TOKEN_BREAK: case TOKEN_CONTINUE: case TOKEN_FUNCTION:
            case TOKEN_SWITCH: case TOKEN_CASE: case TOKEN_DEFAULT:
            case TOKEN_FROM: case TOKEN_TO: case TOKEN_STEP: {
                char *mangled = mangle_name_alloc(tok.value);
                if (!symbol_table_has(table, mangled)) {
                    char *errmsg = _ltng_cfmt("Variable '%s' not declared", tok.value);
                    error_state_add(parser_errors, tok.line, tok.col, errmsg ? errmsg : "");
                    free(errmsg);
                    CE_SNPRINTF_ADD( "0");
                } else {
                    VarType vtype = symbol_table_get(table, mangled);
                    CE_GROW(4096);
                    if (vtype == TYPE_STRING) { CE_SNPRINTF_ADD( "atof(%s)", mangled); if (!type_locked) result_type = TYPE_INT; }
                    else if (vtype == TYPE_INT) CE_SNPRINTF_ADD( "atoll(%s)", mangled);
                    else if (vtype == TYPE_BOOL) CE_SNPRINTF_ADD( "(%s[0]=='t'||%s[0]=='T'||%s[0]=='1'?1:0)", mangled, mangled, mangled);
                    else { CE_SNPRINTF_ADD( "atof(%s)", mangled); if (!type_locked) result_type = TYPE_INT; }
                }
                free(mangled);
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
        CE_GROW(1); if (pos >= out_size - 1) break;
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
            normalize_mixed_expr(&out, &out_size, array_access_used, table); CE_SYNC();
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
            int _pcap = 32;
            char *tmp = ltng_strdup(out ? out : "");
            char **parts = (char**)malloc(_pcap * sizeof(char*));
            char *norm_buf = ltng_strdup("");
            char *split_tmp = ltng_strdup("");
            char **new_parts = (char**)malloc(_pcap * sizeof(char*));
            char *inner_tmp = ltng_strdup("");
            char **cparts = (char**)malloc(_pcap * sizeof(char*));
            char *add_result = ltng_strdup("");
            char *step = ltng_strdup("");
            char *cpart = ltng_strdup("");
            if(!tmp||!parts||!norm_buf||!split_tmp||!new_parts||!inner_tmp||!cparts||!add_result||!step||!cpart){
                for(int _i=0;_i<_pcap;_i++){free(parts[_i]);free(new_parts[_i]);free(cparts[_i]);} free(tmp);free(parts);free(norm_buf);free(split_tmp);free(new_parts);free(inner_tmp);free(cparts);free(add_result);free(step);free(cpart);
            } else {
            for(int _i=0;_i<_pcap;_i++){parts[_i]=ltng_strdup("");new_parts[_i]=ltng_strdup("");cparts[_i]=ltng_strdup("");}
#define ENSURE_PCAP(n) do { while ((n) >= _pcap) { int _nc=_pcap*2; char **_np=(char**)realloc(parts,_nc*sizeof(char*)); char **_nn=(char**)realloc(new_parts,_nc*sizeof(char*)); char **_nc2=(char**)realloc(cparts,_nc*sizeof(char*)); if(!_np||!_nn||!_nc2) break; parts=_np; new_parts=_nn; cparts=_nc2; for(int _j=_pcap;_j<_nc;_j++){parts[_j]=ltng_strdup("");new_parts[_j]=ltng_strdup("");cparts[_j]=ltng_strdup("");} _pcap=_nc; } } while(0)
            int nparts = 0;
            char *start = tmp;
            int depth_paren = 0, depth_bracket = 0, depth_brace = 0;
            int in_str = 0;
            char q = 0;
            for (char *p = tmp; *p; p++) {
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
                    ENSURE_PCAP(nparts); free(parts[nparts]); parts[nparts] = ltng_strdup(s); nparts++;
                    start = p + 1;
                }
            }
            {
                char *s = start;
                while (*s == ' ' || *s == '\t') s++;
                char *e = s + strlen(s) - 1;
                while (e >= s && (*e == ' ' || *e == '\t')) { *e = 0; e--; }
                free(parts[nparts]); parts[nparts] = ltng_strdup(s); nparts++;
            }
            /* transform each part */
            /* If no actual concatenation at top level (e.g. all '+' inside parens),
               try to normalize by unwrapping outer parens to expose top-level '+'. */
            if (nparts <= 1 && has_string_concat) {
                { char *_t=ltng_strdup(parts[0]); if(_t){free(norm_buf); norm_buf=_t;} }
                /* Unwrap whole-expression paren layers dynamically. Each pass either
                   exposes a top-level '+' (then we split) or strips one wrapping layer,
                   so the number of passes is bounded by the string length - no fixed
                   depth cap that would crash on deeply nested parentheses. */
                int unwrap_limit = (int)strlen(norm_buf);
                if (unwrap_limit < 1) unwrap_limit = 1;
                for (int unwrap_try = 0; unwrap_try < unwrap_limit && nparts <= 1; unwrap_try++) {
                    /* try splitting current norm_buf by top-level '+' first */
                    {
                        { char *_t=ltng_strdup(norm_buf); if(_t){free(split_tmp); split_tmp=_t;} }
                        char *ist2 = split_tmp; int d2p=0,d2b=0,d2br=0,in2=0; char q2c=0; int new_n=0;
                        for (char *pp = split_tmp; *pp; pp++) {
                            if (in2) { if (*pp == q2c && (pp == split_tmp || *(pp-1) != '\\')) in2 = 0; continue; }
                            if (*pp == '"' || *pp == '\'') { in2 = 1; q2c = *pp; continue; }
                            if (*pp == '(') d2p++; else if (*pp == ')') { if (d2p>0) d2p--; }
                            else if (*pp == '[') d2b++; else if (*pp == ']') { if (d2b>0) d2b--; }
                            else if (*pp == '{') d2br++; else if (*pp == '}') { if (d2br>0) d2br--; }
                            else if (*pp == '+' && d2p==0 && d2b==0 && d2br==0) {
                                *pp = 0; char *s2 = ist2; while (*s2==' '||*s2=='\t') s2++;
                                char *e2 = s2 + strlen(s2) - 1; while (e2>=s2 && (*e2==' '||*e2=='\t')) { *e2=0; e2--; }
                                ENSURE_PCAP(new_n); free(new_parts[new_n]); new_parts[new_n]=ltng_strdup(s2); new_n++; ist2 = pp+1;
                            }
                        }
                        { char *s2 = ist2; while (*s2==' '||*s2=='\t') s2++;
                          char *e2 = s2 + strlen(s2) - 1; while (e2>=s2 && (*e2==' '||*e2=='\t')) { *e2=0; e2--; }
                          ENSURE_PCAP(new_n); free(new_parts[new_n]); new_parts[new_n]=ltng_strdup(s2); new_n++; }
                        if (new_n > 1) {
                            for (int pi = 0; pi < new_n && pi < _pcap; pi++) { char *_t=ltng_strdup(new_parts[pi]); if(_t){free(parts[pi]); parts[pi]=_t;} }
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
                    if (nil >= 2048) nil = 2047;
                    memcpy(inner_tmp, nq + 1, nil); inner_tmp[nil] = 0;
                    { char *_t=ltng_strdup(inner_tmp); if(_t){free(norm_buf); norm_buf=_t;} }
                    { char *_t=ltng_strdup(inner_tmp); if(_t){free(parts[0]); parts[0]=_t;} }
                }
            }
            /* If no actual concatenation (single part), skip wrapping */
            if (nparts <= 1) {
                /* Handle string-multiply of a (possibly nested) string-concat expression: e.g. (a+b)*n, ((a+b)*n + "x")*m */
                if (has_string_concat && has_string_mult) {
                    /* Resolve nested '+' (concat) and '*' (repeat) with correct
                       precedence, handling arbitrary parenthesis depth. */
                    normalize_string_expr(&out, &out_size, array_access_used, table); CE_SYNC();
                    result_type = TYPE_STRING;
                    nparts = -1;
                }
                /* No concatenation needed, keep original expression, or handled above */
            } else if (array_access_used) {
                /* Array elements are string buffers; use the runtime
                   numeric-or-string add so `arr[1] + arr[2]` adds numbers
                   and concatenates strings. */
                /* cparts allocated above */
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
                        { char *_t=ltng_strdup(unwrapped); if(_t){free(cparts[i]); cparts[i]=_t;} }
                    } else if (!part_is_stringy(part)) {
                        int is_float_expr = (strchr(part, '.') != NULL ||
                            strstr(part, "atof(") != NULL ||
                            strstr(part, "(double)") != NULL ||
                            strstr(part, "_LTNG_NZDIV_D") != NULL ||
                            strstr(part, "_LTNG_NZMOD_D") != NULL ||
                            strstr(part, "_LTNG_FMOD_D") != NULL);
                        { char *_t=_ltng_cfmt("({ _ltng_retbuf_fmt(\"%s\",(%s)(%s)); })",
                                 is_float_expr ? "%g" : "%lld",
                                 is_float_expr ? "double" : "long long",
                                 part);
                          if(_t){free(cparts[i]); cparts[i]=_t;} }
                    } else {
                        { char *_t=ltng_strdup(part); if(_t){free(cparts[i]); cparts[i]=_t;} }
                        { int _ctc=(int)strlen(cparts[i])+1; normalize_string_expr(&cparts[i],&_ctc,array_access_used,table); }
                    }
                    free(unwrapped);
                }
                { char *_t=ltng_strdup(cparts[0]); if(_t){free(add_result); add_result=_t;} }
                for (int i = 1; i < nparts; i++) {
                    { char *_t=_ltng_cfmt("_ltng_add_sn(%s, %s)", add_result, cparts[i]); if(_t){free(step); step=_t;} }
                    { char *_t=ltng_strdup(step); if(_t){free(add_result); add_result=_t;} }
                }
                { size_t _al=strlen(add_result); CE_GROW((int)_al); memcpy(out, add_result, _al+1); pos=(int)_al; }
                result_type = TYPE_STRING;
            } else {
            /* concatenate: build expression from parts */
{ char *_t=ltng_strdup(""); if(_t){free(add_result); add_result=_t;} }
            int all_stringy = 1;
            for (int i = 0; i < nparts; i++) {
                const char *part = parts[i];
                { char *_t=ltng_strdup(""); if(_t){free(cpart); cpart=_t;} }
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
                    { char *_t=ltng_strdup(unwrapped); if(_t){free(cpart); cpart=_t;} }
                    all_stringy = 0; /* force _ltng_add_sn for runtime-correct behavior */
                } else if (!part_is_stringy(part)) {
                    all_stringy = 0;
                    int is_float_expr = (strchr(part, '.') != NULL ||
                        strstr(part, "atof(") != NULL ||
                        strstr(part, "(double)") != NULL ||
                        strstr(part, "_LTNG_NZDIV_D") != NULL ||
                        strstr(part, "_LTNG_NZMOD_D") != NULL ||
                        strstr(part, "_LTNG_FMOD_D") != NULL);
                    { char *_t=_ltng_cfmt("({ _ltng_retbuf_fmt(\"%s\",(%s)(%s)); })",
                             is_float_expr ? "%g" : "%lld",
                             is_float_expr ? "double" : "long long",
                             part);
                      if(_t){free(cpart); cpart=_t;} }
                } else {
                    { char *_t=ltng_strdup(part); if(_t){free(cpart); cpart=_t;} }
                    { int _ctc=(int)strlen(cpart)+1; normalize_string_expr(&cpart,&_ctc,array_access_used,table); }
                }
                free(unwrapped);
                if (i == 0) {
                    { char *_t=ltng_strdup(cpart); if(_t){free(add_result); add_result=_t;} }
                } else {
                    if (all_stringy) {
                        { char *_t=_ltng_cfmt("_ltng_concat_dyn(2, %s, %s)", add_result, cpart); if(_t){free(step); step=_t;} }
                    } else {
                        { char *_t=_ltng_cfmt("_ltng_add_sn(%s, %s)", add_result, cpart); if(_t){free(step); step=_t;} }
                    }
                    { char *_t=ltng_strdup(step); if(_t){free(add_result); add_result=_t;} }
                    add_result[2047] = 0;
                }
            }
            { size_t _al=strlen(add_result); CE_GROW((int)_al); memcpy(out, add_result, _al+1); pos=(int)_al; }
            result_type = TYPE_STRING;
            }
            for(int _i=0;_i<_pcap;_i++){free(parts[_i]);free(new_parts[_i]);free(cparts[_i]);} free(tmp);free(parts);free(norm_buf);free(split_tmp);free(new_parts);free(inner_tmp);free(cparts);free(add_result);free(step);free(cpart);
            }
        } else if (strstr(out, "*") != NULL) {
            /* Use normalize_string_expr for general multiply expressions
               (including nested/chained repeats like (("a"*2)*2)*2). */
            normalize_string_expr(&out, &out_size, array_access_used, table); CE_SYNC();
            result_type = TYPE_STRING;
        }
    }
    g_expr_sl=sl; g_expr_sc=sc; g_expr_el=el; g_expr_ec=ec;
    if (out_sl) *out_sl = sl;
    if (out_sc) *out_sc = sc;
    if (out_el) *out_el = el;
    if (out_ec) *out_ec = ec;
    return result_type;
}

const char *var_type_name(VarType t) {
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
VarType infer_expression_text_type(const char *expr, SymbolTable *table) {
    size_t blen = strlen(expr ? expr : "") + 2;
    char *buf = (char*)malloc(blen);
    if (!buf) return TYPE_NULL;
    snprintf(buf, blen, "%s\n", expr ? expr : "");
    Lexer lexer;
    lexer_init(&lexer, buf);
    ErrorState tmp_errors;
    memset(&tmp_errors, 0, sizeof(tmp_errors));
    tmp_errors.filename = strdup("<typeof>");
    lexer_tokenize(&lexer, &tmp_errors);

    ErrorState *saved = parser_errors;
    parser_errors = &tmp_errors;
    char *scratch = (char*)malloc(65536);
    VarType type = scratch ? collect_expression(&lexer, table, &scratch, &(int){65536},
                                       1, 0, NULL, NULL, NULL, NULL) : TYPE_NULL;
    parser_errors = saved;
    lexer_free(&lexer);
    free(tmp_errors.filename);
    free(scratch);
    free(buf);
    return type;
}

void append_resolved_text(char *resolved, int *rlen, const char *text) {
    int avail = 65536 - *rlen;
    if (avail <= 0) return;
    int n = snprintf(resolved + *rlen, (size_t)avail, "%s", text);
    if (n > 0) *rlen += (n < avail ? n : avail - 1);
}

/* Resolve the inner text of a `$[typeof(...)]` interpolation. Returns 1 when
   handled (including conversion helpers, calls, array elements, and plain vars). */
int resolve_typeof_interpolation(const char *vname, SymbolTable *table,
                                        int line, int col, char *resolved, int *rlen) {
    int vlen = (int)strlen(vname);
    if (strncmp(vname, "typeof(", 7) != 0 || vlen < 8 || vname[vlen - 1] != ')') return 0;

    int ilen = vlen - 8;
    char *inner = (char*)malloc((size_t)ilen + 1);
    if (!inner) return 0;
    memcpy(inner, vname + 7, (size_t)ilen);
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
                        int sl = _il - _conv_prefix - 3;
                        char *sv = (char*)malloc((size_t)sl + 1);
                        if (sv) {
                            memcpy(sv, inner + _conv_prefix + 1, (size_t)sl); sv[sl] = '\0';
                            int ok = 0, k = 0; if (sv[k]=='+'||sv[k]=='-') k++; int digits=0,dot=0,valid=1;
                            for (; sv[k]; k++) { if (isdigit((unsigned char)sv[k])) digits=1; else if (sv[k]=='.'&&!dot) dot=1; else {valid=0;break;} }
                            if (valid && digits) ok = 1;
                            if (!ok) {
                                char *msg = _ltng_cfmt("Invalid conversion: cannot convert \"%s\" with %s", sv, _conv_cname);
                                error_state_add(parser_errors, line, col, msg ? msg : "");
                                free(msg);
                            }
                            free(sv);
                        }
                    }
                }
                append_resolved_text(resolved, rlen, _conv_tname); free(inner); return 1;
            }
            /* Part of a larger expression: infer from full expression */
            VarType t = infer_expression_text_type(inner, table);
            append_resolved_text(resolved, rlen, var_type_name(t == TYPE_NULL ? TYPE_STRING : t));
            free(inner); return 1;
        }
    }

    const char *p2 = inner;
    while (*p2 && (isalnum((unsigned char)*p2) || *p2 == '_')) p2++;
    if (p2 > inner && *p2 == '(') {
        int flen = (int)(p2 - inner);
        char *fname = (char*)malloc((size_t)flen + 1);
        if (fname) {
            memcpy(fname, inner, (size_t)flen);
            fname[flen] = '\0';
            if (find_function_ast_by_name(fname) || is_callable_ident(table, fname) ||
                is_function_name_scoped(table, fname)) {
                VarType t = infer_expression_text_type(inner, table);
                append_resolved_text(resolved, rlen, var_type_name(t == TYPE_NULL ? TYPE_STRING : t));
            } else {
                append_resolved_text(resolved, rlen, "string");
            }
            free(fname);
        } else {
            append_resolved_text(resolved, rlen, "string");
        }
        free(inner); return 1;
    }

    if (strchr(inner, '[')) {
        VarType t = infer_expression_text_type(inner, table);
        append_resolved_text(resolved, rlen, var_type_name(t == TYPE_NULL ? TYPE_STRING : t));
        free(inner); return 1;
    }

    const char *q = inner;
    while (strncmp(q, "typeof(", 7) == 0) {
        int nl = (int)strlen(q);
        if (nl < 8 || q[nl - 1] != ')') break;
        int slen = nl - 8;
        char *stripped = (char*)malloc((size_t)slen + 1);
        if (!stripped) break;
        memcpy(stripped, q + 7, (size_t)slen);
        stripped[slen] = '\0';
        free(inner);
        inner = stripped;
        q = inner;
    }
    if (q != inner || strncmp(inner, "typeof(", 7) == 0) {
        append_resolved_text(resolved, rlen, "string");
        free(inner); return 1;
    }

    {
        const char *dot = strchr(inner, '.');
        if (dot) {
            int blen = (int)(dot - inner);
            char *base = (char*)malloc((size_t)blen + 1);
            const char *field = dot + 1;
            int simple_member = 0;
            if (base && blen > 0 && *field != '\0' && !strchr(field, '.')) {
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
                memcpy(base, inner, (size_t)blen);
                base[blen] = '\0';
                char *mvbase = mangle_name_alloc(base);
                free(base);
                base = NULL;
                if (!symbol_table_has(table, mvbase)) {
                    char *errmsg = _ltng_cfmt("Variable '%s' not declared (used in typeof)", inner);
                    error_state_add(parser_errors, line, col, errmsg ? errmsg : "");
                    free(errmsg);
                    free(mvbase);
                    free(inner);
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
                free(mvbase);
                free(inner);
                return 1;
            }
            free(base);
            VarType t = infer_expression_text_type(inner, table);
            append_resolved_text(resolved, rlen, var_type_name(t == TYPE_NULL ? TYPE_STRING : t));
            free(inner);
            return 1;
        }
    }

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
            char *mvname = mangle_name_alloc(inner);
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
            free(mvname);
            free(inner);
            return 1;
        }
    }

    {
        VarType t = infer_expression_text_type(inner, table);
        append_resolved_text(resolved, rlen, var_type_name(t == TYPE_NULL ? TYPE_STRING : t));
    }
    free(inner);
    return 1;
}

void resolve_string_interpolation(Token *tok, SymbolTable *table, int line, int col) {
    if (tok->type != TOKEN_STRING) return;
    if (!strchr(tok->value, '$')) return;

    size_t res_cap = 65536;
    char *resolved = (char*)malloc(res_cap);
    if (!resolved) return;
    size_t rlen = 0;
    #define RES_ENSURE(n) do { size_t _need = rlen + (size_t)(n) + 1; if (_need > res_cap) { while (res_cap < _need) res_cap *= 2; char *_nr = (char*)realloc(resolved, res_cap); if (!_nr) { free(resolved); return; } resolved = _nr; } } while(0)
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
                char *vname = (char*)malloc((size_t)vlen + 1);
                if (!vname) { sp++; continue; }
                memcpy(vname, vstart, (size_t)vlen);
                vname[vlen] = '\0';
                {
                    int _rlen_i = (int)rlen;
                    if (resolve_typeof_interpolation(vname, table, line, col, resolved, &_rlen_i)) {
                        rlen = (size_t)_rlen_i;
                        /* skip generic variable/expression validation below */
                    } else {
                    rlen = (size_t)_rlen_i;
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
                        char *mvname = mangle_name_alloc(vname);
                        if (!symbol_table_has(table, mvname) && !is_type_keyword(vname) && !is_bool_value(vname)) {
                            char *errmsg = _ltng_cfmt("Variable '%s' not declared (used in string interpolation)", vname);
                            error_state_add(parser_errors, line, col, errmsg ? errmsg : "");
                            free(errmsg);
                        }
                        free(mvname);
                        int voff = (int)(vstart - 2 - tok->value);
                        int vtotal = (int)(sp - (vstart - 2) + 1);
                        for (int ci = voff; ci < voff + vtotal; ci++) {
                            RES_ENSURE(1);
                            resolved[rlen++] = tok->value[ci];
                        }
                    } else {
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
                            int ilen = vlen - prefix - 1; if (ilen < 0) ilen = 0;
                            char *inner = (char*)malloc((size_t)ilen + 1);
                            if (inner) {
                                memcpy(inner, vname + prefix, (size_t)ilen); inner[ilen] = '\0';
                                while (ilen > 0 && (inner[ilen-1] == ' ' || inner[ilen-1] == '\t')) ilen--;
                                inner[ilen] = '\0';
                                int is_str_lit = (ilen >= 2 && inner[0] == '"' && inner[ilen-1] == '"');
                                int is_num_lit = 0, has_dot = 0;
                                if (!is_str_lit && ilen > 0) {
                                    const char *p2n = inner; if (*p2n=='+'||*p2n=='-') p2n++;
                                    int d=0,v=1; for(;*p2n;p2n++){ if(isdigit((unsigned char)*p2n)) d=1; else if(*p2n=='.'&&!has_dot) has_dot=1; else {v=0;break;} } if(d&&v) is_num_lit=1;
                                }
                                int is_bool_lit = (!is_str_lit && !is_num_lit && (strcmp(inner,"true")==0||strcmp(inner,"false")==0||strcmp(inner,"t")==0||strcmp(inner,"f")==0));
                                if (strcmp(conv_name, "toint") == 0) {
                                    if (is_str_lit) {
                                        for (int i=1;i<ilen-1;i++) { RES_ENSURE(1); resolved[rlen++]=inner[i]; }
                                        resolved[rlen]='\0'; folded=1;
                                    } else if (is_num_lit) {
                                        int n = snprintf(NULL,0,"%s", inner);
                                        RES_ENSURE(n); rlen += (size_t)snprintf(resolved + rlen, res_cap - rlen, "%s", inner); folded=1;
                                    }
                                } else if (strcmp(conv_name, "tofloat") == 0) {
                                    if (is_str_lit) {
                                        int cpos2=0; char *sval = (char*)malloc((size_t)ilen + 1);
                                        if (sval) { for (int i=1;i<ilen-1;i++){ if(inner[i]=='\\' && i+1<ilen-1) sval[cpos2++]=inner[++i]; else sval[cpos2++]=inner[i]; } sval[cpos2]='\0';
                                            int n = snprintf(NULL,0,"%g", atof(sval)); RES_ENSURE(n); rlen += (size_t)snprintf(resolved + rlen, res_cap - rlen, "%g", atof(sval)); folded=1; free(sval);
                                        }
                                    } else if (is_num_lit) {
                                        int n = snprintf(NULL,0,"%g", atof(inner)); RES_ENSURE(n); rlen += (size_t)snprintf(resolved + rlen, res_cap - rlen, "%g", atof(inner)); folded=1;
                                    }
                                } else if (strcmp(conv_name, "tostring") == 0) {
                                    if (is_str_lit) {
                                        for (int i=1;i<ilen-1;i++) { RES_ENSURE(1); resolved[rlen++]=inner[i]; }
                                        resolved[rlen]='\0'; folded=1;
                                    } else if (ilen==4 && memcmp(inner,"true",4)==0) { RES_ENSURE(4); memcpy(resolved+rlen,"true",4); rlen+=4; resolved[rlen]='\0'; folded=1; }
                                    else if (ilen==5 && memcmp(inner,"false",5)==0) { RES_ENSURE(5); memcpy(resolved+rlen,"false",5); rlen+=5; resolved[rlen]='\0'; folded=1; }
                                    else if (ilen==1 && inner[0]=='t') { RES_ENSURE(4); memcpy(resolved+rlen,"true",4); rlen+=4; resolved[rlen]='\0'; folded=1; }
                                    else if (ilen==1 && inner[0]=='f') { RES_ENSURE(5); memcpy(resolved+rlen,"false",5); rlen+=5; resolved[rlen]='\0'; folded=1; }
                                    else if (is_num_lit) { int n = snprintf(NULL,0,"%s", inner); RES_ENSURE(n); rlen += (size_t)snprintf(resolved + rlen, res_cap - rlen, "%s", inner); folded=1; }
                                } else if (strcmp(conv_name, "tobool") == 0) {
                                    if (is_num_lit) { const char *bv = (atof(inner)!=0.0)?"true":"false"; int n=(int)strlen(bv); RES_ENSURE(n); memcpy(resolved+rlen,bv,(size_t)n); rlen+=(size_t)n; resolved[rlen]='\0'; folded=1; }
                                    else if (is_bool_lit) { const char *bv = (inner[0]=='t')?"true":"false"; int n=(int)strlen(bv); RES_ENSURE(n); memcpy(resolved+rlen,bv,(size_t)n); rlen+=(size_t)n; resolved[rlen]='\0'; folded=1; }
                                    else if (is_str_lit) {
                                        const char *bv;
                                        if (ilen==2 || (ilen==3 && inner[1]=='0') || (ilen==3 && inner[1]=='f') || (ilen==7 && memcmp(inner+1,"false",5)==0)) bv="false"; else bv="true";
                                        int n=(int)strlen(bv); RES_ENSURE(n); memcpy(resolved+rlen,bv,(size_t)n); rlen+=(size_t)n; resolved[rlen]='\0'; folded=1;
                                    }
                                } else if (strcmp(conv_name, "tochar") == 0) {
                                    if (is_str_lit) {
                                        char ch = (ilen>=3)?inner[1]:'\0';
                                        if (ch=='\\' && ilen>=4) { RES_ENSURE(2); resolved[rlen++]=inner[1]; resolved[rlen++]=inner[2]; resolved[rlen]='\0'; }
                                        else { RES_ENSURE(1); resolved[rlen++]=ch; resolved[rlen]='\0'; }
                                        folded=1;
                                    } else if (is_num_lit && !has_dot) {
                                        RES_ENSURE(1); resolved[rlen++] = (char)(unsigned char)atoi(inner); resolved[rlen]='\0'; folded=1;
                                    }
                                }
                                free(inner);
                            }
                        }
                        if (!folded) {
                            int check_cap = 65536;
                            char *check = (char*)malloc(check_cap);
                            if (check) { check[0]=0; (void)parse_expression_from_string(vname, table, &check, &check_cap); free(check); }
                            int voff = (int)(vstart - 2 - tok->value);
                            int vtotal = (int)(sp - (vstart - 2) + 1);
                            for (int ci = voff; ci < voff + vtotal; ci++) { RES_ENSURE(1); resolved[rlen++] = tok->value[ci]; }
                        }
                    }
                    }
                }
                free(vname);
            } else {
                RES_ENSURE(2); resolved[rlen++] = '$'; resolved[rlen++] = '[';
            }
        } else {
            RES_ENSURE(1); resolved[rlen++] = *sp;
        }
    }
    resolved[rlen] = '\0';
    { free(tok->value); tok->value = ltng_strdup(resolved); }
    free(resolved);
    #undef RES_ENSURE
}

