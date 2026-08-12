#include "internal.h"

#include <math.h>

void archetypon_image_free(Image *image) {
  if (image != NULL) {
    free(image->pixels);
    memset(image, 0, sizeof(*image));
  }
}

i32 archetypon_image_resize(const Image *source, i32 maximum_dimension,
                        f64 aspect_width, f64 aspect_height, Image *result,
                        char *error, size_t error_capacity) {
  size_t bytes;
  i32 y;

  if (result == NULL) {
    return set_error(error, error_capacity, "missing resized image output");
  }
  memset(result, 0, sizeof(*result));
  if (source == NULL || source->width < 1 || source->height < 1 ||
      source->pixels == NULL || maximum_dimension < 1 ||
      !isfinite(aspect_width) || !isfinite(aspect_height) ||
      aspect_width <= 0 || aspect_height <= 0) {
    return set_error(error, error_capacity, "invalid image resize");
  }
  if (aspect_width >= aspect_height) {
    result->width = maximum_dimension;
    result->height = (i32)lround(maximum_dimension * aspect_height / aspect_width);
  } else {
    result->height = maximum_dimension;
    result->width = (i32)lround(maximum_dimension * aspect_width / aspect_height);
  }
  if (result->width < 1) {
    result->width = 1;
  }
  if (result->height < 1) {
    result->height = 1;
  }
  if (!checked_multiply((size_t)result->width, (size_t)result->height,
                        &bytes) ||
      !checked_multiply(bytes, 4, &bytes)) {
    return set_error(error, error_capacity, "image size overflow");
  }
  result->pixels = (u8 *)malloc(bytes);
  if (result->pixels == NULL) {
    return set_error(error, error_capacity, "out of memory resizing image");
  }

  for (y = 0; y < result->height; y++) {
    f64 source_y = ((y + 0.5) * source->height / result->height) - 0.5;
    i32 y0 = (i32)floor(source_y);
    i32 y1;
    f64 fy = source_y - y0;
    i32 x;
    if (y0 < 0) {
      y0 = 0;
      fy = 0;
    }
    y1 = y0 + 1;
    if (y1 >= source->height) {
      y1 = source->height - 1;
    }
    for (x = 0; x < result->width; x++) {
      f64 source_x = ((x + 0.5) * source->width / result->width) - 0.5;
      i32 x0 = (i32)floor(source_x);
      i32 x1;
      f64 fx = source_x - x0;
      f64 weights[4];
      const u8 *pixels[4];
      f64 alpha = 0;
      f64 premultiplied[3] = {0, 0, 0};
      i32 sample;
      u8 *destination;
      if (x0 < 0) {
        x0 = 0;
        fx = 0;
      }
      x1 = x0 + 1;
      if (x1 >= source->width) {
        x1 = source->width - 1;
      }
      weights[0] = (1 - fx) * (1 - fy);
      weights[1] = fx * (1 - fy);
      weights[2] = (1 - fx) * fy;
      weights[3] = fx * fy;
      pixels[0] = source->pixels + ((size_t)y0 * source->width + x0) * 4;
      pixels[1] = source->pixels + ((size_t)y0 * source->width + x1) * 4;
      pixels[2] = source->pixels + ((size_t)y1 * source->width + x0) * 4;
      pixels[3] = source->pixels + ((size_t)y1 * source->width + x1) * 4;
      for (sample = 0; sample < 4; sample++) {
        f64 sample_alpha = pixels[sample][3] / 255.0;
        alpha += sample_alpha * weights[sample];
        premultiplied[0] += pixels[sample][0] * sample_alpha * weights[sample];
        premultiplied[1] += pixels[sample][1] * sample_alpha * weights[sample];
        premultiplied[2] += pixels[sample][2] * sample_alpha * weights[sample];
      }
      destination = result->pixels + ((size_t)y * result->width + x) * 4;
      if (alpha > 1e-12) {
        destination[0] = (u8)lround(fmin(255, premultiplied[0] / alpha));
        destination[1] = (u8)lround(fmin(255, premultiplied[1] / alpha));
        destination[2] = (u8)lround(fmin(255, premultiplied[2] / alpha));
      } else {
        destination[0] = destination[1] = destination[2] = 0;
      }
      destination[3] = (u8)lround(fmin(255, alpha * 255));
    }
  }
  return 1;
}
