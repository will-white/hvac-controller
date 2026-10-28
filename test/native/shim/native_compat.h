// Force-included into every native translation unit (see Makefile).
#pragma once
#include <cstddef>

// glibc < 2.38 has no strlcpy; newlib on the device does.
size_t native_strlcpy(char* dst, const char* src, size_t size);
#define strlcpy native_strlcpy
