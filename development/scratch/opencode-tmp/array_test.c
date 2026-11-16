#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <ctype.h>
#include <setjmp.h>
static const char *_ltng_src_file = "code/array_test.ltng";
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
static void _ltng_arr_slice(char **sd,int sl,int start,int end,char ***dd,int *dl,int *dc){ if(start<0) start=sl+start; if(end<0) end=sl+end; if(start<0) start=0; if(end>sl) end=sl; if(start>end) end=start; int n=end-start; _ltng_arr_ensure(dd,dl,dc,n); for(int i=0;i<*dl;i++){ if((*dd)[i]){ free((*dd)[i]); (*dd)[i]=NULL; } } *dl=0; for(int i=0;i<n;i++){ _ltng_arr_push(dd,dl,dc,sd[start+i]?sd[start+i]:""); } }
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

static char *__ltng_call_indirect(const char *target, char *_ret, int _ltng_cl, int _ltng_cc, int argc, ...) {
    const char *_argv[16] = {"","","","","","","","","","","","","","","",""};
    va_list ap; va_start(ap, argc);
    for (int i = 0; i < argc && i < 16; i++) { const char *v = va_arg(ap, const char*); _argv[i] = v ? v : ""; }
    va_end(ap);
    if (!target) target = "";
    char _tname[256]; snprintf(_tname, sizeof(_tname), "%s", target);
    const char *_caps[8] = {"","","","","","",""}; int _ncaps = 0;
    char *_sep = strchr(_tname, 1);
    if (_sep) { *_sep = 0; char *_p = _sep + 1; while (_p && *_p && _ncaps < 8) { char *_n = strchr(_p, 1); if (_n) { *_n = 0; _caps[_ncaps++] = _p; _p = _n + 1; } else { _caps[_ncaps++] = _p; break; } } }
    _ltng_throw_value("Error", "Unknown function in indirect call", _ltng_cl, _ltng_cc);
    return _ret;
}

