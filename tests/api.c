#include "../archetypon.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char sample_svg[] =
	"<svg viewBox=\"0 0 20 10\"><!--drop--><rect width=\"20\" "
	"height=\"10\" fill=\"#ff0000\"/></svg>";

static int fail(const char *message)
{
	fprintf(stderr, "API test failed: %s\n", message);
	return -1;
}

static bool buffer_contains(const struct archetypon_buffer *buffer,
			    const char *text)
{
	size_t text_length = strlen(text);
	size_t i;

	if (text_length > buffer->length)
		return false;
	for (i = 0; i <= buffer->length - text_length; i++) {
		if (memcmp(buffer->data + i, text, text_length) == 0)
			return true;
	}
	return false;
}

static bool expect_svg_rejected(const char *source, size_t length)
{
	struct archetypon_image image = { 0 };
	char error[256] = { 0 };
	int status;

	status = archetypon_svg_render(source, length, 1, 1, &image, error,
				       sizeof(error));
	archetypon_image_free(&image);
	return status < 0 && error[0] != 0;
}


static bool expect_svg_render_rejected(const char *source, size_t length,
			       int width, int height, const char *message)
{
	struct archetypon_image image = { 0 };
	char error[256] = { 0 };
	int status;

	status = archetypon_svg_render(source, length, width, height, &image,
				      error, sizeof(error));
	archetypon_image_free(&image);
	return status < 0 && strstr(error, message) != NULL;
}

static char *repeated_path_svg(size_t pairs, size_t *length)
{
	static const char prefix[] =
		"<svg viewBox=\"0 0 1 1\" preserveAspectRatio=\"none\"><path d=\"M0 0";
	static const char pair[] = " L1 1 L0 0";
	static const char suffix[] = " Z\"/></svg>";
	size_t capacity = sizeof(prefix) - 1 + pairs * (sizeof(pair) - 1) +
			  sizeof(suffix);
	char *source = malloc(capacity);
	char *cursor;
	size_t i;

	if (!source)
		return NULL;
	cursor = source;
	memcpy(cursor, prefix, sizeof(prefix) - 1);
	cursor += sizeof(prefix) - 1;
	for (i = 0; i < pairs; i++) {
		memcpy(cursor, pair, sizeof(pair) - 1);
		cursor += sizeof(pair) - 1;
	}
	memcpy(cursor, suffix, sizeof(suffix));
	*length = (size_t)(cursor - source) + sizeof(suffix) - 1;
	return source;
}

static int test_svg_resource_limits(void)
{
	static const char empty[] = "<svg viewBox=\"0 0 1 1\"/>";
	char *source;
	size_t length;
	int status = -1;

	if (!expect_svg_render_rejected(empty, sizeof(empty) - 1, 8193, 1,
					"output dimension exceeds 8192") ||
	    !expect_svg_render_rejected(empty, sizeof(empty) - 1, 4097, 1024,
					"surface exceeds 16777216"))
		return fail("oversized SVG render surface was accepted");
	source = repeated_path_svg(2100, &length);
	if (!source)
		return fail("could not allocate render-work test SVG");
	if (!expect_svg_render_rejected(source, length, 1, 8192,
					"render exceeds the work limit"))
		goto out;
	free(source);
	source = repeated_path_svg(131071, &length);
	if (!source)
		return fail("could not allocate sort-work test SVG");
	if (!expect_svg_render_rejected(source, length, 1, 120,
					"render exceeds the work limit"))
		goto out;
	free(source);
	source = repeated_path_svg(131072, &length);
	if (!source)
		return fail("could not allocate point-limit test SVG");
	if (!expect_svg_render_rejected(source, length, 1, 1,
					"path exceeds the point limit"))
		goto out;
	status = 0;

out:
	free(source);
	if (status)
		fail("pathological SVG path was accepted");
	return status;
}

