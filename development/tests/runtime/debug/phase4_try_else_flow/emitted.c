#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <ctype.h>
#include <setjmp.h>
static const char *_ltng_src_file = "tests/runtime/phase4_try_else_flow.ltng";
typedef struct LtngTryFrame {
    jmp_buf env;
    struct LtngTryFrame *prev;
    int stack_depth;
} LtngTryFrame;
static LtngTryFrame *_ltng_try_top = NULL;
#define LTNG_FN_MAX 256
static const char *_ltng_fn_name[LTNG_FN_MAX];
static int _ltng_fn_line[LTNG_FN_MAX];
static int _ltng_fn_col[LTNG_FN_MAX];
static int _ltng_stack_depth = 0;
static char _ltng_exc_type[128] = "";
static char _ltng_exc_msg[1024] = "";
static char _ltng_exc_value[2048] = "";
static char _ltng_exc_trace[8192] = "";
static int _ltng_exc_line = 0;
static int _ltng_exc_col = 0;
static char _ltng_exc_func[256] = "";
typedef struct {
    char type[128];
    char msg[1024];
    char value[2048];
    char trace[8192];
    int line;
    int col;
    char func[256];
} LtngExcSaved;
static void _ltng_fn_push(const char *name, int line, int col) {
    if (_ltng_stack_depth < LTNG_FN_MAX) {
        _ltng_fn_name[_ltng_stack_depth] = name ? name : "?";
        _ltng_fn_line[_ltng_stack_depth] = line;
        _ltng_fn_col[_ltng_stack_depth] = col;
        _ltng_stack_depth++;
    }
}
static void _ltng_fn_pop(void) { if (_ltng_stack_depth > 0) _ltng_stack_depth--; }
static void _ltng_capture_trace(int line, int col) {
    char ln[512];
    _ltng_exc_line = line;
    _ltng_exc_col = col;
    _ltng_exc_func[0] = 0;
    _ltng_exc_trace[0] = 0;
    if (_ltng_stack_depth > 0) {
        snprintf(_ltng_exc_func, sizeof(_ltng_exc_func), "%s", _ltng_fn_name[_ltng_stack_depth - 1]);
        snprintf(ln, sizeof(ln), "  at %s (%s:%d:%d)\n", _ltng_fn_name[_ltng_stack_depth - 1], _ltng_src_file, line, col);
        strncat(_ltng_exc_trace, ln, sizeof(_ltng_exc_trace) - strlen(_ltng_exc_trace) - 1);
        for (int i = _ltng_stack_depth - 2; i >= 0; i--) {
            snprintf(ln, sizeof(ln), "  at %s (%s:%d:%d)\n", _ltng_fn_name[i], _ltng_src_file, _ltng_fn_line[i + 1], _ltng_fn_col[i + 1]);
            strncat(_ltng_exc_trace, ln, sizeof(_ltng_exc_trace) - strlen(_ltng_exc_trace) - 1);
        }
    } else {
        snprintf(ln, sizeof(ln), "  at <top-level> (%s:%d:%d)\n", _ltng_src_file, line, col);
        strncat(_ltng_exc_trace, ln, sizeof(_ltng_exc_trace) - strlen(_ltng_exc_trace) - 1);
    }
}
static void _ltng_exc_save_to(LtngExcSaved *s) {
    snprintf(s->type, sizeof(s->type), "%s", _ltng_exc_type);
    snprintf(s->msg, sizeof(s->msg), "%s", _ltng_exc_msg);
    snprintf(s->value, sizeof(s->value), "%s", _ltng_exc_value);
    snprintf(s->trace, sizeof(s->trace), "%s", _ltng_exc_trace);
    s->line = _ltng_exc_line; s->col = _ltng_exc_col;
    snprintf(s->func, sizeof(s->func), "%s", _ltng_exc_func);
}
static void _ltng_exc_restore_from(const LtngExcSaved *s) {
    snprintf(_ltng_exc_type, sizeof(_ltng_exc_type), "%s", s->type);
    snprintf(_ltng_exc_msg, sizeof(_ltng_exc_msg), "%s", s->msg);
    snprintf(_ltng_exc_value, sizeof(_ltng_exc_value), "%s", s->value);
    snprintf(_ltng_exc_trace, sizeof(_ltng_exc_trace), "%s", s->trace);
    _ltng_exc_line = s->line; _ltng_exc_col = s->col;
    snprintf(_ltng_exc_func, sizeof(_ltng_exc_func), "%s", s->func);
}
static void _ltng_uncaught(void) {
    if (_ltng_exc_msg[0]) fprintf(stderr, "%s: %s\n", _ltng_exc_type[0] ? _ltng_exc_type : "Error", _ltng_exc_msg);
    else fprintf(stderr, "%s\n", _ltng_exc_type[0] ? _ltng_exc_type : "Error");
    if (_ltng_exc_trace[0]) fputs(_ltng_exc_trace, stderr);
    fflush(stderr);
    exit(1);
}
static void _ltng_throw_value(const char *type, const char *msg, int line, int col) {
    snprintf(_ltng_exc_type, sizeof(_ltng_exc_type), "%s", (type && type[0]) ? type : "Error");
    snprintf(_ltng_exc_msg, sizeof(_ltng_exc_msg), "%s", msg ? msg : "");
    snprintf(_ltng_exc_value, sizeof(_ltng_exc_value), "%s", msg ? msg : "");
    _ltng_capture_trace(line, col);
    if (!_ltng_try_top) _ltng_uncaught();
    longjmp(_ltng_try_top->env, 1);
}
static void _ltng_rethrow(void) {
    if (!_ltng_try_top) _ltng_uncaught();
    longjmp(_ltng_try_top->env, 1);
}

