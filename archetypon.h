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

struct archetypon_svg_document;
struct archetypon_svg_plan;

struct archetypon_svg_document *archetypon_svg_document_create(
	const char *source, size_t length, char *error, size_t error_capacity);
void archetypon_svg_document_free(struct archetypon_svg_document *document);
int archetypon_svg_document_canvas_size(
	const struct archetypon_svg_document *document, double *width,
	double *height);
const uint8_t *archetypon_svg_document_source(
	const struct archetypon_svg_document *document);
size_t archetypon_svg_document_source_length(
	const struct archetypon_svg_document *document);

struct archetypon_svg_plan *archetypon_svg_plan_create(
	const struct archetypon_svg_document *document, int32_t output_width,
	int32_t output_height, char *error, size_t error_capacity);
struct archetypon_svg_plan *archetypon_svg_plan_retain(
	struct archetypon_svg_plan *plan);
void archetypon_svg_plan_release(struct archetypon_svg_plan *plan);
const uint8_t *archetypon_svg_plan_pixels(
	const struct archetypon_svg_plan *plan);
int32_t archetypon_svg_plan_width(const struct archetypon_svg_plan *plan);
int32_t archetypon_svg_plan_height(const struct archetypon_svg_plan *plan);
size_t archetypon_svg_plan_cost(const struct archetypon_svg_plan *plan);


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
