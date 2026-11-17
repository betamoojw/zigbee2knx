// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dirk Tostmann
// KGT2: the device template a KNX product carries behind its application parameters in the parameter segment
// (load object 4), so that ETS brings it into the gateway with every download. Container and checks follow
// tools/kgt.py, which ETS 6.4.1 passed on 2026-11-12:
//
//   segment = parameters | ... base64 text ... | "KGTE" + text offset, text length, parameter length (4 hex digits each)
//   blob    = "KGT2" | length of z (u32, big endian) | SHA-256(z)[:8] | key fingerprint (8) | r||s (64) | z
//   z       = zlib(payload); payload = JSON {schema "kgt/1", id, version, manifest, behaviour (Berry source), ..}
//
// The signature is ECDSA P-256 with SHA-256 over the uncompressed payload. The Berry VM is no sandbox, so nothing
// reaches it that a trusted key has not signed: kgt_open() does every check and hands out the payload only then.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_timer.h"
#include "miniz.h"        // tinfl of the ROM
#include "psa/crypto.h"
#include "app.h"

#define TRAILER     16
#define HEAD        88          // magic 4, length 4, checksum 8, fingerprint 8, signature 64
#define PAYLOAD_MAX 65536

// Keys whose signature makes a template loadable, as uncompressed P-256 points; the fingerprint is the first
// 8 octets of the SHA-256 over the point. For now only the development key that signs the templates in this
// repository (tools/kgt_trust.json, fingerprint 91a09904f3083dc6); a release key is still open.
static const uint8_t TRUST[][65] = {
    {0x04, 0x0a, 0x79, 0x97, 0x66, 0x18, 0xce, 0x3b, 0x5e, 0x1d, 0x76, 0x85, 0x56,
     0xd8, 0xb1, 0x03, 0x2f, 0x48, 0xcd, 0xc6, 0x8e, 0x5a, 0xea, 0x6e, 0x79, 0xa7,
     0xc7, 0xce, 0x5a, 0xd5, 0x7f, 0xf0, 0x72, 0x08, 0x5d, 0xa0, 0x16, 0xdf, 0xfb,
     0x8d, 0x29, 0x5d, 0xd5, 0x0d, 0x1c, 0x29, 0x4b, 0xf3, 0x26, 0x48, 0x85, 0x87,
     0x8d, 0xd2, 0xdf, 0xa7, 0x28, 0x8f, 0xa1, 0x75, 0x1a, 0x5f, 0xab, 0x56, 0x98},
};
#define NTRUST ((int)(sizeof TRUST / sizeof TRUST[0]))

static bool sha256(const void *p, size_t n, uint8_t out[32])
{
    size_t got = 0;
    return psa_hash_compute(PSA_ALG_SHA_256, p, n, out, 32, &got) == PSA_SUCCESS && got == 32;
}

static void hex8(const uint8_t *p, char out[17])
{
    for (int i = 0; i < 8; i++) {
        sprintf(out + 2 * i, "%02x", p[i]);
    }
}

// strict, as base64.b64decode(validate=True): the alphabet and padding at the very end only. Octets written, -1: no
static int b64(const uint8_t *in, size_t n, uint8_t *out)
{
    if (n % 4) {
        return -1;
    }
    size_t o = 0;
    for (size_t i = 0; i < n; i += 4) {
        uint32_t acc = 0;
        int pad = 0;
        for (int j = 0; j < 4; j++) {
            int c = in[i + j], v;
            if (c == '=' && i + 4 == n && j >= 2) {
                v = 0;
                pad++;
            } else if (pad) {
                return -1;
            } else if (c >= 'A' && c <= 'Z') {
                v = c - 'A';
            } else if (c >= 'a' && c <= 'z') {
                v = c - 'a' + 26;
            } else if (c >= '0' && c <= '9') {
                v = c - '0' + 52;
            } else if (c == '+') {
                v = 62;
            } else if (c == '/') {
                v = 63;
            } else {
                return -1;
            }
            acc = acc << 6 | (uint32_t)v;
        }
        out[o++] = (uint8_t)(acc >> 16);
        if (pad < 2) {
            out[o++] = (uint8_t)(acc >> 8);
        }
        if (pad < 1) {
            out[o++] = (uint8_t)acc;
        }
    }
    return (int)o;
}

static const uint8_t *trusted(const uint8_t fpr[8])
{
    for (int i = 0; i < NTRUST; i++) {
        uint8_t h[32];
        if (sha256(TRUST[i], 65, h) && memcmp(h, fpr, 8) == 0) {
            return TRUST[i];
        }
    }
    return NULL;
}

// 1: the signature is that key's, 0: it is not, -1: the check itself could not run
static int verify(const uint8_t *point, const uint8_t hash[32], const uint8_t sig[64])
{
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&a, PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&a, 256);
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_VERIFY_HASH);
    psa_set_key_algorithm(&a, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    psa_key_id_t key = 0;
    if (psa_import_key(&a, point, 65, &key) != PSA_SUCCESS) {
        return -1;
    }
    psa_status_t st = psa_verify_hash(key, PSA_ALG_ECDSA(PSA_ALG_SHA_256), hash, 32, sig, 64);
    psa_destroy_key(key);
    return st == PSA_SUCCESS ? 1 : st == PSA_ERROR_INVALID_SIGNATURE ? 0 : -1;
}

