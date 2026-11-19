// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dirk Tostmann
// Host test of main/kgtjson.h: the behaviour of a template found in its payload (the top-level key only, strings with
// braces and quotes in them, blanks) and unescaped in place as Berry's json.load() does it, its edges included (every
// \u on its own, 0x7F as two octets). Ends with "kgtjson checks: N passed, M failed" and exits non-zero on a failure.
// That the four templates given out come out byte for byte as Python's json.loads() reads them was checked on
// 2026-11-19 with the load sequence of bvm.c on the PC (review G).
#include <stdio.h>
#include <string.h>
#include "kgtjson.h"

static int g_pass, g_fail;
static void ck(int ok, const char *what)
{
    if (ok) {
        g_pass++;
    } else {
        g_fail++;
        printf("FAIL: %s\n", what);
    }
}

// the behaviour of payload p, unescaped; returns its length, -1 if not found, -2 if it does not unescape
static long behaviour(const char *p, char *out, size_t on)
{
    char buf[512];
    size_t n = strlen(p), vs, ve;
    memcpy(buf, p, n);
    if (kgtj_behaviour(buf, n, &vs, &ve) != 0) {
        return -1;
    }
    long len = kgtj_unescape(buf + vs, ve - vs);
    if (len < 0) {
        return -2;
    }
    if ((size_t)len < on) {
        memcpy(out, buf + vs, (size_t)len);
        out[len] = 0;
    }
    return len;
}

static void same(const char *payload, const char *want, size_t wn, const char *what)
{
    char out[512];
    long len = behaviour(payload, out, sizeof out);
    ck(len == (long)wn && memcmp(out, want, wn) == 0, what);
}

int main(void)
{
    // as tools/kgt.py writes it: keys sorted, no blanks, behaviour first
    same("{\"behaviour\":\"def f()\\n  return 1\\nend\\n\",\"behaviour_file\":\"x.be\",\"id\":\"t\"}",
         "def f()\n  return 1\nend\n", 23, "the payload as kgt.py writes it");
    // the rest of the object around it stays as it is (bvm.c copies [0, vs) and [ve, n))
    {
        const char *p = "{\"a\":1,\"behaviour\":\"x\\\"y\",\"z\":[2]}";
        size_t vs, ve;
        ck(kgtj_behaviour(p, strlen(p), &vs, &ve) == 0 && vs == 20 && ve == 24 && p[vs - 1] == '"' && p[ve] == '"',
           "the bounds are the string's content, the quotes outside");
    }
    same("{\"a\":1,\"b\":[1,{\"behaviour\":\"no\"}],\"behaviour\":\"yes\"}", "yes", 3,
         "a key inside a nested value does not count");
    same("{\"x\":\"}{\\\"behaviour\\\":\\\"bad\\\"\",\"behaviour\":\"ok\"}", "ok", 2,
         "braces and quoted quotes inside a string are no structure");
    same("{\"m\":{\"behaviour\":{\"language\":\"berry\"}},\"behaviour\":\"src\"}", "src", 3,
         "the manifest's own key behaviour (an object) is not the source");
    same(" { \"behaviour\" : \"w\" , \"id\" : 1 } ", "w", 1, "blanks around keys and values");
    same("{\"n\":-1.5e3,\"t\":true,\"f\":false,\"z\":null,\"behaviour\":\"v\"}", "v", 1, "numbers and literals skipped");
    ck(behaviour("{\"a\":\"b\"}", NULL, 0) == -1, "no behaviour: not found");
    ck(behaviour("[1,2]", NULL, 0) == -1, "no object: not found");
    ck(behaviour("{\"behaviour\":5}", NULL, 0) == -1, "behaviour no string: not found");
    ck(behaviour("{\"behaviour\":\"abc", NULL, 0) == -1, "behaviour without its end: not found");
    ck(behaviour("{\"a\":[1,2", NULL, 0) == -1, "an object that breaks off: not found");
    ck(behaviour("{\"behavio\\u0075r\":\"x\"}", NULL, 0) == -1, "a key written with escapes does not match");
    ck(behaviour("", NULL, 0) == -1, "nothing: not found");
    // the escapes Berry's json.load() takes
    same("{\"behaviour\":\"\\\"\\\\\\/\\b\\f\\n\\r\\t\"}", "\"\\/\b\f\n\r\t", 8, "the eight short escapes");
    same("{\"behaviour\":\"\\u0041\\u00e9\\u20ac\"}", "A\xc3\xa9\xe2\x82\xac", 6, "\\u to UTF-8 in one, two, three octets");
    same("{\"behaviour\":\"\\u007f\"}", "\xc1\xbf", 2, "0x7F as two octets, as Berry does it");
    same("{\"behaviour\":\"\\ud83d\\ude00\"}", "\xed\xa0\xbd\xed\xb8\x80", 6,
         "a surrogate pair as two three-octet sequences, as Berry does it");
    same("{\"behaviour\":\"\\u00E9\"}", "\xc3\xa9", 2, "upper case hexadecimal digits");
    ck(behaviour("{\"behaviour\":\"\\x41\"}", NULL, 0) == -2, "an escape Berry does not take: refused");
    ck(behaviour("{\"behaviour\":\"\\u12g4\"}", NULL, 0) == -2, "\\u with a character that is no digit: refused");
    ck(behaviour("{\"behaviour\":\"\\u12\"}", NULL, 0) == -2, "\\u cut short: refused");
    ck(behaviour("{\"behaviour\":\"a\x01" "b\"}", NULL, 0) == -2, "a control character not escaped: refused");
    // in place: the output never overtakes the input
    {
        char s[] = "\\u20ac\\u20ac\\n";
        long len = kgtj_unescape(s, strlen(s));
        ck(len == 7 && memcmp(s, "\xe2\x82\xac\xe2\x82\xac\n", 7) == 0, "three-octet output in place, twice in a row");
    }
    printf("kgtjson checks: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
