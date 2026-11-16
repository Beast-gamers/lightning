#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <ctype.h>
static char *_ltng_concat(char *buf, size_t sz, int n, ...) { va_list ap; va_start(ap,n); buf[0]=0; for(int i=0;i<n;i++){ const char *s=va_arg(ap,const char*); strncat(buf,s,sz-strlen(buf)-1); } va_end(ap); return buf; }
static char *_ltng_repeat(char *buf, size_t sz, const char *s, long long n) { buf[0]=0; if (n < 0) n = 0; for (long long i=0; i<n && strlen(buf) < sz-1; i++) { strncat(buf, s, sz-strlen(buf)-1); } return buf; }
static char *_ltng_retbuf(void){ static char b[32][256]; static int i; i=(i+1)%32; b[i][0]=0; return b[i]; }
static char *_ltng_ad(double v){ char *b=_ltng_retbuf(); snprintf(b,256,"%g",v); return b; }
static char *_ltng_as(const char *s){ char *b=_ltng_retbuf(); if(s){ strncpy(b,s,255); b[255]=0; } return b; }
static char *_ltng_from_int(char *buf, long long v){ snprintf(buf,256,"%lld",v); return buf; }
static char *_ltng_from_double(char *buf, double v){ snprintf(buf,256,"%g",v); return buf; }
static int _ltng_is_num_str(const char *s){ if(!s||!*s) return 0; const char *p=s; if(*p=='-'||*p=='+') p++; int h=0; for(;*p;p++){ if(isdigit(*p)) h=1; else if(*p=='.'){} else return 0; } return h; }
static char *_ltng_add_sn(const char *a, const char *b){ static char buf[32][256]; static int _ltng_asn_idx=0; _ltng_asn_idx=(_ltng_asn_idx+1)%32; char *r=buf[_ltng_asn_idx]; r[0]=0; if(_ltng_is_num_str(a)&&_ltng_is_num_str(b)){ double na=atof(a),nb=atof(b); snprintf(r,256,"%g",na+nb); } else { snprintf(r,256,"%s%s",a,b); } return r; }

int main(void) {
    printf("3.03428e-86\n", (double)(-3.14));
    return 0;
}
