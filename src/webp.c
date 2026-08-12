#include "internal.h"

typedef struct {
  Buffer *buffer;
  u64 bits;
  i32 bit_count;
  i32 failed;
} BitWriter;

static void bit_writer_put(BitWriter *writer, u32 value, i32 count) {
  if (writer->failed || count < 0 || count > 24) {
    writer->failed = 1;
    return;
  }
  writer->bits |= ((u64)value & ((1ULL << count) - 1)) << writer->bit_count;
  writer->bit_count += count;
  while (writer->bit_count >= 8) {
    if (!buffer_u8(writer->buffer, (u8)writer->bits)) {
      writer->failed = 1;
      return;
    }
    writer->bits >>= 8;
    writer->bit_count -= 8;
  }
}

static void bit_writer_finish(BitWriter *writer) {
  if (!writer->failed && writer->bit_count > 0) {
    if (!buffer_u8(writer->buffer, (u8)writer->bits)) {
      writer->failed = 1;
    }
    writer->bits = 0;
    writer->bit_count = 0;
  }
}

static u32 reverse_bits(u32 value, i32 length) {
  u32 result = 0;
  i32 index;
  for (index = 0; index < length; index++) {
    result = (result << 1) | ((value >> index) & 1U);
  }
  return result;
}

static i32 build_huffman_codes(const u8 *lengths, size_t count, u16 *codes) {
  u32 length_counts[16] = {0};
  u32 next_code[16] = {0};
  u32 code = 0;
  i32 bits;
  size_t symbol;
  for (symbol = 0; symbol < count; symbol++) {
    if (lengths[symbol] > 15) {
      return 0;
    }
    if (lengths[symbol] != 0) {
      length_counts[lengths[symbol]]++;
    }
  }
  for (bits = 1; bits <= 15; bits++) {
    code = (code + length_counts[bits - 1]) << 1;
    next_code[bits] = code;
  }
  for (symbol = 0; symbol < count; symbol++) {
    i32 length = lengths[symbol];
    codes[symbol] = length == 0
                        ? 0
                        : (u16)reverse_bits(next_code[length]++, length);
  }
  return 1;
}

static void webp_simple_code(BitWriter *writer, u8 symbol) {
  bit_writer_put(writer, 1, 1);
  bit_writer_put(writer, 0, 1);
  if (symbol <= 1) {
    bit_writer_put(writer, 0, 1);
    bit_writer_put(writer, symbol, 1);
  } else {
    bit_writer_put(writer, 1, 1);
    bit_writer_put(writer, symbol, 8);
  }
}

static i32 webp_fixed_code(BitWriter *writer, size_t alphabet_size,
                           u8 *lengths, u16 *codes) {
  static const i32 order[19] = {17, 18, 0, 1, 2, 3, 4, 5, 16, 6,
                                7,  8,  9, 10, 11, 12, 13, 14, 15};
  u8 code_length_lengths[19] = {0};
  u16 code_length_codes[19] = {0};
  size_t symbol;
  i32 index;

  memset(lengths, 0, alphabet_size);
  if (alphabet_size == 280) {
    for (symbol = 0; symbol < 232; symbol++) {
      lengths[symbol] = 8;
    }
    for (; symbol < alphabet_size; symbol++) {
      lengths[symbol] = 9;
    }
  } else if (alphabet_size == 256) {
    memset(lengths, 8, alphabet_size);
  } else {
    return 0;
  }
  if (!build_huffman_codes(lengths, alphabet_size, codes)) {
    return 0;
  }

  code_length_lengths[0] = 1;
  code_length_lengths[8] = 2;
  code_length_lengths[9] = 2;
  if (!build_huffman_codes(code_length_lengths, 19, code_length_codes)) {
    return 0;
  }
  bit_writer_put(writer, 0, 1);
  bit_writer_put(writer, 9, 4);
  for (index = 0; index < 13; index++) {
    bit_writer_put(writer, code_length_lengths[order[index]], 3);
  }
  bit_writer_put(writer, 0, 1);
  for (symbol = 0; symbol < alphabet_size; symbol++) {
    u8 length = lengths[symbol];
    bit_writer_put(writer, code_length_codes[length],
                   code_length_lengths[length]);
  }
  return !writer->failed;
}

