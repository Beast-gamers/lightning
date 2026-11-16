#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <ctype.h>
static char *_ltng_concat(char *buf, size_t sz, int n, ...) { va_list ap; va_start(ap,n); buf[0]=0; for(int i=0;i<n;i++){ const char *s=va_arg(ap,const char*); strncat(buf,s,sz-strlen(buf)-1); } va_end(ap); return buf; }
static char *_ltng_repeat(char *buf, size_t sz, const char *s, long long n) { buf[0]=0; if (n < 0) n = 0; for (long long i=0; i<n && strlen(buf) < sz-1; i++) { strncat(buf, s, sz-strlen(buf)-1); } return buf; }
static char *_ltng_arr_get(char data[][256], int len, int idx) { if (idx >= 0 && idx < len) return data[idx]; if (idx < 0 && len + idx >= 0) return data[len + idx]; return ""; }
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
    char idx[256] = "0";
    char total[256] = "";

    char arr_data[3][256] = {"1","",""};
    int arr_len = 3;
    printf("%s\n", ({ int __i0 = (int)(1); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (3); int __idx = __i0; _ltng_arr_get(arr_data, arr_len, __idx); }));
    printf("%s\n", ({ int __i0 = (int)(2); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (3); int __idx = __i0; _ltng_arr_get(arr_data, arr_len, __idx); }));
    printf("%s\n", ({ int __i0 = (int)(3); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (3); int __idx = __i0; _ltng_arr_get(arr_data, arr_len, __idx); }));
    printf("%s\n", ({ int __i0 = (int)(-1); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (3); int __idx = __i0; _ltng_arr_get(arr_data, arr_len, __idx); }));
    printf("%s\n", ({ int __i0 = (int)(-2); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (3); int __idx = __i0; _ltng_arr_get(arr_data, arr_len, __idx); }));
    printf("%s\n", ({ int __i0 = (int)(-3); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (3); int __idx = __i0; _ltng_arr_get(arr_data, arr_len, __idx); }));
    char names_data[3][256] = {"alice","ob","harlie"};
    int names_len = 3;
    printf("%s\n", ({ int __i0 = (int)(1); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (3); int __idx = __i0; _ltng_arr_get(names_data, names_len, __idx); }));
    printf("%s\n", ({ int __i0 = (int)(2); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (3); int __idx = __i0; _ltng_arr_get(names_data, names_len, __idx); }));
    printf("%s\n", ({ int __i0 = (int)(3); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (3); int __idx = __i0; _ltng_arr_get(names_data, names_len, __idx); }));
    printf("%s\n", ({ int __i0 = (int)(-1); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (3); int __idx = __i0; _ltng_arr_get(names_data, names_len, __idx); }));
    char c_data[3][256] = {"10","0","0"};
    int c_len = 3;
    printf("%s\n", ({ int __i0 = (int)(1); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (3); int __idx = __i0; _ltng_arr_get(c_data, c_len, __idx); }));
    printf("%s\n", ({ int __i0 = (int)(2); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (3); int __idx = __i0; _ltng_arr_get(c_data, c_len, __idx); }));
    printf("%s\n", ({ int __i0 = (int)(3); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (3); int __idx = __i0; _ltng_arr_get(c_data, c_len, __idx); }));
    char m_data[6][256] = {"1","","","","",""};
    int m_len = 6;
    printf("%s\n", ({ int __i0 = (int)(1); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (2); int __i1 = (int)(1); __i1 = __i1 > 0 ? __i1 - 1 : __i1 + (3); int __idx = _ltng_idx2(__i0, __i1, 3); _ltng_arr_get(m_data, m_len, __idx); }));
    printf("%s\n", ({ int __i0 = (int)(1); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (2); int __i1 = (int)(2); __i1 = __i1 > 0 ? __i1 - 1 : __i1 + (3); int __idx = _ltng_idx2(__i0, __i1, 3); _ltng_arr_get(m_data, m_len, __idx); }));
    printf("%s\n", ({ int __i0 = (int)(1); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (2); int __i1 = (int)(3); __i1 = __i1 > 0 ? __i1 - 1 : __i1 + (3); int __idx = _ltng_idx2(__i0, __i1, 3); _ltng_arr_get(m_data, m_len, __idx); }));
    printf("%s\n", ({ int __i0 = (int)(2); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (2); int __i1 = (int)(1); __i1 = __i1 > 0 ? __i1 - 1 : __i1 + (3); int __idx = _ltng_idx2(__i0, __i1, 3); _ltng_arr_get(m_data, m_len, __idx); }));
    printf("%s\n", ({ int __i0 = (int)(2); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (2); int __i1 = (int)(2); __i1 = __i1 > 0 ? __i1 - 1 : __i1 + (3); int __idx = _ltng_idx2(__i0, __i1, 3); _ltng_arr_get(m_data, m_len, __idx); }));
    printf("%s\n", ({ int __i0 = (int)(2); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (2); int __i1 = (int)(3); __i1 = __i1 > 0 ? __i1 - 1 : __i1 + (3); int __idx = _ltng_idx2(__i0, __i1, 3); _ltng_arr_get(m_data, m_len, __idx); }));
    printf("%s\n", ({ int __i0 = (int)(-1); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (2); int __i1 = (int)(-1); __i1 = __i1 > 0 ? __i1 - 1 : __i1 + (3); int __idx = _ltng_idx2(__i0, __i1, 3); _ltng_arr_get(m_data, m_len, __idx); }));
    printf("%s\n", ({ int __i0 = (int)(-1); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (2); int __i1 = (int)(-2); __i1 = __i1 > 0 ? __i1 - 1 : __i1 + (3); int __idx = _ltng_idx2(__i0, __i1, 3); _ltng_arr_get(m_data, m_len, __idx); }));
    char names2d_data[4][256] = {"a","","",""};
    int names2d_len = 4;
    printf("%s\n", ({ int __i0 = (int)(1); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (2); int __i1 = (int)(1); __i1 = __i1 > 0 ? __i1 - 1 : __i1 + (2); int __idx = _ltng_idx2(__i0, __i1, 2); _ltng_arr_get(names2d_data, names2d_len, __idx); }));
    printf("%s\n", ({ int __i0 = (int)(1); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (2); int __i1 = (int)(2); __i1 = __i1 > 0 ? __i1 - 1 : __i1 + (2); int __idx = _ltng_idx2(__i0, __i1, 2); _ltng_arr_get(names2d_data, names2d_len, __idx); }));
    printf("%s\n", ({ int __i0 = (int)(2); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (2); int __i1 = (int)(1); __i1 = __i1 > 0 ? __i1 - 1 : __i1 + (2); int __idx = _ltng_idx2(__i0, __i1, 2); _ltng_arr_get(names2d_data, names2d_len, __idx); }));
    printf("%s\n", ({ int __i0 = (int)(2); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (2); int __i1 = (int)(2); __i1 = __i1 > 0 ? __i1 - 1 : __i1 + (2); int __idx = _ltng_idx2(__i0, __i1, 2); _ltng_arr_get(names2d_data, names2d_len, __idx); }));
    char cube_data[8][256] = {"1","","","","","","",""};
    int cube_len = 8;
    printf("%s\n", ({ int __i0 = (int)(1); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (2); int __i1 = (int)(1); __i1 = __i1 > 0 ? __i1 - 1 : __i1 + (2); int __i2 = (int)(1); __i2 = __i2 > 0 ? __i2 - 1 : __i2 + (2); int __idx = _ltng_idx3(__i0, __i1, __i2, 2, 2); _ltng_arr_get(cube_data, cube_len, __idx); }));
    printf("%s\n", ({ int __i0 = (int)(1); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (2); int __i1 = (int)(1); __i1 = __i1 > 0 ? __i1 - 1 : __i1 + (2); int __i2 = (int)(2); __i2 = __i2 > 0 ? __i2 - 1 : __i2 + (2); int __idx = _ltng_idx3(__i0, __i1, __i2, 2, 2); _ltng_arr_get(cube_data, cube_len, __idx); }));
    printf("%s\n", ({ int __i0 = (int)(1); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (2); int __i1 = (int)(2); __i1 = __i1 > 0 ? __i1 - 1 : __i1 + (2); int __i2 = (int)(1); __i2 = __i2 > 0 ? __i2 - 1 : __i2 + (2); int __idx = _ltng_idx3(__i0, __i1, __i2, 2, 2); _ltng_arr_get(cube_data, cube_len, __idx); }));
    printf("%s\n", ({ int __i0 = (int)(1); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (2); int __i1 = (int)(2); __i1 = __i1 > 0 ? __i1 - 1 : __i1 + (2); int __i2 = (int)(2); __i2 = __i2 > 0 ? __i2 - 1 : __i2 + (2); int __idx = _ltng_idx3(__i0, __i1, __i2, 2, 2); _ltng_arr_get(cube_data, cube_len, __idx); }));
    printf("%s\n", ({ int __i0 = (int)(2); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (2); int __i1 = (int)(1); __i1 = __i1 > 0 ? __i1 - 1 : __i1 + (2); int __i2 = (int)(1); __i2 = __i2 > 0 ? __i2 - 1 : __i2 + (2); int __idx = _ltng_idx3(__i0, __i1, __i2, 2, 2); _ltng_arr_get(cube_data, cube_len, __idx); }));
    printf("%s\n", ({ int __i0 = (int)(2); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (2); int __i1 = (int)(1); __i1 = __i1 > 0 ? __i1 - 1 : __i1 + (2); int __i2 = (int)(2); __i2 = __i2 > 0 ? __i2 - 1 : __i2 + (2); int __idx = _ltng_idx3(__i0, __i1, __i2, 2, 2); _ltng_arr_get(cube_data, cube_len, __idx); }));
    printf("%s\n", ({ int __i0 = (int)(2); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (2); int __i1 = (int)(2); __i1 = __i1 > 0 ? __i1 - 1 : __i1 + (2); int __i2 = (int)(1); __i2 = __i2 > 0 ? __i2 - 1 : __i2 + (2); int __idx = _ltng_idx3(__i0, __i1, __i2, 2, 2); _ltng_arr_get(cube_data, cube_len, __idx); }));
    printf("%s\n", ({ int __i0 = (int)(2); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (2); int __i1 = (int)(2); __i1 = __i1 > 0 ? __i1 - 1 : __i1 + (2); int __i2 = (int)(2); __i2 = __i2 > 0 ? __i2 - 1 : __i2 + (2); int __idx = _ltng_idx3(__i0, __i1, __i2, 2, 2); _ltng_arr_get(cube_data, cube_len, __idx); }));
    printf("%s\n", ({ int __i0 = (int)(-1); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (2); int __i1 = (int)(-1); __i1 = __i1 > 0 ? __i1 - 1 : __i1 + (2); int __i2 = (int)(-1); __i2 = __i2 > 0 ? __i2 - 1 : __i2 + (2); int __idx = _ltng_idx3(__i0, __i1, __i2, 2, 2); _ltng_arr_get(cube_data, cube_len, __idx); }));
    printf("%s\n", ({ int __i0 = (int)(-1); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (2); int __i1 = (int)(-1); __i1 = __i1 > 0 ? __i1 - 1 : __i1 + (2); int __i2 = (int)(-2); __i2 = __i2 > 0 ? __i2 - 1 : __i2 + (2); int __idx = _ltng_idx3(__i0, __i1, __i2, 2, 2); _ltng_arr_get(cube_data, cube_len, __idx); }));
    printf("%s\n", ({ int __i0 = (int)(-1); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (2); int __i1 = (int)(-2); __i1 = __i1 > 0 ? __i1 - 1 : __i1 + (2); int __i2 = (int)(-1); __i2 = __i2 > 0 ? __i2 - 1 : __i2 + (2); int __idx = _ltng_idx3(__i0, __i1, __i2, 2, 2); _ltng_arr_get(cube_data, cube_len, __idx); }));
    printf("%s\n", ({ int __i0 = (int)(-2); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (2); int __i1 = (int)(-1); __i1 = __i1 > 0 ? __i1 - 1 : __i1 + (2); int __i2 = (int)(-1); __i2 = __i2 > 0 ? __i2 - 1 : __i2 + (2); int __idx = _ltng_idx3(__i0, __i1, __i2, 2, 2); _ltng_arr_get(cube_data, cube_len, __idx); }));
    snprintf(idx, sizeof(idx), "2");
    printf("%s\n", ({ int __i0 = (int)(atoi(idx)+1); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (3); int __idx = __i0; _ltng_arr_get(arr_data, arr_len, __idx); }));
    printf("%s\n", ({ int __i0 = (int)(atoi(idx)-1); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (3); int __idx = __i0; _ltng_arr_get(arr_data, arr_len, __idx); }));
    printf("%s\n", ({ int __i0 = (int)(-atoi(idx)); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (3); int __idx = __i0; _ltng_arr_get(arr_data, arr_len, __idx); }));
    snprintf(total, sizeof(total), "%s", _ltng_add_sn(_ltng_add_sn(({ int __i0 = (int)(1); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (3); int __idx = __i0; _ltng_arr_get(arr_data, arr_len, __idx); }), ({ int __i0 = (int)(2); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (3); int __idx = __i0; _ltng_arr_get(arr_data, arr_len, __idx); })), ({ int __i0 = (int)(3); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (3); int __idx = __i0; _ltng_arr_get(arr_data, arr_len, __idx); })));
    printf("%s\n", total);
    printf("%s\n", _ltng_add_sn(({ int __i0 = (int)(1); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (2); int __i1 = (int)(1); __i1 = __i1 > 0 ? __i1 - 1 : __i1 + (3); int __idx = _ltng_idx2(__i0, __i1, 3); _ltng_arr_get(m_data, m_len, __idx); }), ({ int __i0 = (int)(2); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (2); int __i1 = (int)(3); __i1 = __i1 > 0 ? __i1 - 1 : __i1 + (3); int __idx = _ltng_idx2(__i0, __i1, 3); _ltng_arr_get(m_data, m_len, __idx); })));
    printf("%s\n", _ltng_add_sn(({ int __i0 = (int)(1); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (2); int __i1 = (int)(1); __i1 = __i1 > 0 ? __i1 - 1 : __i1 + (2); int __i2 = (int)(1); __i2 = __i2 > 0 ? __i2 - 1 : __i2 + (2); int __idx = _ltng_idx3(__i0, __i1, __i2, 2, 2); _ltng_arr_get(cube_data, cube_len, __idx); }), ({ int __i0 = (int)(2); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (2); int __i1 = (int)(2); __i1 = __i1 > 0 ? __i1 - 1 : __i1 + (2); int __i2 = (int)(2); __i2 = __i2 > 0 ? __i2 - 1 : __i2 + (2); int __idx = _ltng_idx3(__i0, __i1, __i2, 2, 2); _ltng_arr_get(cube_data, cube_len, __idx); })));
    printf("%s\n", _ltng_add_sn("sum: ", ({ int __i0 = (int)(3); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (3); int __idx = __i0; _ltng_arr_get(arr_data, arr_len, __idx); })));
    printf("first=%s last=%s\n", ({ int __i0 = (int)(1); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (3); int __idx = __i0; _ltng_arr_get(names_data, names_len, __idx); }), ({ int __i0 = (int)(-1); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (3); int __idx = __i0; _ltng_arr_get(names_data, names_len, __idx); }));
    printf("m22=%s cube222=%s\n", ({ int __i0 = (int)(2); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (2); int __i1 = (int)(2); __i1 = __i1 > 0 ? __i1 - 1 : __i1 + (3); int __idx = _ltng_idx2(__i0, __i1, 3); _ltng_arr_get(m_data, m_len, __idx); }), ({ int __i0 = (int)(2); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (2); int __i1 = (int)(2); __i1 = __i1 > 0 ? __i1 - 1 : __i1 + (2); int __i2 = (int)(2); __i2 = __i2 > 0 ? __i2 - 1 : __i2 + (2); int __idx = _ltng_idx3(__i0, __i1, __i2, 2, 2); _ltng_arr_get(cube_data, cube_len, __idx); }));
    printf("%s\n", _ltng_add_sn(_ltng_add_sn(({ int __i0 = (int)(1); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (2); int __i1 = (int)(1); __i1 = __i1 > 0 ? __i1 - 1 : __i1 + (2); int __idx = _ltng_idx2(__i0, __i1, 2); _ltng_arr_get(names2d_data, names2d_len, __idx); }), "/"), ({ int __i0 = (int)(2); __i0 = __i0 > 0 ? __i0 - 1 : __i0 + (2); int __i1 = (int)(2); __i1 = __i1 > 0 ? __i1 - 1 : __i1 + (2); int __idx = _ltng_idx2(__i0, __i1, 2); _ltng_arr_get(names2d_data, names2d_len, __idx); })));
    return 0;
}
