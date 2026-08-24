#pragma once
#include <ctype.h>
#include <stddef.h>

static inline void gsc_normalize_script_path(char *dst, size_t dst_size, const char *src)
{
	size_t i, j = 0;
	if (!dst || dst_size == 0) return;
	dst[0] = '\0';
	if (!src) return;
	for (i = 0; src[i] && j + 1 < dst_size; ++i) {
		unsigned char c = (unsigned char)src[i];
		if (c == '\\') c = '/';
		else c = (unsigned char)tolower(c);
		dst[j++] = (char)c;
	}
	dst[j] = '\0';
	if (j >= 4) {
		char *ext = dst + (j - 4);
		if (ext[0]=='.' && ext[1]=='g' && ext[2]=='s' && ext[3]=='c')
			*ext = '\0';
	}
}