// zlib stream -> heap buffer of at most PAYLOAD_MAX octets. The decompressor state is about 11 KiB: on the heap
static uint8_t *inflate(const uint8_t *z, size_t zn, size_t *out_n, int *status, bool *oom)
{
    tinfl_decompressor *r = malloc(sizeof *r);
    size_t cap = zn * 6 + 1024;
    uint8_t *out = NULL;
    *status = TINFL_STATUS_FAILED;
    *oom = !r;
    while (r) {
        if (cap > PAYLOAD_MAX) {
            cap = PAYLOAD_MAX;
        }
        uint8_t *p = realloc(out, cap);
        if (!p) {
            *oom = true;
            break;
        }
        out = p;
        size_t in_n = zn, got = cap;
        tinfl_init(r);
        *status = tinfl_decompress(r, z, &in_n, out, out, &got,
                                   TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
        if (*status == TINFL_STATUS_DONE) {
            free(r);
            p = realloc(out, got ? got : 1);   // give back what the guess was too large by
            *out_n = got;
            return p ? p : out;
        }
        if (*status != TINFL_STATUS_HAS_MORE_OUTPUT || cap >= PAYLOAD_MAX) {
            break;
        }
        cap = PAYLOAD_MAX;   // did not fit: once more from the start with the largest buffer
    }
    free(r);
    free(out);
    return NULL;
}

int kgt_open(const uint8_t *seg, size_t n, uint8_t **payload, size_t *plen, kgt_info_t *in, char *why, size_t wn)
{
    static bool s_init;
    int64_t t0 = esp_timer_get_time();
    *payload = NULL;
    *plen = 0;
    memset(in, 0, sizeof *in);
    why[0] = 0;
    if (n < TRAILER || memcmp(seg + n - TRAILER, "KGTE", 4) != 0) {
        return KGT_NONE;   // a product without a template: the whole segment is parameters
    }
    uint32_t v[3] = {0, 0, 0};
    for (int i = 0; i < 12; i++) {
        int c = seg[n - 12 + i];
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) {
            snprintf(why, wn, "trailer malformed");
            in->err = KNX_ERR_CRC;
            return KGT_REFUSED;
        }
        v[i / 4] = v[i / 4] << 4 | (uint32_t)d;
    }
    in->text_off = v[0];
    in->text_len = v[1];
    in->params = v[2];
    if (!(v[2] <= v[0] && v[0] + v[1] <= n - TRAILER)) {
        snprintf(why, wn, "trailer points outside the segment of %u octets", (unsigned)n);
        in->err = KNX_ERR_CRC;
        return KGT_REFUSED;
    }
    if (!s_init) {
        if (psa_crypto_init() != PSA_SUCCESS) {
            snprintf(why, wn, "no crypto");
            in->err = KNX_ERR_SOFTWARE;
            return KGT_REFUSED;
        }
        s_init = true;
    }
    uint8_t *blob = malloc(v[1] / 4 * 3 + 3), *out = NULL, h[32];
    int r = KGT_REFUSED, bl = blob ? b64(seg + v[0], v[1], blob) : -1, st = 0, sig;
    size_t on = 0;
    bool oom = false;
    // what a refusal means for the load state (PID 28): damaged, not acceptable, or no room to check it
    in->err = KNX_ERR_CRC;
    if (!blob) {
        snprintf(why, wn, "no memory for %u characters", (unsigned)v[1]);
        in->err = KNX_ERR_NO_RAM;
    } else if (bl < 0) {
        snprintf(why, wn, "text is not base64");
    } else if (bl >= 16 && memcmp(blob, "KGT1", 4) == 0) {
        snprintf(why, wn, "unsigned template (KGT1)");
        in->err = KNX_ERR_CONFIG;
    } else if (bl < HEAD || memcmp(blob, "KGT2", 4) != 0) {
        snprintf(why, wn, "no KGT magic");
    } else {
        uint32_t zn = (uint32_t)blob[4] << 24 | (uint32_t)blob[5] << 16 | (uint32_t)blob[6] << 8 | blob[7];
        const uint8_t *z = blob + HEAD, *key;
        in->z = zn;
        hex8(blob + 16, in->fpr);
        if ((uint32_t)(bl - HEAD) < zn) {
            snprintf(why, wn, "payload %u octets, header says %u", (unsigned)(bl - HEAD), (unsigned)zn);
        } else if (!sha256(z, zn, h)) {   // a failing check is not a verdict on the content: say which it was
            snprintf(why, wn, "the checksum could not be computed");
            in->err = KNX_ERR_SOFTWARE;
        } else if (memcmp(h, blob + 8, 8) != 0) {
            snprintf(why, wn, "payload checksum differs");
        } else if (!(key = trusted(blob + 16))) {
            snprintf(why, wn, "signed by unknown key %s", in->fpr);
            in->err = KNX_ERR_CONFIG;
        } else if (!(out = inflate(z, zn, &on, &st, &oom))) {
            if (oom) {
                snprintf(why, wn, "no memory to unpack %u octets", (unsigned)zn);
                in->err = KNX_ERR_NO_RAM;
            } else {
                snprintf(why, wn, "payload unreadable (inflate status %d)", st);
            }
        } else if (!sha256(out, on, h) || (sig = verify(key, h, blob + 24)) < 0) {
            snprintf(why, wn, "the signature could not be checked");
            in->err = KNX_ERR_SOFTWARE;
        } else if (!sig) {
            snprintf(why, wn, "signature of key %s does not match", in->fpr);
            in->err = KNX_ERR_CONFIG;
        } else {
            hex8(h, in->digest);
            in->payload = (uint32_t)on;
            *payload = out;
            *plen = on;
            out = NULL;
            in->err = KNX_ERR_NONE;
            r = KGT_OK;
        }
    }
    free(out);
    free(blob);
    in->us = (uint32_t)(esp_timer_get_time() - t0);
    return r;
}
