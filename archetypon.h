#ifndef ARCHETYPON_H
#define ARCHETYPON_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct archetypon_buffer {
	uint8_t *data;
	size_t length;
	size_t capacity;
};

struct archetypon_image {
	int32_t width;
	int32_t height;
	uint8_t *pixels;
};

void archetypon_buffer_free(struct archetypon_buffer *buffer);
void archetypon_image_free(struct archetypon_image *image);

int archetypon_svg_canvas_size(const char *source, size_t length, double *width,
			       double *height, char *error,
			       size_t error_capacity);

int archetypon_svg_render(const char *source, size_t length,
			  int32_t output_width, int32_t output_height,
			  struct archetypon_image *image, char *error,
			  size_t error_capacity);

int archetypon_svg_optimize(const uint8_t *source, size_t length,
			    struct archetypon_buffer *optimized, char *error,
			    size_t error_capacity);

int archetypon_image_resize(const struct archetypon_image *source,
			    int32_t maximum_dimension, double aspect_width,
			    double aspect_height,
			    struct archetypon_image *result, char *error,
			    size_t error_capacity);

int archetypon_png_encode(const struct archetypon_image *image,
			  struct archetypon_buffer *png, char *error,
			  size_t error_capacity);

int archetypon_webp_encode(const struct archetypon_image *image,
			   struct archetypon_buffer *webp, char *error,
			   size_t error_capacity);

int archetypon_ico_encode(const struct archetypon_buffer pngs[3],
			  struct archetypon_buffer *ico, char *error,
			  size_t error_capacity);

#ifdef __cplusplus
}
#endif

#endif
