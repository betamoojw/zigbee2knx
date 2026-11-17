#pragma once
#include <stddef.h>
#include <stdint.h>
// what gw.c reports of the heap
#define MALLOC_CAP_8BIT (1 << 2)
size_t heap_caps_get_free_size(uint32_t caps);
size_t heap_caps_get_minimum_free_size(uint32_t caps);
size_t heap_caps_get_largest_free_block(uint32_t caps);
