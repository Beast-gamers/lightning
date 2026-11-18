#include "ltng.h"

void error_state_init(ErrorState *state, char *source, const char *filename, int check_only) {
    if (!state) return;
    state->errors = NULL;
    state->count = 0;
    state->errors_cap = 0;
    state->source_lines = NULL;
    state->total_lines = 0;
    state->source_lines_cap = 0;
    state->filename = filename ? strdup(filename) : strdup("");
    state->check_only = check_only;

    char *copy = strdup(source ? source : "");
    if (!copy) return;

    char *ptr = copy;
    while (1) {
        if (state->total_lines >= state->source_lines_cap) {
            int nc = state->source_lines_cap ? state->source_lines_cap * 2 : 128;
            char **nl = (char **)realloc(state->source_lines, nc * sizeof(char *));
            if (!nl) return;
            state->source_lines = nl;
            state->source_lines_cap = nc;
        }
        state->source_lines[state->total_lines++] = ptr;
        char *nlc = strchr(ptr, '\n');
        if (!nlc) break;
        *nlc = '\0';
        ptr = nlc + 1;
        if (*ptr == '\0') break;
    }
}

void error_state_add(ErrorState *state, int line, int col, const char *msg) {
    if (!state) return;
    if (state->count >= state->errors_cap) {
        int nc = state->errors_cap ? state->errors_cap * 2 : 16;
        CompileError *ne = (CompileError *)realloc(state->errors, nc * sizeof(CompileError));
        if (!ne) return;
        state->errors = ne;
        state->errors_cap = nc;
    }
    state->errors[state->count].line = line;
    state->errors[state->count].col = col;
    state->errors[state->count].message = strdup(msg ? msg : "");
    state->count++;
}

void error_state_print(ErrorState *state) {
    if (!state || state->count == 0) return;

    fprintf(stderr, "\n");

    for (int i = 0; i < state->count; i++) {
        CompileError *err = &state->errors[i];
        int line_idx = err->line - 1;

        fprintf(stderr, " --> %s:%d:%d\n", state->filename ? state->filename : "", err->line, err->col);
        fprintf(stderr, "   |\n");

        if (state->source_lines && line_idx >= 0 && line_idx < state->total_lines) {
            fprintf(stderr, " %d | %s\n", err->line, state->source_lines[line_idx]);
            fprintf(stderr, "   | ");
            for (int j = 0; j < err->col - 1; j++) {
                fputc(' ', stderr);
            }
            fprintf(stderr, "^\n");
        }

        fprintf(stderr, "   = error: %s\n", err->message ? err->message : "");
        if (i < state->count - 1) {
            fprintf(stderr, "\n");
        }
    }

    fprintf(stderr, "\n");
    if (state->check_only) {
        fprintf(stderr, "Check failed: %d error(s)\n", state->count);
    } else {
        fprintf(stderr, "Compilation failed: %d error(s)\n", state->count);
    }
}

int error_state_has_errors(ErrorState *state) {
    return state && state->count > 0;
}
