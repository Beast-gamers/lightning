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
    char a1[256] = "0";
    char a2[256] = "0";
    char a3[256] = "";
    char a4[256] = "";
    char a5[256] = "false";
    char a6[256] = "false";
    char a7[256] = "false";
    char a8[256] = "false";
    char b1[256] = "0";
    char b2[256] = "0";
    char b3[256] = "";
    char b4[256] = "";
    char b5[256] = "false";
    char c1[256] = "0";
    char c2[256] = "0";
    char c3[256] = "";
    char c4[256] = "";
    char c5[256] = "false";
    char d1[256] = "0";
    char d2[256] = "0";
    char e1[256] = "0";
    char e2[256] = "0";
    char e3[256] = "0";
    char e4[256] = "0";
    char f1[256] = "0";
    char f2[256] = "0";
    char f3[256] = "0";
    char g1[256] = "0";
    char g2[256] = "0";
    char g3[256] = "0";
    char h1[256] = "";
    char h2[256] = "";
    char i1[256] = "false";
    char i2[256] = "";
    char i3[256] = "0";
    char i4[256] = "";
    char i5[256] = "0";
    char i6[256] = "";
    char j1[256] = "0";
    char j2[256] = "0";
    char j3[256] = "0";
    char k1[256] = "0";
    char k2[256] = "0";
    char l1[256] = "";
    char l2[256] = "";
    char l3[256] = "";
    char l4[256] = "";
    char l5[256] = "";

    snprintf(a1, sizeof(a1), "42");
    snprintf(a2, sizeof(a2), "3");
    snprintf(a3, sizeof(a3), "hello");
    snprintf(a4, sizeof(a4), "A");
    snprintf(a5, sizeof(a5), "true");
    snprintf(a6, sizeof(a6), "true");
    snprintf(a7, sizeof(a7), "false");
    snprintf(a8, sizeof(a8), "false");
    printf("%s\n", a1);
    printf("%s\n", a2);
    printf("%s\n", a3);
    printf("%s\n", a4);
    printf("%s\n", a5);
    printf("%s\n", a6);
    printf("%s\n", a7);
    printf("%s\n", a8);
    snprintf(b1, sizeof(b1), "0");
    snprintf(b2, sizeof(b2), "0");
    b3[0] = '\0';
    snprintf(b4, sizeof(b4), "Z");
    snprintf(b5, sizeof(b5), "true");
    printf("%s\n", b1);
    printf("%s\n", b2);
    printf("%s\n", b3);
    printf("%s\n", b4);
    printf("%s\n", b5);
    snprintf(c1, sizeof(c1), "0");
    snprintf(c2, sizeof(c2), "0");
    c3[0] = '\0';
    c4[0] = '\0';
    snprintf(c5, sizeof(c5), "false");
    printf("%s\n", c1);
    printf("%s\n", c2);
    printf("%s\n", c3);
    printf("%s\n", c4);
    printf("%s\n", c5);
    { long long _e = (-5); snprintf(d1, sizeof(d1), "%lld", _e); }
    snprintf(d2, sizeof(d2), "0");
    { long long _e = (-3.14); snprintf(d2, sizeof(d2), "%lld", _e); }
    printf("%s\n", d1);
    printf("%s\n", d2);
    { long long _e = (2+3); snprintf(e1, sizeof(e1), "%lld", _e); }
    { long long _e = (1.5+2.5); snprintf(e2, sizeof(e2), "%lld", _e); }
    { long long _e = (10*2-5); snprintf(e3, sizeof(e3), "%lld", _e); }
    { long long _e = ((1+2)*3); snprintf(e4, sizeof(e4), "%lld", _e); }
    printf("%s\n", e1);
    printf("%s\n", e2);
    printf("%s\n", e3);
    printf("%s\n", e4);
    snprintf(f1, sizeof(f1), "100");
    { long long _e = (atoi(f1)); snprintf(f2, sizeof(f2), "%lld", _e); }
    { long long _e = (atoi(f2)); snprintf(f3, sizeof(f3), "%lld", _e); }
    printf("%s\n", f1);
    printf("%s\n", f2);
    printf("%s\n", f3);
    snprintf(g1, sizeof(g1), "100");
    { long long _e = (atoi(g1)); snprintf(g2, sizeof(g2), "%lld", _e); }
    { long long _e = (atoi(g2)); snprintf(g3, sizeof(g3), "%lld", _e); }
    printf("%s\n", g1);
    printf("%s\n", g2);
    printf("%s\n", g3);
    snprintf(h1, sizeof(h1), "first");
    snprintf(h2, sizeof(h2), "%s", h1);
    printf("%s\n", h1);
    printf("%s\n", h2);
    snprintf(i1, sizeof(i1), "true");
    snprintf(i2, sizeof(i2), "A");
    { long long _e = ((i1[0]=='t'?1:0)+1); snprintf(i3, sizeof(i3), "%lld", _e); }
    snprintf(i4, sizeof(i4), "%s", ({ char _ltc[512]; snprintf(_ltc,512,"%s%s",i2,1); _ltc; }));
    { long long _e = ((i1[0]=='t'?1:0)*10); snprintf(i5, sizeof(i5), "%lld", _e); }
    snprintf(i6, sizeof(i6), "%s", ({ char _ltc[512]; snprintf(_ltc,512,"%s%s",i2,25); _ltc; }));
    printf("%s\n", i3);
    printf("%s\n", i4);
    printf("%s\n", i5);
    printf("%s\n", i6);
    snprintf(j1, sizeof(j1), "1");
    snprintf(j2, sizeof(j2), "2");
    snprintf(j3, sizeof(j3), "3");
    printf("%lld\n", (long long)(atoi(j1)+atoi(j2)+atoi(j3)));
    snprintf(k1, sizeof(k1), "999999");
    { long long _e = (-999999); snprintf(k2, sizeof(k2), "%lld", _e); }
    printf("%s\n", k1);
    printf("%s\n", k2);
    snprintf(l1, sizeof(l1), "\n");
    snprintf(l2, sizeof(l2), "\t");
    snprintf(l3, sizeof(l3), "\\");
    snprintf(l4, sizeof(l4), "\"");
    snprintf(l5, sizeof(l5), "'");
    printf("%s\n", l1);
    printf("%s\n", l2);
    printf("%s\n", l3);
    printf("%s\n", l4);
    printf("%s\n", l5);
    return 0;
}