int main(void) {
    _ltng_fn_push("main", 1, 1);
    char ix[256] = "0";
    char i[256] = "0";

    int a_len = 0;
    int a_cap = 0;
    char **a_data = _ltng_arr_new(4);
    _ltng_arr_push(&a_data, &a_len, &a_cap, "10");
    _ltng_arr_push(&a_data, &a_len, &a_cap, "20");
    _ltng_arr_push(&a_data, &a_len, &a_cap, "30");
    _ltng_arr_push(&a_data, &a_len, &a_cap, "40");
    printf("%s\n", "idx_basic");
    printf("%s\n", ({ int __i0 = (int)(1); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (4); int __idx = __i0; _ltng_arr_get(a_data, a_len, __idx); }));
    printf("%s\n", ({ int __i0 = (int)(4); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (4); int __idx = __i0; _ltng_arr_get(a_data, a_len, __idx); }));
    printf("%s\n", ({ int __i0 = (int)(-1); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (4); int __idx = __i0; _ltng_arr_get(a_data, a_len, __idx); }));
    printf("%s\n", ({ int __i0 = (int)(-4); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (4); int __idx = __i0; _ltng_arr_get(a_data, a_len, __idx); }));
    printf("%s\n", ({ int __i0 = (int)(0); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (4); int __idx = __i0; _ltng_arr_get(a_data, a_len, __idx); }));
    printf("%s\n", ({ int __i0 = (int)(5); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (4); int __idx = __i0; _ltng_arr_get(a_data, a_len, __idx); }));
    printf("%s\n", ({ int __i0 = (int)(-5); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (4); int __idx = __i0; _ltng_arr_get(a_data, a_len, __idx); }));
    printf("%s\n", "length_basic");
    printf("%lld\n", (long long)(a_len));
    int e_len = 0;
    int e_cap = 0;
    char **e_data = _ltng_arr_new(1);
    printf("%lld\n", (long long)(e_len));
    printf("%s\n", "push_growth");
    { char __apv[256]; snprintf(__apv, 256, "%lld", (long long)(1)); _ltng_arr_push(&e_data, &e_len, &e_cap, __apv); }
    { char __apv[256]; snprintf(__apv, 256, "%lld", (long long)(2)); _ltng_arr_push(&e_data, &e_len, &e_cap, __apv); }
    { char __apv[256]; snprintf(__apv, 256, "%lld", (long long)(3)); _ltng_arr_push(&e_data, &e_len, &e_cap, __apv); }
    { char __apv[256]; snprintf(__apv, 256, "%lld", (long long)(4)); _ltng_arr_push(&e_data, &e_len, &e_cap, __apv); }
    { char __apv[256]; snprintf(__apv, 256, "%lld", (long long)(5)); _ltng_arr_push(&e_data, &e_len, &e_cap, __apv); }
    printf("%lld\n", (long long)(e_len));
    printf("%s\n", ({ int __i0 = (int)(1); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (0); int __idx = __i0; _ltng_arr_get(e_data, e_len, __idx); }));
    printf("%s\n", ({ int __i0 = (int)(5); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (0); int __idx = __i0; _ltng_arr_get(e_data, e_len, __idx); }));
    printf("%s\n", "pop_ops");
    printf("%s\n", ({ char *__r = _ltng_arr_pop(e_data, &e_len); __r; }));
    printf("%lld\n", (long long)(e_len));
    printf("%s\n", ({ char *__r = _ltng_arr_pop_index(e_data, &e_len, (int)(2)); __r; }));
    printf("%s\n", ({ int __i0 = (int)(1); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (0); int __idx = __i0; _ltng_arr_get(e_data, e_len, __idx); }));
    printf("%s\n", ({ int __i0 = (int)(2); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (0); int __idx = __i0; _ltng_arr_get(e_data, e_len, __idx); }));
    printf("%s\n", ({ int __i0 = (int)(3); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (0); int __idx = __i0; _ltng_arr_get(e_data, e_len, __idx); }));
    printf("%lld\n", (long long)(e_len));
    printf("%s\n", ({ char *__r = _ltng_arr_pop_index(e_data, &e_len, (int)(-1)); __r; }));
    printf("%lld\n", (long long)(e_len));
    printf("%s\n", ({ int __i0 = (int)(1); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (0); int __idx = __i0; _ltng_arr_get(e_data, e_len, __idx); }));
    printf("%s\n", ({ int __i0 = (int)(2); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (0); int __idx = __i0; _ltng_arr_get(e_data, e_len, __idx); }));
    printf("%s\n", "insert_ops");
    int ins_len = 0;
    int ins_cap = 0;
    char **ins_data = _ltng_arr_new(3);
    _ltng_arr_push(&ins_data, &ins_len, &ins_cap, "10");
    _ltng_arr_push(&ins_data, &ins_len, &ins_cap, "30");
    _ltng_arr_push(&ins_data, &ins_len, &ins_cap, "40");
    { int __ii = (int)(2); char __aiv[256]; snprintf(__aiv, 256, "%lld", (long long)(20)); _ltng_arr_insert(&ins_data, &ins_len, &ins_cap, __ii, __aiv); }
    printf("%s\n", ({ int __i0 = (int)(1); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (3); int __idx = __i0; _ltng_arr_get(ins_data, ins_len, __idx); }));
    printf("%s\n", ({ int __i0 = (int)(2); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (3); int __idx = __i0; _ltng_arr_get(ins_data, ins_len, __idx); }));
    printf("%s\n", ({ int __i0 = (int)(3); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (3); int __idx = __i0; _ltng_arr_get(ins_data, ins_len, __idx); }));
    printf("%s\n", ({ int __i0 = (int)(4); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (3); int __idx = __i0; _ltng_arr_get(ins_data, ins_len, __idx); }));
    { int __ii = (int)(1); char __aiv[256]; snprintf(__aiv, 256, "%lld", (long long)(5)); _ltng_arr_insert(&ins_data, &ins_len, &ins_cap, __ii, __aiv); }
    printf("%s\n", ({ int __i0 = (int)(1); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (3); int __idx = __i0; _ltng_arr_get(ins_data, ins_len, __idx); }));
    printf("%lld\n", (long long)(ins_len));
    { int __ii = (int)(-1); char __aiv[256]; snprintf(__aiv, 256, "%lld", (long long)(35)); _ltng_arr_insert(&ins_data, &ins_len, &ins_cap, __ii, __aiv); }
    printf("%s\n", ({ int __i0 = (int)(5); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (3); int __idx = __i0; _ltng_arr_get(ins_data, ins_len, __idx); }));
    printf("%s\n", ({ int __i0 = (int)(6); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (3); int __idx = __i0; _ltng_arr_get(ins_data, ins_len, __idx); }));
    printf("%lld\n", (long long)(ins_len));
    printf("%s\n", "remove_ops");
    int rem_len = 0;
    int rem_cap = 0;
    char **rem_data = _ltng_arr_new(5);
    _ltng_arr_push(&rem_data, &rem_len, &rem_cap, "10");
    _ltng_arr_push(&rem_data, &rem_len, &rem_cap, "20");
    _ltng_arr_push(&rem_data, &rem_len, &rem_cap, "30");
    _ltng_arr_push(&rem_data, &rem_len, &rem_cap, "40");
    _ltng_arr_push(&rem_data, &rem_len, &rem_cap, "50");
    printf("%s\n", ({ char *__r = _ltng_arr_pop_index(rem_data, &rem_len, (int)(3)); __r; }));
    printf("%s\n", ({ int __i0 = (int)(1); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (5); int __idx = __i0; _ltng_arr_get(rem_data, rem_len, __idx); }));
    printf("%s\n", ({ int __i0 = (int)(2); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (5); int __idx = __i0; _ltng_arr_get(rem_data, rem_len, __idx); }));
    printf("%s\n", ({ int __i0 = (int)(3); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (5); int __idx = __i0; _ltng_arr_get(rem_data, rem_len, __idx); }));
    printf("%s\n", ({ int __i0 = (int)(4); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (5); int __idx = __i0; _ltng_arr_get(rem_data, rem_len, __idx); }));
    printf("%lld\n", (long long)(rem_len));
    printf("%s\n", ({ char *__r = _ltng_arr_pop_index(rem_data, &rem_len, (int)(-1)); __r; }));
    printf("%lld\n", (long long)(rem_len));
    printf("%s\n", ({ char *__r = _ltng_arr_pop_index(rem_data, &rem_len, (int)(1)); __r; }));
    printf("%s\n", ({ int __i0 = (int)(1); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (5); int __idx = __i0; _ltng_arr_get(rem_data, rem_len, __idx); }));
    printf("%lld\n", (long long)(rem_len));
    printf("%s\n", "contains_index_of");
    int c_len = 0;
    int c_cap = 0;
    char **c_data = _ltng_arr_new(4);
    _ltng_arr_push(&c_data, &c_len, &c_cap, "alice");
    _ltng_arr_push(&c_data, &c_len, &c_cap, "bob");
    _ltng_arr_push(&c_data, &c_len, &c_cap, "charlie");
    _ltng_arr_push(&c_data, &c_len, &c_cap, "bob");
    printf("%s\n", ({ char __cv[256]; snprintf(__cv, 256, "%s", "bob"); char *__r = _ltng_retbuf(); snprintf(__r, 256, "%s", _ltng_arr_contains(c_data, c_len, __cv) ? "true" : "false"); __r; }));
    printf("%s\n", ({ char __cv[256]; snprintf(__cv, 256, "%s", "dave"); char *__r = _ltng_retbuf(); snprintf(__r, 256, "%s", _ltng_arr_contains(c_data, c_len, __cv) ? "true" : "false"); __r; }));
    printf("%s\n", ({ char __iv[256]; snprintf(__iv, 256, "%s", "alice"); char *__r = _ltng_retbuf(); snprintf(__r, 256, "%d", _ltng_arr_index_of(c_data, c_len, __iv)); __r; }));
    printf("%s\n", ({ char __iv[256]; snprintf(__iv, 256, "%s", "bob"); char *__r = _ltng_retbuf(); snprintf(__r, 256, "%d", _ltng_arr_index_of(c_data, c_len, __iv)); __r; }));
    printf("%s\n", ({ char __iv[256]; snprintf(__iv, 256, "%s", "charlie"); char *__r = _ltng_retbuf(); snprintf(__r, 256, "%d", _ltng_arr_index_of(c_data, c_len, __iv)); __r; }));
    printf("%s\n", ({ char __iv[256]; snprintf(__iv, 256, "%s", "dave"); char *__r = _ltng_retbuf(); snprintf(__r, 256, "%d", _ltng_arr_index_of(c_data, c_len, __iv)); __r; }));
    int nums_len = 0;
    int nums_cap = 0;
    char **nums_data = _ltng_arr_new(3);
    _ltng_arr_push(&nums_data, &nums_len, &nums_cap, "10");
    _ltng_arr_push(&nums_data, &nums_len, &nums_cap, "20");
    _ltng_arr_push(&nums_data, &nums_len, &nums_cap, "30");
    printf("%s\n", ({ char __cv[256]; snprintf(__cv, 256, "%lld", (long long)(20)); char *__r = _ltng_retbuf(); snprintf(__r, 256, "%s", _ltng_arr_contains(nums_data, nums_len, __cv) ? "true" : "false"); __r; }));
    printf("%s\n", ({ char __cv[256]; snprintf(__cv, 256, "%lld", (long long)(99)); char *__r = _ltng_retbuf(); snprintf(__r, 256, "%s", _ltng_arr_contains(nums_data, nums_len, __cv) ? "true" : "false"); __r; }));
    printf("%s\n", ({ char __iv[256]; snprintf(__iv, 256, "%lld", (long long)(20)); char *__r = _ltng_retbuf(); snprintf(__r, 256, "%d", _ltng_arr_index_of(nums_data, nums_len, __iv)); __r; }));
    printf("%s\n", ({ char __iv[256]; snprintf(__iv, 256, "%lld", (long long)(99)); char *__r = _ltng_retbuf(); snprintf(__r, 256, "%d", _ltng_arr_index_of(nums_data, nums_len, __iv)); __r; }));
    printf("%s\n", "sort_reverse");
    int s_len = 0;
    int s_cap = 0;
    char **s_data = _ltng_arr_new(7);
    _ltng_arr_push(&s_data, &s_len, &s_cap, "3");
    _ltng_arr_push(&s_data, &s_len, &s_cap, "1");
    _ltng_arr_push(&s_data, &s_len, &s_cap, "4");
    _ltng_arr_push(&s_data, &s_len, &s_cap, "1");
    _ltng_arr_push(&s_data, &s_len, &s_cap, "5");
    _ltng_arr_push(&s_data, &s_len, &s_cap, "9");
    _ltng_arr_push(&s_data, &s_len, &s_cap, "2");
    _ltng_arr_sort(s_data, s_len);
    printf("%s\n", ({ int __i0 = (int)(1); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (7); int __idx = __i0; _ltng_arr_get(s_data, s_len, __idx); }));
    printf("%s\n", ({ int __i0 = (int)(2); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (7); int __idx = __i0; _ltng_arr_get(s_data, s_len, __idx); }));
    printf("%s\n", ({ int __i0 = (int)(3); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (7); int __idx = __i0; _ltng_arr_get(s_data, s_len, __idx); }));
    printf("%s\n", ({ int __i0 = (int)(4); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (7); int __idx = __i0; _ltng_arr_get(s_data, s_len, __idx); }));
    printf("%s\n", ({ int __i0 = (int)(5); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (7); int __idx = __i0; _ltng_arr_get(s_data, s_len, __idx); }));
    printf("%s\n", ({ int __i0 = (int)(6); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (7); int __idx = __i0; _ltng_arr_get(s_data, s_len, __idx); }));
    printf("%s\n", ({ int __i0 = (int)(7); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (7); int __idx = __i0; _ltng_arr_get(s_data, s_len, __idx); }));
    _ltng_arr_reverse(s_data, s_len);
    printf("%s\n", ({ int __i0 = (int)(1); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (7); int __idx = __i0; _ltng_arr_get(s_data, s_len, __idx); }));
    printf("%s\n", ({ int __i0 = (int)(7); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (7); int __idx = __i0; _ltng_arr_get(s_data, s_len, __idx); }));
    int words_len = 0;
    int words_cap = 0;
    char **words_data = _ltng_arr_new(3);
    _ltng_arr_push(&words_data, &words_len, &words_cap, "pear");
    _ltng_arr_push(&words_data, &words_len, &words_cap, "apple");
    _ltng_arr_push(&words_data, &words_len, &words_cap, "banana");
    _ltng_arr_sort(words_data, words_len);
    printf("%s\n", ({ int __i0 = (int)(1); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (3); int __idx = __i0; _ltng_arr_get(words_data, words_len, __idx); }));
    printf("%s\n", ({ int __i0 = (int)(2); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (3); int __idx = __i0; _ltng_arr_get(words_data, words_len, __idx); }));
    printf("%s\n", ({ int __i0 = (int)(3); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (3); int __idx = __i0; _ltng_arr_get(words_data, words_len, __idx); }));
    _ltng_arr_reverse(words_data, words_len);
    printf("%s\n", ({ int __i0 = (int)(1); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (3); int __idx = __i0; _ltng_arr_get(words_data, words_len, __idx); }));
    printf("%s\n", ({ int __i0 = (int)(3); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (3); int __idx = __i0; _ltng_arr_get(words_data, words_len, __idx); }));
    printf("%s\n", "aliasing");
    int orig_len = 0;
    int orig_cap = 0;
    char **orig_data = _ltng_arr_new(3);
    _ltng_arr_push(&orig_data, &orig_len, &orig_cap, "1");
    _ltng_arr_push(&orig_data, &orig_len, &orig_cap, "2");
    _ltng_arr_push(&orig_data, &orig_len, &orig_cap, "3");
    int alias_len = 0;
    int alias_cap = 0;
    char **alias_data = _ltng_arr_new(1);
    _ltng_arr_copy(orig_data, &alias_data, &alias_len, &alias_cap, orig_len);
    { char __apv[256]; snprintf(__apv, 256, "%lld", (long long)(4)); _ltng_arr_push(&alias_data, &alias_len, &alias_cap, __apv); }
    printf("%lld\n", (long long)(orig_len));
    printf("%s\n", ({ int __i0 = (int)(4); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (3); int __idx = __i0; _ltng_arr_get(orig_data, orig_len, __idx); }));
    { int __aidx = ({ int __i0 = (int)(1); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (3); int __idx = __i0; __idx; }); char __asv[256]; snprintf(__asv, 256, "%lld", (long long)(99)); _ltng_arr_set(orig_data, &orig_len, &orig_cap, __aidx, __asv); }
    printf("%s\n", ({ int __i0 = (int)(1); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (0); int __idx = __i0; _ltng_arr_get(alias_data, alias_len, __idx); }));
    printf("%s\n", "copy");
    int c1_len = 0;
    int c1_cap = 0;
    char **c1_data = _ltng_arr_new(3);
    _ltng_arr_push(&c1_data, &c1_len, &c1_cap, "7");
    _ltng_arr_push(&c1_data, &c1_len, &c1_cap, "8");
    _ltng_arr_push(&c1_data, &c1_len, &c1_cap, "9");
    int c2_len = 0;
    int c2_cap = 0;
    char **c2_data = _ltng_arr_new(1);
    _ltng_arr_copy(c1_data, &c2_data, &c2_len, &c2_cap, c1_len);
    { char __apv[256]; snprintf(__apv, 256, "%lld", (long long)(10)); _ltng_arr_push(&c2_data, &c2_len, &c2_cap, __apv); }
    { int __aidx = ({ int __i0 = (int)(1); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (0); int __idx = __i0; __idx; }); char __asv[256]; snprintf(__asv, 256, "%lld", (long long)(70)); _ltng_arr_set(c2_data, &c2_len, &c2_cap, __aidx, __asv); }
    printf("%lld\n", (long long)(c1_len));
    printf("%s\n", ({ int __i0 = (int)(1); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (3); int __idx = __i0; _ltng_arr_get(c1_data, c1_len, __idx); }));
    printf("%s\n", ({ int __i0 = (int)(4); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (3); int __idx = __i0; _ltng_arr_get(c1_data, c1_len, __idx); }));
    printf("%lld\n", (long long)(c2_len));
    printf("%s\n", ({ int __i0 = (int)(1); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (0); int __idx = __i0; _ltng_arr_get(c2_data, c2_len, __idx); }));
    printf("%s\n", ({ int __i0 = (int)(4); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (0); int __idx = __i0; _ltng_arr_get(c2_data, c2_len, __idx); }));
    printf("%s\n", "slice");
    int sl_len = 0;
    int sl_cap = 0;
    char **sl_data = _ltng_arr_new(5);
    _ltng_arr_push(&sl_data, &sl_len, &sl_cap, "10");
    _ltng_arr_push(&sl_data, &sl_len, &sl_cap, "20");
    _ltng_arr_push(&sl_data, &sl_len, &sl_cap, "30");
    _ltng_arr_push(&sl_data, &sl_len, &sl_cap, "40");
    _ltng_arr_push(&sl_data, &sl_len, &sl_cap, "50");
    int sl1_len = 0;
    int sl1_cap = 0;
    char **sl1_data = _ltng_arr_new(1);
    _ltng_arr_slice(sl_data, sl_len, 2,4, &sl1_data, &sl1_len, &sl1_cap);
    printf("%lld\n", (long long)(sl1_len));
    printf("%s\n", ({ int __i0 = (int)(1); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (0); int __idx = __i0; _ltng_arr_get(sl1_data, sl1_len, __idx); }));
    printf("%s\n", ({ int __i0 = (int)(2); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (0); int __idx = __i0; _ltng_arr_get(sl1_data, sl1_len, __idx); }));
    int sl2_len = 0;
    int sl2_cap = 0;
    char **sl2_data = _ltng_arr_new(1);
    _ltng_arr_slice(sl_data, sl_len, 1,6, &sl2_data, &sl2_len, &sl2_cap);
    printf("%lld\n", (long long)(sl2_len));
    printf("%s\n", ({ int __i0 = (int)(1); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (0); int __idx = __i0; _ltng_arr_get(sl2_data, sl2_len, __idx); }));
    printf("%s\n", ({ int __i0 = (int)(5); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (0); int __idx = __i0; _ltng_arr_get(sl2_data, sl2_len, __idx); }));
    int sl3_len = 0;
    int sl3_cap = 0;
    char **sl3_data = _ltng_arr_new(1);
    _ltng_arr_slice(sl_data, sl_len, -3,6, &sl3_data, &sl3_len, &sl3_cap);
    printf("%lld\n", (long long)(sl3_len));
    printf("%s\n", ({ int __i0 = (int)(1); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (0); int __idx = __i0; _ltng_arr_get(sl3_data, sl3_len, __idx); }));
    printf("%s\n", ({ int __i0 = (int)(3); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (0); int __idx = __i0; _ltng_arr_get(sl3_data, sl3_len, __idx); }));
    int sl4_len = 0;
    int sl4_cap = 0;
    char **sl4_data = _ltng_arr_new(1);
    _ltng_arr_slice(sl_data, sl_len, 3,3, &sl4_data, &sl4_len, &sl4_cap);
    printf("%lld\n", (long long)(sl4_len));
    printf("%s\n", "set_expr_index");
    int m_len = 0;
    int m_cap = 0;
    char **m_data = _ltng_arr_new(3);
    _ltng_arr_push(&m_data, &m_len, &m_cap, "1");
    _ltng_arr_push(&m_data, &m_len, &m_cap, "2");
    _ltng_arr_push(&m_data, &m_len, &m_cap, "3");
    { int __aidx = ({ int __i0 = (int)(2); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (3); int __idx = __i0; __idx; }); char __asv[256]; snprintf(__asv, 256, "%lld", (long long)(22)); _ltng_arr_set(m_data, &m_len, &m_cap, __aidx, __asv); }
    printf("%s\n", ({ int __i0 = (int)(2); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (3); int __idx = __i0; _ltng_arr_get(m_data, m_len, __idx); }));
    snprintf(ix, 256, "3");
    { int __aidx = ({ int __i0 = (int)(atoi(ix)); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (3); int __idx = __i0; __idx; }); char __asv[256]; snprintf(__asv, 256, "%lld", (long long)(33)); _ltng_arr_set(m_data, &m_len, &m_cap, __aidx, __asv); }
    printf("%s\n", ({ int __i0 = (int)(3); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (3); int __idx = __i0; _ltng_arr_get(m_data, m_len, __idx); }));
    { int __aidx = ({ int __i0 = (int)(atoi(ix)-2); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (3); int __idx = __i0; __idx; }); char __asv[256]; snprintf(__asv, 256, "%lld", (long long)(11)); _ltng_arr_set(m_data, &m_len, &m_cap, __aidx, __asv); }
    printf("%s\n", ({ int __i0 = (int)(1); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (3); int __idx = __i0; _ltng_arr_get(m_data, m_len, __idx); }));
    printf("%s\n", "set_array_ops");
    int sa_len = 0;
    int sa_cap = 0;
    char **sa_data = _ltng_arr_new(3);
    _ltng_arr_push(&sa_data, &sa_len, &sa_cap, "1");
    _ltng_arr_push(&sa_data, &sa_len, &sa_cap, "2");
    _ltng_arr_push(&sa_data, &sa_len, &sa_cap, "3");
    int sb_len = 0;
    int sb_cap = 0;
    char **sb_data = _ltng_arr_new(1);
    _ltng_arr_push(&sb_data, &sb_len, &sb_cap, "9");
    _ltng_arr_copy(sa_data, &sb_data, &sb_len, &sb_cap, sa_len);
    { char __apv[256]; snprintf(__apv, 256, "%lld", (long long)(4)); _ltng_arr_push(&sb_data, &sb_len, &sb_cap, __apv); }
    printf("%lld\n", (long long)(sa_len));
    printf("%s\n", ({ int __i0 = (int)(4); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (3); int __idx = __i0; _ltng_arr_get(sa_data, sa_len, __idx); }));
    printf("%lld\n", (long long)(sb_len));
    printf("%s\n", ({ int __i0 = (int)(4); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (1); int __idx = __i0; _ltng_arr_get(sb_data, sb_len, __idx); }));
    int sc_len = 0;
    int sc_cap = 0;
    char **sc_data = _ltng_arr_new(1);
    _ltng_arr_push(&sc_data, &sc_len, &sc_cap, "0");
    _ltng_arr_copy(sa_data, &sc_data, &sc_len, &sc_cap, sa_len);
    { int __aidx = ({ int __i0 = (int)(1); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (1); int __idx = __i0; __idx; }); char __asv[256]; snprintf(__asv, 256, "%lld", (long long)(99)); _ltng_arr_set(sc_data, &sc_len, &sc_cap, __aidx, __asv); }
    printf("%s\n", ({ int __i0 = (int)(1); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (3); int __idx = __i0; _ltng_arr_get(sa_data, sa_len, __idx); }));
    printf("%s\n", ({ int __i0 = (int)(1); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (1); int __idx = __i0; _ltng_arr_get(sc_data, sc_len, __idx); }));
    int sd_len = 0;
    int sd_cap = 0;
    char **sd_data = _ltng_arr_new(1);
    _ltng_arr_push(&sd_data, &sd_len, &sd_cap, "0");
    _ltng_arr_slice(sa_data, sa_len, 2,4, &sd_data, &sd_len, &sd_cap);
    printf("%lld\n", (long long)(sd_len));
    printf("%s\n", ({ int __i0 = (int)(1); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (1); int __idx = __i0; _ltng_arr_get(sd_data, sd_len, __idx); }));
    printf("%s\n", ({ int __i0 = (int)(2); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (1); int __idx = __i0; _ltng_arr_get(sd_data, sd_len, __idx); }));
    printf("%s\n", "iteration");
    int it_len = 0;
    int it_cap = 0;
    char **it_data = _ltng_arr_new(4);
    _ltng_arr_push(&it_data, &it_len, &it_cap, "5");
    _ltng_arr_push(&it_data, &it_len, &it_cap, "6");
    _ltng_arr_push(&it_data, &it_len, &it_cap, "7");
    _ltng_arr_push(&it_data, &it_len, &it_cap, "8");
    snprintf(i, 256, "1");
    while (atoi(i)<=it_len) {
        printf("%s\n", ({ int __i0 = (int)(atoi(i)); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (4); int __idx = __i0; _ltng_arr_get(it_data, it_len, __idx); }));
        { long long _e = (atoi(i)+1); snprintf(i, 256, "%lld", _e); }
    }
    {
        char j[256] = "0";
        snprintf(j, 256, "%lld", (long long)(j));
        for (; (long long)atoll(j) <= (long long)(it_len); snprintf(j, 256, "%lld", (long long)atoll(j) + (long long)(j))) {
            printf("%s\n", ({ int __i0 = (int)(atoi(j)); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (4); int __idx = __i0; _ltng_arr_get(it_data, it_len, __idx); }));
        }
    }
    {
        char val[256] = "";
        for (int __fe_i0 = 0; __fe_i0 < it_len; __fe_i0++) {
            snprintf(val, 256, "%s", val_data[__fe_i23521484] ? (null)_data[__fe_i23521484] : "");
            printf("%s\n", val);
        }
    }
    printf("%s\n", "empty_edges");
    int ee_len = 0;
    int ee_cap = 0;
    char **ee_data = _ltng_arr_new(1);
    printf("%s\n", ({ char *__r = _ltng_arr_pop(ee_data, &ee_len); __r; }));
    printf("%s\n", ({ char __cv[256]; snprintf(__cv, 256, "%lld", (long long)(1)); char *__r = _ltng_retbuf(); snprintf(__r, 256, "%s", _ltng_arr_contains(ee_data, ee_len, __cv) ? "true" : "false"); __r; }));
    printf("%s\n", ({ char __iv[256]; snprintf(__iv, 256, "%lld", (long long)(1)); char *__r = _ltng_retbuf(); snprintf(__r, 256, "%d", _ltng_arr_index_of(ee_data, ee_len, __iv)); __r; }));
    printf("%s\n", ({ char *__r = _ltng_arr_pop_index(ee_data, &ee_len, (int)(1)); __r; }));
    printf("%lld\n", (long long)(ee_len));
    printf("%s\n", "multidim");
    int md_len = 0;
    int md_cap = 0;
    char **md_data = _ltng_arr_new(4);
    _ltng_arr_push(&md_data, &md_len, &md_cap, "1");
    _ltng_arr_push(&md_data, &md_len, &md_cap, "2");
    _ltng_arr_push(&md_data, &md_len, &md_cap, "3");
    _ltng_arr_push(&md_data, &md_len, &md_cap, "4");
    printf("%s\n", ({ int __i0 = (int)(1); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (2); int __i1 = (int)(1); __i1 = __i1 > 0 ? __i1 - 1 : __i1 + (2); int __idx = __i0; __idx = __idx * 2 + __i1; _ltng_arr_get(md_data, md_len, __idx); }));
    printf("%s\n", ({ int __i0 = (int)(2); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (2); int __i1 = (int)(2); __i1 = __i1 > 0 ? __i1 - 1 : __i1 + (2); int __idx = __i0; __idx = __idx * 2 + __i1; _ltng_arr_get(md_data, md_len, __idx); }));
    printf("%s\n", ({ int __i0 = (int)(-1); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (2); int __i1 = (int)(-2); __i1 = __i1 > 0 ? __i1 - 1 : __i1 + (2); int __idx = __i0; __idx = __idx * 2 + __i1; _ltng_arr_get(md_data, md_len, __idx); }));
    { int __aidx = ({ int __i0 = (int)(1); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (2); int __i1 = (int)(2); __i1 = __i1 > 0 ? __i1 - 1 : __i1 + (2); int __idx = __i0; __idx = __idx * 2 + __i1; __idx; }); char __asv[256]; snprintf(__asv, 256, "%lld", (long long)(22)); _ltng_arr_set(md_data, &md_len, &md_cap, __aidx, __asv); }
    printf("%s\n", ({ int __i0 = (int)(1); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (2); int __i1 = (int)(2); __i1 = __i1 > 0 ? __i1 - 1 : __i1 + (2); int __idx = __i0; __idx = __idx * 2 + __i1; _ltng_arr_get(md_data, md_len, __idx); }));
    printf("%s\n", "array_test_done");
    return 0;
}
