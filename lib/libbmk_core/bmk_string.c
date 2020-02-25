/*-
 * Copyright (c) 2014 Antti Kantee.  All Rights Reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * The assumption is that these won't be used very often,
 * only for the very low-level routines.
 *
 * Some code from public domain implementations.
 */

#include <bmk-core/null.h>
#include <bmk-core/string.h>

unsigned long
bmk_strlen(const char *str)
{
	unsigned long rv = 0;

	while (*str++)
		rv++;
	return rv;
}

int
bmk_strcmp(const char *a, const char *b)
{

	while (*a && *a++ == *b++) {
		continue;
	}
	if (*a) {
		a--;
		b--;
	}
	return *a - *b;
}

int
bmk_strncmp(const char *a, const char *b, unsigned long n)
{
	unsigned char u1, u2;

	while (n-- > 0) {
		u1 = (unsigned char)*a++;
		u2 = (unsigned char)*b++;
		if (u1 != u2)
			return u1 - u2;
		if (u1 == '\0')
			return 0;
	}
	return 0;
}

char *
bmk_strcpy(char *d, const char *s)
{
	char *orig = d;

	while ((*d++ = *s++) != '\0')
		continue;
	return orig;
}

char *
bmk_strncpy(char *d, const char *s, unsigned long n)
{
	char *orig = d;

	while (n && (*d++ = *s++))
		n--;
	while (n--)
		*d++ = '\0';
	return orig;
}

void *
bmk_memset(void *b, int c, unsigned long n)
{
	unsigned char *v = b;

	while (n--)
		*v++ = (unsigned char)c;

	return b;
}

#ifdef __i386__
# define NETDOM_MOVS "movsd"
#elif __x86_64__
# define NETDOM_MOVS "movsq"
#endif

void *
bmk_mempcpy(void *d, const void *src, unsigned long n)
{
	unsigned char *dp = d;
	const unsigned char *sp = src;
#if defined(__i386__) || defined(__x86_64__)
	unsigned long units = n;
	if (units >= 48) {
		/* Align as necessary */
		while ((unsigned long) dp & (sizeof(unsigned long) - 1)) {
			*dp++ = *sp++;
			n--;
		}
		units = n / sizeof(unsigned long);
		__asm__ __volatile__ (
			"rep " NETDOM_MOVS
			: "+D" (dp), "+S" (sp), "+c" (units)
			:
			: "cc", "memory"
		);
		units = n & (sizeof(unsigned long) - 1);
	}
	__asm__ __volatile__ (
		"rep movsb"
		: "+D" (dp), "+S" (sp), "+c" (units)
		:
		: "cc", "memory"
	);
#else
	while (n--)
		*dp++ = *sp++;
#endif
	return dp;
}

void *
bmk_memchr(const void *d, int c, unsigned long n)
{
	const unsigned char *p = d;

	while (n--) {
		if (*p == (unsigned char)c)
			return (void *)(unsigned long)p;
		p++;
	}
	return NULL;
}

void *
bmk_memrchr(const void *d, int c, unsigned long n)
{
	const unsigned char *p = d;

	while (n--) {
		if (*p == (unsigned char)c)
			return (void *)(unsigned long)p;
		p--;
	}
	return NULL;
}
