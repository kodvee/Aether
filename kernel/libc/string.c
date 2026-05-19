/**
 * string.c: String functions
 */

#include <stdint.h>
#include <stddef.h>
#include <limits.h>
#include <string.h>
#include <kernel/mmu.h>
#include <memory.h>

/* Word-at-a-time helpers for strlen - local to this translation unit */
#define _STR_ALIGN  (sizeof(size_t))
#define _STR_ONES   ((size_t)-1 / UCHAR_MAX)
#define _STR_HIGHS  (_STR_ONES * (UCHAR_MAX / 2 + 1))
#define _STR_HASZERO(x) (((x) - _STR_ONES) & ~(x) & _STR_HIGHS)

/* Get length of string */
size_t strlen(const char* s) {
	const char* a = s;
	const size_t *w;
	for(; (uintptr_t)s % _STR_ALIGN; s++) {
		if(!*s) {
			return s-a;
		}
	}
	for(w = (const void*)s; !_STR_HASZERO(*w); w++);
	for (s = (const void *)w; *s; s++);
	return s-a;
}

/* Compare strings */
int strcmp(const char* s1, const char* s2) {
	while(*s1 && (*s1 == *s2)) {
		s1++;
		s2++;
	}
	return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

/* Compare up to n bytes */
int strncmp(const char* s1, const char* s2, size_t n) {
	while (n && *s1 && (*s1 == *s2)) {
		s1++; s2++; n--;
	}
	if (!n) return 0;
	return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

/* Find needle in haystack */
const char* strstr(const char* haystack, const char* needle) {
	if (!*needle) return haystack;
	size_t nlen = strlen(needle);
	for (; *haystack; haystack++) {
		if (*haystack == *needle && strncmp(haystack, needle, nlen) == 0)
			return haystack;
	}
	return NULL;
}

/* Trim a string */
char* strctrim(const char* s, char c) {
	size_t len = strlen(s);
	const char* end = s + len - 1;

	while(*end == c && end >= s) {
		--end;
	}

	size_t newLen = end - s + 1;
	char* trimmed = (char*)malloc(newLen + 1);
	memcpy(trimmed, s, newLen);
	trimmed[newLen] = '\0';
	
	return trimmed;
}

/* Duplicate a string */
char* strdup(const char* s) {
	size_t length = strlen(s) + 1;
	char* new = (char*)malloc(length);
	memcpy(new, s, length);
	return new;
}