static int test_rejections(void)
{
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
		"<svg viewBox=\"0 0 1 1\"></svg><rect width=\"1\" "
		"height=\"1\"/>";
	static const char mismatched_close[] =
		"<svg viewBox=\"0 0 1 1\"><g></svg></g>";

	static const char invalid_opacity[] =
		"<svg viewBox=\"0 0 1 1\"><rect width=\"1\" height=\"1\" "
		"opacity=\"invalid\"/></svg>";
	static const char invalid_dash[] =
		"<svg viewBox=\"0 0 10 10\"><path d=\"M0 5L10 5\" "
		"stroke=\"black\" stroke-dasharray=\"2,-1\"/></svg>";
	static const char too_many_dashes[] =
		"<svg viewBox=\"0 0 10 10\"><path d=\"M0 5L10 5\" "
		"stroke=\"black\" stroke-dasharray=\"1,1,1,1,1,1,1,1,1,1,"
		"1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,"
		"1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,"
		"1,1,1,1,1,1,1\"/></svg>";
	static const char invalid_miterlimit[] =
		"<svg viewBox=\"0 0 10 10\"><path d=\"M0 5L10 5\" "
		"stroke=\"black\" stroke-miterlimit=\"0.5\"/></svg>";

	if (!expect_svg_rejected(nonfinite_color,
				 sizeof(nonfinite_color) - 1) ||
	    !expect_svg_rejected(huge_geometry, sizeof(huge_geometry) - 1) ||
	    !expect_svg_rejected(huge_circle, sizeof(huge_circle) - 1) ||
	    !expect_svg_rejected(huge_arc, sizeof(huge_arc) - 1) ||
	    !expect_svg_rejected(group_opacity, sizeof(group_opacity) - 1) ||
	    !expect_svg_rejected(trailing_element,
				 sizeof(trailing_element) - 1) ||
	    !expect_svg_rejected(mismatched_close,
				 sizeof(mismatched_close) - 1) ||
	    !expect_svg_rejected(invalid_opacity,
				 sizeof(invalid_opacity) - 1) ||
	    !expect_svg_rejected(invalid_dash, sizeof(invalid_dash) - 1) ||
	    !expect_svg_rejected(too_many_dashes,
				 sizeof(too_many_dashes) - 1) ||
	    !expect_svg_rejected(invalid_miterlimit,
				 sizeof(invalid_miterlimit) - 1))
		return fail("unsafe or unsupported SVG was accepted");
	return 0;
}

static int test_render(struct archetypon_image *image, double *width,
		       double *height, char *error, size_t error_capacity)
{
	const uint8_t *pixel;

	if (archetypon_svg_canvas_size(sample_svg, sizeof(sample_svg) - 1,
				       width, height, error, error_capacity) ||
	    *width != 20.0 || *height != 10.0)
		return fail(error[0] ? error : "wrong SVG canvas size");
	if (archetypon_svg_render(sample_svg, sizeof(sample_svg) - 1, 64, 32,
				  image, error, error_capacity) ||
	    image->width != 64 || image->height != 32)
		return fail(error[0] ? error : "SVG render failed");
	pixel = image->pixels + ((size_t)16 * image->width + 32) * 4;
	if (pixel[0] != 255 || pixel[1] != 0 || pixel[2] != 0 ||
	    pixel[3] != 255)
		return fail("SVG renderer returned the wrong center pixel");
	return 0;
}

static int test_png(const struct archetypon_image *image, char *error,
		    size_t error_capacity)
{
	struct archetypon_buffer png = { 0 };
	int status = 0;

	if (archetypon_png_encode(image, &png, error, error_capacity) ||
	    png.length < 8 || memcmp(png.data, "\x89PNG\r\n\x1a\n", 8) != 0)
		status = fail(error[0] ? error :
			      "PNG encoder returned invalid data");
	archetypon_buffer_free(&png);
	return status;
}

static int test_webp(const struct archetypon_image *image, char *error,
		     size_t error_capacity)
{
	struct archetypon_buffer webp = { 0 };
	int status = 0;

	if (archetypon_webp_encode(image, &webp, error, error_capacity) ||
	    webp.length < 12 || memcmp(webp.data, "RIFF", 4) != 0 ||
	    memcmp(webp.data + 8, "WEBP", 4) != 0)
		status = fail(error[0] ? error :
			      "WebP encoder returned invalid data");
	archetypon_buffer_free(&webp);
	return status;
}

