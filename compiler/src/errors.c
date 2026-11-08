#include "ltng.h"

void error_state_init(ErrorState *state, char *source, const char *filename, int check_only) {
    state->count = 0;
    state->filename = strdup(filename);
    state->check_only = check_only;

    char *copy = strdup(source);
    int line_count = 0;
    char *ptr = copy;

    state->source_lines[0] = ptr;
    line_count = 1;

    while (*ptr && line_count < MAX_SOURCE_LINES) {
        if (*ptr == '\n') {
            *ptr = '\0';
            ptr++;
            if (*ptr) {
                state->source_lines[line_count++] = ptr;
            }
        } else {
            ptr++;
        }
    }

    state->total_lines = line_count;
}

void error_state_add(ErrorState *state, int line, int col, const char *msg) {
    if (state->count >= MAX_ERRORS) return;
    state->errors[state->count].line = line;
    state->errors[state->count].col = col;
    strncpy(state->errors[state->count].message, msg, MAX_STRING_LEN - 1);
    state->errors[state->count].message[MAX_STRING_LEN - 1] = '\0';
    state->count++;
}

void error_state_print(ErrorState *state) {
    if (state->count == 0) return;

    fprintf(stderr, "\n");

    for (int i = 0; i < state->count; i++) {
        CompileError *err = &state->errors[i];
        int line_idx = err->line - 1;

        fprintf(stderr, " --> %s:%d:%d\n", state->filename, err->line, err->col);
        fprintf(stderr, "   |\n");

        if (line_idx >= 0 && line_idx < state->total_lines) {
            fprintf(stderr, " %d | %s\n", err->line, state->source_lines[line_idx]);
            fprintf(stderr, "   | ");
            for (int j = 0; j < err->col - 1; j++) {
                fputc(' ', stderr);
            }
            fprintf(stderr, "^\n");
        }

        fprintf(stderr, "   = error: %s\n", err->message);
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
    return state->count > 0;
}
