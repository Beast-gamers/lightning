#include "ltng.h"
#include <unistd.h>
#include <sys/stat.h>
#include <libgen.h>
#include <errno.h>
#include <fcntl.h>

static int mkdir_p(const char *path) {
    char tmp[4096];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(tmp)) return -1;
    memcpy(tmp, path, len + 1);
    if (tmp[len - 1] == '/') tmp[len - 1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

static int copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) return -1;
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); return -1; }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) { fclose(in); fclose(out); return -1; }
    }
    fclose(in);
    fclose(out);
    return 0;
}

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

static void run_cmd_capture(const char *cmd, const char *label) {
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        fprintf(stderr, "%s: failed to run command\n", label);
        return;
    }
    char buf[4096];
    size_t n;
    int any = 0;
    while ((n = fread(buf, 1, sizeof(buf) - 1, fp)) > 0) {
        buf[n] = '\0';
        for (size_t i = 0; i < n; i++) if (buf[i] == '\r') buf[i] = ' ';
        fprintf(stderr, "%s", buf);
        any = 1;
    }
    int status = pclose(fp);
    if (status != 0) {
        fprintf(stderr, "%s exited with status %d\n", label, status);
    } else if (!any) {
        fprintf(stderr, "%s: ok\n", label);
    }
}

