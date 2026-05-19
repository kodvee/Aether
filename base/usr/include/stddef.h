#ifndef _STDDEF_H
#define _STDDEF_H

#include <stdint.h>

#define NULL (void*)0
typedef int64_t ptrdiff_t;
typedef uint64_t size_t;

#define offsetof(type, member) __builtin_offsetof(type, member)

#endif