static char *_ltng_concat(char *buf, size_t sz, int n, ...) { va_list ap; va_start(ap,n); buf[0]=0; for(int i=0;i<n;i++){ const char *s=va_arg(ap,const char*); strncat(buf,s,sz-strlen(buf)-1); } va_end(ap); return buf; }
static char *_ltng_repeat(char *buf, size_t sz, const char *s, long long n) { buf[0]=0; if (n < 0) n = 0; for (long long i=0; i<n && strlen(buf) < sz-1; i++) { strncat(buf, s, sz-strlen(buf)-1); } return buf; }
static char *_ltng_arr_strdup(const char *s){ if(!s) s=""; size_t n=strlen(s)+1; char *p=malloc(n); if(p) memcpy(p,s,n); return p; }
static char **_ltng_arr_new(int cap){ if(cap<1)cap=1; char **d=malloc(sizeof(char*)*cap); for(int i=0;i<cap;i++) d[i]=NULL; return d; }
static void _ltng_arr_ensure(char ***d,int *len,int *cap,int need){ if(*cap>=need) return; int nc=*cap?*cap*2:1; while(nc<need) nc*=2; *d=realloc(*d,sizeof(char*)*nc); for(int i=*cap;i<nc;i++) (*d)[i]=NULL; *cap=nc; }
static void _ltng_arr_set_elem(char **d,int idx,const char *v){ if(d[idx]) free(d[idx]); d[idx]=v?_ltng_arr_strdup(v):_ltng_arr_strdup(""); }
static const char *_ltng_arr_get(char **d,int len,int idx){ if(idx>=0&&idx<len) return d[idx]?d[idx]:""; if(idx<0&&len+idx>=0) return d[len+idx]?d[len+idx]:""; return ""; }
static void _ltng_arr_set(char **d,int *len,int *cap,int idx,const char *v){ (void)cap; if(idx<0||idx>=*len) return; _ltng_arr_set_elem(d,idx,v); }
static void _ltng_arr_push(char ***d,int *len,int *cap,const char *v){ _ltng_arr_ensure(d,len,cap,*len+1); _ltng_arr_set_elem(*d,*len,v); (*len)++; }
static char *_ltng_arr_pop(char **d,int *len){ static char b[256]; if(*len<=0){ b[0]=0; return b; } (*len)--; const char *v=d[*len]?d[*len]:""; snprintf(b,256,"%s",v); free(d[*len]); d[*len]=NULL; return b; }
static char *_ltng_arr_pop_index(char **d,int *len,int idx){ static char b[256]; /* idx is 1-based positive, negative from end, 0 invalid */ if(idx>0) idx=idx-1; else if(idx<0) idx=*len+idx; if(idx<0||idx>=*len){ b[0]=0; return b; } const char *v=d[idx]?d[idx]:""; snprintf(b,256,"%s",v); free(d[idx]); for(int i=idx;i<*len-1;i++) d[i]=d[i+1]; d[*len-1]=NULL; (*len)--; return b; }
static void _ltng_arr_insert(char ***d,int *len,int *cap,int idx,const char *v){ /* idx is 1-based positive, negative from end */ if(idx>0) idx=idx-1; else if(idx<0) idx=*len+idx; if(idx<0) idx=0; if(idx>*len) idx=*len; _ltng_arr_ensure(d,len,cap,*len+1); for(int i=*len;i>idx;i--) (*d)[i]=(*d)[i-1]; _ltng_arr_set_elem(*d,idx,v); (*len)++; }
static void _ltng_arr_remove(char **d,int *len,int idx){ /* idx is 1-based positive, negative from end, 0 invalid */ if(idx>0) idx=idx-1; else if(idx<0) idx=*len+idx; if(idx<0||idx>=*len) return; free(d[idx]); for(int i=idx;i<*len-1;i++) d[i]=d[i+1]; d[*len-1]=NULL; (*len)--; }
static int _ltng_arr_contains(char **d,int len,const char *v){ for(int i=0;i<len;i++){ if(d[i]&&strcmp(d[i],v)==0) return 1; } return 0; }
static int _ltng_arr_index_of(char **d,int len,const char *v){ for(int i=0;i<len;i++){ if(d[i]&&strcmp(d[i],v)==0) return i+1; } return 0; }
static void _ltng_arr_reverse(char **d,int len){ for(int i=0,j=len-1;i<j;i++,j--){ char *t=d[i]; d[i]=d[j]; d[j]=t; } }
static int _ltng_arr_numcmp(const char *a,const char *b){ char *ea=NULL,*eb=NULL; double da=strtod(a?a:"",&ea); double db=strtod(b?b:"",&eb); int na=(ea&&*ea==0&&a&&*a); int nb=(eb&&*eb==0&&b&&*b); if(na&&nb){ if(da<db) return -1; if(da>db) return 1; return 0; } return strcmp(a?a:"", b?b:""); }
static int _ltng_arr_cmp(const void *a,const void *b){ return _ltng_arr_numcmp(*(const char * const *)a, *(const char * const *)b); }
static void _ltng_arr_sort(char **d,int len){ qsort(d,len,sizeof(char *),_ltng_arr_cmp); }
static void _ltng_arr_copy(char **sd,char ***dd,int *dl,int *dc,int sl){ _ltng_arr_ensure(dd,dl,dc,sl); for(int i=sl;i<*dl;i++){ if((*dd)[i]){ free((*dd)[i]); (*dd)[i]=NULL; } } for(int i=0;i<sl;i++){ if((*dd)[i]) free((*dd)[i]); (*dd)[i]=sd[i]?_ltng_arr_strdup(sd[i]):_ltng_arr_strdup(""); } *dl=sl; }
static void _ltng_arr_slice(char **sd,int sl,int start,int end,char ***dd,int *dl,int *dc){ if(start>0) start=start-1; else if(start<0) start=sl+start; if(end>0) end=end-1; else if(end<0) end=sl+end; if(start<0) start=0; if(end<0) end=0; if(end>sl) end=sl; if(start>sl) start=sl; if(start>end) start=end; int n=end-start; _ltng_arr_ensure(dd,dl,dc,n); for(int i=0;i<*dl;i++){ if((*dd)[i]){ free((*dd)[i]); (*dd)[i]=NULL; } } *dl=0; for(int i=0;i<n;i++){ _ltng_arr_push(dd,dl,dc,sd[start+i]?sd[start+i]:""); } }
static void _ltng_arr_free(char **d,int len){ if(!d) return; for(int i=0;i<len;i++){ if(d[i]) free(d[i]); } free(d); }
static int _ltng_idx2(int i0, int i1, int d1) { return i0 * d1 + i1; }
static int _ltng_idx3(int i0, int i1, int i2, int d1, int d2) { return (i0 * d1 + i1) * d2 + i2; }
static char *_ltng_retbuf(void){ static char b[32][256]; static int i; i=(i+1)%32; b[i][0]=0; return b[i]; }
static char *_ltng_ad(double v){ char *b=_ltng_retbuf(); snprintf(b,256,"%g",v); return b; }
static char *_ltng_as(const char *s){ char *b=_ltng_retbuf(); if(s){ strncpy(b,s,255); b[255]=0; } return b; }
static char *_ltng_from_int(char *buf, long long v){ snprintf(buf,256,"%lld",v); return buf; }
static char *_ltng_from_double(char *buf, double v){ snprintf(buf,256,"%g",v); return buf; }
static int _ltng_is_num_str(const char *s){ if(!s||!*s) return 0; const char *p=s; if(*p=='-'||*p=='+') p++; int h=0; for(;*p;p++){ if(isdigit(*p)) h=1; else if(*p=='.'){} else return 0; } return h; }
static char *_ltng_add_sn(const char *a, const char *b){ static char buf[32][256]; static int _ltng_asn_idx=0; _ltng_asn_idx=(_ltng_asn_idx+1)%32; char *r=buf[_ltng_asn_idx]; r[0]=0; if(_ltng_is_num_str(a)&&_ltng_is_num_str(b)){ double na=atof(a),nb=atof(b); snprintf(r,256,"%g",na+nb); } else { snprintf(r,256,"%s%s",a,b); } return r; }
static char *_ltng_ptr_to_str(void *p){ static char b[32][32]; static int i; i=(i+1)%32; snprintf(b[i], sizeof(b[i]), "%llu", (unsigned long long)(uintptr_t)p); return b[i]; }
static void *_ltng_str_to_ptr(const char *s){ if(!s||!*s) return NULL; return (void*)(uintptr_t)strtoull(s, NULL, 10); }


