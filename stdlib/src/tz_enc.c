/* enc.c — hashing and text encodings */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include "tz_std.h"

long long tz_hash_djb2(const char *s) {
    uint64_t h = 5381;
    for (; *s; s++) h = h * 33 + (unsigned char)*s;
    return (long long)(h & 0x7fffffffffffffffULL);
}
long long tz_hash_fnv1a(const char *s) {
    uint64_t h = 1469598103934665603ULL;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ULL; }
    return (long long)(h & 0x7fffffffffffffffULL);
}
long long tz_crc32(const char *s) {
    uint32_t c = 0xffffffffu;
    for (; *s; s++) {
        c ^= (unsigned char)*s;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
    }
    return (long long)(c ^ 0xffffffffu);
}

/* SHA-256 */
static const uint32_t K[64] = {
 0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
 0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
 0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
 0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
 0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
 0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
 0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
 0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sha_block(uint32_t h[8], const unsigned char *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4*i] << 24 | (uint32_t)p[4*i+1] << 16 | (uint32_t)p[4*i+2] << 8 | p[4*i+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i-15], 7) ^ ROR(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = ROR(w[i-2], 17) ^ ROR(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = hh + S1 + ch + K[i] + w[i];
        uint32_t S0 = ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}
const char *tz_sha256(const char *s) {
    uint32_t h[8] = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 };
    size_t n = strlen(s), i;
    for (i = 0; i + 64 <= n; i += 64) sha_block(h, (const unsigned char *)s + i);
    unsigned char tail[128]; size_t rem = n - i;
    memset(tail, 0, sizeof tail);
    memcpy(tail, s + i, rem);
    tail[rem] = 0x80;
    size_t tl = rem + 9 <= 64 ? 64 : 128;
    uint64_t bits = (uint64_t)n * 8;
    for (int k = 0; k < 8; k++) tail[tl - 1 - k] = (unsigned char)(bits >> (8 * k));
    for (size_t k = 0; k < tl; k += 64) sha_block(h, tail + k);
    char out[65];
    for (int k = 0; k < 8; k++) snprintf(out + 8 * k, 9, "%08x", h[k]);
    return tz_dup(out, 64);
}

/* base64 */
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
const char *tz_base64_encode(const char *s) {
    size_t n = strlen(s), o = 0;
    char *r = (char *)malloc((n + 2) / 3 * 4 + 1);
    if (!r) exit(1);
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)(unsigned char)s[i] << 16;
        if (i + 1 < n) v |= (uint32_t)(unsigned char)s[i + 1] << 8;
        if (i + 2 < n) v |= (unsigned char)s[i + 2];
        r[o++] = B64[v >> 18 & 63]; r[o++] = B64[v >> 12 & 63];
        r[o++] = i + 1 < n ? B64[v >> 6 & 63] : '=';
        r[o++] = i + 2 < n ? B64[v & 63] : '=';
    }
    r[o] = '\0';
    return r;
}
const char *tz_base64_decode(const char *s) {
    size_t n = strlen(s), o = 0;
    char *r = (char *)malloc(n / 4 * 3 + 4);
    if (!r) exit(1);
    uint32_t v = 0; int bits = 0;
    for (size_t i = 0; i < n; i++) {
        const char *p = s[i] == '=' ? NULL : strchr(B64, s[i]);
        if (s[i] == '=') break;
        if (!p || !s[i]) continue;                    /* skip whitespace and junk */
        v = v << 6 | (uint32_t)(p - B64); bits += 6;
        if (bits >= 8) { bits -= 8; r[o++] = (char)(v >> bits & 0xff); }
    }
    r[o] = '\0';
    return r;
}

/* hex and url */
const char *tz_hex_encode(const char *s) {
    size_t n = strlen(s);
    char *r = (char *)malloc(n * 2 + 1);
    if (!r) exit(1);
    for (size_t i = 0; i < n; i++) snprintf(r + 2 * i, 3, "%02x", (unsigned char)s[i]);
    r[n * 2] = '\0';
    return r;
}
static int hexv(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
         : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}
const char *tz_hex_decode(const char *s) {
    size_t n = strlen(s), o = 0;
    char *r = (char *)malloc(n / 2 + 1);
    if (!r) exit(1);
    for (size_t i = 0; i + 1 < n; i += 2) {
        int a = hexv(s[i]), b = hexv(s[i + 1]);
        if (a < 0 || b < 0) break;
        r[o++] = (char)(a * 16 + b);
    }
    r[o] = '\0';
    return r;
}
const char *tz_url_encode(const char *s) {
    size_t n = strlen(s), o = 0;
    char *r = (char *)malloc(n * 3 + 1);
    if (!r) exit(1);
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') r[o++] = (char)c;
        else { snprintf(r + o, 4, "%%%02X", c); o += 3; }
    }
    r[o] = '\0';
    return r;
}
const char *tz_url_decode(const char *s) {
    size_t n = strlen(s), o = 0;
    char *r = (char *)malloc(n + 1);
    if (!r) exit(1);
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '%' && i + 2 < n + 0 + 1 && hexv(s[i + 1]) >= 0 && hexv(s[i + 2]) >= 0) {
            r[o++] = (char)(hexv(s[i + 1]) * 16 + hexv(s[i + 2])); i += 2;
        } else r[o++] = s[i] == '+' ? ' ' : s[i];
    }
    r[o] = '\0';
    return r;
}
