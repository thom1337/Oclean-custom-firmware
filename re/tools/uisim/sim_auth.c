// Host test of the web password's header checks (main/web_auth.c).
//   cc -std=gnu11 -Wall -Wextra -I main re/tools/uisim/sim_auth.c main/web_auth.c -o sim_auth && ./sim_auth
// The inputs are the header values real clients send (Chrome / Firefox fetch, curl,
// Python urllib), plus the ones an attacker would try: a DNS name that rebinds to the
// brush, a cross-site "simple" content type that merely mentions JSON, malformed
// credentials. Exit status is non-zero when a check fails.
#include <stdio.h>
#include <string.h>
#include "web_auth.h"

static int s_fail, s_checks;
#define CHECK(c) do { s_checks++; if (!(c)) { s_fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

// "Basic " + base64(s), as curl -u, urllib and the login form's btoa() build it.
static const char *basic(const char *s)
{
    static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    static char out[512];
    size_t n = strlen(s), o = 0;
    o += (size_t)sprintf(out, "Basic ");
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = (unsigned char)s[i] << 16 | (i + 1 < n ? (unsigned char)s[i + 1] << 8 : 0) | (i + 2 < n ? (unsigned char)s[i + 2] : 0);
        out[o++] = A[v >> 18 & 63];
        out[o++] = A[v >> 12 & 63];
        out[o++] = i + 1 < n ? A[v >> 6 & 63] : '=';
        out[o++] = i + 2 < n ? A[v & 63] : '=';
    }
    out[o] = '\0';
    return out;
}

static bool pass_is(const char *authz, const char *want)
{
    char p[WEB_PASS_MAX + 1];
    return web_basic_pass(authz, p) && strcmp(p, want) == 0;
}

