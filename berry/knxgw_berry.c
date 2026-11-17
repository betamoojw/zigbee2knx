/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 Dirk Tostmann
 *
 * knxgw-berry: the Berry host the template tests run in, the firmware's Berry VM as a program for the PC.
 *
 *   knxgw-berry PRELUDE.be TEMPLATE.be [MORE.be ...]
 *
 * Loads the files into one Berry VM in the order given, then calls the prelude's _main(). _main() reads
 * one JSON request per line from stdin and answers with JSON lines on stdout (berry/PROTOCOL.md).
 * stdout carries the protocol only: print() and Berry's tracebacks go to stderr.
 *
 * The VM is the one the firmware uses: Berry sources and berry_conf.h come unchanged from
 * firmware/components/berry (build_hostvm.sh), only the port functions below differ
 * (be_port_idf.c is left out). The host.* API of the templates is the Berry code in prelude.be,
 * which the firmware loads unchanged as well.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "berry.h"
#include "be_sys.h"

#ifndef KNXGW_REV
#define KNXGW_REV "unknown"
#endif

/* ---------------------------------------------------------------- port: console + file system stubs */

BERRY_API void be_writebuffer(const char *buffer, size_t length)
{
    if (buffer && length) {
        fwrite(buffer, 1, length, stderr);
    }
}

/* input() is not part of the protocol (_in() is). Answer "" at once, so that a template calling input()
 * cannot block the host: m_readline pushes the buffer even when nothing was read. */
BERRY_API char *be_readstring(char *buffer, size_t size)
{
    if (buffer && size) {
        buffer[0] = '\0';
    }
    return NULL;
}

void  *be_fopen(const char *filename, const char *modes) { (void)filename; (void)modes; return NULL; }
int    be_fclose(void *hfile)                            { (void)hfile; return -1; }
size_t be_fwrite(void *hfile, const void *buffer, size_t length) { (void)hfile; (void)buffer; return length; }
size_t be_fread(void *hfile, void *buffer, size_t length)        { (void)hfile; (void)buffer; (void)length; return 0; }
char  *be_fgets(void *hfile, void *buffer, int size)             { (void)hfile; (void)buffer; (void)size; return NULL; }
int    be_fseek(void *hfile, long offset)                        { (void)hfile; (void)offset; return -1; }
long   be_ftell(void *hfile)                                     { (void)hfile; return -1; }
long   be_fflush(void *hfile)                                    { (void)hfile; return 0; }
size_t be_fsize(void *hfile)                                     { (void)hfile; return 0; }
int    be_isdir(const char *path)     { (void)path; return 0; }
int    be_isfile(const char *path)    { (void)path; return 0; }
int    be_isexist(const char *path)   { (void)path; return 0; }
char  *be_getcwd(char *buf, size_t size) { (void)size; if (buf) buf[0] = '\0'; return buf; }
int    be_chdir(const char *path)     { (void)path; return -1; }
int    be_mkdir(const char *path)     { (void)path; return -1; }
int    be_unlink(const char *filename){ (void)filename; return -1; }
int    be_dirfirst(bdirinfo *info, const char *path) { (void)info; (void)path; return -1; }
int    be_dirnext(bdirinfo *info)     { (void)info; return -1; }
int    be_dirdestroy(bdirinfo *info)  { (void)info; return -1; }

/* ---------------------------------------------------------------- natives the prelude uses */

/* _in() -> the next line of stdin without its line end, or nil at the end of input */
static int m_in(bvm *vm)
{
    char *line = NULL;
    size_t cap = 0;
    ssize_t n = getline(&line, &cap, stdin);
    if (n < 0) {
        free(line);
        be_return_nil(vm);
    }
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
        line[--n] = '\0';
    }
    be_pushnstring(vm, line, (size_t)n);
    free(line);
    be_return(vm);
}

/* _out(s): s as one protocol line on stdout, flushed */
static int m_out(bvm *vm)
{
    if (be_top(vm) >= 1 && be_isstring(vm, 1)) {
        fputs(be_tostring(vm, 1), stdout);
        fputc('\n', stdout);
        fflush(stdout);
    }
    be_return_nil(vm);
}

/* _now_ms() -> monotonic milliseconds */
static int m_now_ms(bvm *vm)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    be_pushint(vm, (bint)ts.tv_sec * 1000 + (bint)(ts.tv_nsec / 1000000));
    be_return(vm);
}

/* ---------------------------------------------------------------- loading */

static int load_file(bvm *vm, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "knxgw-berry: cannot open %s\n", path);
        return 1;
    }
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    size_t n;
    while (buf && (n = fread(buf + len, 1, cap - len, f)) > 0) {
        len += n;
        if (len == cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) {
                free(buf);
                buf = NULL;
            } else {
                buf = nb;
            }
        }
    }
    fclose(f);
    if (!buf) {
        fprintf(stderr, "knxgw-berry: out of memory reading %s\n", path);
        return 1;
    }
    int r = be_loadbuffer(vm, path, buf, len);
    free(buf);
    if (r == 0) {
        r = be_pcall(vm, 0);
    }
    if (r != 0) {
        fprintf(stderr, "knxgw-berry: %s failed to load:\n", path);
        be_dumpexcept(vm);
        return 1;
    }
    be_pop(vm, 1);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        printf("knxgw-berry %s (Berry %s)\n", KNXGW_REV, BERRY_VERSION);
        return 0;
    }
    if (argc < 2) {
        fprintf(stderr, "usage: knxgw-berry PRELUDE.be TEMPLATE.be [MORE.be ...]\n");
        return 2;
    }
    bvm *vm = be_vm_new();
    be_regfunc(vm, "_in", m_in);
    be_regfunc(vm, "_out", m_out);
    be_regfunc(vm, "_now_ms", m_now_ms);
    for (int i = 1; i < argc; i++) {
        if (load_file(vm, argv[i])) {
            be_vm_delete(vm);
            return 2;
        }
    }
    if (!be_getglobal(vm, "_main") || !be_isfunction(vm, -1)) {
        fprintf(stderr, "knxgw-berry: the prelude defines no _main()\n");
        be_vm_delete(vm);
        return 2;
    }
    int r = be_pcall(vm, 0);
    if (r != 0) {
        be_dumpexcept(vm);
    }
    be_vm_delete(vm);
    return r ? 3 : 0;
}