static int test_optimizer(char *error, size_t error_capacity)
{
	struct archetypon_buffer optimized = { 0 };
	int status = 0;

	if (archetypon_svg_optimize((const uint8_t *)sample_svg,
				    sizeof(sample_svg) - 1, &optimized, error,
				    error_capacity) ||
	    optimized.length == 0 ||
	    buffer_contains(&optimized, "<!--drop-->"))
		status = fail(error[0] ? error :
			      "SVG optimizer kept a comment");
	archetypon_buffer_free(&optimized);
	return status;
}

static int test_ico(const struct archetypon_image *image, double width,
		    double height, char *error, size_t error_capacity)
{
	static const int32_t sizes[] = { 16, 32, 48 };
	struct archetypon_image resized = { 0 };
	struct archetypon_buffer pngs[3] = { { 0 }, { 0 }, { 0 } };
	struct archetypon_buffer ico = { 0 };
	size_t i;
	int status = -1;

	for (i = 0; i < 3; i++) {
		if (archetypon_image_resize(image, sizes[i], width, height,
					    &resized, error, error_capacity) ||
		    archetypon_png_encode(&resized, &pngs[i], error,
					  error_capacity))
			goto out_free;
		archetypon_image_free(&resized);
	}
	if (archetypon_ico_encode(pngs, &ico, error, error_capacity) ||
	    ico.length < 4 || ico.data[0] != 0 || ico.data[1] != 0 ||
	    ico.data[2] != 1 || ico.data[3] != 0)
		goto out_free;
	status = 0;

out_free:
	if (status)
		fail(error[0] ? error : "ICO encoder returned invalid data");
	archetypon_image_free(&resized);
	for (i = 0; i < 3; i++)
		archetypon_buffer_free(&pngs[i]);
	archetypon_buffer_free(&ico);
	return status;
}

static int test_invalid_viewbox(void)
{
	static const char source[] = "<svg><text>x</text></svg>";
	struct archetypon_image image = { 0 };
	char error[256] = { 0 };
	int status;

	status = archetypon_svg_render(source, sizeof(source) - 1, 16, 16,
				       &image, error, sizeof(error));
	archetypon_image_free(&image);
	if (status >= 0 || !strstr(error, "positive viewBox"))
		return fail(error[0] ? error : "invalid viewBox was accepted");
	return 0;
}

static int test_trailing_transform_separator(void)
{
	static const char source[] =
		"<svg viewBox=\"0 0 20 20\"><rect "
		"transform=\"translate(5,5) \" "
		"width=\"10\" height=\"10\" fill=\"red\"/></svg>";
	static const char failure[] =
		"trailing transform separator changed the output";
	struct archetypon_image image = { 0 };
	char error[256] = { 0 };
	const uint8_t *pixel;
	int status;

	status = archetypon_svg_render(source, sizeof(source) - 1, 20, 20,
				       &image, error, sizeof(error));
	if (status < 0)
		return fail(error);
	pixel = image.pixels + ((size_t)10 * image.width + 10) * 4;
	if (pixel[0] != 255 || pixel[1] != 0 || pixel[2] != 0 ||
	    pixel[3] != 255)
		status = fail(failure);
	archetypon_image_free(&image);
	return status;
}

static const uint8_t *svg_pixel(const struct archetypon_image *image,
				int x, int y)
{
	return image->pixels + ((size_t)y * image->width + x) * 4;
}

static int render_test_svg(const char *source, int width, int height,
			   struct archetypon_image *image)
{
	char error[256] = { 0 };

	if (archetypon_svg_render(source, strlen(source), width, height, image,
				 error, sizeof(error)))
		return fail(error);
	return 0;
}

