#ifndef ARCHETYPON_INTERNAL_H
#define ARCHETYPON_INTERNAL_H

#include "../archetypon.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef float f32;
typedef double f64;

typedef ArchetyponBuffer Buffer;
typedef ArchetyponImage Image;

#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

i32 archetypon_internal_set_error(char *error, size_t capacity,
                                   const char *format, ...);
i32 archetypon_internal_checked_multiply(size_t left, size_t right,
                                          size_t *result);
i32 archetypon_internal_buffer_reserve(Buffer *buffer, size_t extra);
i32 archetypon_internal_buffer_append(Buffer *buffer, const void *data,
                                       size_t length);
i32 archetypon_internal_buffer_u8(Buffer *buffer, u8 value);
i32 archetypon_internal_buffer_u16le(Buffer *buffer, u16 value);
i32 archetypon_internal_buffer_u32le(Buffer *buffer, u32 value);
i32 archetypon_internal_buffer_u32be(Buffer *buffer, u32 value);
void archetypon_internal_buffer_free(Buffer *buffer);

#define set_error archetypon_internal_set_error
#define checked_multiply archetypon_internal_checked_multiply
#define buffer_reserve archetypon_internal_buffer_reserve
#define buffer_append archetypon_internal_buffer_append
#define buffer_u8 archetypon_internal_buffer_u8
#define buffer_u16le archetypon_internal_buffer_u16le
#define buffer_u32le archetypon_internal_buffer_u32le
#define buffer_u32be archetypon_internal_buffer_u32be
#define buffer_free archetypon_internal_buffer_free

#endif
