#include "../archetypon.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void archetypon_svg_counters_reset(void);
size_t archetypon_svg_document_compile_count(void);
size_t archetypon_svg_plan_compile_count(void);

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

static char *repeated_shapes_svg(size_t count, size_t *length)
{
	static const char prefix[] = "<svg viewBox=\"0 0 1 1\">";
	static const char shape[] = "<rect width=\"1\" height=\"1\"/>";
	static const char suffix[] = "</svg>";
	size_t capacity = sizeof(prefix) - 1 + count * (sizeof(shape) - 1) +
			  sizeof(suffix);
	char *source = malloc(capacity);
	char *cursor;
	size_t index;

	if (!source)
		return NULL;
	cursor = source;
	memcpy(cursor, prefix, sizeof(prefix) - 1);
	cursor += sizeof(prefix) - 1;
	for (index = 0; index < count; index++) {
		memcpy(cursor, shape, sizeof(shape) - 1);
		cursor += sizeof(shape) - 1;
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
	free(source);
	source = repeated_shapes_svg(100000, &length);
	if (!source)
		return fail("could not allocate scene-limit test SVG");
	if (!expect_svg_rejected(source, length))
		goto out;
	free(source);
	length = 32u * 1024u * 1024u + 1u;
	source = calloc(length, 1);
	if (!source)
		return fail("could not allocate source-limit test SVG");
	if (!expect_svg_rejected(source, length))
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
	static const char percentage[] =
		"<svg width=\"100%\" height=\"100%\" viewBox=\"0 0 30 12\" "
		"style=\"clip-rule:evenodd\"/>";
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
	if (archetypon_svg_canvas_size(percentage, strlen(percentage), &width,
				       &height, error, sizeof(error)) ||
	    width != 30 || height != 12)
		return fail(error[0] ? error : "percentage root size is wrong");
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

static int test_retained_svg_api(void)
{
	static const char source[] =
		"<svg viewBox=\"0 0 2 1\"><rect width=\"1\" height=\"1\" "
		"fill=\"red\"/><rect x=\"1\" width=\"1\" height=\"1\" "
		"fill=\"blue\"/></svg>";
	struct archetypon_svg_document *document = NULL;
	struct archetypon_svg_plan *plan = NULL;
	struct archetypon_image legacy = { 0 };
	char error[256] = { 0 };
	size_t bytes = 80u * 40u * 4u;
	int status = -1;

	if (archetypon_svg_render(source, sizeof(source) - 1, 80, 40,
				  &legacy, error, sizeof(error)))
		goto cleanup;
	archetypon_svg_counters_reset();
	document = archetypon_svg_document_create(source, sizeof(source) - 1,
						  error, sizeof(error));
	if (!document || archetypon_svg_document_compile_count() != 1 ||
	    archetypon_svg_document_source_length(document) !=
		    sizeof(source) - 1 ||
	    memcmp(archetypon_svg_document_source(document), source,
		   sizeof(source) - 1) != 0)
		goto cleanup;
	plan = archetypon_svg_plan_create(document, 80, 40, error,
					  sizeof(error));
	if (!plan || archetypon_svg_plan_compile_count() != 1 ||
	    archetypon_svg_plan_width(plan) != 80 ||
	    archetypon_svg_plan_height(plan) != 40 ||
	    memcmp(legacy.pixels, archetypon_svg_plan_pixels(plan), bytes) != 0)
		goto cleanup;
	status = 0;
cleanup:
	archetypon_svg_plan_release(plan);
	archetypon_svg_document_free(document);
	archetypon_image_free(&legacy);
	if (status)
		fail(error[0] ? error : "retained SVG plan differs from legacy render");
	return status;
}

static int test_modern_rgb(void)
{
	static const char *const paints[] = {
		"rgb(255 64 128)", "rgb(255,64,128)",
		"rgb(255 64 128 / 0.5)", "rgba(255, 64, 128, 0.5)"
	};
	static const char *const invalid[] = {
		"rgb(1,2,3)junk", "rgb(1 2 3)junk", "rgb(1,2,3",
		"rgb(1 2 3", "rgb(1,2 3)", "rgb(1 2,3)",
		"rgb(1 2 3 /)", "rgb(1 2 3 / nan)", "rgb(1,2,3,4)",
		"rgb(1.2.3)", "rgba(1,2,3)", "rgb(256 0 0)"
	};
	struct archetypon_image image = {0};
	char source[256];
	size_t index;

	for (index = 0; index < sizeof(paints) / sizeof(*paints); index++) {
		const uint8_t *pixel;
		snprintf(source, sizeof(source), "<svg viewBox='0 0 1 1'>"
			 "<rect width='1' height='1' fill='%s'/></svg>", paints[index]);
		if (render_test_svg(source, 1, 1, &image))
			return -1;
		pixel = image.pixels;
		if (pixel[0] != 255 || pixel[1] != 64 || pixel[2] != 128 ||
		    pixel[3] != (index < 2 ? 255 : 128)) {
			archetypon_image_free(&image);
			return fail("modern RGB color or alpha differs");
		}
		archetypon_image_free(&image);
	}
	for (index = 0; index < sizeof(invalid) / sizeof(*invalid); index++) {
		snprintf(source, sizeof(source), "<svg viewBox='0 0 1 1'>"
			 "<rect width='1' height='1' fill='%s'/></svg>", invalid[index]);
		if (!expect_svg_rejected(source, strlen(source)))
			return fail("malformed RGB color was accepted");
	}
	return 0;
}

static int test_radial_gradients(void)
{
	static const char prefix[] = "<svg viewBox='0 0 10 10'><defs>"
		"<radialGradient id='r' gradientUnits='userSpaceOnUse' ";
	static const char suffix[] = "><stop offset='0' stop-color='red'/>"
		"<stop offset='1' stop-color='blue'/></radialGradient></defs>"
		"<rect width='10' height='10' fill='url(#r)'/></svg>";
	static const char *const invalid[] = {
		"cx='50%' cy='4.5' r='4'", "cx='4.5' cy='4.5' r='-1'",
		"cx='4.5' cy='4.5' r='4' fx='8.5'", "cx='4.5' cy='4.5' r='4' fr='1'",
		"cx='4.5' cy='4.5' r='4' href='#r'", "cx='4.5' cy='4.5' r='4' spreadMethod='repeat'"
	};
	struct archetypon_image image = {0};
	char source[1024];
	const uint8_t *pixel;
	size_t index;

	snprintf(source, sizeof(source), "%scx='4.5' cy='4.5' r='4'%s", prefix, suffix);
	if (render_test_svg(source, 10, 10, &image))
		return -1;
	pixel = svg_pixel(&image, 4, 4);
	if (pixel[0] != 232 || pixel[1] != 0 || pixel[2] != 23 || pixel[3] != 255 ||
	    svg_pixel(&image, 0, 0)[2] != 255 ||
	    memcmp(svg_pixel(&image, 3, 4), svg_pixel(&image, 5, 4), 4)) {
		archetypon_image_free(&image);
		return fail("radial center, symmetry, or pad color is wrong");
	}
	archetypon_image_free(&image);
	snprintf(source, sizeof(source), "%scx='4.5' cy='4.5' r='4' fx='2.5' fy='4.5'%s", prefix, suffix);
	if (render_test_svg(source, 10, 10, &image))
		return -1;
	if (svg_pixel(&image, 2, 4)[0] < 225 || svg_pixel(&image, 7, 4)[2] < 210) {
		archetypon_image_free(&image);
		return fail("off-center radial focus is wrong");
	}
	archetypon_image_free(&image);
	snprintf(source, sizeof(source), "%scx='4.5' cy='4.5' r='4' gradientTransform='translate(1 0)'%s", prefix, suffix);
	if (render_test_svg(source, 10, 10, &image))
		return -1;
	if (svg_pixel(&image, 5, 4)[0] != 232) {
		archetypon_image_free(&image);
		return fail("radial gradient transform is wrong");
	}
	archetypon_image_free(&image);
	snprintf(source, sizeof(source), "%scx='4.5' cy='4.5' r='0'%s", prefix, suffix);
	if (render_test_svg(source, 10, 10, &image))
		return -1;
	if (svg_pixel(&image, 4, 4)[2] != 255 || svg_pixel(&image, 0, 0)[2] != 255) {
		archetypon_image_free(&image);
		return fail("zero-radius gradient did not use the final stop");
	}
	archetypon_image_free(&image);
	for (index = 0; index < sizeof(invalid) / sizeof(*invalid); index++) {
		snprintf(source, sizeof(source), "%s%s%s", prefix, invalid[index], suffix);
		if (!expect_svg_rejected(source, strlen(source)))
			return fail("unsupported radial gradient was accepted");
	}
	return 0;
}

static char *effect_limit_svg(size_t effects, size_t shapes)
{
	static const char shape[] = "<path d='M0 0H8V8H0Z' fill='white'/>";
	size_t capacity = effects * (shapes * (sizeof(shape) - 1) + 192) + 256;
	char *source = malloc(capacity);
	char *cursor = source;
	size_t i, j;

	if (!source)
		return NULL;
	cursor += snprintf(cursor, capacity - (size_t)(cursor - source), "<svg viewBox='0 0 8 8'><defs>");
	for (i = 0; i < effects; i++) {
		cursor += snprintf(cursor, capacity - (size_t)(cursor - source), "<mask id='m%zu' mask-type='alpha' maskUnits='userSpaceOnUse' "
			"x='0' y='0' width='8' height='8'>", i);
		for (j = 0; j < shapes; j++) {
			memcpy(cursor, shape, sizeof(shape) - 1);
			cursor += sizeof(shape) - 1;
		}
		cursor += snprintf(cursor, capacity - (size_t)(cursor - source), "</mask>");
	}
	snprintf(cursor, capacity - (size_t)(cursor - source), "</defs><g mask='url(#m0)'><rect width='8' height='8' fill='red'/></g></svg>");
	return source;
}

static int test_dynamic_effect_limits(void)
{
	static const size_t cases[][2] = {{1, 65}, {1, 4096}, {432, 1}};
	static const char nested[] = "<svg viewBox='0 0 8 8'><defs>"
		"<mask id='a'><path d='M0 0H4V8H0Z'/></mask>"
		"<mask id='b'><path d='M0 0H8V8H0Z' mask='url(#a)'/></mask>"
		"</defs><rect width='8' height='8' mask='url(#b)'/></svg>";
	struct archetypon_image image = {0};
	char *source;
	size_t index;

	for (index = 0; index < sizeof(cases) / sizeof(*cases); index++) {
		source = effect_limit_svg(cases[index][0], cases[index][1]);
		if (!source)
			return fail("could not allocate effect limit fixture");
		if (render_test_svg(source, 8, 8, &image)) {
			free(source);
			return -1;
		}
		free(source);
		if (svg_pixel(&image, 4, 4)[0] != 255 || svg_pixel(&image, 4, 4)[3] != 255) {
			archetypon_image_free(&image);
			return fail("dynamic mask storage lost shapes");
		}
		archetypon_image_free(&image);
	}
	source = effect_limit_svg(1, 25001);
	if (!source)
		return fail("could not allocate effect limit rejection fixture");
	if (!expect_svg_render_rejected(source, strlen(source), 8, 8, "25000 clip/mask shapes")) {
		free(source);
		return fail("effect count limit did not report an error");
	}
	free(source);
	if (!expect_svg_render_rejected(nested, sizeof(nested) - 1, 8, 8, "nested SVG clip/mask references"))
		return fail("unsupported nested mask reference was silently ignored");
	return 0;
}

static int test_compact_scene_commands(void)
{
	struct archetypon_image image = {0};
	size_t length;
	char *source = repeated_shapes_svg(25000, &length);

	if (!source)
		return fail("could not allocate compact scene fixture");
	if (render_test_svg(source, 8, 8, &image)) {
		free(source);
		return -1;
	}
	free(source);
	if (svg_pixel(&image, 4, 4)[3] != 255) {
		archetypon_image_free(&image);
		return fail("compact commands lost shape records");
	}
	archetypon_image_free(&image);
	source = repeated_shapes_svg(32769, &length);
	if (!source)
		return fail("could not allocate scene limit fixture");
	if (!expect_svg_render_rejected(source, length, 1, 1, "SVG scene exceeds")) {
		free(source);
		return fail("compact commands bypassed the scene budget");
	}
	free(source);
	return 0;
}

static int test_cropped_effect_pixels(void)
{
	static const char scene[] =
		"<linearGradient id='l' gradientUnits='userSpaceOnUse' x1='0' y1='0' x2='96' y2='0'>"
		"<stop stop-color='red'/><stop offset='1' stop-color='blue'/></linearGradient>"
		"<radialGradient id='r' gradientUnits='userSpaceOnUse' cx='48' cy='32' r='24' fx='44'>"
		"<stop stop-color='white'/><stop offset='1' stop-color='black'/></radialGradient>"
		"<mask id='a' maskUnits='userSpaceOnUse' x='0' y='0' width='96' height='64' mask-type='alpha'>"
		"<path d='M15.25 10H73V53H15.25Z' fill='white' transform='rotate(7 48 32)'/></mask>"
		"<mask id='b' maskUnits='userSpaceOnUse' x='0' y='0' width='96' height='64' mask-type='alpha'>"
		"<path d='M24 15H67V48H24Z M30 24H36V36H30Z' fill='white' fill-rule='evenodd'/></mask>"
		"<clipPath id='c'><path d='M18 8H70V55H18Z'/></clipPath>"
		"<g transform='translate(7 3) scale(.9 .8)' opacity='.75' mask='url(#a)'>"
		"<g mask='url(#b)' clip-path='url(#c)'><rect width='96' height='64' fill='url(#l)'/>"
		"<path d='M40 20H60V45H40Z' fill='url(#r)'/></g></g>";
	static const char unused_bbox[] = "<mask id='full-surface-fallback'><rect width='1' height='1'/></mask>";
	static const char descendant_bbox[] =
		"<svg viewBox='0 0 100 100'><mask id='outer' maskUnits='userSpaceOnUse' "
		"x='0' y='0' width='100' height='100'><path d='M60 0H100V100H60Z' fill='white'/></mask>"
		"<mask id='inner' x='0' y='0' width='.5' height='1'>"
		"<path d='M0 0H100V100H0Z' fill='white'/></mask>"
		"<g mask='url(#outer)'><g mask='url(#inner)'><rect width='100' height='100' fill='red'/></g></g></svg>";
	static const char stroked_mask[] =
		"<svg viewBox='0 0 100 100'><mask id='s' maskUnits='userSpaceOnUse' "
		"x='0' y='0' width='100' height='100' mask-type='alpha'>"
		"<path d='M20 20H80V80H20Z' fill='none' stroke='white' stroke-width='8'/></mask>"
		"<g mask='url(#s)'><rect width='100' height='100' fill='red'/></g></svg>";
	static const char empty_invalid[] = "<svg viewBox='0 0 10 10'><mask id='m' maskUnits='userSpaceOnUse'>"
		"<path d='M20 20H30V30H20Z'/></mask><g mask='url(#m)'><path d='M nope'/></g></svg>";
	struct archetypon_image cropped = {0}, full = {0};
	char source[4096];
	int status = -1;

	snprintf(source, sizeof(source), "<svg viewBox='0 0 96 64'>%s</svg>", scene);
	if (render_test_svg(source, 96, 64, &cropped))
		goto out;
	/* An objectBoundingBox resource selects the conservative original path. */
	snprintf(source, sizeof(source), "<svg viewBox='0 0 96 64'>%s%s</svg>", unused_bbox, scene);
	if (render_test_svg(source, 96, 64, &full))
		goto out;
	if (memcmp(cropped.pixels, full.pixels, 96u * 64u * 4u) ||
	    svg_pixel(&cropped, 1, 1)[3] != 0 || svg_pixel(&cropped, 45, 30)[3] == 0) {
		fail("cropped masks changed transformed gradient/opacity pixels");
		goto out;
	}
	archetypon_image_free(&cropped);
	if (render_test_svg(descendant_bbox, 100, 100, &cropped))
		goto out;
	if (svg_pixel(&cropped, 70, 50)[3] != 0) {
		fail("ancestor crop changed descendant objectBoundingBox source bounds");
		goto out;
	}
	archetypon_image_free(&cropped);
	if (render_test_svg(stroked_mask, 100, 100, &cropped))
		goto out;
	if (svg_pixel(&cropped, 17, 50)[3] != 255 ||
	    svg_pixel(&cropped, 50, 50)[3] != 0) {
		fail("cropped mask lost its stroke extent");
		goto out;
	}
	if (!expect_svg_rejected(empty_invalid, sizeof(empty_invalid) - 1)) {
		fail("empty cropped effect skipped invalid child geometry");
		goto out;
	}
	status = 0;
out:
	archetypon_image_free(&cropped);
	archetypon_image_free(&full);
	return status;
}

static int test_repeated_cropped_masks(void)
{
	struct archetypon_image image = {0};
	size_t capacity = 256000;
	char *source = malloc(capacity);
	int counts[] = {4, 12};
	size_t test;

	if (!source)
		return fail("could not allocate repeated mask fixture");
	for (test = 0; test < sizeof(counts) / sizeof(*counts); test++) {
		int count = counts[test], cell = 720 / count;
		char *cursor = source;
		int x, y, mask;

		cursor += snprintf(cursor, capacity - (size_t)(cursor - source), "<svg viewBox='0 0 720 720'><defs>"
			"<linearGradient id='g' gradientUnits='userSpaceOnUse' x1='0' x2='720'>"
			"<stop stop-color='red'/><stop offset='1' stop-color='blue'/></linearGradient>");
		for (y = 0; y < count; y++) {
			for (x = 0; x < count; x++) {
				for (mask = 0; mask < 3; mask++) {
					int inset = 2 + mask * 2;
					cursor += snprintf(cursor, capacity - (size_t)(cursor - source), "<mask id='m%d-%d-%d' maskUnits='userSpaceOnUse' "
						"x='0' y='0' width='720' height='720' mask-type='alpha'>"
						"<path d='M%d %dH%dV%dH%dZ' transform='translate(%d %d)' fill='white'/></mask>",
						x, y, mask, inset, inset, cell - inset, cell - inset, inset, x * cell, y * cell);
				}
			}
		}
		cursor += snprintf(cursor, capacity - (size_t)(cursor - source), "</defs>");
		for (y = 0; y < count; y++) {
			for (x = 0; x < count; x++) {
				cursor += snprintf(cursor, capacity - (size_t)(cursor - source), "<g mask='url(#m%d-%d-0)'><g mask='url(#m%d-%d-1)'>"
					"<g mask='url(#m%d-%d-2)'><rect width='720' height='720' fill='url(#g)'/></g></g></g>",
					x, y, x, y, x, y);
			}
		}
		snprintf(cursor, capacity - (size_t)(cursor - source), "</svg>");
		if (render_test_svg(source, 720, 720, &image)) {
			free(source);
			return -1;
		}
		for (y = 0; y < count; y++) {
			for (x = 0; x < count; x++) {
				int px = x * cell + cell / 2;
				const uint8_t *pixel = svg_pixel(&image, px, y * cell + cell / 2);
				int blue = (int)(255.0 * (px + .5) / 720 + .5);
				if (pixel[3] != 255 || abs(pixel[2] - blue) > 1 ||
				    svg_pixel(&image, x * cell + 1, y * cell + 1)[3] != 0) {
					archetypon_image_free(&image);
					free(source);
					return fail("repeated mask ROI shifted global gradient or cell bounds");
				}
			}
		}
		archetypon_image_free(&image);
	}
	free(source);
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
	    test_svg_strokes() || test_svg_resource_limits() ||
	    test_retained_svg_api() || test_modern_rgb() ||
	    test_radial_gradients() || test_dynamic_effect_limits() ||
	    test_compact_scene_commands() || test_cropped_effect_pixels() ||
	    test_repeated_cropped_masks())
		goto out_free_image;
	status = 0;

out_free_image:
	archetypon_image_free(&image);
	return status;
}