static char *__ltng_call_indirect(const char *target, char *_ret, int _ltng_cl, int _ltng_cc, int argc, ...);

static char *ret_try(char *_ret, int _ltng_cl, int _ltng_cc);
static char *ret_else(char *_ret, int _ltng_cl, int _ltng_cc);

static char *ret_try(char *_ret, int _ltng_cl, int _ltng_cc) {
    if(!_ret) _ret=_ltng_retbuf(); _ret[0]=0;
    _ltng_fn_push("ret_try", _ltng_cl, _ltng_cc);
    {
        LtngTryFrame _ltng_tf0;
        _ltng_tf0.prev = _ltng_try_top;
        _ltng_tf0.stack_depth = _ltng_stack_depth;
        _ltng_try_top = &_ltng_tf0;
        int _ltng_tr0 = setjmp(_ltng_tf0.env);
        if (_ltng_tr0 == 0) {
            printf("%s\n", "try");
            {
                char _ltng_rv[256] = "";
                snprintf(_ltng_rv, 256, "%s", "from-try");
                _ltng_try_top = _ltng_tf0.prev;
                printf("%s\n", "finally-ret-try");
                _ltng_fn_pop();
                snprintf(_ret, 256, "%s", _ltng_rv); return _ret;
            }
            _ltng_try_top = _ltng_tf0.prev;
            {
                LtngTryFrame _ltng_ef0;
                _ltng_ef0.prev = _ltng_try_top;
                _ltng_ef0.stack_depth = _ltng_stack_depth;
                _ltng_try_top = &_ltng_ef0;
                int _ltng_er0 = setjmp(_ltng_ef0.env);
                if (_ltng_er0 == 0) {
                    printf("%s\n", "else-should-not-run");
                    _ltng_try_top = _ltng_ef0.prev;
                    printf("%s\n", "finally-ret-try");
                } else {
                    _ltng_try_top = _ltng_ef0.prev;
                    _ltng_stack_depth = _ltng_ef0.stack_depth;
                    LtngExcSaved _ltng_ee0;
                    _ltng_exc_save_to(&_ltng_ee0);
                    printf("%s\n", "finally-ret-try");
                    _ltng_exc_restore_from(&_ltng_ee0);
                    _ltng_rethrow();
                }
            }
        } else {
            _ltng_try_top = _ltng_tf0.prev;
            _ltng_stack_depth = _ltng_tf0.stack_depth;
            char _v0_e[256] = "";
            snprintf(_v0_e, 256, "%s: %s", _ltng_exc_type, _ltng_exc_msg);
            char _v0_e__type[256] = "";
            snprintf(_v0_e__type, 256, "%s", _ltng_exc_type);
            char _v0_e__message[256] = "";
            snprintf(_v0_e__message, 256, "%s", _ltng_exc_msg);
            char _v0_e__trace[4096] = "";
            snprintf(_v0_e__trace, 4096, "%s", _ltng_exc_trace);
            char _v0_e__line[256] = "";
            snprintf(_v0_e__line, 256, "%d", _ltng_exc_line);
            char _v0_e__column[256] = "";
            snprintf(_v0_e__column, 256, "%d", _ltng_exc_col);
            char _v0_e__function[256] = "";
            snprintf(_v0_e__function, 256, "%s", _ltng_exc_func);
            char _v0_e__value[256] = "";
            snprintf(_v0_e__value, 256, "%s", _ltng_exc_value);
            char _v0_e__obj[512] = "";
            snprintf(_v0_e__obj, 512, "%s", _v0_e);
            LtngTryFrame _ltng_cf0;
            _ltng_cf0.prev = _ltng_try_top;
            _ltng_cf0.stack_depth = _ltng_stack_depth;
            _ltng_try_top = &_ltng_cf0;
            int _ltng_cr0 = setjmp(_ltng_cf0.env);
            if (_ltng_cr0 == 0) {
                {
                    char _ltng_rv[256] = "";
                    snprintf(_ltng_rv, 256, "%s", "catch");
                    _ltng_try_top = _ltng_cf0.prev;
                    printf("%s\n", "finally-ret-try");
                    _ltng_fn_pop();
                    snprintf(_ret, 256, "%s", _ltng_rv); return _ret;
                }
                _ltng_try_top = _ltng_cf0.prev;
                printf("%s\n", "finally-ret-try");
            } else {
                _ltng_try_top = _ltng_cf0.prev;
                _ltng_stack_depth = _ltng_cf0.stack_depth;
                LtngExcSaved _ltng_ec0;
                _ltng_exc_save_to(&_ltng_ec0);
                printf("%s\n", "finally-ret-try");
                _ltng_exc_restore_from(&_ltng_ec0);
                _ltng_rethrow();
            }
        }
    }
    _ltng_fn_pop();
    return _ret;
}

