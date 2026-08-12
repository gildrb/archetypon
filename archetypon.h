#ifndef ARCHETYPON_H
#define ARCHETYPON_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  uint8_t *data;
  size_t length;
  size_t capacity;
} ArchetyponBuffer;

typedef struct {
  int32_t width;
  int32_t height;
  uint8_t *pixels;
} ArchetyponImage;

void archetypon_buffer_free(ArchetyponBuffer *buffer);
void archetypon_image_free(ArchetyponImage *image);

int32_t archetypon_svg_canvas_size(const char *source, size_t length,
                                   double *width, double *height, char *error,
                                   size_t error_capacity);
int32_t archetypon_svg_render(const char *source, size_t length,
                              int32_t output_width, int32_t output_height,
                              ArchetyponImage *image, char *error,
                              size_t error_capacity);
int32_t archetypon_svg_optimize(const uint8_t *source, size_t length,
                                ArchetyponBuffer *optimized, char *error,
                                size_t error_capacity);

int32_t archetypon_image_resize(const ArchetyponImage *source,
                                int32_t maximum_dimension,
                                double aspect_width, double aspect_height,
                                ArchetyponImage *result, char *error,
                                size_t error_capacity);
int32_t archetypon_png_encode(const ArchetyponImage *image,
                              ArchetyponBuffer *png, char *error,
                              size_t error_capacity);
int32_t archetypon_webp_encode(const ArchetyponImage *image,
                               ArchetyponBuffer *webp, char *error,
                               size_t error_capacity);
int32_t archetypon_ico_encode(const ArchetyponBuffer pngs[3],
                              ArchetyponBuffer *ico, char *error,
                              size_t error_capacity);

#ifdef __cplusplus
}
#endif

#endif
