#pragma once

#include <stdint.h>
#include <stddef.h>

size_t strlen(const char* s);
int strcmp(const char* s1, const char* s2);
int strncmp(const char* s1, const char* s2, size_t n);
const char* strstr(const char* haystack, const char* needle);
char* strctrim(const char* s, char c);
char* strdup(const char* s);