int main(int argc, char *argv[]) {
    int debug = 0;
    int check_only = 0;
    int emit_c = 0;
    char *cc_cmd = "cc";
    int file_arg = 0;
    int out_arg = 0;
    int positional = 0;
    int orig_stdout_fd = -1, orig_stderr_fd = -1, log_fd = -1;

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

    char *base = get_basename(argv[file_arg]);
    char *dot = strrchr(base, '.');
    if (dot) *dot = '\0';

    char *src_copy = strdup(argv[file_arg]);
    char *src_dir = dirname(src_copy);

    char debug_dir[4096] = "";
    char debug_ir[4096] = "";
    char debug_c[4096] = "";
    char debug_src[4096] = "";
    char debug_log[4096] = "";
    if (debug) {
        snprintf(debug_dir, sizeof(debug_dir), "%s/debug/%s", src_dir, base);
        mkdir_p(debug_dir);
        snprintf(debug_src, sizeof(debug_src), "%s/source.ltng", debug_dir);
        snprintf(debug_ir, sizeof(debug_ir), "%s/intermediete.ltng_ir", debug_dir);
        snprintf(debug_c, sizeof(debug_c), "%s/emitted.c", debug_dir);
        snprintf(debug_log, sizeof(debug_log), "%s/logs.txt", debug_dir);
        copy_file(argv[file_arg], debug_src);
        log_fd = open(debug_log, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (log_fd >= 0) {
            orig_stdout_fd = dup(STDOUT_FILENO);
            orig_stderr_fd = dup(STDERR_FILENO);
            dup2(log_fd, STDOUT_FILENO);
            dup2(log_fd, STDERR_FILENO);
            setvbuf(stdout, NULL, _IONBF, 0);
            setvbuf(stderr, NULL, _IONBF, 0);
        }
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
        free(src_copy);
        free(base);
        if (log_fd >= 0) { fflush(stdout); fflush(stderr); if (orig_stdout_fd>=0) dup2(orig_stdout_fd,STDOUT_FILENO); if (orig_stderr_fd>=0) dup2(orig_stderr_fd,STDERR_FILENO); close(log_fd); if(orig_stdout_fd>=0)close(orig_stdout_fd); if(orig_stderr_fd>=0)close(orig_stderr_fd); }
        return 1;
    }

    char ir_path[4096];
    snprintf(ir_path, sizeof(ir_path), "%s/%s.ltng_ir", src_dir, base);

    FILE *ir_out = fopen(ir_path, "w");
    if (!ir_out) {
        fprintf(stderr, "Error: Cannot create IR file '%s'\n", ir_path);
        free(src_copy);
        free(source);
        codegen_free(program);
        free(base);
        if (log_fd >= 0) { fflush(stdout); fflush(stderr); if (orig_stdout_fd>=0) dup2(orig_stdout_fd,STDOUT_FILENO); if (orig_stderr_fd>=0) dup2(orig_stderr_fd,STDERR_FILENO); close(log_fd); if(orig_stdout_fd>=0)close(orig_stdout_fd); if(orig_stderr_fd>=0)close(orig_stderr_fd); }
        return 1;
    }
    ir_write(program, ir_out);
    fclose(ir_out);
    if (debug && debug_ir[0]) copy_file(ir_path, debug_ir);

    if (check_only) {
        printf("No errors found in '%s'\n", argv[file_arg]);
        if (!debug) remove(ir_path);
        free(src_copy);
        free(source);
        codegen_free(program);
        free(base);
        if (log_fd >= 0) { fflush(stdout); fflush(stderr); if (orig_stdout_fd>=0) dup2(orig_stdout_fd,STDOUT_FILENO); if (orig_stderr_fd>=0) dup2(orig_stderr_fd,STDERR_FILENO); close(log_fd); if(orig_stdout_fd>=0)close(orig_stdout_fd); if(orig_stderr_fd>=0)close(orig_stderr_fd); }
        return 0;
    }

    char tmp_c[512];
    snprintf(tmp_c, sizeof(tmp_c), "/tmp/ltng_%d.c", getpid());
    FILE *out = fopen(tmp_c, "w");
    if (!out) {
        fprintf(stderr, "Error: Cannot create temporary C file\n");
        if (!debug) remove(ir_path);
        free(src_copy);
        free(source);
        codegen_free(program);
        free(base);
        if (log_fd >= 0) { fflush(stdout); fflush(stderr); if (orig_stdout_fd>=0) dup2(orig_stdout_fd,STDOUT_FILENO); if (orig_stderr_fd>=0) dup2(orig_stderr_fd,STDERR_FILENO); close(log_fd); if(orig_stdout_fd>=0)close(orig_stdout_fd); if(orig_stderr_fd>=0)close(orig_stderr_fd); }
        return 1;
    }

    codegen(program, &program->symbol_table, out, &errors);
    fclose(out);
    if (debug && debug_c[0]) copy_file(tmp_c, debug_c);

    if (error_state_has_errors(&errors)) {
        error_state_print(&errors);
        remove(tmp_c);
        if (!debug) remove(ir_path);
        free(src_copy);
        free(source);
        codegen_free(program);
        free(base);
        if (log_fd >= 0) { fflush(stdout); fflush(stderr); if (orig_stdout_fd>=0) dup2(orig_stdout_fd,STDOUT_FILENO); if (orig_stderr_fd>=0) dup2(orig_stderr_fd,STDERR_FILENO); close(log_fd); if(orig_stdout_fd>=0)close(orig_stdout_fd); if(orig_stderr_fd>=0)close(orig_stderr_fd); }
        return 1;
    }

    char *output_name;
    if (out_arg > 0) {
        output_name = strdup(argv[out_arg]);
    } else {
        /* Create compiled folder in the directory containing the source file */
        char compiled_dir[4096];
        snprintf(compiled_dir, sizeof(compiled_dir), "%s/compiled", src_dir);
        mkdir(compiled_dir, 0755);
        output_name = malloc(4096);
        if (emit_c) {
            snprintf(output_name, 4096, "%s/%s.c", compiled_dir, base);
        } else {
            snprintf(output_name, 4096, "%s/%s", compiled_dir, base);
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
            if (!debug) remove(ir_path);
            free(src_copy);
            free(source);
            codegen_free(program);
            free(base);
            free(output_name);
            if (log_fd >= 0) { fflush(stdout); fflush(stderr); if (orig_stdout_fd>=0) dup2(orig_stdout_fd,STDOUT_FILENO); if (orig_stderr_fd>=0) dup2(orig_stderr_fd,STDERR_FILENO); close(log_fd); if(orig_stdout_fd>=0)close(orig_stdout_fd); if(orig_stderr_fd>=0)close(orig_stderr_fd); }
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
        if (!debug) remove(ir_path);
        if (debug) {
            if (debug_c[0]) copy_file(output_name, debug_c);
            printf("Emitted C '%s' -> '%s'\n", argv[file_arg], output_name);
        }
        free(src_copy);
        free(source);
        codegen_free(program);
        free(base);
        free(output_name);
        if (log_fd >= 0) { fflush(stdout); fflush(stderr); if (orig_stdout_fd>=0) dup2(orig_stdout_fd,STDOUT_FILENO); if (orig_stderr_fd>=0) dup2(orig_stderr_fd,STDERR_FILENO); close(log_fd); if(orig_stdout_fd>=0)close(orig_stdout_fd); if(orig_stderr_fd>=0)close(orig_stderr_fd); }
        return 0;
    }

    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "%s -w -static -o \"%s\" \"%s\" 2>&1", cc_cmd, output_name, tmp_c);

    /* Run compiler and capture output */
    FILE *cfp = popen(cmd, "r");
    if (!cfp) {
        fprintf(stderr, "Error: Cannot run C compiler\n");
        remove(tmp_c);
        if (!debug) remove(ir_path);
        free(src_copy);
        free(source);
        codegen_free(program);
        free(base);
        free(output_name);
        if (log_fd >= 0) { fflush(stdout); fflush(stderr); if (orig_stdout_fd>=0) dup2(orig_stdout_fd,STDOUT_FILENO); if (orig_stderr_fd>=0) dup2(orig_stderr_fd,STDERR_FILENO); close(log_fd); if(orig_stdout_fd>=0)close(orig_stdout_fd); if(orig_stderr_fd>=0)close(orig_stderr_fd); }
        return 1;
    }

    /* Read compiler output */
    char compiler_err[4096] = "";
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf) - 1, cfp)) > 0) {
        buf[n] = '\0';
        for (size_t i = 0; i < n; i++) {
            if (buf[i] == '\r') buf[i] = ' ';
        }
        strncat(compiler_err, buf, sizeof(compiler_err) - strlen(compiler_err) - 1);
    }
    int compiler_exit = pclose(cfp);

    if (debug) {
        fprintf(stderr, "C compiler command: %s\n", cmd);
        if (strlen(compiler_err) > 0) {
            fprintf(stderr, "C compiler output:\n%s\n", compiler_err);
        }
        if (compiler_exit == 0) {
            printf("Compiled '%s' -> '%s'\n", argv[file_arg], output_name);
            /* generate debug artifacts: binary, object, assembly */
            char dbg_bin[4096], dbg_obj[4096], dbg_asm[4096], cmd2[8192];
            snprintf(dbg_bin, sizeof(dbg_bin), "%s/%s", debug_dir, base);
            snprintf(dbg_obj, sizeof(dbg_obj), "%s/%s.o", debug_dir, base);
            snprintf(dbg_asm, sizeof(dbg_asm), "%s/%s.s", debug_dir, base);
            copy_file(output_name, dbg_bin);
            chmod(dbg_bin, 0755);
            snprintf(cmd2, sizeof(cmd2), "%s -w -c -o \"%s\" \"%s\" 2>&1", cc_cmd, dbg_obj, tmp_c);
            run_cmd_capture(cmd2, "C compiler (object)");
            snprintf(cmd2, sizeof(cmd2), "%s -w -S -o \"%s\" \"%s\" 2>&1", cc_cmd, dbg_asm, tmp_c);
            run_cmd_capture(cmd2, "C compiler (assembly)");
        }
    } else {
        /* Hide C compiler errors unless --debug */
        if (compiler_exit != 0 && strlen(compiler_err) > 0) {
            fprintf(stderr, "Error: internal error occured\n");
        } else if (compiler_exit != 0) {
            fprintf(stderr, "Error: C compilation failed\n");
        }
    }

    remove(tmp_c);

    if (compiler_exit == 0 && !debug) {
        remove(ir_path);
    }

    int rc = (compiler_exit == 0) ? 0 : 1;

    free(src_copy);
    free(source);
    codegen_free(program);
    free(base);
    free(output_name);

    if (log_fd >= 0) { fflush(stdout); fflush(stderr); if (orig_stdout_fd>=0) dup2(orig_stdout_fd,STDOUT_FILENO); if (orig_stderr_fd>=0) dup2(orig_stderr_fd,STDERR_FILENO); close(log_fd); if(orig_stdout_fd>=0)close(orig_stdout_fd); if(orig_stderr_fd>=0)close(orig_stderr_fd); }
    return rc;
}
