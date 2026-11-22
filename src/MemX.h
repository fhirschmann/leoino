#pragma once

#include <stddef.h>
#include <stdint.h>

char *x_calloc(uint32_t _allocSize, uint32_t _unitSize);
void *x_malloc(uint32_t _allocSize);
void *x_realloc(void *_ptr, size_t _newSize);
char *x_strdup(const char *_str);
