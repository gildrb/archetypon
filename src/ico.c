#include "internal.h"

static u32 png_dimension(const struct archetypon_buffer *png, size_t offset)
{
	return ((u32)png->data[offset] << 24) |
	       ((u32)png->data[offset + 1] << 16) |
	       ((u32)png->data[offset + 2] << 8) | png->data[offset + 3];
}

static bool ico_entry(struct archetypon_buffer *ico,
		      const struct archetypon_buffer *png, u32 offset)
{
	u32 width;
	u32 height;

	if (!png->data || png->length < 24 || png->length > UINT32_MAX)
		return false;
	width = png_dimension(png, 16);
	height = png_dimension(png, 20);
	if (width > 256 || height > 256)
		return false;
	return archetypon_buffer_put_u8(ico, width == 256 ? 0 : (u8)width) &&
	       archetypon_buffer_put_u8(ico, height == 256 ? 0 : (u8)height) &&
	       archetypon_buffer_put_u8(ico, 0) &&
	       archetypon_buffer_put_u8(ico, 0) &&
	       archetypon_buffer_put_u16le(ico, 1) &&
	       archetypon_buffer_put_u16le(ico, 32) &&
	       archetypon_buffer_put_u32le(ico, (u32)png->length) &&
	       archetypon_buffer_put_u32le(ico, offset);
}

int archetypon_ico_encode(const struct archetypon_buffer pngs[3],
			  struct archetypon_buffer *ico, char *error,
			  size_t error_capacity)
{
	u32 offset = 6 + 3 * 16;
	size_t i;

	if (!ico || !pngs) {
		archetypon_set_error(error, error_capacity,
				     "missing ICO output");
		return -1;
	}
	memset(ico, 0, sizeof(*ico));
	if (!archetypon_buffer_put_u16le(ico, 0) ||
	    !archetypon_buffer_put_u16le(ico, 1) ||
	    !archetypon_buffer_put_u16le(ico, 3))
		goto out_memory;
	for (i = 0; i < 3; i++) {
		if (!ico_entry(ico, &pngs[i], offset)) {
			archetypon_buffer_reset(ico);
			archetypon_set_error(error, error_capacity,
					     "invalid PNG while encoding favicon.ico");
			return -1;
		}
		offset += (u32)pngs[i].length;
	}
	for (i = 0; i < 3; i++) {
		if (!archetypon_buffer_append(ico, pngs[i].data,
					      pngs[i].length))
			goto out_memory;
	}
	return 0;

out_memory:
	archetypon_buffer_reset(ico);
	archetypon_set_error(error, error_capacity,
			     "out of memory encoding favicon.ico");
	return -1;
}
