#include "internal.h"

static u32 crc32_update(u32 crc, const u8 *data, size_t length)
{
	size_t i;

	crc = ~crc;
	for (i = 0; i < length; i++) {
		s32 bit;

		crc ^= data[i];
		for (bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^
			      (0xedb88320U & (0U - (crc & 1U)));
	}
	return ~crc;
}

static bool png_chunk(struct archetypon_buffer *png, const char type[4],
		      const u8 *data, size_t length)
{
	u32 crc;

	if (length > UINT32_MAX ||
	    !archetypon_buffer_put_u32be(png, (u32)length) ||
	    !archetypon_buffer_append(png, type, 4) ||
	    !archetypon_buffer_append(png, data, length))
		return false;
	crc = crc32_update(0, (const u8 *)type, 4);
	crc = crc32_update(crc, data, length);
	return archetypon_buffer_put_u32be(png, crc);
}

static bool png_raw_image(const struct archetypon_image *image,
			  struct archetypon_buffer *raw)
{
	size_t row_bytes;
	size_t raw_size;
	s32 row;

	if (!archetypon_multiply_size((size_t)image->width, 4, &row_bytes) ||
	    row_bytes == SIZE_MAX ||
	    !archetypon_multiply_size(row_bytes + 1, (size_t)image->height,
				      &raw_size) ||
	    !archetypon_buffer_reserve(raw, raw_size))
		return false;
	for (row = 0; row < image->height; row++) {
		const u8 *pixels =
			image->pixels + (size_t)row * image->width * 4;

		if (!archetypon_buffer_put_u8(raw, 0) ||
		    !archetypon_buffer_append(raw, pixels, row_bytes))
			return false;
	}
	return true;
}

static bool png_zlib_blocks(const struct archetypon_buffer *raw,
			    struct archetypon_buffer *zlib)
{
	size_t offset = 0;

	if (!archetypon_buffer_put_u8(zlib, 0x78) ||
	    !archetypon_buffer_put_u8(zlib, 0x01))
		return false;
	while (offset < raw->length) {
		size_t length = raw->length - offset;
		u16 length16;
		bool last;

		if (length > 65535)
			length = 65535;
		length16 = (u16)length;
		last = offset + length == raw->length;
		if (!archetypon_buffer_put_u8(zlib, last) ||
		    !archetypon_buffer_put_u16le(zlib, length16) ||
		    !archetypon_buffer_put_u16le(zlib, (u16)~length16) ||
		    !archetypon_buffer_append(zlib, raw->data + offset, length))
			return false;
		offset += length;
	}
	return true;
}

static bool png_adler(const struct archetypon_buffer *raw,
		      struct archetypon_buffer *zlib)
{
	u32 a = 1;
	u32 b = 0;
	size_t i;

	for (i = 0; i < raw->length; i++) {
		a = (a + raw->data[i]) % 65521;
		b = (b + a) % 65521;
	}
	return archetypon_buffer_put_u32be(zlib, (b << 16) | a);
}

static bool png_complete(const struct archetypon_image *image,
			 const struct archetypon_buffer *zlib,
			 struct archetypon_buffer *png)
{
	static const u8 signature[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
	struct archetypon_buffer header = { 0 };
	bool ok;

	ok = archetypon_buffer_put_u32be(&header, (u32)image->width) &&
	     archetypon_buffer_put_u32be(&header, (u32)image->height) &&
	     archetypon_buffer_put_u8(&header, 8) &&
	     archetypon_buffer_put_u8(&header, 6) &&
	     archetypon_buffer_put_u8(&header, 0) &&
	     archetypon_buffer_put_u8(&header, 0) &&
	     archetypon_buffer_put_u8(&header, 0) &&
	     archetypon_buffer_append(png, signature, sizeof(signature)) &&
	     png_chunk(png, "IHDR", header.data, header.length) &&
	     png_chunk(png, "IDAT", zlib->data, zlib->length) &&
	     png_chunk(png, "IEND", NULL, 0);
	archetypon_buffer_reset(&header);
	return ok;
}

int archetypon_png_encode(const struct archetypon_image *image,
			  struct archetypon_buffer *png, char *error,
			  size_t error_capacity)
{
	struct archetypon_buffer raw = { 0 };
	struct archetypon_buffer zlib = { 0 };
	bool ok = false;

	if (!png) {
		archetypon_set_error(error, error_capacity,
				     "missing PNG output buffer");
		return -1;
	}
	memset(png, 0, sizeof(*png));
	if (!image || image->width < 1 || image->height < 1 ||
	    !image->pixels) {
		archetypon_set_error(error, error_capacity,
				     "invalid PNG image");
		return -1;
	}
	if (!png_raw_image(image, &raw))
		goto out_reset;
	if (!png_zlib_blocks(&raw, &zlib) || !png_adler(&raw, &zlib))
		goto out_reset;
	ok = png_complete(image, &zlib, png);

out_reset:
	archetypon_buffer_reset(&raw);
	archetypon_buffer_reset(&zlib);
	if (!ok) {
		archetypon_buffer_reset(png);
		archetypon_set_error(error, error_capacity,
				     "out of memory encoding PNG");
	}
	return ok ? 0 : -1;
}
