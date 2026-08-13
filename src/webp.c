#include "internal.h"

struct bit_writer {
	struct archetypon_buffer *buffer;
	u64 bits;
	s32 bit_count;
	bool failed;
};

static void bit_writer_put(struct bit_writer *writer, u32 value, s32 count)
{
	if (writer->failed || count < 0 || count > 24) {
		writer->failed = true;
		return;
	}
	writer->bits |= ((u64)value & ((1ULL << count) - 1))
			<< writer->bit_count;
	writer->bit_count += count;
	while (writer->bit_count >= 8) {
		if (!archetypon_buffer_put_u8(writer->buffer,
					      (u8)writer->bits)) {
			writer->failed = true;
			return;
		}
		writer->bits >>= 8;
		writer->bit_count -= 8;
	}
}

static void bit_writer_finish(struct bit_writer *writer)
{
	if (!writer->failed && writer->bit_count > 0) {
		if (!archetypon_buffer_put_u8(writer->buffer,
					      (u8)writer->bits))
			writer->failed = true;
		writer->bits = 0;
		writer->bit_count = 0;
	}
}

static u32 reverse_bits(u32 value, s32 length)
{
	u32 result = 0;
	s32 index;

	for (index = 0; index < length; index++)
		result = (result << 1) | ((value >> index) & 1U);
	return result;
}

static bool build_huffman_codes(const u8 *lengths, size_t count, u16 *codes)
{
	u32 length_counts[16] = { 0 };
	u32 next_code[16] = { 0 };
	u32 code = 0;
	s32 bits;
	size_t symbol;

	for (symbol = 0; symbol < count; symbol++) {
		if (lengths[symbol] > 15)
			return false;
		if (lengths[symbol] != 0)
			length_counts[lengths[symbol]]++;
	}
	for (bits = 1; bits <= 15; bits++) {
		code = (code + length_counts[bits - 1]) << 1;
		next_code[bits] = code;
	}
	for (symbol = 0; symbol < count; symbol++) {
		s32 length = lengths[symbol];

		codes[symbol] =
			length == 0 ?
				0 :
				(u16)reverse_bits(next_code[length]++, length);
	}
	return true;
}

