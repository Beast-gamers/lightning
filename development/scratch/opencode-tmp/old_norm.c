/* Normalize a string-expression fragment: resolve '+' (concat) and '*'
   (repeat) with correct operator precedence, resolving innermost
   parenthesized groups first.  Statement expressions ({ ... }) are treated
   as atomic operands.  array_access_used selects _ltng_add_sn (array
   elements) vs the snprintf(...) concat form (general contexts).  Each
   pass resolves one group, so the number of passes is bounded by the
   string length - no fixed depth cap that would crash on deeply nested
   parentheses. */
static void normalize_string_expr(char *buf, int buf_size, int array_access_used) {
    if (!buf || buf_size < 2) return;
    char work[2048];
    strncpy(work, buf, sizeof(work) - 1);
    work[sizeof(work) - 1] = 0;

    /* Resolve innermost parenthesized groups containing '+' or '*',
       repeatedly, until no such group remains.  Each pass resolves one
       group, so the number of passes is bounded by the string length. */
    for (int pass = 0; pass < (int)strlen(work); pass++) {
        int wlen = (int)strlen(work);
        int bs = -1, be = -1;
        int do_plus = 0, do_mul = 0;
        for (int ii = 0; ii < wlen; ii++) {
            if (work[ii] != '(' || (ii + 1 < wlen && work[ii + 1] == '{')) continue;
            int d = 0, inq = 0; char qc = 0; int jj = ii;
            while (jj < wlen) {
                if (inq) { if (work[jj] == '\\' && jj + 1 < wlen) { jj += 2; continue; } if (work[jj] == qc) inq = 0; jj++; continue; }
                if (work[jj] == '"' || work[jj] == '\'') { inq = 1; qc = work[jj]; jj++; continue; }
                if (work[jj] == '(') d++; else if (work[jj] == ')') { d--; if (d == 0) break; }
                jj++;
            }
            if (jj >= wlen) continue;
            /* skip whole-buffer wrap */
            { char *pre = work; while (*pre == ' ' || *pre == '\t') pre++; const char *aft = work + jj + 1; while (*aft == ' ' || *aft == '\t') aft++; if (pre == work + ii && *aft == '\0') continue; }
            /* scan content for top-level '+' and '*' */
            int id = 0, iin = 0; char iqc = 0; int hp = 0, hm = 0;
            for (int kk = ii + 1; kk < jj; kk++) {
                char cc = work[kk];
                if (iin) { if (cc == '\\' && kk + 1 < jj) { kk++; continue; } if (cc == iqc) iin = 0; continue; }
                if (cc == '"' || cc == '\'') { inq = 1; iqc = cc; continue; }
                if (cc == '(' || cc == '{' || cc == '[') id++; else if (cc == ')' || cc == '}' || cc == ']') { if (id > 0) id--; }
                else if (cc == '+' && id == 0) hp = 1;
                else if (cc == '*' && id == 0) hm = 1;
            }
            if (!hp && !hm) continue;
            /* innermost: no inner group has '+' or '*' */
            int has_inner = 0;
            for (int xi = ii + 1; xi < jj && !has_inner; xi++) {
                if (work[xi] != '(' || (xi + 1 < wlen && work[xi + 1] == '{')) continue;
                int xd = 0, xinq = 0; char xqc = 0; int xj = xi;
                while (xj < jj) {
                    if (xinq) { if (work[xj] == '\\' && xj + 1 < jj) { xj += 2; continue; } if (work[xj] == xqc) xinq = 0; xj++; continue; }
                    if (work[xj] == '"' || work[xj] == '\'') { xinq = 1; xqc = work[xj]; xj++; continue; }
                    if (work[xj] == '(') xd++; else if (work[xj] == ')') { xd--; if (xd == 0) break; }
                    xj++;
                }
                if (xj >= jj) continue;
                int xid = 0, xiin = 0; char xiqc = 0; int xhp = 0, xhm = 0;
                for (int xk = xi + 1; xk < xj; xk++) {
                    char xc = work[xk];
                    if (xiin) { if (xc == '\\' && xk + 1 < xj) { xk++; continue; } if (xc == xiqc) xiin = 0; continue; }
                    if (xc == '"' || xc == '\'') { xiin = 1; xiqc = xc; continue; }
                    if (xc == '(' || xc == '{' || xc == '[') xid++; else if (xc == ')' || xc == '}' || xc == ']') { if (xid > 0) xid--; }
                    else if (xc == '+' && xid == 0) xhp = 1;
                    else if (xc == '*' && xid == 0) xhm = 1;
                }
                if (xhp || xhm) has_inner = 1;
            }
            if (has_inner) continue;
            bs = ii; be = jj; do_plus = hp; do_mul = hm; break;
        }
        if (bs == -1) break;
        int clen = be - bs - 1;
        char content[1024];
        if (clen >= (int)sizeof(content)) clen = sizeof(content) - 1;
        memcpy(content, work + bs + 1, clen);
        content[clen] = 0;

        if (do_plus) {
            char parts[16][512]; int nip = 0; char *ist = content;
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
            char ires[1024];
            strncpy(ires, parts[0], sizeof(ires) - 1); ires[sizeof(ires) - 1] = 0;
            normalize_string_expr(ires, sizeof(ires), array_access_used);
            for (int k = 1; k < nip; k++) {
                char p[512];
                strncpy(p, parts[k], sizeof(p) - 1); p[sizeof(p) - 1] = 0;
                normalize_string_expr(p, sizeof(p), array_access_used);
                char st[1200];
                if (array_access_used) snprintf(st, sizeof(st), "_ltng_add_sn(%s, %s)", ires, p);
                else snprintf(st, sizeof(st), "({ char _ltc[512]; snprintf(_ltc,512,\"%s%s\", %s, %s); _ltc; })", ires, p);
                strncpy(ires, st, sizeof(ires) - 1); ires[sizeof(ires) - 1] = 0;
            }
            char nb[2048];
            snprintf(nb, sizeof(nb), "%.*s%s%s", bs, work, ires, work + be + 1);
            strncpy(work, nb, sizeof(work) - 1); work[sizeof(work) - 1] = 0;
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
            char left[512] = {0}, right[512] = {0};
            int llen = star; if (llen >= (int)sizeof(left)) llen = sizeof(left) - 1;
            memcpy(left, content, llen); left[llen] = 0;
            int rlen = clen - star - 1; if (rlen >= (int)sizeof(right)) rlen = sizeof(right) - 1;
            memcpy(right, content + star + 1, rlen); right[rlen] = 0;
            { char *s = left; while (*s == ' ' || *s == '\t') s++; if (s != left) memmove(left, s, strlen(s) + 1); }
            { char *e = left + strlen(left) - 1; while (e >= left && (*e == ' ' || *e == '\t')) { *e = 0; e--; } }
            { char *s = right; while (*s == ' ' || *s == '\t') s++; if (s != right) memmove(right, s, strlen(s) + 1); }
            { char *e = right + strlen(right) - 1; while (e >= right && (*e == ' ' || *e == '\t')) { *e = 0; e--; } }
            if (left[0] == '\0' || right[0] == '\0') break;
            normalize_string_expr(left, sizeof(left), array_access_used);
            int right_is_num = 1;
            const char *rsp = right;
            if (*rsp == '-') rsp++;
            if (*rsp == 0) right_is_num = 0;
            else { int hd = 0; for (; *rsp; rsp++) { if (isdigit((unsigned char)*rsp)) hd = 1; else if (*rsp == '.') {} else { right_is_num = 0; break; } } if (!hd) right_is_num = 0; }
            char countbuf[256];
            if (right_is_num) snprintf(countbuf, sizeof(countbuf), "%s", right);
            else snprintf(countbuf, sizeof(countbuf), "atoi(%s)", right);
            char rep[1024];
            snprintf(rep, sizeof(rep), "({ char _ltm[512]; _ltng_repeat(_ltm,512,%s,(long long)(%s)); _ltm; })", left, countbuf);
            char nb[2048];
            snprintf(nb, sizeof(nb), "%.*s%s%s", bs, work, rep, work + be + 1);
            strncpy(work, nb, sizeof(work) - 1); work[sizeof(work) - 1] = 0;
        }
    }

    strncpy(buf, work, buf_size - 1);
    buf[buf_size - 1] = 0;
}
