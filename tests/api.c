#include "../archetypon.h"

#include <stdbool.h>
#include <stdio.h>
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
	static const char unsupported_join[] =
		"<svg viewBox=\"0 0 10 10\"><polyline "
		"points=\"1,9 5,1 9,9\" fill=\"none\" stroke=\"black\" "
		"stroke-linejoin=\"miter\"/></svg>";
	static const char invalid_opacity[] =
		"<svg viewBox=\"0 0 1 1\"><rect width=\"1\" height=\"1\" "
		"opacity=\"invalid\"/></svg>";

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
	    !expect_svg_rejected(unsupported_join,
				 sizeof(unsupported_join) - 1) ||
	    !expect_svg_rejected(invalid_opacity,
				 sizeof(invalid_opacity) - 1))
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
			goto out;
		archetypon_image_free(&resized);
	}
	if (archetypon_ico_encode(pngs, &ico, error, error_capacity) ||
	    ico.length < 4 || ico.data[0] != 0 || ico.data[1] != 0 ||
	    ico.data[2] != 1 || ico.data[3] != 0)
		goto out;
	status = 0;

out:
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
		"<svg viewBox=\"0 0 20 20\"><rect transform=\"translate(5,5) \" "
		"width=\"10\" height=\"10\" fill=\"red\"/></svg>";
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
		status = fail("trailing transform separator changed the output");
	archetypon_image_free(&image);
	return status;
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
	    test_invalid_viewbox() || test_trailing_transform_separator())
		goto out;
	status = 0;

out:
	archetypon_image_free(&image);
	return status;
}
