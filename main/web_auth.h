#pragma once
#include <stdbool.h>
#include <stdint.h>

// The request checks of the web password (web_server.c): the parts that only read
// header values. No ESP-IDF in here, so the host test re/tools/uisim/sim_auth.c can
// feed them the headers real browsers, curl and urllib send.

#define WEB_TOKEN_LEN  16    // bytes of the stored session token, and of the cookie value made from it (32 hex digits)
#define WEB_PASS_MIN   12
#define WEB_PASS_MAX   64

// Host header, NULL if there is none. True for none, an IPv4 address or an [IPv6]
// literal, each with an optional :port. A host name is refused: a DNS-rebinding page
// always sends its own.
bool web_host_ok(const char *host);

// Content-Type of a POST, NULL if there is none. True only for application/json and
// application/octet-stream (parameters after ';' ignored). Another site's page can
// send neither without a CORS preflight, which this server never answers.
bool web_ct_ok(const char *ct);

// A password the web UI takes: 12..64 printable ASCII characters (what btoa() in the
// browser and the JSON of the settings form carry byte for byte).
bool web_pass_valid(const char *p);

// The value of the "oclean=<32 hex digits>" cookie in a Cookie header: not the session
// token itself but its HMAC (session_of() in web_server.c). False if there is none.
bool web_cookie_token(const char *cookie, uint8_t tok[WEB_TOKEN_LEN]);

// The password of an "Authorization: Basic ..." value: what follows the first ':'
// of the decoded credentials (the user name is ignored), NUL-terminated into
// out[WEB_PASS_MAX + 1]. False if absent, malformed or longer than WEB_PASS_MAX.
bool web_basic_pass(const char *authz, char out[WEB_PASS_MAX + 1]);
