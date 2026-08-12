#include "internal.h"

#include <stdarg.h>
#include <stdio.h>

i32 set_error(char *error, size_t capacity, const char *format, ...) {
  va_list arguments;
  if (error != NULL && capacity > 0) {
    va_start(arguments, format);
    vsnprintf(error, capacity, format, arguments);
    va_end(arguments);
  }
  return 0;
}

i32 checked_multiply(size_t left, size_t right, size_t *result) {
  if (left != 0 && right > SIZE_MAX / left) {
    return 0;
  }
  *result = left * right;
  return 1;
}

i32 buffer_reserve(Buffer *buffer, size_t extra) {
  size_t required;
  size_t capacity;
  u8 *data;

  if (extra > SIZE_MAX - buffer->length) {
    return 0;
  }
  required = buffer->length + extra;
  if (required <= buffer->capacity) {
    return 1;
  }

  capacity = buffer->capacity == 0 ? 256 : buffer->capacity;
  while (capacity < required) {
    if (capacity > SIZE_MAX / 2) {
      capacity = required;
      break;
    }
    capacity *= 2;
  }

  data = (u8 *)realloc(buffer->data, capacity);
  if (data == NULL) {
    return 0;
  }
  buffer->data = data;
  buffer->capacity = capacity;
  return 1;
}

i32 buffer_append(Buffer *buffer, const void *data, size_t length) {
  if (length == 0) {
    return 1;
  }
  if (!buffer_reserve(buffer, length)) {
    return 0;
  }
  memcpy(buffer->data + buffer->length, data, length);
  buffer->length += length;
  return 1;
}

i32 buffer_u8(Buffer *buffer, u8 value) {
  return buffer_append(buffer, &value, 1);
}

i32 buffer_u16le(Buffer *buffer, u16 value) {
  u8 bytes[2] = {(u8)value, (u8)(value >> 8)};
  return buffer_append(buffer, bytes, sizeof(bytes));
}

i32 buffer_u32le(Buffer *buffer, u32 value) {
  u8 bytes[4] = {(u8)value, (u8)(value >> 8), (u8)(value >> 16),
                 (u8)(value >> 24)};
  return buffer_append(buffer, bytes, sizeof(bytes));
}

i32 buffer_u32be(Buffer *buffer, u32 value) {
  u8 bytes[4] = {(u8)(value >> 24), (u8)(value >> 16), (u8)(value >> 8),
                 (u8)value};
  return buffer_append(buffer, bytes, sizeof(bytes));
}

void buffer_free(Buffer *buffer) {
  free(buffer->data);
  memset(buffer, 0, sizeof(*buffer));
}

void archetypon_buffer_free(ArchetyponBuffer *buffer) {
  if (buffer != NULL) {
    buffer_free(buffer);
  }
}