static char *ret_else(char *_ret, int _ltng_cl, int _ltng_cc) {
    if(!_ret) _ret=_ltng_retbuf(); _ret[0]=0;
    _ltng_fn_push("ret_else", _ltng_cl, _ltng_cc);
    {
        LtngTryFrame _ltng_tf1;
        _ltng_tf1.prev = _ltng_try_top;
        _ltng_tf1.stack_depth = _ltng_stack_depth;
        _ltng_try_top = &_ltng_tf1;
        int _ltng_tr1 = setjmp(_ltng_tf1.env);
        if (_ltng_tr1 == 0) {
            printf("%s\n", "try2");
            _ltng_try_top = _ltng_tf1.prev;
            {
                LtngTryFrame _ltng_ef1;
                _ltng_ef1.prev = _ltng_try_top;
                _ltng_ef1.stack_depth = _ltng_stack_depth;
                _ltng_try_top = &_ltng_ef1;
                int _ltng_er1 = setjmp(_ltng_ef1.env);
                if (_ltng_er1 == 0) {
                    printf("%s\n", "else2");
                    {
                        char _ltng_rv[256] = "";
                        snprintf(_ltng_rv, 256, "%s", "from-else");
                        _ltng_try_top = _ltng_tf1.prev;
                        printf("%s\n", "finally-ret-else");
                        _ltng_fn_pop();
                        snprintf(_ret, 256, "%s", _ltng_rv); return _ret;
                    }
                    _ltng_try_top = _ltng_ef1.prev;
                    printf("%s\n", "finally-ret-else");
                } else {
                    _ltng_try_top = _ltng_ef1.prev;
                    _ltng_stack_depth = _ltng_ef1.stack_depth;
                    LtngExcSaved _ltng_ee1;
                    _ltng_exc_save_to(&_ltng_ee1);
                    printf("%s\n", "finally-ret-else");
                    _ltng_exc_restore_from(&_ltng_ee1);
                    _ltng_rethrow();
                }
            }
        } else {
            _ltng_try_top = _ltng_tf1.prev;
            _ltng_stack_depth = _ltng_tf1.stack_depth;
            char _v1_e[256] = "";
            snprintf(_v1_e, 256, "%s: %s", _ltng_exc_type, _ltng_exc_msg);
            char _v1_e__type[256] = "";
            snprintf(_v1_e__type, 256, "%s", _ltng_exc_type);
            char _v1_e__message[256] = "";
            snprintf(_v1_e__message, 256, "%s", _ltng_exc_msg);
            char _v1_e__trace[4096] = "";
            snprintf(_v1_e__trace, 4096, "%s", _ltng_exc_trace);
            char _v1_e__line[256] = "";
            snprintf(_v1_e__line, 256, "%d", _ltng_exc_line);
            char _v1_e__column[256] = "";
            snprintf(_v1_e__column, 256, "%d", _ltng_exc_col);
            char _v1_e__function[256] = "";
            snprintf(_v1_e__function, 256, "%s", _ltng_exc_func);
            char _v1_e__value[256] = "";
            snprintf(_v1_e__value, 256, "%s", _ltng_exc_value);
            char _v1_e__obj[512] = "";
            snprintf(_v1_e__obj, 512, "%s", _v1_e);
            LtngTryFrame _ltng_cf1;
            _ltng_cf1.prev = _ltng_try_top;
            _ltng_cf1.stack_depth = _ltng_stack_depth;
            _ltng_try_top = &_ltng_cf1;
            int _ltng_cr1 = setjmp(_ltng_cf1.env);
            if (_ltng_cr1 == 0) {
                {
                    char _ltng_rv[256] = "";
                    snprintf(_ltng_rv, 256, "%s", "catch2");
                    _ltng_try_top = _ltng_cf1.prev;
                    printf("%s\n", "finally-ret-else");
                    _ltng_fn_pop();
                    snprintf(_ret, 256, "%s", _ltng_rv); return _ret;
                }
                _ltng_try_top = _ltng_cf1.prev;
                printf("%s\n", "finally-ret-else");
            } else {
                _ltng_try_top = _ltng_cf1.prev;
                _ltng_stack_depth = _ltng_cf1.stack_depth;
                LtngExcSaved _ltng_ec1;
                _ltng_exc_save_to(&_ltng_ec1);
                printf("%s\n", "finally-ret-else");
                _ltng_exc_restore_from(&_ltng_ec1);
                _ltng_rethrow();
            }
        }
    }
    _ltng_fn_pop();
    return _ret;
}

