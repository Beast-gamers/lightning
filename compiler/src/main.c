#include "ltng.h"
#include <unistd.h>
#include <sys/stat.h>
#include <libgen.h>

static char *read_file(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "Error: Cannot open file '%s'\n", path);
        exit(1);
    }

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    rewind(file);

    char *buffer = (char *)malloc(size + 1);
    if (!buffer) {
        fprintf(stderr, "Error: Memory allocation failed\n");
        fclose(file);
        exit(1);
    }

    size_t read_size = fread(buffer, 1, size, file);
    buffer[read_size] = '\0';
    fclose(file);
    return buffer;
}

static char *get_basename(const char *path) {
    const char *slash = strrchr(path, '/');
    if (!slash) slash = strrchr(path, '\\');
    if (slash) return strdup(slash + 1);
    return strdup(path);
}

int main(int argc, char *argv[]) {
    int debug = 0;
    int check_only = 0;
    int emit_c = 0;
    char *cc_cmd = "cc";
    int file_arg = 0;
    int out_arg = 0;
    int positional = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--debug") == 0) {
            debug = 1;
        } else if (strcmp(argv[i], "--check") == 0) {
            check_only = 1;
        } else if (strcmp(argv[i], "--emit-c") == 0) {
            emit_c = 1;
        } else if (strcmp(argv[i], "--cc") == 0) {
            if (i + 1 < argc) {
                cc_cmd = argv[++i];
            }
        } else {
            positional++;
            if (positional == 1) file_arg = i;
            else if (positional == 2) out_arg = i;
        }
    }

    if (file_arg == 0) {
        fprintf(stderr, "Usage: ltng [--debug] [--check] [--emit-c] [--cc <compiler>] <file.ltng> [output]\n");
        fprintf(stderr, "  Compiles a .ltng file to a native executable.\n");
        fprintf(stderr, "  --check     Only check for errors, don't compile.\n");
        return 1;
    }

    char *source = read_file(argv[file_arg]);

    ErrorState errors;
    error_state_init(&errors, source, argv[file_arg], check_only);

    Lexer lexer;
    lexer_init(&lexer, source);
    lexer_tokenize(&lexer, &errors);

    Program *program = parse(&lexer, &errors);

    if (error_state_has_errors(&errors)) {
        error_state_print(&errors);
        free(source);
        codegen_free(program);
        return 1;
    }

    if (check_only) {
        printf("No errors found in '%s'\n", argv[file_arg]);
        free(source);
        codegen_free(program);
        return 0;
    }

    char *base = get_basename(argv[file_arg]);
    char *dot = strrchr(base, '.');
    if (dot) *dot = '\0';

    char tmp_c[512];
    snprintf(tmp_c, sizeof(tmp_c), "/tmp/ltng_%d.c", getpid());
    FILE *out = fopen(tmp_c, "w");
    if (!out) {
        fprintf(stderr, "Error: Cannot create temporary C file\n");
        free(source);
        codegen_free(program);
        free(base);
        return 1;
    }

    codegen(program, &program->symbol_table, out, &errors);
    fclose(out);

    if (error_state_has_errors(&errors)) {
        error_state_print(&errors);
        free(source);
        codegen_free(program);
        return 1;
    }

    char *output_name;
    if (out_arg > 0) {
        output_name = strdup(argv[out_arg]);
    } else {
        /* Create compiled folder in the directory containing the source file */
        char *src_copy = strdup(argv[file_arg]);
        char *src_dir = dirname(src_copy);
        char compiled_dir[1024];
        snprintf(compiled_dir, sizeof(compiled_dir), "%s/compiled", src_dir);
        mkdir(compiled_dir, 0755);
        free(src_copy);
        output_name = malloc(1024);
        if (emit_c) {
            snprintf(output_name, 1024, "%s/%s.c", compiled_dir, base);
        } else {
            snprintf(output_name, 1024, "%s/%s", compiled_dir, base);
        }
    }

    if (emit_c) {
        /* Copy C to output */
        FILE *cin = fopen(tmp_c, "r");
        FILE *cout = fopen(output_name, "w");
        if (!cin || !cout) {
            fprintf(stderr, "Error: Cannot emit C file\n");
            if (cin) fclose(cin);
            if (cout) fclose(cout);
            remove(tmp_c);
            free(source);
            codegen_free(program);
            free(base);
            free(output_name);
            return 1;
        }
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), cin)) > 0) {
            fwrite(buf, 1, n, cout);
        }
        fclose(cin);
        fclose(cout);
        remove(tmp_c);
        if (debug) {
            printf("Emitted C '%s' -> '%s'\n", argv[file_arg], output_name);
        }
        free(source);
        codegen_free(program);
        free(base);
        free(output_name);
        return 0;
    }

    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "%s -w -static -o \"%s\" \"%s\" 2>&1", cc_cmd, output_name, tmp_c);

    /* Run compiler and capture output */
    FILE *cfp = popen(cmd, "r");
    if (!cfp) {
        fprintf(stderr, "Error: Cannot run C compiler\n");
        remove(tmp_c);
        free(source);
        codegen_free(program);
        free(base);
        free(output_name);
        return 1;
    }

    /* Read compiler output */
    char compiler_err[4096] = "";
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), cfp)) > 0) {
        /* Strip carriage returns */
        for (size_t i = 0; i < n; i++) {
            if (buf[i] == '\r') memmove(&buf[i], &buf[i+1], n - i - 1);
        }
        strncat(compiler_err, buf, sizeof(compiler_err) - strlen(compiler_err) - 1);
    }
    int compiler_exit = pclose(cfp);

    remove(tmp_c);

    if (debug) {
        /* Show actual compiler output when --debug is used */
        if (strlen(compiler_err) > 0) {
            fprintf(stderr, "C compiler output:\n%s\n", compiler_err);
        }
        printf("Compiled '%s' -> '%s'\n", argv[file_arg], output_name);
    } else {
        /* Hide C compiler errors unless --debug */
        if (compiler_exit != 0 && strlen(compiler_err) > 0) {
            fprintf(stderr, "Error: internal error occured\n");
        } else if (compiler_exit != 0) {
            fprintf(stderr, "Error: C compilation failed\n");
        } else {
            printf("Compiled '%s' -> '%s'\n", argv[file_arg], output_name);
        }
    }

    free(source);
    codegen_free(program);
    free(base);
    free(output_name);

    return 0;
}
