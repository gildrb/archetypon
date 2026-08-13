#include "internal.h"

#include <stdarg.h>
#include <stdio.h>

void archetypon_set_error(char *error, size_t capacity, const char *format, ...)
{
	va_list arguments;

	if (error && capacity > 0) {
		va_start(arguments, format);
		vsnprintf(error, capacity, format, arguments);
		va_end(arguments);
	}
}

bool archetypon_multiply_size(size_t left, size_t right, size_t *result)
{
	if (left != 0 && right > SIZE_MAX / left)
		return false;
	*result = left * right;
	return true;
}

bool archetypon_buffer_reserve(struct archetypon_buffer *buffer, size_t extra)
{
	size_t required;
	size_t capacity;
	u8 *data;

	if (extra > SIZE_MAX - buffer->length)
		return false;
	required = buffer->length + extra;
	if (required <= buffer->capacity)
		return true;

	capacity = buffer->capacity == 0 ? 256 : buffer->capacity;
	while (capacity < required) {
		if (capacity > SIZE_MAX / 2) {
			capacity = required;
			break;
		}
		capacity *= 2;
	}

	data = realloc(buffer->data, capacity);
	if (!data)
		return false;
	buffer->data = data;
	buffer->capacity = capacity;
	return true;
}

bool archetypon_buffer_append(struct archetypon_buffer *buffer,
			      const void *data, size_t length)
{
	if (length == 0)
		return true;
	if (!archetypon_buffer_reserve(buffer, length))
		return false;
	memcpy(buffer->data + buffer->length, data, length);
	buffer->length += length;
	return true;
}

bool archetypon_buffer_put_u8(struct archetypon_buffer *buffer, u8 value)
{
	return archetypon_buffer_append(buffer, &value, 1);
}

bool archetypon_buffer_put_u16le(struct archetypon_buffer *buffer, u16 value)
{
	u8 bytes[2] = { (u8)value, (u8)(value >> 8) };

	return archetypon_buffer_append(buffer, bytes, sizeof(bytes));
}

bool archetypon_buffer_put_u32le(struct archetypon_buffer *buffer, u32 value)
{
	u8 bytes[4] = { (u8)value, (u8)(value >> 8), (u8)(value >> 16),
			(u8)(value >> 24) };

	return archetypon_buffer_append(buffer, bytes, sizeof(bytes));
}

bool archetypon_buffer_put_u32be(struct archetypon_buffer *buffer, u32 value)
{
	u8 bytes[4] = { (u8)(value >> 24), (u8)(value >> 16), (u8)(value >> 8),
			(u8)value };

	return archetypon_buffer_append(buffer, bytes, sizeof(bytes));
}

void archetypon_buffer_reset(struct archetypon_buffer *buffer)
{
	free(buffer->data);
	memset(buffer, 0, sizeof(*buffer));
}

void archetypon_buffer_free(struct archetypon_buffer *buffer)
{
	if (buffer)
		archetypon_buffer_reset(buffer);
}