static int test_svg_strokes(void)
{
	static const char miter[] =
		"<svg viewBox=\"0 0 20 20\"><polyline points=\"6,18 10,6 14,18\" "
		"fill=\"none\" stroke=\"black\" stroke-width=\"4\" "
		"stroke-linejoin=\"miter\"/></svg>";
	static const char bevel[] =
		"<svg viewBox=\"0 0 20 20\"><polyline points=\"6,18 10,6 14,18\" "
		"fill=\"none\" stroke=\"black\" stroke-width=\"4\" "
		"stroke-linejoin=\"bevel\"/></svg>";
	static const char limited[] =
		"<svg viewBox=\"0 0 20 20\"><polyline points=\"6,18 10,6 14,18\" "
		"fill=\"none\" stroke=\"black\" stroke-width=\"4\" "
		"stroke-linejoin=\"miter\" stroke-miterlimit=\"1\"/></svg>";
	static const char dashed[] =
		"<svg viewBox=\"0 0 20 20\"><line x1=\"1\" y1=\"10\" x2=\"19\" "
		"y2=\"10\" stroke=\"black\" stroke-width=\"2\" "
		"stroke-dasharray=\"4 4\"/></svg>";
	static const char offset[] =
		"<svg viewBox=\"0 0 20 20\"><line x1=\"1\" y1=\"10\" x2=\"19\" "
		"y2=\"10\" stroke=\"black\" stroke-width=\"2\" "
		"style=\"stroke-dasharray: 4,4; stroke-dashoffset: 4\"/></svg>";
	static const char odd[] =
		"<svg viewBox=\"0 0 20 20\"><line x2=\"20\" y2=\"20\" "
		"stroke=\"black\" stroke-dasharray=\"1,2,3\" "
		"stroke-linejoin=\"round\"/></svg>";
	struct archetypon_image image = { 0 };
	int status = -1;

	if (render_test_svg(miter, 20, 20, &image) ||
	    svg_pixel(&image, 10, 1)[3] == 0)
		goto out;
	archetypon_image_free(&image);
	if (render_test_svg(bevel, 20, 20, &image) ||
	    svg_pixel(&image, 10, 1)[3] != 0)
		goto out;
	archetypon_image_free(&image);
	if (render_test_svg(limited, 20, 20, &image) ||
	    svg_pixel(&image, 10, 1)[3] != 0)
		goto out;
	archetypon_image_free(&image);
	if (render_test_svg(dashed, 20, 20, &image) ||
	    svg_pixel(&image, 2, 10)[3] == 0 ||
	    svg_pixel(&image, 6, 10)[3] != 0)
		goto out;
	archetypon_image_free(&image);
	if (render_test_svg(offset, 20, 20, &image) ||
	    svg_pixel(&image, 2, 10)[3] != 0 ||
	    svg_pixel(&image, 6, 10)[3] == 0)
		goto out;
	archetypon_image_free(&image);
	if (render_test_svg(odd, 20, 20, &image))
		goto out;
	status = 0;

out:
	archetypon_image_free(&image);
	if (status)
		fail("SVG stroke joins or dashes rendered incorrectly");
	return status;
}

