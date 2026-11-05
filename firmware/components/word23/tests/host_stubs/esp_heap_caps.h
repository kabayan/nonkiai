#pragma once
#include <stddef.h>
#include <stdint.h>
#define MALLOC_CAP_INTERNAL 1u
#define MALLOC_CAP_SPIRAM 2u
#define MALLOC_CAP_8BIT 4u
extern "C" void *heap_caps_aligned_alloc(size_t alignment, size_t bytes, uint32_t caps);
extern "C" void heap_caps_free(void *pointer);