static char *__ltng_call_indirect(const char *target, char *_ret, int _ltng_cl, int _ltng_cc, int argc, ...) {
    const char *_argv[64] = {"","","","","","","","","","","","","","","","","","","","","","","","","","","","","","","","","","","","","","","","","","","","","","","","","","","","","","","","","","","","","","","",""};
    va_list ap; va_start(ap, argc);
    for (int i = 0; i < argc && i < 64; i++) { const char *v = va_arg(ap, const char*); _argv[i] = v ? v : ""; }
    va_end(ap);
    if (!target) target = "";
    char _tname[256]; snprintf(_tname, sizeof(_tname), "%s", target);
    const char *_caps[8] = {"","","","","","",""}; int _ncaps = 0;
    char *_sep = strchr(_tname, 1);
    if (_sep) { *_sep = 0; char *_p = _sep + 1; while (_p && *_p && _ncaps < 8) { char *_n = strchr(_p, 1); if (_n) { *_n = 0; _caps[_ncaps++] = _p; _p = _n + 1; } else { _caps[_ncaps++] = _p; break; } } }
    if (strcmp(_tname, "ret_try") == 0) return ret_try(_ret, _ltng_cl, _ltng_cc);
    if (strcmp(_tname, "ret_else") == 0) return ret_else(_ret, _ltng_cl, _ltng_cc);
    _ltng_throw_value("Error", "Unknown function in indirect call", _ltng_cl, _ltng_cc);
    return _ret;
}