i32 archetypon_webp_encode(const Image *image, Buffer *webp, char *error,
                       size_t error_capacity) {
  Buffer payload = {0};
  BitWriter writer;
  u8 green_lengths[280];
  u16 green_codes[280];
  u8 channel_lengths[256];
  u16 channel_codes[256];
  size_t pixel_count;
  size_t index;
  i32 uses_alpha = 0;
  i32 ok = 0;

  if (webp == NULL) {
    return set_error(error, error_capacity, "missing WebP output buffer");
  }
  memset(webp, 0, sizeof(*webp));
  if (image == NULL || image->pixels == NULL || image->width < 1 ||
      image->height < 1 || image->width > 16384 ||
      image->height > 16384 ||
      !checked_multiply((size_t)image->width, (size_t)image->height,
                        &pixel_count)) {
    return set_error(error, error_capacity, "invalid WebP dimensions");
  }
  for (index = 0; index < pixel_count; index++) {
    if (image->pixels[index * 4 + 3] != 255) {
      uses_alpha = 1;
      break;
    }
  }
  if (!buffer_u8(&payload, 0x2f)) {
    return set_error(error, error_capacity, "out of memory encoding WebP");
  }
  writer = (BitWriter){&payload, 0, 0, 0};
  bit_writer_put(&writer, (u32)(image->width - 1), 14);
  bit_writer_put(&writer, (u32)(image->height - 1), 14);
  bit_writer_put(&writer, (u32)uses_alpha, 1);
  bit_writer_put(&writer, 0, 3);
  bit_writer_put(&writer, 0, 1);
  bit_writer_put(&writer, 0, 1);
  bit_writer_put(&writer, 0, 1);

  if (!webp_fixed_code(&writer, 280, green_lengths, green_codes) ||
      !webp_fixed_code(&writer, 256, channel_lengths, channel_codes) ||
      !webp_fixed_code(&writer, 256, channel_lengths, channel_codes) ||
      !webp_fixed_code(&writer, 256, channel_lengths, channel_codes)) {
    set_error(error, error_capacity, "cannot build WebP prefix codes");
    goto cleanup;
  }
  webp_simple_code(&writer, 0);

  for (index = 0; index < pixel_count; index++) {
    const u8 *pixel = image->pixels + index * 4;
    u8 green = pixel[1];
    u8 red = pixel[0];
    u8 blue = pixel[2];
    u8 alpha = pixel[3];
    bit_writer_put(&writer, green_codes[green], green_lengths[green]);
    bit_writer_put(&writer, channel_codes[red], channel_lengths[red]);
    bit_writer_put(&writer, channel_codes[blue], channel_lengths[blue]);
    bit_writer_put(&writer, channel_codes[alpha], channel_lengths[alpha]);
  }
  bit_writer_finish(&writer);
  if (writer.failed || payload.length > UINT32_MAX ||
      !buffer_append(webp, "RIFF", 4) ||
      !buffer_u32le(webp, (u32)(4 + 8 + payload.length +
                               (payload.length & 1))) ||
      !buffer_append(webp, "WEBP", 4) || !buffer_append(webp, "VP8L", 4) ||
      !buffer_u32le(webp, (u32)payload.length) ||
      !buffer_append(webp, payload.data, payload.length) ||
      ((payload.length & 1) && !buffer_u8(webp, 0))) {
    set_error(error, error_capacity, "out of memory encoding WebP");
    goto cleanup;
  }
  ok = 1;

cleanup:
  buffer_free(&payload);
  if (!ok) {
    buffer_free(webp);
  }
  return ok;
}
