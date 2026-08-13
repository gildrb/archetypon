#include "internal.h"

#include <math.h>

struct axis_sample {
	s32 first;
	s32 second;
	double fraction;
};

void archetypon_image_free(struct archetypon_image *image)
{
	if (!image)
		return;
	free(image->pixels);
	memset(image, 0, sizeof(*image));
}

static struct axis_sample axis_sample(double coordinate, s32 limit)
{
	struct axis_sample sample;

	sample.first = (s32)floor(coordinate);
	sample.fraction = coordinate - sample.first;
	if (sample.first < 0) {
		sample.first = 0;
		sample.fraction = 0;
	}
	sample.second = sample.first + 1;
	if (sample.second >= limit)
		sample.second = limit - 1;
	return sample;
}

static const u8 *source_pixel(const struct archetypon_image *image, s32 x,
			      s32 y)
{
	return image->pixels + ((size_t)y * image->width + x) * 4;
}

static void resize_pixel(const struct archetypon_image *source,
			 struct archetypon_image *result, s32 x, s32 y,
			 struct axis_sample sy)
{
	double source_x = ((x + 0.5) * source->width / result->width) - 0.5;
	struct axis_sample sx = axis_sample(source_x, source->width);
	const u8 *pixels[4];
	double weights[4];
	double alpha = 0;
	double color[3] = { 0, 0, 0 };
	u8 *destination;
	s32 i;

	weights[0] = (1 - sx.fraction) * (1 - sy.fraction);
	weights[1] = sx.fraction * (1 - sy.fraction);
	weights[2] = (1 - sx.fraction) * sy.fraction;
	weights[3] = sx.fraction * sy.fraction;
	pixels[0] = source_pixel(source, sx.first, sy.first);
	pixels[1] = source_pixel(source, sx.second, sy.first);
	pixels[2] = source_pixel(source, sx.first, sy.second);
	pixels[3] = source_pixel(source, sx.second, sy.second);
	for (i = 0; i < 4; i++) {
		double sample_alpha = pixels[i][3] / 255.0;

		alpha += sample_alpha * weights[i];
		color[0] += pixels[i][0] * sample_alpha * weights[i];
		color[1] += pixels[i][1] * sample_alpha * weights[i];
		color[2] += pixels[i][2] * sample_alpha * weights[i];
	}
	destination = result->pixels + ((size_t)y * result->width + x) * 4;
	if (alpha > 1e-12) {
		destination[0] = (u8)lround(fmin(255, color[0] / alpha));
		destination[1] = (u8)lround(fmin(255, color[1] / alpha));
		destination[2] = (u8)lround(fmin(255, color[2] / alpha));
	} else {
		destination[0] = 0;
		destination[1] = 0;
		destination[2] = 0;
	}
	destination[3] = (u8)lround(fmin(255, alpha * 255));
}

static void resize_dimensions(struct archetypon_image *result, s32 edge,
			      double width, double height)
{
	if (width >= height) {
		result->width = edge;
		result->height = (s32)lround(edge * height / width);
	} else {
		result->height = edge;
		result->width = (s32)lround(edge * width / height);
	}
	if (result->width < 1)
		result->width = 1;
	if (result->height < 1)
		result->height = 1;
}

int archetypon_image_resize(const struct archetypon_image *source, s32 edge,
			    double width, double height,
			    struct archetypon_image *result, char *error,
			    size_t error_capacity)
{
	size_t bytes;
	s32 y;

	if (!result) {
		archetypon_set_error(error, error_capacity,
				     "missing resized image output");
		return -1;
	}
	memset(result, 0, sizeof(*result));
	if (!source || source->width < 1 || source->height < 1 ||
	    !source->pixels || edge < 1 || !isfinite(width) ||
	    !isfinite(height) || width <= 0 || height <= 0) {
		archetypon_set_error(error, error_capacity,
				     "invalid image resize");
		return -1;
	}
	resize_dimensions(result, edge, width, height);
	if (!archetypon_multiply_size((size_t)result->width,
				      (size_t)result->height, &bytes) ||
	    !archetypon_multiply_size(bytes, 4, &bytes)) {
		archetypon_set_error(error, error_capacity,
				     "image size overflow");
		return -1;
	}
	result->pixels = malloc(bytes);
	if (!result->pixels) {
		archetypon_set_error(error, error_capacity,
				     "out of memory resizing image");
		return -1;
	}
	for (y = 0; y < result->height; y++) {
		double source_y =
			((y + 0.5) * source->height / result->height) - 0.5;
		struct axis_sample sy = axis_sample(source_y, source->height);
		s32 x;

		for (x = 0; x < result->width; x++)
			resize_pixel(source, result, x, y, sy);
	}
	return 0;
}
