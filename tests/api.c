#include "../archetypon.h"

#include <stdio.h>
#include <string.h>

static int fail(const char *message) {
  fprintf(stderr, "API test failed: %s\n", message);
  return 1;
}

static int buffer_contains(const ArchetyponBuffer *buffer, const char *text) {
  size_t text_length = strlen(text);
  size_t index;

  if (text_length > buffer->length) {
    return 0;
  }
  for (index = 0; index <= buffer->length - text_length; index++) {
    if (memcmp(buffer->data + index, text, text_length) == 0) {
      return 1;
    }
  }
  return 0;
}

static int expect_svg_rejected(const char *source, size_t length) {
  ArchetyponImage image = {0};
  char error[256] = {0};
  int32_t success = archetypon_svg_render(source, length, 1, 1, &image, error,
                                           sizeof(error));

  archetypon_image_free(&image);
  return !success && error[0] != 0;
}

int main(void) {
  static const char svg[] =
      "<svg viewBox=\"0 0 20 10\"><!--drop--><rect width=\"20\" "
      "height=\"10\" fill=\"#ff0000\"/></svg>";
  static const char nonfinite_color[] =
      "<svg viewBox=\"0 0 1 1\"><rect width=\"1\" height=\"1\" "
      "fill=\"rgb(nan,0,0)\"/></svg>";
  static const char huge_geometry[] =
      "<svg viewBox=\"0 0 1 1\"><rect y=\"1e308\" width=\"1\" "
      "height=\"1\"/></svg>";
  static const char huge_circle[] =
      "<svg viewBox=\"0 0 1 1\"><circle r=\"1e308\"/></svg>";
  static const char huge_arc[] =
      "<svg viewBox=\"0 0 1 1\"><path "
      "d=\"M0 0 A1e308 1e308 0 0 1 1 1 Z\"/></svg>";
  static const char group_opacity[] =
      "<svg viewBox=\"0 0 1 1\"><g opacity=\"0.5\"><rect width=\"1\" "
      "height=\"1\"/></g></svg>";
  static const char trailing_element[] =
      "<svg viewBox=\"0 0 1 1\"></svg><rect width=\"1\" height=\"1\"/>";
  static const char mismatched_close[] =
      "<svg viewBox=\"0 0 1 1\"><g></svg></g>";
  static const char unsupported_join[] =
      "<svg viewBox=\"0 0 10 10\"><polyline points=\"1,9 5,1 9,9\" "
      "fill=\"none\" stroke=\"black\" stroke-linejoin=\"miter\"/></svg>";
  ArchetyponImage image = {0};
  ArchetyponImage resized = {0};
  ArchetyponBuffer png = {0};
  ArchetyponBuffer webp = {0};
  ArchetyponBuffer optimized = {0};
  ArchetyponBuffer ico_pngs[3] = {{0}, {0}, {0}};
  ArchetyponBuffer ico = {0};
  double width;
  double height;
  char error[256] = {0};
  size_t index;
  int status = 1;

  if (!expect_svg_rejected(nonfinite_color, sizeof(nonfinite_color) - 1) ||
      !expect_svg_rejected(huge_geometry, sizeof(huge_geometry) - 1) ||
      !expect_svg_rejected(huge_circle, sizeof(huge_circle) - 1) ||
      !expect_svg_rejected(huge_arc, sizeof(huge_arc) - 1) ||
      !expect_svg_rejected(group_opacity, sizeof(group_opacity) - 1) ||
      !expect_svg_rejected(trailing_element, sizeof(trailing_element) - 1) ||
      !expect_svg_rejected(mismatched_close, sizeof(mismatched_close) - 1) ||
      !expect_svg_rejected(unsupported_join, sizeof(unsupported_join) - 1)) {
    return fail("unsafe or unsupported SVG was accepted");
  }
  if (!archetypon_svg_canvas_size(svg, sizeof(svg) - 1, &width, &height,
                                   error, sizeof(error)) ||
      width != 20.0 || height != 10.0) {
    return fail(error[0] == 0 ? "wrong SVG canvas size" : error);
  }
  if (!archetypon_svg_render(svg, sizeof(svg) - 1, 64, 32, &image, error,
                              sizeof(error)) ||
      image.width != 64 || image.height != 32) {
    return fail(error[0] == 0 ? "SVG render failed" : error);
  }
  {
    const uint8_t *pixel = image.pixels + ((size_t)16 * image.width + 32) * 4;
    if (pixel[0] != 255 || pixel[1] != 0 || pixel[2] != 0 || pixel[3] != 255) {
      return fail("SVG renderer returned the wrong center pixel");
    }
  }
  if (!archetypon_png_encode(&image, &png, error, sizeof(error)) ||
      png.length < 8 || memcmp(png.data, "\x89PNG\r\n\x1a\n", 8) != 0) {
    return fail(error[0] == 0 ? "PNG encoder returned invalid data" : error);
  }
  if (!archetypon_webp_encode(&image, &webp, error, sizeof(error)) ||
      webp.length < 12 || memcmp(webp.data, "RIFF", 4) != 0 ||
      memcmp(webp.data + 8, "WEBP", 4) != 0) {
    return fail(error[0] == 0 ? "WebP encoder returned invalid data" : error);
  }
  if (!archetypon_svg_optimize((const uint8_t *)svg, sizeof(svg) - 1,
                                &optimized, error, sizeof(error)) ||
      optimized.length == 0 ||
      buffer_contains(&optimized, "<!--drop-->")) {
    return fail(error[0] == 0 ? "SVG optimizer kept a comment" : error);
  }

  for (index = 0; index < 3; index++) {
    static const int32_t sizes[] = {16, 32, 48};
    if (!archetypon_image_resize(&image, sizes[index], width, height, &resized,
                                  error, sizeof(error)) ||
        !archetypon_png_encode(&resized, &ico_pngs[index], error,
                               sizeof(error))) {
      goto cleanup;
    }
    archetypon_image_free(&resized);
  }
  if (!archetypon_ico_encode(ico_pngs, &ico, error, sizeof(error)) ||
      ico.length < 4 || ico.data[0] != 0 || ico.data[1] != 0 ||
      ico.data[2] != 1 || ico.data[3] != 0) {
    goto cleanup;
  }

  error[0] = 0;
  if (archetypon_svg_render("<svg><text>x</text></svg>", 25, 16, 16,
                            &resized, error, sizeof(error)) ||
      strstr(error, "positive viewBox") == NULL) {
    goto cleanup;
  }
  status = 0;

cleanup:
  if (status != 0) {
    fail(error[0] == 0 ? "format package check failed" : error);
  }
  archetypon_image_free(&resized);
  archetypon_image_free(&image);
  archetypon_buffer_free(&png);
  archetypon_buffer_free(&webp);
  archetypon_buffer_free(&optimized);
  for (index = 0; index < 3; index++) {
    archetypon_buffer_free(&ico_pngs[index]);
  }
  archetypon_buffer_free(&ico);
  return status;
}
