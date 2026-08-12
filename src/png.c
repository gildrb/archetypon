#include "internal.h"

static u32 crc32_update(u32 crc, const u8 *data, size_t length) {
  size_t index;
  crc = ~crc;
  for (index = 0; index < length; index++) {
    i32 bit;
    crc ^= data[index];
    for (bit = 0; bit < 8; bit++) {
      crc = (crc >> 1) ^ (0xedb88320U & (u32)-(i32)(crc & 1));
    }
  }
  return ~crc;
}

static i32 png_chunk(Buffer *png, const char type[4], const u8 *data,
                     size_t length) {
  u32 crc;
  if (length > UINT32_MAX || !buffer_u32be(png, (u32)length) ||
      !buffer_append(png, type, 4) || !buffer_append(png, data, length)) {
    return 0;
  }
  crc = crc32_update(0, (const u8 *)type, 4);
  crc = crc32_update(crc, data, length);
  return buffer_u32be(png, crc);
}

i32 archetypon_png_encode(const Image *image, Buffer *png, char *error,
                      size_t error_capacity) {
  static const u8 signature[8] = {137, 80, 78, 71, 13, 10, 26, 10};
  Buffer raw = {0};
  Buffer zlib = {0};
  Buffer header = {0};
  size_t row_bytes;
  size_t raw_size;
  i32 row;
  u32 adler_a = 1;
  u32 adler_b = 0;
  size_t offset;
  i32 ok = 0;

  if (png == NULL) {
    return set_error(error, error_capacity, "missing PNG output buffer");
  }
  memset(png, 0, sizeof(*png));
  if (image == NULL || image->width < 1 || image->height < 1 ||
      image->pixels == NULL) {
    return set_error(error, error_capacity, "invalid PNG image");
  }
  if (!checked_multiply((size_t)image->width, 4, &row_bytes) ||
      row_bytes > SIZE_MAX - 1 ||
      !checked_multiply(row_bytes + 1, (size_t)image->height, &raw_size) ||
      !buffer_reserve(&raw, raw_size)) {
    set_error(error, error_capacity, "PNG is too large");
    goto cleanup;
  }
  for (row = 0; row < image->height; row++) {
    if (!buffer_u8(&raw, 0) ||
        !buffer_append(&raw,
                       image->pixels + (size_t)row * image->width * 4,
                       row_bytes)) {
      set_error(error, error_capacity, "out of memory encoding PNG");
      goto cleanup;
    }
  }
  if (!buffer_u8(&zlib, 0x78) || !buffer_u8(&zlib, 0x01)) {
    set_error(error, error_capacity, "out of memory encoding PNG");
    goto cleanup;
  }
  offset = 0;
  while (offset < raw.length) {
    size_t block_length = raw.length - offset;
    u16 length16;
    if (block_length > 65535) {
      block_length = 65535;
    }
    length16 = (u16)block_length;
    if (!buffer_u8(&zlib, offset + block_length == raw.length ? 1 : 0) ||
        !buffer_u16le(&zlib, length16) ||
        !buffer_u16le(&zlib, (u16)~length16) ||
        !buffer_append(&zlib, raw.data + offset, block_length)) {
      set_error(error, error_capacity, "out of memory encoding PNG");
      goto cleanup;
    }
    offset += block_length;
  }
  for (offset = 0; offset < raw.length; offset++) {
    adler_a = (adler_a + raw.data[offset]) % 65521;
    adler_b = (adler_b + adler_a) % 65521;
  }
  if (!buffer_u32be(&zlib, (adler_b << 16) | adler_a) ||
      !buffer_u32be(&header, (u32)image->width) ||
      !buffer_u32be(&header, (u32)image->height) ||
      !buffer_u8(&header, 8) || !buffer_u8(&header, 6) ||
      !buffer_u8(&header, 0) || !buffer_u8(&header, 0) ||
      !buffer_u8(&header, 0) || !buffer_append(png, signature, 8) ||
      !png_chunk(png, "IHDR", header.data, header.length) ||
      !png_chunk(png, "IDAT", zlib.data, zlib.length) ||
      !png_chunk(png, "IEND", NULL, 0)) {
    set_error(error, error_capacity, "out of memory encoding PNG");
    goto cleanup;
  }
  ok = 1;

cleanup:
  buffer_free(&raw);
  buffer_free(&zlib);
  buffer_free(&header);
  if (!ok) {
    buffer_free(png);
  }
  return ok;
}
