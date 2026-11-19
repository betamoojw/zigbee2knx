// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dirk Tostmann
// The behaviour of a template, taken out of its payload in C (bvm.c tpl_load_vm()), so that Berry compiles it straight
// from the payload's own buffer: before 2026-11-19 the whole payload went into a Berry string and json.load() made the
// behaviour into a second and third block of its length (a buffer and a string), which a fragmented heap could not
// always give (tplmem.h). Kept apart so that the host tests run it (test/host/kgtjson_test.c).
//
// The payload is JSON as tools/kgt.py payload_json() writes it: one object, keys sorted, no blanks, ASCII only
// (ensure_ascii), "behaviour" a string. It is signed, so these functions see nothing a key holder did not make; they
// still check every step and give up on anything else, and the template is then refused as before.
#ifndef KGTJSON_H
#define KGTJSON_H

#include <stddef.h>
#include <string.h>

static inline size_t kgtj_ws(const char *s, size_t n, size_t i)
{
    while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) {
        i++;
    }
    return i;
}

// s[i] is the opening quote of a string: the index of its closing quote, or n
static inline size_t kgtj_strend(const char *s, size_t n, size_t i)
{
    for (i++; i < n; i++) {
        if (s[i] == '\\') {
            i++;
        } else if (s[i] == '"') {
            return i;
        }
    }
    return n;
}

// a value from s[i] on (blanks skipped before): the index after it, or n if it does not end
static inline size_t kgtj_skip(const char *s, size_t n, size_t i)
{
    i = kgtj_ws(s, n, i);
    if (i >= n) {
        return n;
    }
    if (s[i] == '"') {
        size_t e = kgtj_strend(s, n, i);
        return e < n ? e + 1 : n;
    }
    if (s[i] == '{' || s[i] == '[') {
        int depth = 0;
        for (; i < n; i++) {
            if (s[i] == '"') {
                i = kgtj_strend(s, n, i);
                if (i >= n) {
                    return n;
                }
            } else if (s[i] == '{' || s[i] == '[') {
                depth++;
            } else if (s[i] == '}' || s[i] == ']') {
                if (--depth == 0) {
                    return i + 1;
                }
            }
        }
        return n;
    }
    while (i < n && s[i] != ',' && s[i] != '}' && s[i] != ']' && s[i] != ' ' && s[i] != '\t' && s[i] != '\n' &&
           s[i] != '\r') {
        i++;   // number, true, false, null
    }
    return i;
}

// The string value of the key "behaviour" of the top-level object: *vs and *ve bracket its content (the quotes left
// out, escapes still in). 0, or -1 if the payload is no object or has no such string. A key written with escapes
// does not match; kgt.py writes none.
static inline int kgtj_behaviour(const char *s, size_t n, size_t *vs, size_t *ve)
{
    static const char key[] = "behaviour";
    size_t i = kgtj_ws(s, n, 0);
    if (i >= n || s[i] != '{') {
        return -1;
    }
    i++;
    for (;;) {
        i = kgtj_ws(s, n, i);
        if (i >= n || s[i] != '"') {
            return -1;   // also "}": the object ended without it
        }
        size_t ke = kgtj_strend(s, n, i);
        if (ke >= n) {
            return -1;
        }
        int match = ke - i - 1 == sizeof key - 1 && memcmp(s + i + 1, key, sizeof key - 1) == 0;
        i = kgtj_ws(s, n, ke + 1);
        if (i >= n || s[i] != ':') {
            return -1;
        }
        i = kgtj_ws(s, n, i + 1);
        if (match) {
            if (i >= n || s[i] != '"') {
                return -1;
            }
            size_t e = kgtj_strend(s, n, i);
            if (e >= n) {
                return -1;
            }
            *vs = i + 1;
            *ve = e;
            return 0;
        }
        i = kgtj_skip(s, n, i);
        i = kgtj_ws(s, n, i);
        if (i >= n || s[i] != ',') {
            return -1;
        }
        i++;
    }
}

// \uXXXX as Berry's json.load() takes it (be_lexer.c be_load_unicode(), its edges kept: every escape on its own, no
// surrogate pairs, 0x7F as two octets), so that the behaviour compiles to the same source as before. The end of the
// output, or NULL if the four characters are no hexadecimal number.
static inline char *kgtj_unicode(char *dst, const char *src)
{
    int ucode = 0;
    for (int i = 0; i < 4; i++) {
        int ch = (unsigned char)src[i];
        if (ch >= '0' && ch <= '9') {
            ucode = (ucode << 4) | (ch - '0');
        } else if (ch >= 'A' && ch <= 'F') {
            ucode = (ucode << 4) | (ch - 'A' + 0x0A);
        } else if (ch >= 'a' && ch <= 'f') {
            ucode = (ucode << 4) | (ch - 'a' + 0x0A);
        } else {
            return NULL;
        }
    }
    if (ucode < 0x007F) {
        *dst++ = (char)(ucode & 0x7F);
    } else if (ucode < 0x7FF) {
        *dst++ = (char)(((ucode >> 6) & 0x1F) | 0xC0);
        *dst++ = (char)((ucode & 0x3F) | 0x80);
    } else {
        *dst++ = (char)(((ucode >> 12) & 0x0F) | 0xE0);
        *dst++ = (char)(((ucode >> 6) & 0x3F) | 0x80);
        *dst++ = (char)((ucode & 0x3F) | 0x80);
    }
    return dst;
}

// The content of a JSON string (s, n octets, quotes left out) unescaped in place, as Berry's json.load() does it
// (be_jsonlib.c parser_string()): the escapes " \ / b f n r t u, nothing else, no control character unescaped. The
// output is never longer than what it was read from, so writing over it is safe. Its length, or -1.
static inline long kgtj_unescape(char *s, size_t n)
{
    char *dst = s;
    size_t i = 0;
    while (i < n) {
        unsigned char ch = (unsigned char)s[i++];
        if (ch == '\\') {
            if (i >= n) {
                return -1;
            }
            ch = (unsigned char)s[i++];
            switch (ch) {
            case '"': *dst++ = '"'; break;
            case '\\': *dst++ = '\\'; break;
            case '/': *dst++ = '/'; break;
            case 'b': *dst++ = '\b'; break;
            case 'f': *dst++ = '\f'; break;
            case 'n': *dst++ = '\n'; break;
            case 'r': *dst++ = '\r'; break;
            case 't': *dst++ = '\t'; break;
            case 'u':
                if (n - i < 4) {
                    return -1;
                }
                dst = kgtj_unicode(dst, s + i);
                if (!dst) {
                    return -1;
                }
                i += 4;
                break;
            default:
                return -1;
            }
        } else if (ch <= 0x1f) {
            return -1;
        } else {
            *dst++ = (char)ch;
        }
    }
    return (long)(dst - s);
}

#endif
