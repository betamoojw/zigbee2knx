// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dirk Tostmann
/* be_port_idf.c — Berry-VM port glue for ESP-IDF.
 *
 * Routes Berry's `be_writebuffer` to esp_rom_printf so script `print(...)`
 * lines show up in the UART/USB-Serial console (cheap, no esp_log
 * formatting overhead).  `be_readstring` returns NULL because we don't run
 * an interactive REPL.  The filesystem helpers are stubs — the firmware does
 * no file I/O through Berry (BE_USE_FILE_SYSTEM=0), so Berry's internal
 * `be_f*` symbols stay unused at runtime.
 */

#include <stddef.h>
#include "berry.h"
#include "be_sys.h"
#include "esp_rom_sys.h"   /* esp_rom_printf — minimal, no flushing tricks */

BERRY_API void be_writebuffer(const char *buffer, size_t length)
{
    if (!buffer || !length) return;
    /* esp_rom_printf does not understand %.*s — write byte by byte to keep
     * binary output (\n etc.) intact.  Berry only emits short strings here
     * (print + traceback frames), so the per-char cost is negligible. */
    for (size_t i = 0; i < length; ++i) {
        esp_rom_printf("%c", buffer[i]);
    }
}

BERRY_API char *be_readstring(char *buffer, size_t size)
{
    (void)buffer;
    (void)size;
    return NULL;
}

/* ----- FS stubs (BE_USE_FILE_SYSTEM=0, never called at runtime) ---------- */

void  *be_fopen(const char *filename, const char *modes) { (void)filename; (void)modes; return NULL; }
int    be_fclose(void *hfile)                            { (void)hfile; return -1; }
size_t be_fwrite(void *hfile, const void *buffer, size_t length) { (void)hfile; (void)buffer; return length; }
size_t be_fread(void *hfile, void *buffer, size_t length)        { (void)hfile; (void)buffer; (void)length; return 0; }
char  *be_fgets(void *hfile, void *buffer, int size)             { (void)hfile; (void)buffer; (void)size; return NULL; }
int    be_fseek(void *hfile, long offset)                        { (void)hfile; (void)offset; return -1; }
long   be_ftell(void *hfile)                                     { (void)hfile; return -1; }
long   be_fflush(void *hfile)                                    { (void)hfile; return 0; }
size_t be_fsize(void *hfile)                                     { (void)hfile; return 0; }

int    be_isdir(const char *path)    { (void)path; return 0; }
int    be_isfile(const char *path)   { (void)path; return 0; }
int    be_isexist(const char *path)  { (void)path; return 0; }
char  *be_getcwd(char *buf, size_t size) { (void)size; if (buf) buf[0] = '\0'; return buf; }
int    be_chdir(const char *path)    { (void)path; return -1; }
int    be_mkdir(const char *path)    { (void)path; return -1; }
int    be_unlink(const char *filename){ (void)filename; return -1; }
int    be_dirfirst(bdirinfo *info, const char *path) { (void)info; (void)path; return -1; }
int    be_dirnext(bdirinfo *info)  { (void)info; return -1; }
int    be_dirdestroy(bdirinfo *info){ (void)info; return -1; }
