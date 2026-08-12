#include "internal.h"

#include <ctype.h>

i32 archetypon_svg_optimize(const u8 *source, size_t length, Buffer *optimized,
                        char *error, size_t error_capacity) {
  size_t index = 0;

  if (optimized == NULL) {
    return set_error(error, error_capacity, "missing optimized SVG output");
  }
  memset(optimized, 0, sizeof(*optimized));
  if (source == NULL && length != 0) {
    return set_error(error, error_capacity, "missing SVG source");
  }
  while (index < length) {
    if (index + 4 <= length && memcmp(source + index, "<!--", 4) == 0) {
      size_t end = index + 4;
      while (end + 3 <= length && memcmp(source + end, "-->", 3) != 0) {
        end++;
      }
      if (end + 3 > length) {
        buffer_free(optimized);
        return set_error(error, error_capacity, "unterminated SVG comment");
      }
      index = end + 3;
      continue;
    }
    if (isspace(source[index])) {
      size_t whitespace_start = index;
      size_t next;
      while (index < length && isspace(source[index])) {
        index++;
      }
      next = index;
      if (optimized->length > 0 && optimized->data[optimized->length - 1] == '>' &&
          next < length && source[next] == '<') {
        continue;
      }
      if (!buffer_append(optimized, source + whitespace_start,
                         index - whitespace_start)) {
        goto memory_error;
      }
      continue;
    }
    if (!buffer_u8(optimized, source[index++])) {
      goto memory_error;
    }
  }
  while (optimized->length > 0 &&
         isspace(optimized->data[optimized->length - 1])) {
    optimized->length--;
  }
  if (!buffer_u8(optimized, '\n')) {
    goto memory_error;
  }
  return 1;

memory_error:
  buffer_free(optimized);
  return set_error(error, error_capacity, "out of memory optimizing SVG");
}