static void webp_simple_code(struct bit_writer *writer, u8 symbol)
{
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

static bool webp_fixed_code(struct bit_writer *writer, size_t alphabet_size,
			    u8 *lengths, u16 *codes)
{
	static const s32 order[19] = { 17, 18, 0, 1,  2,  3,  4,  5,  16, 6,
				       7,  8,  9, 10, 11, 12, 13, 14, 15 };
	u8 code_length_lengths[19] = { 0 };
	u16 code_length_codes[19] = { 0 };
	size_t symbol;
	s32 index;

	memset(lengths, 0, alphabet_size);
	if (alphabet_size == 280) {
		for (symbol = 0; symbol < 232; symbol++)
			lengths[symbol] = 8;
		for (; symbol < alphabet_size; symbol++)
			lengths[symbol] = 9;
	} else if (alphabet_size == 256) {
		memset(lengths, 8, alphabet_size);
	} else {
		return false;
	}
	if (!build_huffman_codes(lengths, alphabet_size, codes))
		return false;

	code_length_lengths[0] = 1;
	code_length_lengths[8] = 2;
	code_length_lengths[9] = 2;
	if (!build_huffman_codes(code_length_lengths, 19, code_length_codes))
		return false;
	bit_writer_put(writer, 0, 1);
	bit_writer_put(writer, 9, 4);
	for (index = 0; index < 13; index++)
		bit_writer_put(writer, code_length_lengths[order[index]], 3);
	bit_writer_put(writer, 0, 1);
	for (symbol = 0; symbol < alphabet_size; symbol++) {
		u8 length = lengths[symbol];

		bit_writer_put(writer, code_length_codes[length],
			       code_length_lengths[length]);
	}
	return !writer->failed;
}

struct webp_codes {
	u8 green_lengths[280];
	u16 green_codes[280];
	u8 channel_lengths[256];
	u16 channel_codes[256];
};

static bool webp_has_alpha(const struct archetypon_image *image, size_t count)
{
	size_t i;

	for (i = 0; i < count; i++) {
		if (image->pixels[i * 4 + 3] != 255)
			return true;
	}
	return false;
}

static bool webp_write_codes(struct bit_writer *writer,
			     struct webp_codes *codes)
{
	return webp_fixed_code(writer, 280, codes->green_lengths,
			       codes->green_codes) &&
	       webp_fixed_code(writer, 256, codes->channel_lengths,
			       codes->channel_codes) &&
	       webp_fixed_code(writer, 256, codes->channel_lengths,
			       codes->channel_codes) &&
	       webp_fixed_code(writer, 256, codes->channel_lengths,
			       codes->channel_codes);
}

static void webp_write_pixels(struct bit_writer *writer,
			      const struct archetypon_image *image,
			      size_t count, const struct webp_codes *codes)
{
	size_t i;

	for (i = 0; i < count; i++) {
		const u8 *pixel = image->pixels + i * 4;
		u8 green = pixel[1];
		u8 red = pixel[0];
		u8 blue = pixel[2];
		u8 alpha = pixel[3];

		bit_writer_put(writer, codes->green_codes[green],
			       codes->green_lengths[green]);
		bit_writer_put(writer, codes->channel_codes[red],
			       codes->channel_lengths[red]);
		bit_writer_put(writer, codes->channel_codes[blue],
			       codes->channel_lengths[blue]);
		bit_writer_put(writer, codes->channel_codes[alpha],
			       codes->channel_lengths[alpha]);
	}
}

static bool webp_container(struct archetypon_buffer *webp,
			   const struct archetypon_buffer *payload)
{
	return payload->length <= UINT32_MAX &&
	       archetypon_buffer_append(webp, "RIFF", 4) &&
	       archetypon_buffer_put_u32le(webp,
					   (u32)(4 + 8 + payload->length +
						 (payload->length & 1))) &&
	       archetypon_buffer_append(webp, "WEBP", 4) &&
	       archetypon_buffer_append(webp, "VP8L", 4) &&
	       archetypon_buffer_put_u32le(webp, (u32)payload->length) &&
	       archetypon_buffer_append(webp, payload->data, payload->length) &&
	       (!(payload->length & 1) || archetypon_buffer_put_u8(webp, 0));
}

static bool webp_payload(const struct archetypon_image *image, size_t count,
			 struct archetypon_buffer *payload)
{
	struct webp_codes codes;
	struct bit_writer writer = { payload, 0, 0, false };

	if (!archetypon_buffer_put_u8(payload, 0x2f))
		return false;
	bit_writer_put(&writer, (u32)(image->width - 1), 14);
	bit_writer_put(&writer, (u32)(image->height - 1), 14);
	bit_writer_put(&writer, webp_has_alpha(image, count), 1);
	bit_writer_put(&writer, 0, 3);
	bit_writer_put(&writer, 0, 1);
	bit_writer_put(&writer, 0, 1);
	bit_writer_put(&writer, 0, 1);
	if (!webp_write_codes(&writer, &codes))
		return false;
	webp_simple_code(&writer, 0);
	webp_write_pixels(&writer, image, count, &codes);
	bit_writer_finish(&writer);
	return !writer.failed;
}

int archetypon_webp_encode(const struct archetypon_image *image,
			   struct archetypon_buffer *webp, char *error,
			   size_t error_capacity)
{
	struct archetypon_buffer payload = { 0 };
	size_t pixel_count;
	bool ok = false;

	if (!webp) {
		archetypon_set_error(error, error_capacity,
				     "missing WebP output buffer");
		return -1;
	}
	memset(webp, 0, sizeof(*webp));
	if (!image || !image->pixels || image->width < 1 ||
	    image->height < 1 || image->width > 16384 ||
	    image->height > 16384 ||
	    !archetypon_multiply_size((size_t)image->width,
				      (size_t)image->height, &pixel_count)) {
		archetypon_set_error(error, error_capacity,
				     "invalid WebP dimensions");
		return -1;
	}
	if (!webp_payload(image, pixel_count, &payload)) {
		archetypon_set_error(error, error_capacity,
				     "cannot encode WebP payload");
		goto cleanup;
	}
	if (!webp_container(webp, &payload)) {
		archetypon_set_error(error, error_capacity,
				     "out of memory encoding WebP");
		goto cleanup;
	}
	ok = true;

cleanup:
	archetypon_buffer_reset(&payload);
	if (!ok)
		archetypon_buffer_reset(webp);
	return ok ? 0 : -1;
}
