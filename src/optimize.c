#include "internal.h"

#include <ctype.h>

static bool skip_comment(const u8 *source, size_t length, size_t *index,
			 char *error, size_t error_capacity)
{
	size_t end;

	if (*index + 4 > length || memcmp(source + *index, "<!--", 4) != 0)
		return false;
	end = *index + 4;
	while (end + 3 <= length && memcmp(source + end, "-->", 3) != 0)
		end++;
	if (end + 3 > length) {
		archetypon_set_error(error, error_capacity,
				     "unterminated SVG comment");
		return false;
	}
	*index = end + 3;
	return true;
}

static bool append_whitespace(const u8 *source, size_t length, size_t *index,
			      struct archetypon_buffer *optimized)
{
	size_t start = *index;

	while (*index < length && isspace(source[*index]))
		(*index)++;
	if (optimized->length > 0 &&
	    optimized->data[optimized->length - 1] == '>' && *index < length &&
	    source[*index] == '<')
		return true;
	return archetypon_buffer_append(optimized, source + start,
					*index - start);
}

int archetypon_svg_optimize(const u8 *source, size_t length,
			    struct archetypon_buffer *optimized, char *error,
			    size_t error_capacity)
{
	size_t index = 0;

	if (!optimized || (!source && length != 0)) {
		archetypon_set_error(error, error_capacity,
				     "missing SVG optimizer input");
		return -1;
	}
	memset(optimized, 0, sizeof(*optimized));
	while (index < length) {
		bool comment = index + 4 <= length &&
			       memcmp(source + index, "<!--", 4) == 0;
		bool whitespace = isspace(source[index]);

		if (comment &&
		    !skip_comment(source, length, &index, error,
				  error_capacity))
			goto out_error;
		if (comment)
			continue;
		if (whitespace &&
		    !append_whitespace(source, length, &index, optimized))
			goto out_memory;
		if (whitespace)
			continue;
		if (!archetypon_buffer_put_u8(optimized, source[index++]))
			goto out_memory;
	}
	while (optimized->length > 0 &&
	       isspace(optimized->data[optimized->length - 1]))
		optimized->length--;
	if (!archetypon_buffer_put_u8(optimized, '\n'))
		goto out_memory;
	return 0;

out_memory:
	archetypon_set_error(error, error_capacity,
			     "out of memory optimizing SVG");
out_error:
	archetypon_buffer_reset(optimized);
	return -1;
}
