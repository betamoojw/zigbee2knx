/********************************************************************
** Copyright (c) 2018-2020 Guan Wenliang
** This file is part of the Berry default interpreter.
** skiars@qq.com, https://github.com/Skiars/berry
** See Copyright Notice in the LICENSE file or at
** https://github.com/Skiars/berry/blob/master/LICENSE
********************************************************************/
#ifndef BE_MEM_H
#define BE_MEM_H

#include "berry.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define be_malloc(vm, size)         be_realloc((vm), NULL, 0, (size))
#define be_free(vm, ptr, size)      be_realloc((vm), (ptr), (size), 0)

BERRY_API void* be_os_malloc(size_t size);
BERRY_API void be_os_free(void *ptr);
BERRY_API void* be_os_realloc(void *ptr, size_t size);
BERRY_API void* be_realloc(bvm *vm, void *ptr, size_t old_size, size_t new_size);
BERRY_API void be_gc_memory_pools(bvm *vm);
BERRY_API void be_gc_free_memory_pools(bvm *vm);
BERRY_API void be_gc_init_memory_pools(bvm *vm);
BERRY_API void be_gc_memory_pools_info(bvm *vm, size_t* slots_used, size_t* slots_allocated);

/* OS-level accounting of every heap block the Berry allocator holds (pool pages
 * included), split internal RAM vs PSRAM.  All zero on non-ESP builds. */
typedef enum {
    BE_OS_MEM_INT_BYTES = 0,    /* bytes currently held in internal RAM   */
    BE_OS_MEM_EXT_BYTES,        /* bytes currently held in PSRAM          */
    BE_OS_MEM_INT_PEAK,         /* max of INT_BYTES since boot/reset      */
    BE_OS_MEM_EXT_PEAK,         /* max of EXT_BYTES since boot/reset      */
    BE_OS_MEM_TOTAL_PEAK,       /* max of INT_BYTES + EXT_BYTES           */
    BE_OS_MEM_BLOCKS,           /* number of live heap blocks             */
    BE_OS_MEM_LARGEST,          /* largest block handed out since reset   */
    BE_OS_MEM_FIELDS
} be_os_mem_field;
BERRY_API size_t be_os_mem_get(be_os_mem_field field);
BERRY_API void be_os_mem_reset_peak(void);
/* Opt-in: every Berry block prefers PSRAM (fallback internal).  Call before the VM exists. */
BERRY_API void be_os_mem_prefer_psram(bool on);

/* The following moves a portion of memory to constraint regions with 32-bits read/write acess */
/* Effective only if `BE_USE_MEM_ALIGNED` is set to `1`*/
BERRY_API void* be_move_to_aligned(bvm *vm, void *ptr, size_t size);

#ifdef __cplusplus
}
#endif

#endif
