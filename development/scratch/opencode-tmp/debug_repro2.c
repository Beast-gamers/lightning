#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <ctype.h>
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

int main(void) {
    char x[256] = "0";
    char y[256] = "0";

    x[0] = '\0';
    snprintf(y, sizeof(y), "0");
    { long long _e = ((long long)atoll("3")); snprintf(x, sizeof(x), "%lld", _e); }
    printf("%s\n", "int");
    { long long _e = (atoi(x)+3); snprintf(y, sizeof(y), "%lld", _e); }
    printf("%s\n", y);
    return 0;
}
