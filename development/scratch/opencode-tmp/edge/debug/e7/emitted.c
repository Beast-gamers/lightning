#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <ctype.h>
#include <setjmp.h>
static const char *_ltng_src_file = "/tmp/opencode/edge/e7.ltng";
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
    _ltng_throw_value("Error", "Unknown function in indirect call", _ltng_cl, _ltng_cc);
    return _ret;
}

int main(void) {
    _ltng_fn_push("main", 1, 1);
    char a[256] = "";
    char b[256] = "";

    snprintf(a, 256, "1");
    snprintf(b, 256, "2");
    printf("%s\n", _ltng_add_sn(_ltng_add_sn(({ char __ltcn[64]; snprintf(__ltcn,64,"%lld",(long long)((a + b) * 2)); __ltcn; }), ","), ({ char __ltcn[64]; snprintf(__ltcn,64,"%lld",(long long)((b + a) * 2)); __ltcn; })));
    return 0;
}
