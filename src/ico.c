#include "internal.h"

i32 archetypon_ico_encode(const Buffer pngs[3], Buffer *ico, char *error,
                      size_t error_capacity) {
  size_t index;
  u32 offset = 6 + 3 * 16;

  if (ico == NULL) {
    return set_error(error, error_capacity, "missing ICO output buffer");
  }
  memset(ico, 0, sizeof(*ico));
  if (pngs == NULL) {
    return set_error(error, error_capacity, "missing PNG inputs for ICO");
  }
  if (!buffer_u16le(ico, 0) || !buffer_u16le(ico, 1) ||
      !buffer_u16le(ico, 3)) {
    goto memory_error;
  }
  for (index = 0; index < 3; index++) {
    u32 width;
    u32 height;
    if (pngs[index].data == NULL || pngs[index].length < 24) {
      buffer_free(ico);
      return set_error(error, error_capacity,
                       "invalid PNG while encoding favicon.ico");
    }
    width = ((u32)pngs[index].data[16] << 24) |
            ((u32)pngs[index].data[17] << 16) |
            ((u32)pngs[index].data[18] << 8) | pngs[index].data[19];
    height = ((u32)pngs[index].data[20] << 24) |
             ((u32)pngs[index].data[21] << 16) |
             ((u32)pngs[index].data[22] << 8) | pngs[index].data[23];
    if (pngs[index].length > UINT32_MAX || width > 256 || height > 256 ||
        !buffer_u8(ico, width == 256 ? 0 : (u8)width) ||
        !buffer_u8(ico, height == 256 ? 0 : (u8)height) ||
        !buffer_u8(ico, 0) || !buffer_u8(ico, 0) ||
        !buffer_u16le(ico, 1) || !buffer_u16le(ico, 32) ||
        !buffer_u32le(ico, (u32)pngs[index].length) ||
        !buffer_u32le(ico, offset)) {
      goto memory_error;
    }
    offset += (u32)pngs[index].length;
  }
  for (index = 0; index < 3; index++) {
    if (!buffer_append(ico, pngs[index].data, pngs[index].length)) {
      goto memory_error;
    }
  }
  return 1;

memory_error:
  buffer_free(ico);
  return set_error(error, error_capacity, "out of memory encoding favicon.ico");
}
