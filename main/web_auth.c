#include "web_auth.h"
#include <ctype.h>
#include <string.h>
#include <strings.h>

static const char *skip_ws(const char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

// Length of the item at s up to the next ';' (or the end), trailing blanks dropped.
static size_t item_len(const char *s)
{
    size_t n = strcspn(s, ";");
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
    return n;
}

// Nothing, or ":<1..5 digits>", then the end of the value.
static bool port_ok(const char *s)
{
    if (*s == ':') {
        int n = 0;
        for (s++; *s >= '0' && *s <= '9'; s++) n++;
        if (n < 1 || n > 5) return false;
    }
    return *skip_ws(s) == '\0';
}

bool web_host_ok(const char *h)
{
    if (!h) return true;                     // HTTP/1.0 clients may send none
    h = skip_ws(h);
    if (*h == '[') {                         // [IPv6]
        const char *e = strchr(h, ']');
        if (!e || e == h + 1) return false;
        for (const char *p = h + 1; p < e; p++)
            if (!isxdigit((unsigned char)*p) && *p != ':' && *p != '.') return false;
        return port_ok(e + 1);
    }
    for (int i = 0; i < 4; i++) {            // a.b.c.d, each 0..255
        if (i && *h++ != '.') return false;
        int v = 0, n = 0;
        for (; *h >= '0' && *h <= '9' && n < 3; h++, n++) v = v * 10 + (*h - '0');
        if (!n || v > 255) return false;
    }
    return port_ok(h);
}

bool web_ct_ok(const char *ct)
{
    static const char *const OK[] = { "application/json", "application/octet-stream" };
    if (!ct) return false;
    ct = skip_ws(ct);
    size_t n = item_len(ct);
    for (unsigned i = 0; i < sizeof OK / sizeof OK[0]; i++)
        if (n == strlen(OK[i]) && strncasecmp(ct, OK[i], n) == 0) return true;
    return false;
}

bool web_pass_valid(const char *p)
{
    size_t n = strlen(p);
    if (n < WEB_PASS_MIN || n > WEB_PASS_MAX) return false;
    for (; *p; p++)
        if ((unsigned char)*p < 0x20 || (unsigned char)*p > 0x7e) return false;
    return true;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool web_cookie_token(const char *c, uint8_t tok[WEB_TOKEN_LEN])
{
    while (c && *c) {
        c = skip_ws(c);
        size_t n = item_len(c);
        if (n == 7 + 2 * WEB_TOKEN_LEN && strncmp(c, "oclean=", 7) == 0) {
            bool ok = true;
            for (int i = 0; ok && i < WEB_TOKEN_LEN; i++) {
                int hi = hexval(c[7 + 2 * i]), lo = hexval(c[8 + 2 * i]);
                ok = hi >= 0 && lo >= 0;
                if (ok) tok[i] = (uint8_t)(hi << 4 | lo);
            }
            if (ok) return true;
        }
        c += strcspn(c, ";");
        if (*c == ';') c++;
    }
    return false;
}

static int b64val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

bool web_basic_pass(const char *a, char out[WEB_PASS_MAX + 1])
{
    if (!a) return false;
    a = skip_ws(a);
    if (strncasecmp(a, "Basic", 5) != 0 || (a[5] != ' ' && a[5] != '\t')) return false;
    a = skip_ws(a + 5);
    uint8_t buf[192];                        // the decoded "user:password"
    size_t n = 0;
    uint32_t acc = 0;
    int bits = 0, pad = 0;
    for (; *a && *a != ' ' && *a != '\t'; a++) {
        if (*a == '=') { pad++; continue; }  // padding, only at the end
        int v = b64val(*a);
        if (v < 0 || pad) return false;
        acc = acc << 6 | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n == sizeof buf) return false;
            buf[n++] = (uint8_t)(acc >> bits);
        }
    }
    if (*skip_ws(a) != '\0') return false;
    const uint8_t *colon = memchr(buf, ':', n);
    if (!colon) return false;
    size_t len = n - (size_t)(colon + 1 - buf);
    if (len > WEB_PASS_MAX || memchr(colon + 1, '\0', len)) return false;
    memcpy(out, colon + 1, len);
    out[len] = '\0';
    return true;
}