int main(void) {
    _ltng_fn_push("main", 1, 1);
    printf("%s\n", ret_try(_ltng_retbuf(), 27, 7));
    printf("%s\n", ret_else(_ltng_retbuf(), 28, 7));
    {
        char i[256] = "0";
        snprintf(i, 256, "%lld", (long long)(0));
        for (; (long long)atoll(i) <= (long long)(2); snprintf(i, 256, "%lld", (long long)atoll(i) + (long long)(1))) {
            {
                LtngTryFrame _ltng_tf2;
                _ltng_tf2.prev = _ltng_try_top;
                _ltng_tf2.stack_depth = _ltng_stack_depth;
                _ltng_try_top = &_ltng_tf2;
                int _ltng_tr2 = setjmp(_ltng_tf2.env);
                if (_ltng_tr2 == 0) {
                    if ((atoi(i)) == (0)) {
                        {
                            _ltng_try_top = _ltng_tf2.prev;
                            printf("%s\n", ({ char _ltc[512]; snprintf(_ltc,512,"%s%s", "finally ", i); _ltc; }));
                            continue;
                        }
                    }
                    if ((atoi(i)) == (2)) {
                        {
                            _ltng_try_top = _ltng_tf2.prev;
                            printf("%s\n", ({ char _ltc[512]; snprintf(_ltc,512,"%s%s", "finally ", i); _ltc; }));
                            break;
                        }
                    }
                    printf("%s\n", ({ char _ltc[512]; snprintf(_ltc,512,"%s%s", "body ", i); _ltc; }));
                    _ltng_try_top = _ltng_tf2.prev;
                    {
                        LtngTryFrame _ltng_ef2;
                        _ltng_ef2.prev = _ltng_try_top;
                        _ltng_ef2.stack_depth = _ltng_stack_depth;
                        _ltng_try_top = &_ltng_ef2;
                        int _ltng_er2 = setjmp(_ltng_ef2.env);
                        if (_ltng_er2 == 0) {
                            printf("%s\n", ({ char _ltc[512]; snprintf(_ltc,512,"%s%s", "else ", i); _ltc; }));
                            _ltng_try_top = _ltng_ef2.prev;
                            printf("%s\n", ({ char _ltc[512]; snprintf(_ltc,512,"%s%s", "finally ", i); _ltc; }));
                        } else {
                            _ltng_try_top = _ltng_ef2.prev;
                            _ltng_stack_depth = _ltng_ef2.stack_depth;
                            LtngExcSaved _ltng_ee2;
                            _ltng_exc_save_to(&_ltng_ee2);
                            printf("%s\n", ({ char _ltc[512]; snprintf(_ltc,512,"%s%s", "finally ", i); _ltc; }));
                            _ltng_exc_restore_from(&_ltng_ee2);
                            _ltng_rethrow();
                        }
                    }
                } else {
                    _ltng_try_top = _ltng_tf2.prev;
                    _ltng_stack_depth = _ltng_tf2.stack_depth;
                    char _v2_e[256] = "";
                    snprintf(_v2_e, 256, "%s: %s", _ltng_exc_type, _ltng_exc_msg);
                    char _v2_e__type[256] = "";
                    snprintf(_v2_e__type, 256, "%s", _ltng_exc_type);
                    char _v2_e__message[256] = "";
                    snprintf(_v2_e__message, 256, "%s", _ltng_exc_msg);
                    char _v2_e__trace[4096] = "";
                    snprintf(_v2_e__trace, 4096, "%s", _ltng_exc_trace);
                    char _v2_e__line[256] = "";
                    snprintf(_v2_e__line, 256, "%d", _ltng_exc_line);
                    char _v2_e__column[256] = "";
                    snprintf(_v2_e__column, 256, "%d", _ltng_exc_col);
                    char _v2_e__function[256] = "";
                    snprintf(_v2_e__function, 256, "%s", _ltng_exc_func);
                    char _v2_e__value[256] = "";
                    snprintf(_v2_e__value, 256, "%s", _ltng_exc_value);
                    char _v2_e__obj[512] = "";
                    snprintf(_v2_e__obj, 512, "%s", _v2_e);
                    LtngTryFrame _ltng_cf2;
                    _ltng_cf2.prev = _ltng_try_top;
                    _ltng_cf2.stack_depth = _ltng_stack_depth;
                    _ltng_try_top = &_ltng_cf2;
                    int _ltng_cr2 = setjmp(_ltng_cf2.env);
                    if (_ltng_cr2 == 0) {
                        printf("%s\n", "catch-loop");
                        _ltng_try_top = _ltng_cf2.prev;
                        printf("%s\n", ({ char _ltc[512]; snprintf(_ltc,512,"%s%s", "finally ", i); _ltc; }));
                    } else {
                        _ltng_try_top = _ltng_cf2.prev;
                        _ltng_stack_depth = _ltng_cf2.stack_depth;
                        LtngExcSaved _ltng_ec2;
                        _ltng_exc_save_to(&_ltng_ec2);
                        printf("%s\n", ({ char _ltc[512]; snprintf(_ltc,512,"%s%s", "finally ", i); _ltc; }));
                        _ltng_exc_restore_from(&_ltng_ec2);
                        _ltng_rethrow();
                    }
                }
            }
        }
    }
    {
        char i[256] = "0";
        snprintf(i, 256, "%lld", (long long)(0));
        for (; (long long)atoll(i) <= (long long)(2); snprintf(i, 256, "%lld", (long long)atoll(i) + (long long)(1))) {
            {
                LtngTryFrame _ltng_tf3;
                _ltng_tf3.prev = _ltng_try_top;
                _ltng_tf3.stack_depth = _ltng_stack_depth;
                _ltng_try_top = &_ltng_tf3;
                int _ltng_tr3 = setjmp(_ltng_tf3.env);
                if (_ltng_tr3 == 0) {
                    printf("%s\n", ({ char _ltc[512]; snprintf(_ltc,512,"%s%s", "t", i); _ltc; }));
                    _ltng_try_top = _ltng_tf3.prev;
                    {
                        LtngTryFrame _ltng_ef3;
                        _ltng_ef3.prev = _ltng_try_top;
                        _ltng_ef3.stack_depth = _ltng_stack_depth;
                        _ltng_try_top = &_ltng_ef3;
                        int _ltng_er3 = setjmp(_ltng_ef3.env);
                        if (_ltng_er3 == 0) {
                            printf("%s\n", ({ char _ltc[512]; snprintf(_ltc,512,"%s%s", "e", i); _ltc; }));
                            {
                                _ltng_try_top = _ltng_tf3.prev;
                                printf("%s\n", ({ char _ltc[512]; snprintf(_ltc,512,"%s%s", "f", i); _ltc; }));
                                break;
                            }
                            _ltng_try_top = _ltng_ef3.prev;
                            printf("%s\n", ({ char _ltc[512]; snprintf(_ltc,512,"%s%s", "f", i); _ltc; }));
                        } else {
                            _ltng_try_top = _ltng_ef3.prev;
                            _ltng_stack_depth = _ltng_ef3.stack_depth;
                            LtngExcSaved _ltng_ee3;
                            _ltng_exc_save_to(&_ltng_ee3);
                            printf("%s\n", ({ char _ltc[512]; snprintf(_ltc,512,"%s%s", "f", i); _ltc; }));
                            _ltng_exc_restore_from(&_ltng_ee3);
                            _ltng_rethrow();
                        }
                    }
                } else {
                    _ltng_try_top = _ltng_tf3.prev;
                    _ltng_stack_depth = _ltng_tf3.stack_depth;
                    char _v3_e[256] = "";
                    snprintf(_v3_e, 256, "%s: %s", _ltng_exc_type, _ltng_exc_msg);
                    char _v3_e__type[256] = "";
                    snprintf(_v3_e__type, 256, "%s", _ltng_exc_type);
                    char _v3_e__message[256] = "";
                    snprintf(_v3_e__message, 256, "%s", _ltng_exc_msg);
                    char _v3_e__trace[4096] = "";
                    snprintf(_v3_e__trace, 4096, "%s", _ltng_exc_trace);
                    char _v3_e__line[256] = "";
                    snprintf(_v3_e__line, 256, "%d", _ltng_exc_line);
                    char _v3_e__column[256] = "";
                    snprintf(_v3_e__column, 256, "%d", _ltng_exc_col);
                    char _v3_e__function[256] = "";
                    snprintf(_v3_e__function, 256, "%s", _ltng_exc_func);
                    char _v3_e__value[256] = "";
                    snprintf(_v3_e__value, 256, "%s", _ltng_exc_value);
                    char _v3_e__obj[512] = "";
                    snprintf(_v3_e__obj, 512, "%s", _v3_e);
                    LtngTryFrame _ltng_cf3;
                    _ltng_cf3.prev = _ltng_try_top;
                    _ltng_cf3.stack_depth = _ltng_stack_depth;
                    _ltng_try_top = &_ltng_cf3;
                    int _ltng_cr3 = setjmp(_ltng_cf3.env);
                    if (_ltng_cr3 == 0) {
                        printf("%s\n", ({ char _ltc[512]; snprintf(_ltc,512,"%s%s", "c", i); _ltc; }));
                        _ltng_try_top = _ltng_cf3.prev;
                        printf("%s\n", ({ char _ltc[512]; snprintf(_ltc,512,"%s%s", "f", i); _ltc; }));
                    } else {
                        _ltng_try_top = _ltng_cf3.prev;
                        _ltng_stack_depth = _ltng_cf3.stack_depth;
                        LtngExcSaved _ltng_ec3;
                        _ltng_exc_save_to(&_ltng_ec3);
                        printf("%s\n", ({ char _ltc[512]; snprintf(_ltc,512,"%s%s", "f", i); _ltc; }));
                        _ltng_exc_restore_from(&_ltng_ec3);
                        _ltng_rethrow();
                    }
                }
            }
        }
    }
    {
        LtngTryFrame _ltng_tf4;
        _ltng_tf4.prev = _ltng_try_top;
        _ltng_tf4.stack_depth = _ltng_stack_depth;
        _ltng_try_top = &_ltng_tf4;
        int _ltng_tr4 = setjmp(_ltng_tf4.env);
        if (_ltng_tr4 == 0) {
            {
                LtngTryFrame _ltng_tf5;
                _ltng_tf5.prev = _ltng_try_top;
                _ltng_tf5.stack_depth = _ltng_stack_depth;
                _ltng_try_top = &_ltng_tf5;
                int _ltng_tr5 = setjmp(_ltng_tf5.env);
                if (_ltng_tr5 == 0) {
                    printf("%s\n", "inner-try");
                    _ltng_try_top = _ltng_tf5.prev;
                    {
                        LtngTryFrame _ltng_ef5;
                        _ltng_ef5.prev = _ltng_try_top;
                        _ltng_ef5.stack_depth = _ltng_stack_depth;
                        _ltng_try_top = &_ltng_ef5;
                        int _ltng_er5 = setjmp(_ltng_ef5.env);
                        if (_ltng_er5 == 0) {
                            printf("%s\n", "inner-else");
                            _ltng_throw_value("Error", "from-else", 68, 9);
                            _ltng_try_top = _ltng_ef5.prev;
                            printf("%s\n", "inner-finally");
                        } else {
                            _ltng_try_top = _ltng_ef5.prev;
                            _ltng_stack_depth = _ltng_ef5.stack_depth;
                            LtngExcSaved _ltng_ee5;
                            _ltng_exc_save_to(&_ltng_ee5);
                            printf("%s\n", "inner-finally");
                            _ltng_exc_restore_from(&_ltng_ee5);
                            _ltng_rethrow();
                        }
                    }
                } else {
                    _ltng_try_top = _ltng_tf5.prev;
                    _ltng_stack_depth = _ltng_tf5.stack_depth;
                    char _v4_e[256] = "";
                    snprintf(_v4_e, 256, "%s: %s", _ltng_exc_type, _ltng_exc_msg);
                    char _v4_e__type[256] = "";
                    snprintf(_v4_e__type, 256, "%s", _ltng_exc_type);
                    char _v4_e__message[256] = "";
                    snprintf(_v4_e__message, 256, "%s", _ltng_exc_msg);
                    char _v4_e__trace[4096] = "";
                    snprintf(_v4_e__trace, 4096, "%s", _ltng_exc_trace);
                    char _v4_e__line[256] = "";
                    snprintf(_v4_e__line, 256, "%d", _ltng_exc_line);
                    char _v4_e__column[256] = "";
                    snprintf(_v4_e__column, 256, "%d", _ltng_exc_col);
                    char _v4_e__function[256] = "";
                    snprintf(_v4_e__function, 256, "%s", _ltng_exc_func);
                    char _v4_e__value[256] = "";
                    snprintf(_v4_e__value, 256, "%s", _ltng_exc_value);
                    char _v4_e__obj[512] = "";
                    snprintf(_v4_e__obj, 512, "%s", _v4_e);
                    LtngTryFrame _ltng_cf5;
                    _ltng_cf5.prev = _ltng_try_top;
                    _ltng_cf5.stack_depth = _ltng_stack_depth;
                    _ltng_try_top = &_ltng_cf5;
                    int _ltng_cr5 = setjmp(_ltng_cf5.env);
                    if (_ltng_cr5 == 0) {
                        printf("%s\n", "inner-catch");
                        _ltng_try_top = _ltng_cf5.prev;
                        printf("%s\n", "inner-finally");
                    } else {
                        _ltng_try_top = _ltng_cf5.prev;
                        _ltng_stack_depth = _ltng_cf5.stack_depth;
                        LtngExcSaved _ltng_ec5;
                        _ltng_exc_save_to(&_ltng_ec5);
                        printf("%s\n", "inner-finally");
                        _ltng_exc_restore_from(&_ltng_ec5);
                        _ltng_rethrow();
                    }
                }
            }
            _ltng_try_top = _ltng_tf4.prev;
        } else {
            _ltng_try_top = _ltng_tf4.prev;
            _ltng_stack_depth = _ltng_tf4.stack_depth;
            char _v5_e[256] = "";
            snprintf(_v5_e, 256, "%s: %s", _ltng_exc_type, _ltng_exc_msg);
            char _v5_e__type[256] = "";
            snprintf(_v5_e__type, 256, "%s", _ltng_exc_type);
            char _v5_e__message[256] = "";
            snprintf(_v5_e__message, 256, "%s", _ltng_exc_msg);
            char _v5_e__trace[4096] = "";
            snprintf(_v5_e__trace, 4096, "%s", _ltng_exc_trace);
            char _v5_e__line[256] = "";
            snprintf(_v5_e__line, 256, "%d", _ltng_exc_line);
            char _v5_e__column[256] = "";
            snprintf(_v5_e__column, 256, "%d", _ltng_exc_col);
            char _v5_e__function[256] = "";
            snprintf(_v5_e__function, 256, "%s", _ltng_exc_func);
            char _v5_e__value[256] = "";
            snprintf(_v5_e__value, 256, "%s", _ltng_exc_value);
            char _v5_e__obj[512] = "";
            snprintf(_v5_e__obj, 512, "%s", _v5_e);
            printf("%s\n", ({ char _ltc[512]; snprintf(_ltc,512,"%s%s", "outer-catch ", _v5_e__message); _ltc; }));
        }
    }
    {
        LtngTryFrame _ltng_tf6;
        _ltng_tf6.prev = _ltng_try_top;
        _ltng_tf6.stack_depth = _ltng_stack_depth;
        _ltng_try_top = &_ltng_tf6;
        int _ltng_tr6 = setjmp(_ltng_tf6.env);
        if (_ltng_tr6 == 0) {
            printf("%s\n", "solo-try");
            _ltng_try_top = _ltng_tf6.prev;
            printf("%s\n", "solo-else");
        } else {
            _ltng_try_top = _ltng_tf6.prev;
            _ltng_stack_depth = _ltng_tf6.stack_depth;
            char _v6_e[256] = "";
            snprintf(_v6_e, 256, "%s: %s", _ltng_exc_type, _ltng_exc_msg);
            char _v6_e__type[256] = "";
            snprintf(_v6_e__type, 256, "%s", _ltng_exc_type);
            char _v6_e__message[256] = "";
            snprintf(_v6_e__message, 256, "%s", _ltng_exc_msg);
            char _v6_e__trace[4096] = "";
            snprintf(_v6_e__trace, 4096, "%s", _ltng_exc_trace);
            char _v6_e__line[256] = "";
            snprintf(_v6_e__line, 256, "%d", _ltng_exc_line);
            char _v6_e__column[256] = "";
            snprintf(_v6_e__column, 256, "%d", _ltng_exc_col);
            char _v6_e__function[256] = "";
            snprintf(_v6_e__function, 256, "%s", _ltng_exc_func);
            char _v6_e__value[256] = "";
            snprintf(_v6_e__value, 256, "%s", _ltng_exc_value);
            char _v6_e__obj[512] = "";
            snprintf(_v6_e__obj, 512, "%s", _v6_e);
            printf("%s\n", "solo-catch");
        }
    }
    {
        LtngTryFrame _ltng_tf7;
        _ltng_tf7.prev = _ltng_try_top;
        _ltng_tf7.stack_depth = _ltng_stack_depth;
        _ltng_try_top = &_ltng_tf7;
        int _ltng_tr7 = setjmp(_ltng_tf7.env);
        if (_ltng_tr7 == 0) {
            {
                LtngTryFrame _ltng_tf8;
                _ltng_tf8.prev = _ltng_try_top;
                _ltng_tf8.stack_depth = _ltng_stack_depth;
                _ltng_try_top = &_ltng_tf8;
                int _ltng_tr8 = setjmp(_ltng_tf8.env);
                if (_ltng_tr8 == 0) {
                    printf("%s\n", "inner2-try");
                    _ltng_try_top = _ltng_tf8.prev;
                    printf("%s\n", "inner2-else");
                    _ltng_throw_value("Error", "from-else2", 91, 9);
                } else {
                    _ltng_try_top = _ltng_tf8.prev;
                    _ltng_stack_depth = _ltng_tf8.stack_depth;
                    char _v7_e[256] = "";
                    snprintf(_v7_e, 256, "%s: %s", _ltng_exc_type, _ltng_exc_msg);
                    char _v7_e__type[256] = "";
                    snprintf(_v7_e__type, 256, "%s", _ltng_exc_type);
                    char _v7_e__message[256] = "";
                    snprintf(_v7_e__message, 256, "%s", _ltng_exc_msg);
                    char _v7_e__trace[4096] = "";
                    snprintf(_v7_e__trace, 4096, "%s", _ltng_exc_trace);
                    char _v7_e__line[256] = "";
                    snprintf(_v7_e__line, 256, "%d", _ltng_exc_line);
                    char _v7_e__column[256] = "";
                    snprintf(_v7_e__column, 256, "%d", _ltng_exc_col);
                    char _v7_e__function[256] = "";
                    snprintf(_v7_e__function, 256, "%s", _ltng_exc_func);
                    char _v7_e__value[256] = "";
                    snprintf(_v7_e__value, 256, "%s", _ltng_exc_value);
                    char _v7_e__obj[512] = "";
                    snprintf(_v7_e__obj, 512, "%s", _v7_e);
                    printf("%s\n", "inner2-catch");
                }
            }
            _ltng_try_top = _ltng_tf7.prev;
        } else {
            _ltng_try_top = _ltng_tf7.prev;
            _ltng_stack_depth = _ltng_tf7.stack_depth;
            char _v8_e[256] = "";
            snprintf(_v8_e, 256, "%s: %s", _ltng_exc_type, _ltng_exc_msg);
            char _v8_e__type[256] = "";
            snprintf(_v8_e__type, 256, "%s", _ltng_exc_type);
            char _v8_e__message[256] = "";
            snprintf(_v8_e__message, 256, "%s", _ltng_exc_msg);
            char _v8_e__trace[4096] = "";
            snprintf(_v8_e__trace, 4096, "%s", _ltng_exc_trace);
            char _v8_e__line[256] = "";
            snprintf(_v8_e__line, 256, "%d", _ltng_exc_line);
            char _v8_e__column[256] = "";
            snprintf(_v8_e__column, 256, "%d", _ltng_exc_col);
            char _v8_e__function[256] = "";
            snprintf(_v8_e__function, 256, "%s", _ltng_exc_func);
            char _v8_e__value[256] = "";
            snprintf(_v8_e__value, 256, "%s", _ltng_exc_value);
            char _v8_e__obj[512] = "";
            snprintf(_v8_e__obj, 512, "%s", _v8_e);
            printf("%s\n", ({ char _ltc[512]; snprintf(_ltc,512,"%s%s", "outer2-catch ", _v8_e__message); _ltc; }));
        }
    }
    return 0;
}