static int test_svg_geometry_and_aspect_ratio(void)
{
	static const char intrinsic[] =
		"<svg width=\"40\" height=\"20\" viewBox=\"0 0 10 10\"/>";
	static const char aligned[] =
		"<svg viewBox=\"0 0 10 10\" preserveAspectRatio=\"xMinYMid meet\">"
		"<rect width=\"10\" height=\"10\" fill=\"red\"/></svg>";
	static const char stretched[] =
		"<svg viewBox=\"0 0 10 10\" preserveAspectRatio=\"none\">"
		"<rect width=\"10\" height=\"10\" fill=\"red\"/></svg>";
	static const char sliced[] =
		"<svg viewBox=\"0 0 20 10\" preserveAspectRatio=\"xMaxYMax slice\">"
		"<rect x=\"10\" width=\"10\" height=\"10\" fill=\"red\"/></svg>";
	struct archetypon_image image = { 0 };
	char error[256] = { 0 };
	double width;
	double height;
	int status = -1;

	if (archetypon_svg_canvas_size(intrinsic, strlen(intrinsic), &width,
				       &height, error, sizeof(error)) ||
	    width != 40 || height != 20)
		return fail(error[0] ? error : "root size did not override viewBox");
	if (render_test_svg(aligned, 20, 10, &image) ||
	    svg_pixel(&image, 0, 5)[3] != 255 ||
	    svg_pixel(&image, 15, 5)[3] != 0)
		goto out;
	archetypon_image_free(&image);
	if (render_test_svg(stretched, 20, 10, &image) ||
	    svg_pixel(&image, 19, 5)[3] != 255)
		goto out;
	archetypon_image_free(&image);
	if (render_test_svg(sliced, 10, 10, &image) ||
	    svg_pixel(&image, 0, 5)[3] != 255)
		goto out;
	status = 0;

out:
	archetypon_image_free(&image);
	if (status)
		fail("preserveAspectRatio mapping is incorrect");
	return status;
}

static int test_svg_visibility_override(void)
{
	static const char source[] =
		"<svg viewBox=\"0 0 1 1\"><g visibility=\"hidden\">"
		"<rect width=\"1\" height=\"1\" fill=\"red\"/>"
		"<rect width=\"1\" height=\"1\" fill=\"blue\" "
		"visibility=\"visible\"/></g></svg>";
	struct archetypon_image image = { 0 };
	const uint8_t *pixel;
	int status = -1;

	if (render_test_svg(source, 1, 1, &image))
		goto out;
	pixel = svg_pixel(&image, 0, 0);
	if (pixel[0] != 0 || pixel[1] != 0 || pixel[2] != 255 ||
	    pixel[3] != 255)
		goto out;
	status = 0;

out:
	archetypon_image_free(&image);
	if (status)
		fail("visibility:visible did not override hidden ancestor");
	return status;
}

static int test_svg_well_formedness(void)
{
	static const char * const invalid[] = {
		"<svg viewBox=\"0 0 1 1\"><rect width=1/></svg>",
		"<svg viewBox=\"0 0 1 1\"></svg extra>",
		"< svg viewBox=\"0 0 1 1\"/>",
		"<svg viewBox=\"0 0 1 1\"><g / ></g></svg>",
		"<svg viewBox=\"0 0 1 1\"><rect width=\"1></svg>",
		"<svg viewBox=\"0 0 1 1\"width=\"1\"/>",
		"<svg viewBox=\"0 0 1 1\" viewBox=\"0 0 2 2\"/>"
	};
	static const char unicode_name[] =
		"<svg viewBox=\"0 0 1 1\" données=\"valid\"/>";
	char error[256] = { 0 };
	double width;
	double height;
	size_t i;

	for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
		if (!expect_svg_rejected(invalid[i], strlen(invalid[i])))
			return fail("malformed XML-like SVG was accepted");
	}
	if (archetypon_svg_canvas_size(unicode_name, strlen(unicode_name),
				       &width, &height, error, sizeof(error)) ||
	    width != 1 || height != 1)
		return fail(error[0] ? error : "valid Unicode XML name was rejected");
	return 0;
}

int main(void)
{
	struct archetypon_image image = { 0 };
	double width;
	double height;
	char error[256] = { 0 };
	int status = 1;

	if (test_rejections() ||
	    test_render(&image, &width, &height, error, sizeof(error)) ||
	    test_png(&image, error, sizeof(error)) ||
	    test_webp(&image, error, sizeof(error)) ||
	    test_optimizer(error, sizeof(error)) ||
	    test_ico(&image, width, height, error, sizeof(error)) ||
	    test_invalid_viewbox() || test_trailing_transform_separator() ||
	    test_svg_geometry_and_aspect_ratio() ||
	    test_svg_visibility_override() || test_svg_well_formedness() ||
	    test_svg_strokes() || test_svg_resource_limits())
		goto out_free_image;
	status = 0;

out_free_image:
	archetypon_image_free(&image);
	return status;
}