int main(void)
{
    // ---- Host ----
    CHECK(web_host_ok(NULL));                                // HTTP/1.0, no Host
    CHECK(web_host_ok("192.168.4.1"));                       // the setup AP
    CHECK(web_host_ok("10.0.0.23"));
    CHECK(web_host_ok("10.0.0.23:80"));                      // some clients add the default port
    CHECK(web_host_ok(" 10.0.0.23 "));
    CHECK(web_host_ok("[fe80::1a2b:3c4d]"));
    CHECK(web_host_ok("[fe80::1]:8080"));
    CHECK(!web_host_ok("oclean.lan"));                       // names are refused (DNS rebinding)
    CHECK(!web_host_ok("10.0.0.23.nip.io"));                 // a name that looks like an address
    CHECK(!web_host_ok("evil.example:80"));
    CHECK(!web_host_ok("localhost"));
    CHECK(!web_host_ok("10.0.0"));
    CHECK(!web_host_ok("10.0.0.256"));
    CHECK(!web_host_ok("10.0.0.1234"));
    CHECK(!web_host_ok("10.0.0.23:"));
    CHECK(!web_host_ok("10.0.0.23:123456"));
    CHECK(!web_host_ok("10.0.0.23x"));
    CHECK(!web_host_ok("[]"));
    CHECK(!web_host_ok("[fe80::1"));
    CHECK(!web_host_ok("[fe80::g]"));

    // ---- Content-Type of a POST ----
    CHECK(web_ct_ok("application/json"));                    // app.js fetch
    CHECK(web_ct_ok("application/json; charset=utf-8"));
    CHECK(web_ct_ok("Application/JSON"));
    CHECK(web_ct_ok("application/octet-stream"));            // the OTA upload (XHR, curl -H)
    CHECK(web_ct_ok(" application/json ;"));
    CHECK(!web_ct_ok(NULL));
    CHECK(!web_ct_ok(""));
    CHECK(!web_ct_ok("text/plain"));                         // what a cross-site form or fetch can send freely
    CHECK(!web_ct_ok("text/plain; x=application/json"));
    CHECK(!web_ct_ok("text/plain;charset=UTF-8"));
    CHECK(!web_ct_ok("application/x-www-form-urlencoded"));  // curl -d without -H
    CHECK(!web_ct_ok("multipart/form-data; boundary=----x"));
    CHECK(!web_ct_ok("application/jsonp"));
    CHECK(!web_ct_ok("application/json-patch+json"));

    // ---- password rules ----
    CHECK(web_pass_valid("abcdefghijkl"));                   // 12
    CHECK(web_pass_valid("correct horse battery"));
    CHECK(web_pass_valid("0123456789012345678901234567890123456789012345678901234567890123"));    // 64
    CHECK(!web_pass_valid("abcdefghijk"));                   // 11
    CHECK(!web_pass_valid(""));
    CHECK(!web_pass_valid("01234567890123456789012345678901234567890123456789012345678901234"));  // 65
    CHECK(!web_pass_valid("abcdefghijk\x7f"));
    CHECK(!web_pass_valid("abcdefghijk\t"));
    CHECK(!web_pass_valid("p\xc3\xa4sswordpassword"));      // UTF-8: btoa() would throw on it

    // ---- session cookie ----
    uint8_t t[WEB_TOKEN_LEN];
    CHECK(web_cookie_token("oclean=00112233445566778899aabbccddeeff", t) && t[0] == 0x00 && t[1] == 0x11 && t[15] == 0xff);
    CHECK(web_cookie_token("theme=dark; oclean=00112233445566778899AABBCCDDEEFF; x=1", t) && t[10] == 0xaa);
    CHECK(web_cookie_token("oclean=zz; oclean=0123456789abcdef0123456789abcdef", t) && t[0] == 0x01);
    CHECK(!web_cookie_token(NULL, t));
    CHECK(!web_cookie_token("", t));
    CHECK(!web_cookie_token("oclean=0011", t));
    CHECK(!web_cookie_token("oclean=00112233445566778899aabbccddeeff00", t));
    CHECK(!web_cookie_token("xoclean=00112233445566778899aabbccddeeff", t));
    CHECK(!web_cookie_token("oclean=0011223344556677 8899aabbccddeef", t));
    CHECK(!web_cookie_token("oclean=g0112233445566778899aabbccddeeff", t));

    // ---- Authorization: Basic ----
    CHECK(pass_is(basic("oclean:hunter2hunter2"), "hunter2hunter2"));      // curl -u oclean
    CHECK(pass_is(basic(":hunter2hunter2"), "hunter2hunter2"));            // no user name
    CHECK(pass_is(basic("anyone:pa:ss:with:colons"), "pa:ss:with:colons"));
    CHECK(pass_is(basic("oclean:a b c"), "a b c"));
    CHECK(pass_is(basic("oclean:0123456789012345678901234567890123456789012345678901234567890123"),
                  "0123456789012345678901234567890123456789012345678901234567890123"));
    CHECK(pass_is("basic b2NsZWFuOmhlbGxv", "hello"));       // the scheme is case-insensitive
    CHECK(pass_is("Basic  b2NsZWFuOmhlbGxv ", "hello"));
    CHECK(pass_is(basic("oclean:"), ""));                    // empty: the caller's length rule refuses it
    char p[WEB_PASS_MAX + 1];
    CHECK(!web_basic_pass(NULL, p));
    CHECK(!web_basic_pass("", p));
    CHECK(!web_basic_pass("Bearer b2NsZWFuOmhlbGxv", p));
    CHECK(!web_basic_pass("Basic", p));
    CHECK(!web_basic_pass("Basicb2NsZWFuOmhlbGxv", p));
    CHECK(!web_basic_pass(basic("ocleanhello"), p));         // no ':'
    CHECK(!web_basic_pass("Basic b2Ns*WFuOmhlbGxv", p));
    CHECK(!web_basic_pass("Basic b2NsZWFuOmhlbGxv x", p));
    CHECK(!web_basic_pass("Basic b2Ns=ZWFuOmhlbGxv", p));    // padding in the middle
    CHECK(!web_basic_pass(basic("oclean:01234567890123456789012345678901234567890123456789012345678901234"), p));   // 65
    CHECK(!web_basic_pass(basic("oclean:a\0b"), p) || strcmp(p, "a") == 0);   // C string: stops at the NUL anyway
    {   // longer than the decode buffer
        char big[300]; memset(big, 'a', sizeof big - 1); big[sizeof big - 1] = '\0'; big[0] = ':';
        CHECK(!web_basic_pass(basic(big), p));
    }

    printf("%d checks, %d failed\n", s_checks, s_fail);
    return s_fail != 0;
}
