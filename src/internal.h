#ifndef ARCHETYPON_INTERNAL_H
#define ARCHETYPON_INTERNAL_H

#include "../archetypon.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef int32_t s32;
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

void archetypon_set_error(char *error, size_t capacity, const char *format,
			  ...);
bool archetypon_multiply_size(size_t left, size_t right, size_t *result);
bool archetypon_buffer_reserve(struct archetypon_buffer *buffer, size_t extra);
bool archetypon_buffer_append(struct archetypon_buffer *buffer,
			      const void *data, size_t length);
bool archetypon_buffer_put_u8(struct archetypon_buffer *buffer, u8 value);
bool archetypon_buffer_put_u16le(struct archetypon_buffer *buffer, u16 value);
bool archetypon_buffer_put_u32le(struct archetypon_buffer *buffer, u32 value);
bool archetypon_buffer_put_u32be(struct archetypon_buffer *buffer, u32 value);
void archetypon_buffer_reset(struct archetypon_buffer *buffer);

#endif
