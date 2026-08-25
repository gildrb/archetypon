#include "internal.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdatomic.h>

#define SVG_MAX_DEPTH 128
#define SVG_MAX_ELEMENTS 100000
#define SVG_MAX_ATTRIBUTES 256
#define SVG_MAX_SOURCE_BYTES (32 * 1024 * 1024)
#define SVG_MAX_SCENE_BYTES (32 * 1024 * 1024)
#define SVG_MAX_PATH_POINTS 262144
#define SVG_MAX_TOTAL_POINTS 1000000
#define SVG_MAX_RENDER_WORK (64 * 1024 * 1024)
#define SVG_MAX_SURFACE_PIXELS (16 * 1024 * 1024)
#define SVG_MAX_OUTPUT_DIMENSION 8192
#define SVG_MAX_DASHES 64
#define SUPERSAMPLE 2
#define PI 3.14159265358979323846

struct slice {
	const char *begin;
	const char *end;
};

struct point {
	double x;
	double y;
};

struct matrix {
	double a;
	double b;
	double c;
	double d;
	double e;
	double f;
};

struct contour {
	size_t start;
	size_t count;
	bool closed;
};

struct path {
	struct point *points;
	size_t point_count;
	size_t point_capacity;
	struct contour *contours;
	size_t contour_count;
	size_t contour_capacity;
	struct matrix matrix;
	char *error;
	size_t error_capacity;
};

struct color {
	u8 r;
	u8 g;
	u8 b;
	u8 a;
	bool none;
};

struct linear_gradient;

#define SVG_MAX_GRADIENTS 1024
#define SVG_MAX_STOPS 256

struct gradient_stop {
	double offset;
	struct color color;
};

struct linear_gradient {
	struct slice id;
	struct slice href;
	double x1, y1, x2, y2;
	bool x1_percent, y1_percent, x2_percent, y2_percent;
	bool user_space;
	bool has_x1;
	bool has_y1;
	bool has_x2;
	bool has_y2;
	bool has_units;
	bool has_transform;
	u8 resolve_state;
	struct color current_color;
	struct matrix transform;
	struct gradient_stop *stops;
	size_t stop_count;
	size_t stop_capacity;
};

struct style {
	struct color fill;
	struct slice fill_url;
	struct slice clip_url;
	struct slice mask_url;
	const struct linear_gradient *gradient;
	struct point gradient_start;
	struct point gradient_end;
	struct matrix gradient_inverse;
	struct color stroke;
	struct color current_color;
	double opacity;
	double fill_opacity;
	double stroke_opacity;
	double stroke_width;
	double miter_limit;
	double dash_offset;
	double dashes[SVG_MAX_DASHES];
	size_t dash_count;
	bool fill_rule_evenodd;
	bool clip_rule_evenodd;
	bool fill_current_color;
	bool stroke_current_color;
	s32 line_cap;
	s32 line_join;
	bool hidden;
	bool display_none;
};

struct context {
	struct matrix matrix;
	struct style style;
	struct slice name;
	double own_opacity;
	bool render;
	bool isolate;
};

struct tag {
	struct slice name;
	const char *attributes;
	const char *end;
	bool closing;
	bool self_closing;
};

struct intersection {
	double x;
	s32 winding;
};

static struct slice slice_trim(struct slice value)
{
	while (value.begin < value.end &&
	       isspace((unsigned char)*value.begin))
		value.begin++;
	while (value.end > value.begin &&
	       isspace((unsigned char)value.end[-1]))
		value.end--;
	return value;
}

static bool slice_equal(struct slice value, const char *text)
{
	size_t length = strlen(text);

	return (size_t)(value.end - value.begin) == length &&
	       memcmp(value.begin, text, length) == 0;
}

static bool slice_same(struct slice left, struct slice right)
{
	size_t length = (size_t)(left.end - left.begin);

	return (size_t)(right.end - right.begin) == length &&
	       memcmp(left.begin, right.begin, length) == 0;
}

static bool slice_equal_ci(struct slice value, const char *text)
{
	size_t index;
	size_t length = strlen(text);

	if ((size_t)(value.end - value.begin) != length)
		return false;
	for (index = 0; index < length; index++) {
		if (tolower((unsigned char)value.begin[index]) !=
		    tolower((unsigned char)text[index]))
			return false;
	}
	return true;
}

static struct slice local_name(struct slice name)
{
	const char *cursor;

	for (cursor = name.begin; cursor < name.end; cursor++) {
		if (*cursor == ':')
			name.begin = cursor + 1;
	}
	return name;
}

static const char *attribute_name_end(const char *cursor, const char *end)
{
	while (cursor < end && !isspace((unsigned char)*cursor) &&
	       *cursor != '=' && *cursor != '/')
		cursor++;
	return cursor;
}

static const char *attribute_token_end(const char *cursor, const char *end)
{
	while (cursor < end && !isspace((unsigned char)*cursor))
		cursor++;
	return cursor;
}

static bool attribute_value(const char **position, const char *end,
			    struct slice *value)
{
	const char *cursor = *position;
	char quote;

	while (cursor < end && isspace((unsigned char)*cursor))
		cursor++;
	if (cursor >= end || *cursor != '=')
		return false;
	cursor++;
	while (cursor < end && isspace((unsigned char)*cursor))
		cursor++;
	if (cursor >= end)
		return false;
	quote = *cursor;
	if (quote == '\'' || quote == '"')
		cursor++;
	else
		quote = 0;
	value->begin = cursor;
	while (cursor < end &&
	       ((quote && *cursor != quote) ||
		(!quote && !isspace((unsigned char)*cursor) && *cursor != '/')))
		cursor++;
	value->end = cursor;
	if (quote && cursor < end)
		cursor++;
	*position = cursor;
	return true;
}

static bool attribute_find(const struct tag *tag, const char *wanted,
			   struct slice *value)
{
	const char *cursor = tag->attributes;
	size_t wanted_length = strlen(wanted);

	while (cursor < tag->end) {
		const char *name_begin;
		const char *name_end;

		while (cursor < tag->end &&
		       (isspace((unsigned char)*cursor) || *cursor == '/'))
			cursor++;
		if (cursor >= tag->end)
			break;
		name_begin = cursor;
		name_end = attribute_name_end(cursor, tag->end);
		cursor = name_end;
		if (!attribute_value(&cursor, tag->end, value)) {
			cursor = attribute_token_end(cursor, tag->end);
			continue;
		}
		if ((size_t)(name_end - name_begin) == wanted_length &&
		    memcmp(name_begin, wanted, wanted_length) == 0)
			return true;
	}
	return false;
}

static const char *tag_close(const char *cursor, const char *end)
{
	char quote = 0;

	while (cursor < end) {
		if (quote && *cursor == quote)
			quote = 0;
		else if (!quote && (*cursor == '\'' || *cursor == '"'))
			quote = *cursor;
		else if (!quote && *cursor == '>')
			break;
		cursor++;
	}
	return cursor;
}

static int skip_special_tag(const char **position, const char *end, char *error,
			    size_t capacity)
{
	const char *cursor = *position;
	const char *close;

	if ((size_t)(end - cursor) >= 4 && memcmp(cursor, "<!--", 4) == 0) {
		close = cursor + 4;
		while ((size_t)(end - close) >= 3 &&
		       memcmp(close, "-->", 3) != 0)
			close++;
		if ((size_t)(end - close) < 3)
			goto out_unterminated;
		*position = close + 3;
		return 1;
	}
	if ((size_t)(end - cursor) < 2 ||
	    (cursor[1] != '?' && cursor[1] != '!'))
		return 0;
	close = cursor + 2;
	while (close < end && *close != '>')
		close++;
	if (close == end)
		goto out_unterminated;
	*position = close + 1;
	return 1;

out_unterminated:
	archetypon_set_error(error, capacity, "unterminated SVG declaration");
	return -1;
}

static bool xml_name_start(char value)
{
	return (unsigned char)value >= 0x80 || isalpha((unsigned char)value) ||
	       value == '_' || value == ':';
}

static bool xml_name_char(char value)
{
	return xml_name_start(value) || isdigit((unsigned char)value) ||
	       value == '-' || value == '.';
}

static bool validate_attributes(const char *cursor, const char *end,
				char *error, size_t capacity)
{
	struct slice names[SVG_MAX_ATTRIBUTES];
	size_t count = 0;

	while (cursor < end) {
		const char *name_begin;
		char quote;
		size_t index;

		while (cursor < end && isspace((unsigned char)*cursor))
			cursor++;
		if (cursor == end)
			return true;
		if (!xml_name_start(*cursor))
			goto out_invalid;
		name_begin = cursor++;
		while (cursor < end && xml_name_char(*cursor))
			cursor++;
		if (count == SVG_MAX_ATTRIBUTES) {
			archetypon_set_error(error, capacity,
					     "SVG element has too many attributes");
			return false;
		}
		names[count] = (struct slice){ name_begin, cursor };
		for (index = 0; index < count; index++) {
			if (slice_same(names[index], names[count])) {
				archetypon_set_error(error, capacity,
						     "SVG element has duplicate attributes");
				return false;
			}
		}
		count++;
		while (cursor < end && isspace((unsigned char)*cursor))
			cursor++;
		if (cursor == end || *cursor++ != '=')
			goto out_invalid;
		while (cursor < end && isspace((unsigned char)*cursor))
			cursor++;
		if (cursor == end || (*cursor != '\'' && *cursor != '"'))
			goto out_invalid;
		quote = *cursor++;
		while (cursor < end && *cursor != quote) {
			if (*cursor == '<')
				goto out_invalid;
			cursor++;
		}
		if (cursor == end)
			goto out_invalid;
		cursor++;
		if (cursor < end && !isspace((unsigned char)*cursor))
			goto out_invalid;
	}
	return true;

out_invalid:
	archetypon_set_error(error, capacity, "invalid SVG attributes");
	return false;
}

static bool parse_tag(const char *cursor, const char *end, struct tag *tag,
		      const char **next, char *error, size_t capacity)
{
	const char *name_begin;
	const char *close;
	const char *content_end;

	memset(tag, 0, sizeof(*tag));
	if (cursor < end && *cursor == '/') {
		tag->closing = true;
		cursor++;
	}
	if (cursor >= end || !xml_name_start(*cursor))
		goto out_invalid;
	name_begin = cursor++;
	while (cursor < end && xml_name_char(*cursor))
		cursor++;
	tag->name = (struct slice){ name_begin, cursor };
	tag->attributes = cursor;
	close = tag_close(cursor, end);
	if (close >= end)
		goto out_invalid;
	tag->end = close;
	content_end = close;
	while (content_end > tag->attributes &&
	       isspace((unsigned char)content_end[-1]))
		content_end--;
	if (!tag->closing && content_end > tag->attributes &&
	    content_end[-1] == '/') {
		tag->self_closing = true;
		content_end--;
	}
	if (tag->closing) {
		while (cursor < content_end && isspace((unsigned char)*cursor))
			cursor++;
		if (cursor != content_end)
			goto out_invalid;
	} else if (!validate_attributes(cursor, content_end, error, capacity)) {
		return false;
	}
	*next = tag->end + 1;
	return true;

out_invalid:
	archetypon_set_error(error, capacity,
			     "invalid or unterminated SVG tag");
	return false;
}

static bool next_tag(const char **position, const char *end, struct tag *tag,
		     char *error, size_t error_capacity)
{
	const char *cursor = *position;

	while (cursor < end) {
		int special;

		while (cursor < end && *cursor != '<')
			cursor++;
		if (cursor >= end) {
			*position = end;
			return false;
		}
		special = skip_special_tag(&cursor, end, error, error_capacity);
		if (special < 0)
			return false;
		if (special > 0)
			continue;
		return parse_tag(cursor + 1, end, tag, position, error,
				 error_capacity);
	}
	return false;
}

static bool parse_length(struct slice value, double *result)
{
	char *after;

	value = slice_trim(value);
	if (value.begin == value.end)
		return false;
	errno = 0;
	*result = strtod(value.begin, &after);
	if (after == value.begin || after > value.end || errno == ERANGE ||
	    !isfinite(*result))
		return false;
	while (after < value.end && isspace((unsigned char)*after))
		after++;
	if ((size_t)(value.end - after) == 2 && memcmp(after, "px", 2) == 0)
		after += 2;
	return after == value.end;
}

static bool parse_percentage(struct slice value, double *result)
{
	char *after;

	value = slice_trim(value);
	if (value.begin == value.end)
		return false;
	errno = 0;
	*result = strtod(value.begin, &after);
	return after == value.end - 1 && *after == '%' && errno != ERANGE &&
	       isfinite(*result);
}

static bool parse_number_list(struct slice value, double *numbers, size_t count)
{
	size_t index;
	const char *cursor = value.begin;

	for (index = 0; index < count; index++) {
		char *after;

		while (cursor < value.end &&
		       (isspace((unsigned char)*cursor) || *cursor == ','))
			cursor++;
		if (cursor >= value.end)
			return false;
		errno = 0;
		numbers[index] = strtod(cursor, &after);
		if (after == cursor || after > value.end || errno == ERANGE ||
		    !isfinite(numbers[index]))
			return false;
		cursor = after;
	}
	while (cursor < value.end &&
	       (isspace((unsigned char)*cursor) || *cursor == ','))
		cursor++;
	return cursor == value.end;
}

static struct matrix matrix_identity(void)
{
	struct matrix matrix = { 1, 0, 0, 1, 0, 0 };
	return matrix;
}

static struct matrix matrix_multiply(struct matrix left, struct matrix right)
{
	struct matrix result;

	result.a = left.a * right.a + left.c * right.b;
	result.b = left.b * right.a + left.d * right.b;
	result.c = left.a * right.c + left.c * right.d;
	result.d = left.b * right.c + left.d * right.d;
	result.e = left.a * right.e + left.c * right.f + left.e;
	result.f = left.b * right.e + left.d * right.f + left.f;
	return result;
}

static struct point matrix_point(struct matrix matrix, double x, double y)
{
	struct point point;

	point.x = matrix.a * x + matrix.c * y + matrix.e;
	point.y = matrix.b * x + matrix.d * y + matrix.f;
	return point;
}

static bool matrix_inverse(struct matrix value, struct matrix *inverse)
{
	double determinant = value.a * value.d - value.b * value.c;

	if (!isfinite(determinant) || fabs(determinant) < 1e-20)
		return false;
	inverse->a = value.d / determinant;
	inverse->b = -value.b / determinant;
	inverse->c = -value.c / determinant;
	inverse->d = value.a / determinant;
	inverse->e = (value.c * value.f - value.d * value.e) / determinant;
	inverse->f = (value.b * value.e - value.a * value.f) / determinant;
	return isfinite(inverse->a) && isfinite(inverse->b) &&
	       isfinite(inverse->c) && isfinite(inverse->d) &&
	       isfinite(inverse->e) && isfinite(inverse->f);
}

static double matrix_scale(struct matrix matrix)
{
	return sqrt((matrix.a * matrix.a + matrix.b * matrix.b +
		     matrix.c * matrix.c + matrix.d * matrix.d) /
		    2.0);
}

static bool transform_numbers(struct slice arguments, double values[6],
			      size_t *count, char *error, size_t capacity)
{
	const char *cursor = arguments.begin;

	*count = 0;
	while (cursor < arguments.end) {
		char *after;

		while (cursor < arguments.end &&
		       (isspace((unsigned char)*cursor) || *cursor == ','))
			cursor++;
		if (cursor == arguments.end)
			break;
		if (*count == 6)
			goto out_invalid;
		values[*count] = strtod(cursor, &after);
		if (after == cursor || after > arguments.end ||
		    !isfinite(values[*count]))
			goto out_invalid;
		(*count)++;
		cursor = after;
	}
	return true;

out_invalid:
	archetypon_set_error(error, capacity,
			     "invalid SVG transform arguments");
	return false;
}

static bool transform_rotate(struct matrix *transform, const double *values,
			     size_t count)
{
	struct matrix before = matrix_identity();
	struct matrix after = matrix_identity();
	double angle;

	if (count != 1 && count != 3)
		return false;
	angle = values[0] * PI / 180.0;
	transform->a = cos(angle);
	transform->b = sin(angle);
	transform->c = -sin(angle);
	transform->d = cos(angle);
	if (count == 1)
		return true;
	before.e = values[1];
	before.f = values[2];
	after.e = -values[1];
	after.f = -values[2];
	*transform =
		matrix_multiply(before, matrix_multiply(*transform, after));
	return true;
}

static bool transform_values(struct matrix *transform, const double *values,
			     size_t count)
{
	if (count != 6)
		return false;
	*transform = (struct matrix){ values[0], values[1], values[2],
				      values[3], values[4], values[5] };
	return true;
}

static bool transform_translate(struct matrix *transform, const double *values,
				size_t count)
{
	if (count < 1 || count > 2)
		return false;
	transform->e = values[0];
	transform->f = count == 2 ? values[1] : 0;
	return true;
}

static bool transform_scale(struct matrix *transform, const double *values,
			    size_t count)
{
	if (count < 1 || count > 2)
		return false;
	transform->a = values[0];
	transform->d = count == 2 ? values[1] : values[0];
	return true;
}

static bool transform_skew(double *component, const double *values,
			   size_t count)
{
	if (count != 1)
		return false;
	*component = tan(values[0] * PI / 180.0);
	return true;
}

static bool transform_matrix(struct slice name, const double *values,
			     size_t count, struct matrix *transform)
{
	if (slice_equal(name, "matrix"))
		return transform_values(transform, values, count);
	if (slice_equal(name, "translate"))
		return transform_translate(transform, values, count);
	if (slice_equal(name, "scale"))
		return transform_scale(transform, values, count);
	if (slice_equal(name, "rotate"))
		return transform_rotate(transform, values, count);
	if (slice_equal(name, "skewX"))
		return transform_skew(&transform->c, values, count);
	if (slice_equal(name, "skewY"))
		return transform_skew(&transform->b, values, count);
	return false;
}

static bool transform_item(const char **position, const char *end,
			   struct matrix *transform, char *error,
			   size_t capacity)
{
	const char *cursor = *position;
	struct slice name;
	struct slice arguments;
	double values[6];
	size_t count;

	while (cursor < end &&
	       (isspace((unsigned char)*cursor) || *cursor == ','))
		cursor++;
	if (cursor == end) {
		*position = cursor;
		return true;
	}
	name.begin = cursor;
	while (cursor < end && isalpha((unsigned char)*cursor))
		cursor++;
	name.end = cursor;
	while (cursor < end && isspace((unsigned char)*cursor))
		cursor++;
	if (cursor >= end || *cursor++ != '(')
		goto out_invalid;
	arguments.begin = cursor;
	while (cursor < end && *cursor != ')')
		cursor++;
	if (cursor >= end)
		goto out_invalid;
	arguments.end = cursor++;
	if (!transform_numbers(arguments, values, &count, error, capacity))
		return false;
	*transform = matrix_identity();
	if (!transform_matrix(name, values, count, transform))
		goto out_invalid;
	*position = cursor;
	return true;

out_invalid:
	archetypon_set_error(error, capacity, "unsupported SVG transform");
	return false;
}

static bool parse_transform(struct slice value, struct matrix *result,
			    char *error, size_t error_capacity)
{
	const char *cursor = value.begin;
	struct matrix matrix = matrix_identity();

	while (cursor < value.end) {
		struct matrix transform = matrix_identity();

		while (cursor < value.end &&
		       (isspace((unsigned char)*cursor) || *cursor == ','))
			cursor++;
		if (cursor == value.end)
			break;
		if (!transform_item(&cursor, value.end, &transform, error,
				    error_capacity))
			return false;
		matrix = matrix_multiply(matrix, transform);
	}
	*result = matrix;
	return true;
}

static struct color color_rgba(u8 red, u8 green, u8 blue, u8 alpha)
{
	struct color color = { red, green, blue, alpha, 0 };
	return color;
}

static s32 hex_value(char character)
{
	if (character >= '0' && character <= '9')
		return character - '0';
	character = (char)tolower((unsigned char)character);
	if (character >= 'a' && character <= 'f')
		return character - 'a' + 10;
	return -1;
}

struct named_color {
	const char *name;
	u8 red;
	u8 green;
	u8 blue;
};

static const struct named_color named_colors[] = {
	{ "aqua", 0, 255, 255 },   { "black", 0, 0, 0 },
	{ "blue", 0, 0, 255 },	   { "fuchsia", 255, 0, 255 },
	{ "gray", 128, 128, 128 }, { "green", 0, 128, 0 },
	{ "lime", 0, 255, 0 },	   { "maroon", 128, 0, 0 },
	{ "navy", 0, 0, 128 },	   { "olive", 128, 128, 0 },
	{ "orange", 255, 165, 0 }, { "purple", 128, 0, 128 },
	{ "red", 255, 0, 0 },	   { "silver", 192, 192, 192 },
	{ "teal", 0, 128, 128 },   { "white", 255, 255, 255 },
	{ "yellow", 255, 255, 0 },
};

static bool parse_hex_color(const char *text, size_t length,
			    struct color *color)
{
	s32 digits[8];
	size_t i;

	if (length != 4 && length != 5 && length != 7 && length != 9)
		return false;
	for (i = 1; i < length; i++) {
		digits[i - 1] = hex_value(text[i]);
		if (digits[i - 1] < 0)
			return false;
	}
	if (length == 4 || length == 5) {
		*color = color_rgba((u8)(digits[0] * 17), (u8)(digits[1] * 17),
				    (u8)(digits[2] * 17),
				    length == 5 ? (u8)(digits[3] * 17) : 255);
	} else {
		*color = color_rgba((u8)(digits[0] * 16 + digits[1]),
				    (u8)(digits[2] * 16 + digits[3]),
				    (u8)(digits[4] * 16 + digits[5]),
				    length == 9 ?
					    (u8)(digits[6] * 16 + digits[7]) :
					    255);
	}
	return true;
}

static bool parse_rgb_color(const char *text, bool has_alpha,
			    struct color *color)
{
	double red;
	double green;
	double blue;
	double alpha = 1;
	s32 parsed;

	if (has_alpha)
		parsed = sscanf(text, "rgba(%lf,%lf,%lf,%lf)", &red, &green,
				&blue, &alpha);
	else
		parsed = sscanf(text, "rgb(%lf,%lf,%lf)", &red, &green, &blue);
	if (parsed != (has_alpha ? 4 : 3))
		return false;
	if (!isfinite(red) || !isfinite(green) || !isfinite(blue) ||
	    !isfinite(alpha) || red < 0 || red > 255 || green < 0 ||
	    green > 255 || blue < 0 || blue > 255 || alpha < 0 || alpha > 1)
		return false;
	*color = color_rgba((u8)lround(red), (u8)lround(green),
			    (u8)lround(blue), (u8)lround(alpha * 255));
	return true;
}

static bool parse_named_color(const char *text, struct color *color)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(named_colors); i++) {
		if (strcmp(text, named_colors[i].name) != 0)
			continue;
		*color = color_rgba(named_colors[i].red, named_colors[i].green,
				    named_colors[i].blue, 255);
		return true;
	}
	return false;
}

static bool parse_color(struct slice value, struct color current_color,
			struct color *result)
{
	char text[128];
	size_t length;
	size_t index;

	value = slice_trim(value);
	length = (size_t)(value.end - value.begin);
	if (length == 0 || length >= sizeof(text))
		return false;
	for (index = 0; index < length; index++)
		text[index] = (char)tolower((unsigned char)value.begin[index]);
	text[length] = 0;

	if (strcmp(text, "none") == 0) {
		*result = color_rgba(0, 0, 0, 0);
		result->none = true;
		return true;
	}
	if (strcmp(text, "transparent") == 0) {
		*result = color_rgba(0, 0, 0, 0);
		return true;
	}
	if (strcmp(text, "currentcolor") == 0) {
		*result = current_color;
		return true;
	}
	if (text[0] == '#')
		return parse_hex_color(text, length, result);
	if (strncmp(text, "rgba(", 5) == 0)
		return parse_rgb_color(text, true, result);
	if (strncmp(text, "rgb(", 4) == 0)
		return parse_rgb_color(text, false, result);
	return parse_named_color(text, result);
}

static bool parse_opacity(struct slice value, double *result)
{
	char *after;
	double number;

	value = slice_trim(value);
	if (value.begin == value.end)
		return false;
	number = strtod(value.begin, &after);
	if (after == value.begin || after > value.end || !isfinite(number))
		return false;
	if (after < value.end && *after == '%') {
		number /= 100.0;
		after++;
	}
	while (after < value.end && isspace((unsigned char)*after))
		after++;
	if (after != value.end)
		return false;
	if (number < 0)
		number = 0;
	if (number > 1)
		number = 1;
	*result = number;
	return true;
}

static bool style_opacity(struct slice value, double *opacity,
			  const char *name, char *error, size_t error_capacity)
{
	if (parse_opacity(value, opacity))
		return true;
	archetypon_set_error(error, error_capacity, "invalid SVG %s", name);
	return false;
}

static struct style style_default(void)
{
	struct style style;

	memset(&style, 0, sizeof(style));
	style.fill = color_rgba(0, 0, 0, 255);
	style.stroke = color_rgba(0, 0, 0, 0);
	style.stroke.none = true;
	style.current_color = color_rgba(0, 0, 0, 255);
	style.opacity = 1;
	style.fill_opacity = 1;
	style.stroke_opacity = 1;
	style.stroke_width = 1;
	style.miter_limit = 4;
	style.line_join = 0;
	return style;
}

static bool style_paint(struct style *style, struct slice name,
			struct slice value, char *error, size_t error_capacity)
{
	static const char unsupported_paint[] =
		"unsupported SVG paint (paint servers are not supported)";
	struct slice trimmed = slice_trim(value);
	struct color *color;
	bool *current = NULL;

	if (slice_equal_ci(name, "color")) {
		color = &style->current_color;
	} else if (slice_equal_ci(name, "fill")) {
		if ((size_t)(trimmed.end - trimmed.begin) >= 7 &&
		    memcmp(trimmed.begin, "url(#", 5) == 0 &&
		    trimmed.end[-1] == ')') {
			style->fill_url = (struct slice){trimmed.begin + 5,
							 trimmed.end - 1};
			style->fill.none = false;
			style->fill_current_color = false;
			return style->fill_url.begin < style->fill_url.end;
		}
		style->fill_url = (struct slice){0};
		color = &style->fill;
		current = &style->fill_current_color;
	} else if (slice_equal_ci(name, "stroke")) {
		color = &style->stroke;
		current = &style->stroke_current_color;
	} else {
		return false;
	}
	if (current && slice_equal_ci(trimmed, "currentcolor")) {
		*current = true;
		color->none = false;
		return true;
	}
	if (parse_color(value, style->current_color, color)) {
		if (current)
			*current = false;
		return true;
	}
	archetypon_set_error(error, error_capacity, unsupported_paint);
	return false;
}

static bool style_line_cap(struct style *style, struct slice value)
{
	value = slice_trim(value);
	if (slice_equal_ci(value, "butt"))
		style->line_cap = 0;
	else if (slice_equal_ci(value, "round"))
		style->line_cap = 1;
	else if (slice_equal_ci(value, "square"))
		style->line_cap = 2;
	else
		return false;
	return true;
}

static bool style_dasharray(struct style *style, struct slice value)
{
	const char *cursor;
	double total = 0;

	value = slice_trim(value);
	if (slice_equal_ci(value, "none")) {
		style->dash_count = 0;
		return true;
	}
	style->dash_count = 0;
	cursor = value.begin;
	while (cursor < value.end) {
		char *after;
		double dash;

		if (style->dash_count == SVG_MAX_DASHES)
			return false;
		errno = 0;
		dash = strtod(cursor, &after);
		if (after == cursor || after > value.end || errno == ERANGE ||
		    !isfinite(dash) || dash < 0)
			return false;
		style->dashes[style->dash_count++] = dash;
		total += dash;
		cursor = after;
		while (cursor < value.end && isspace((unsigned char)*cursor))
			cursor++;
		if (cursor == value.end)
			break;
		if (*cursor == ',') {
			cursor++;
			while (cursor < value.end && isspace((unsigned char)*cursor))
				cursor++;
			if (cursor == value.end)
				return false;
		} else if (after == cursor) {
			return false;
		}
	}
	if (style->dash_count == 0 || !isfinite(total))
		return false;
	if (total == 0)
		style->dash_count = 0;
	return true;
}

static bool style_stroke(struct style *style, struct slice name,
			 struct slice value, char *error, size_t error_capacity)
{
	double number;
	struct slice trimmed = slice_trim(value);

	if (slice_equal_ci(name, "stroke-width")) {
		if (!parse_length(value, &number) || number < 0)
			goto out_invalid;
		style->stroke_width = number;
		return true;
	}
	if (slice_equal_ci(name, "stroke-linecap")) {
		if (!style_line_cap(style, value))
			goto out_invalid;
		return true;
	}
	if (slice_equal_ci(name, "stroke-linejoin")) {
		if (slice_equal_ci(trimmed, "miter"))
			style->line_join = 0;
		else if (slice_equal_ci(trimmed, "round"))
			style->line_join = 1;
		else if (slice_equal_ci(trimmed, "bevel"))
			style->line_join = 2;
		else
			goto out_invalid;
		return true;
	}
	if (slice_equal_ci(name, "stroke-miterlimit")) {
		if (!parse_length(value, &number) || number < 1)
			goto out_invalid;
		style->miter_limit = number;
		return true;
	}
	if (slice_equal_ci(name, "stroke-dashoffset")) {
		if (!parse_length(value, &number))
			goto out_invalid;
		style->dash_offset = number;
		return true;
	}
	if (slice_equal_ci(name, "stroke-dasharray")) {
		if (!style_dasharray(style, value))
			goto out_invalid;
		return true;
	}
	return false;

out_invalid:
	archetypon_set_error(error, error_capacity, "unsupported SVG stroke");
	return false;
}

static bool style_visibility(struct style *style, struct slice name,
			     struct slice value)
{
	struct slice trimmed = slice_trim(value);

	if (slice_equal_ci(name, "display")) {
		if (slice_equal_ci(trimmed, "none"))
			style->display_none = true;
		return true;
	}
	if (!slice_equal_ci(name, "visibility"))
		return false;
	if (slice_equal_ci(trimmed, "hidden") ||
	    slice_equal_ci(trimmed, "collapse"))
		style->hidden = true;
	else if (slice_equal_ci(trimmed, "visible"))
		style->hidden = false;
	return true;
}

static bool style_property(struct style *style, struct slice name,
			   struct slice value, double *own_opacity, char *error,
			   size_t error_capacity)
{
	if (slice_equal_ci(name, "color") || slice_equal_ci(name, "fill") ||
	    slice_equal_ci(name, "stroke"))
		return style_paint(style, name, value, error, error_capacity);
	if (slice_equal_ci(name, "opacity"))
		return style_opacity(value, own_opacity, "opacity", error,
				     error_capacity);
	if (slice_equal_ci(name, "fill-opacity"))
		return style_opacity(value, &style->fill_opacity,
				     "fill-opacity", error, error_capacity);
	if (slice_equal_ci(name, "stroke-opacity"))
		return style_opacity(value, &style->stroke_opacity,
				     "stroke-opacity", error, error_capacity);
	if (slice_equal_ci(name, "fill-rule") ||
	    slice_equal_ci(name, "clip-rule")) {
		bool evenodd;

		if (slice_equal_ci(slice_trim(value), "evenodd"))
			evenodd = true;
		else if (slice_equal_ci(slice_trim(value), "nonzero"))
			evenodd = false;
		else
			goto out_unsupported;
		if (slice_equal_ci(name, "fill-rule"))
			style->fill_rule_evenodd = evenodd;
		else
			style->clip_rule_evenodd = evenodd;
		return true;
	}
	if (slice_equal_ci(name, "stroke-width") ||
	    slice_equal_ci(name, "stroke-linecap") ||
	    slice_equal_ci(name, "stroke-linejoin") ||
	    slice_equal_ci(name, "stroke-miterlimit") ||
	    slice_equal_ci(name, "stroke-dasharray") ||
	    slice_equal_ci(name, "stroke-dashoffset"))
		return style_stroke(style, name, value, error, error_capacity);
	if (slice_equal_ci(name, "display") ||
	    slice_equal_ci(name, "visibility"))
		return style_visibility(style, name, value);
	if (slice_equal_ci(name, "clip-path") || slice_equal_ci(name, "mask")) {
		struct slice trimmed = slice_trim(value),
			     *target = slice_equal_ci(name, "mask")
					       ? &style->mask_url
					       : &style->clip_url;
		if (slice_equal_ci(trimmed, "none")) {
			*target = (struct slice){0};
			return true;
		}
		if ((size_t)(trimmed.end - trimmed.begin) >= 7 &&
		    memcmp(trimmed.begin, "url(#", 5) == 0 &&
		    trimmed.end[-1] == ')') {
			*target = (struct slice){trimmed.begin + 5,
						 trimmed.end - 1};
			return target->begin < target->end;
		}
		goto out_unsupported;
	}
	if (slice_equal_ci(name, "filter") &&
	    !slice_equal_ci(slice_trim(value), "none"))
		goto out_unsupported;
	return true;

out_unsupported:
	archetypon_set_error(error, error_capacity, "unsupported SVG style");
	return false;
}

static const char * const style_properties[] = {
	"color",
	"fill",
	"stroke",
	"opacity",
	"fill-opacity",
	"stroke-opacity",
	"stroke-width",
	"fill-rule",
	"clip-rule",
	"stroke-linecap",
	"stroke-linejoin",
	"stroke-miterlimit",
	"stroke-dasharray",
	"stroke-dashoffset",
	"display",
	"visibility",
	"clip-path",
	"mask",
	"filter"
};

static bool apply_presentation_attributes(const struct tag *tag,
					  struct style *style,
					  double *own_opacity, char *error,
					  size_t error_capacity)
{
	struct slice value;
	size_t i;

	for (i = 0; i < ARRAY_SIZE(style_properties); i++) {
		struct slice name;

		if (!attribute_find(tag, style_properties[i], &value))
			continue;
		name.begin = style_properties[i];
		name.end = name.begin + strlen(style_properties[i]);
		if (!style_property(style, name, value, own_opacity, error,
				    error_capacity))
			return false;
	}
	return true;
}

static const char *style_declaration_end(const char *cursor, const char *end)
{
	while (cursor < end && *cursor != ';')
		cursor++;
	return cursor;
}

static bool apply_style_attribute_raw(struct slice value, struct style *style,
				      double *own_opacity, char *error,
				      size_t error_capacity)
{
	const char *cursor = value.begin;

	while (cursor < value.end) {
		struct slice name;
		struct slice property_value;

		while (cursor < value.end &&
		       (isspace((unsigned char)*cursor) || *cursor == ';'))
			cursor++;
		name.begin = cursor;
		while (cursor < value.end && *cursor != ':' && *cursor != ';')
			cursor++;
		name.end = cursor;
		if (cursor >= value.end || *cursor != ':') {
			cursor = style_declaration_end(cursor, value.end);
			continue;
		}
		cursor++;
		property_value.begin = cursor;
		cursor = style_declaration_end(cursor, value.end);
		property_value.end = cursor;
		if (!style_property(style, slice_trim(name),
				    slice_trim(property_value), own_opacity,
				    error, error_capacity))
			return false;
	}
	return true;
}

static bool css_without_comments(struct slice value, char **storage,
				 struct slice *clean, char *error,
				 size_t error_capacity)
{
	size_t length = (size_t)(value.end - value.begin);
	const char *cursor = value.begin;
	char *output;
	char *destination;

	output = malloc(length + 1);
	if (!output) {
		archetypon_set_error(error, error_capacity,
				     "out of memory parsing SVG CSS");
		return false;
	}
	destination = output;
	while (cursor < value.end) {
		if ((size_t)(value.end - cursor) >= 2 && cursor[0] == '/' &&
		    cursor[1] == '*') {
			const char *comment = cursor;

			cursor += 2;
			while ((size_t)(value.end - cursor) >= 2 &&
			       !(cursor[0] == '*' && cursor[1] == '/'))
				cursor++;
			if ((size_t)(value.end - cursor) < 2) {
				free(output);
				archetypon_set_error(
					error, error_capacity,
					"unterminated SVG CSS comment");
				return false;
			}
			cursor += 2;
			while (comment++ < cursor)
				*destination++ = ' ';
		} else {
			*destination++ = *cursor++;
		}
	}
	*destination = 0;
	*storage = output;
	*clean = (struct slice){output, destination};
	return true;
}

static bool apply_style_attribute(struct slice value, struct style *style,
				  double *own_opacity, char *error,
				  size_t error_capacity)
{
	char *storage;
	struct slice clean;
	bool result;

	if (!css_without_comments(value, &storage, &clean, error,
				  error_capacity))
		return false;
	result = apply_style_attribute_raw(clean, style, own_opacity, error,
					   error_capacity);
	if (style->fill_url.begin >= clean.begin &&
	    style->fill_url.end <= clean.end)
		style->fill_url = (struct slice){
			value.begin + (style->fill_url.begin - clean.begin),
			value.begin + (style->fill_url.end - clean.begin)};
	if (style->clip_url.begin >= clean.begin &&
	    style->clip_url.end <= clean.end)
		style->clip_url = (struct slice){
			value.begin + (style->clip_url.begin - clean.begin),
			value.begin + (style->clip_url.end - clean.begin)};
	if (style->mask_url.begin >= clean.begin &&
	    style->mask_url.end <= clean.end)
		style->mask_url = (struct slice){
			value.begin + (style->mask_url.begin - clean.begin),
			value.begin + (style->mask_url.end - clean.begin)};
	free(storage);
	return result;
}

static void resolve_current_color(struct style *style)
{
	if (style->fill_current_color)
		style->fill = style->current_color;
	if (style->stroke_current_color)
		style->stroke = style->current_color;
}

static bool path_reserve_points(struct path *path, size_t extra)
{
	size_t needed;
	size_t capacity;
	struct point *points;

	if (extra > SVG_MAX_PATH_POINTS - path->point_count) {
		archetypon_set_error(path->error, path->error_capacity,
				     "SVG path exceeds the point limit");
		return false;
	}
	needed = path->point_count + extra;
	if (needed <= path->point_capacity)
		return true;
	capacity = path->point_capacity == 0 ? 128 : path->point_capacity;
	while (capacity < needed)
		capacity *= 2;
	points = realloc(path->points, capacity * sizeof(*points));
	if (!points) {
		archetypon_set_error(path->error, path->error_capacity,
				     "out of memory parsing SVG path");
		return false;
	}
	path->points = points;
	path->point_capacity = capacity;
	return true;
}

static bool path_device_point(struct path *path, double x, double y,
			      struct point *point)
{
	*point = matrix_point(path->matrix, x, y);
	if (!isfinite(point->x) || !isfinite(point->y)) {
		archetypon_set_error(path->error, path->error_capacity,
				     "non-finite SVG path point");
		return false;
	}
	return true;
}

static bool path_reserve_contours(struct path *path)
{
	struct contour *contours;
	size_t capacity;

	if (path->contour_count < path->contour_capacity)
		return true;
	capacity = path->contour_capacity == 0 ?
		   8 :
		   path->contour_capacity * 2;
	contours = realloc(path->contours, capacity * sizeof(*contours));
	if (!contours) {
		archetypon_set_error(path->error, path->error_capacity,
				     "out of memory parsing SVG contours");
		return false;
	}
	path->contours = contours;
	path->contour_capacity = capacity;
	return true;
}

static bool path_begin_contour(struct path *path, double x, double y)
{
	struct point point;

	if (!path_device_point(path, x, y, &point) ||
	    !path_reserve_points(path, 1) || !path_reserve_contours(path))
		return false;
	path->contours[path->contour_count] =
		(struct contour){ path->point_count, 1, 0 };
	path->contour_count++;
	path->points[path->point_count++] = point;
	return true;
}

static bool path_line_to(struct path *path, double x, double y)
{
	struct point point;
	struct contour *contour;

	if (path->contour_count == 0) {
		archetypon_set_error(path->error, path->error_capacity,
				     "SVG path draws before its first move");
		return false;
	}
	if (!path_device_point(path, x, y, &point))
		return false;
	contour = &path->contours[path->contour_count - 1];
	if (contour->count > 0) {
		struct point previous = path->points[path->point_count - 1];

		if (fabs(previous.x - point.x) < 1e-9 &&
		    fabs(previous.y - point.y) < 1e-9)
			return true;
	}
	if (!path_reserve_points(path, 1))
		return false;
	path->points[path->point_count++] = point;
	contour->count++;
	return true;
}

static double point_line_distance_squared(struct point point,
					  struct point start, struct point end)
{
	double dx = end.x - start.x;
	double dy = end.y - start.y;
	double cross;
	double length_squared = dx * dx + dy * dy;

	if (length_squared < 1e-20) {
		dx = point.x - start.x;
		dy = point.y - start.y;
		return dx * dx + dy * dy;
	}
	cross = (point.x - start.x) * dy - (point.y - start.y) * dx;
	return cross * cross / length_squared;
}

static bool flatten_cubic_device(struct path *path, struct point p0,
				 struct point p1, struct point p2,
				 struct point p3, s32 depth)
{
	struct point p01;
	struct point p12;
	struct point p23;
	struct point p012;
	struct point p123;
	struct point midpoint;
	const double tolerance_squared = 0.0625;

	if (depth >= 14 ||
	    (point_line_distance_squared(p1, p0, p3) <= tolerance_squared &&
	     point_line_distance_squared(p2, p0, p3) <= tolerance_squared)) {
		if (!path_reserve_points(path, 1))
			return false;
		path->points[path->point_count++] = p3;
		path->contours[path->contour_count - 1].count++;
		return true;
	}

	p01 = (struct point){ (p0.x + p1.x) / 2, (p0.y + p1.y) / 2 };
	p12 = (struct point){ (p1.x + p2.x) / 2, (p1.y + p2.y) / 2 };
	p23 = (struct point){ (p2.x + p3.x) / 2, (p2.y + p3.y) / 2 };
	p012 = (struct point){ (p01.x + p12.x) / 2, (p01.y + p12.y) / 2 };
	p123 = (struct point){ (p12.x + p23.x) / 2, (p12.y + p23.y) / 2 };
	midpoint =
		(struct point){ (p012.x + p123.x) / 2, (p012.y + p123.y) / 2 };
	return flatten_cubic_device(path, p0, p01, p012, midpoint, depth + 1) &&
	       flatten_cubic_device(path, midpoint, p123, p23, p3, depth + 1);
}

static bool path_cubic_to(struct path *path, double x0, double y0, double x1,
			  double y1, double x2, double y2, double x3, double y3)
{
	return flatten_cubic_device(path, matrix_point(path->matrix, x0, y0),
				    matrix_point(path->matrix, x1, y1),
				    matrix_point(path->matrix, x2, y2),
				    matrix_point(path->matrix, x3, y3), 0);
}

static bool path_quadratic_to(struct path *path, double x0, double y0,
			      double x1, double y1, double x2, double y2)
{
	double c1x = x0 + (2.0 / 3.0) * (x1 - x0);
	double c1y = y0 + (2.0 / 3.0) * (y1 - y0);
	double c2x = x2 + (2.0 / 3.0) * (x1 - x2);
	double c2y = y2 + (2.0 / 3.0) * (y1 - y2);

	return path_cubic_to(path, x0, y0, c1x, c1y, c2x, c2y, x2, y2);
}

static double vector_angle(double ux, double uy, double vx, double vy)
{
	double denominator = sqrt((ux * ux + uy * uy) * (vx * vx + vy * vy));
	double cosine;
	double angle;

	if (denominator < 1e-20)
		return 0;
	cosine = (ux * vx + uy * vy) / denominator;
	if (cosine < -1)
		cosine = -1;
	if (cosine > 1)
		cosine = 1;
	angle = acos(cosine);
	return ux * vy - uy * vx < 0 ? -angle : angle;
}

struct arc_geometry {
	double rx;
	double ry;
	double cosine;
	double sine;
	double cx;
	double cy;
	double start;
	double delta;
};

static bool arc_geometry(struct arc_geometry *arc, double x0, double y0,
			 double x1, double y1, double rotation, bool large,
			 bool sweep)
{
	double dx = (x0 - x1) / 2;
	double dy = (y0 - y1) / 2;
	double px;
	double py;
	double factor;
	double cxp;
	double cyp;
	double lambda;
	double phi = fmod(rotation, 360.0) * PI / 180.0;

	arc->cosine = cos(phi);
	arc->sine = sin(phi);
	px = arc->cosine * dx + arc->sine * dy;
	py = -arc->sine * dx + arc->cosine * dy;
	lambda = px * px / (arc->rx * arc->rx) + py * py / (arc->ry * arc->ry);
	if (lambda > 1) {
		double scale = sqrt(lambda);

		arc->rx *= scale;
		arc->ry *= scale;
	}
	factor = arc->rx * arc->rx * arc->ry * arc->ry -
		 arc->rx * arc->rx * py * py - arc->ry * arc->ry * px * px;
	lambda = arc->rx * arc->rx * py * py + arc->ry * arc->ry * px * px;
	factor = lambda <= 0 ? 0 : sqrt(fmax(0, factor / lambda));
	if (large == sweep)
		factor = -factor;
	cxp = factor * arc->rx * py / arc->ry;
	cyp = -factor * arc->ry * px / arc->rx;
	arc->cx = arc->cosine * cxp - arc->sine * cyp + (x0 + x1) / 2;
	arc->cy = arc->sine * cxp + arc->cosine * cyp + (y0 + y1) / 2;
	arc->start =
		vector_angle(1, 0, (px - cxp) / arc->rx, (py - cyp) / arc->ry);
	arc->delta = vector_angle((px - cxp) / arc->rx, (py - cyp) / arc->ry,
				  (-px - cxp) / arc->rx, (-py - cyp) / arc->ry);
	if (!sweep && arc->delta > 0)
		arc->delta -= 2 * PI;
	else if (sweep && arc->delta < 0)
		arc->delta += 2 * PI;
	return isfinite(arc->cx) && isfinite(arc->cy);
}

static bool flatten_arc(struct path *path, const struct arc_geometry *arc)
{
	double radius = fmax(arc->rx, arc->ry) * matrix_scale(path->matrix);
	double estimate = fabs(arc->delta) * sqrt(fmax(radius, 1.0));
	s32 segments;
	s32 i;

	if (!isfinite(estimate))
		goto out_invalid;
	segments = estimate > 4096 ? 4096 : (s32)ceil(estimate);
	if (segments < 4)
		segments = 4;
	for (i = 1; i <= segments; i++) {
		double angle = arc->start + arc->delta * i / segments;
		double ux = cos(angle);
		double uy = sin(angle);
		double x = arc->cx + arc->cosine * arc->rx * ux -
			   arc->sine * arc->ry * uy;
		double y = arc->cy + arc->sine * arc->rx * ux +
			   arc->cosine * arc->ry * uy;

		if (!path_line_to(path, x, y))
			return false;
	}
	return true;

out_invalid:
	archetypon_set_error(path->error, path->error_capacity,
			     "non-finite SVG arc geometry");
	return false;
}

static bool path_arc_to(struct path *path, double x0, double y0, double rx,
			double ry, double rotation, bool large_arc, bool sweep,
			double x1, double y1)
{
	struct arc_geometry arc = { .rx = fabs(rx), .ry = fabs(ry) };

	if (arc.rx < 1e-12 || arc.ry < 1e-12 ||
	    (fabs(x0 - x1) < 1e-12 && fabs(y0 - y1) < 1e-12))
		return path_line_to(path, x1, y1);
	if (!arc_geometry(&arc, x0, y0, x1, y1, rotation, large_arc, sweep))
		return false;
	return flatten_arc(path, &arc);
}

struct path_parser {
	const char *cursor;
	const char *end;
};

static void path_skip_separators(struct path_parser *parser)
{
	while (parser->cursor < parser->end &&
	       (isspace((unsigned char)*parser->cursor) ||
		*parser->cursor == ','))
		parser->cursor++;
}

static bool path_has_number(struct path_parser *parser)
{
	const char *cursor = parser->cursor;

	while (cursor < parser->end &&
	       (isspace((unsigned char)*cursor) || *cursor == ','))
		cursor++;
	return cursor < parser->end &&
	       (*cursor == '+' || *cursor == '-' || *cursor == '.' ||
		isdigit((unsigned char)*cursor));
}

static bool path_number(struct path_parser *parser, double *number)
{
	char *after;

	path_skip_separators(parser);
	if (parser->cursor >= parser->end)
		return false;
	errno = 0;
	*number = strtod(parser->cursor, &after);
	if (after == parser->cursor || after > parser->end || errno == ERANGE ||
	    !isfinite(*number))
		return false;
	parser->cursor = after;
	return true;
}

struct path_state {
	struct path_parser parser;
	struct path *path;
	char command;
	char previous;
	double x;
	double y;
	double start_x;
	double start_y;
	double cubic_x;
	double cubic_y;
	double quadratic_x;
	double quadratic_y;
	bool relative;
};

static bool path_pair(struct path_state *state, double *x, double *y)
{
	if (!path_number(&state->parser, x) || !path_number(&state->parser, y))
		return false;
	if (state->relative) {
		*x += state->x;
		*y += state->y;
	}
	return true;
}

static bool path_move_command(struct path_state *state)
{
	double x;
	double y;

	x = 0;
	y = 0;
	if (!path_pair(state, &x, &y))
		goto out_invalid;
	state->x = x;
	state->y = y;
	state->start_x = x;
	state->start_y = y;
	if (!path_begin_contour(state->path, x, y))
		return false;
	state->command = state->relative ? 'l' : 'L';
	state->previous = 'm';
	return true;

out_invalid:
	archetypon_set_error(state->path->error, state->path->error_capacity,
			     "invalid SVG move command");
	return false;
}

static bool path_line_command(struct path_state *state, char command)
{
	double x = state->x;
	double y = state->y;
	bool valid;

	switch (command) {
	case 'l':
		valid = path_pair(state, &x, &y);
		break;
	case 'h':
		valid = path_number(&state->parser, &x);
		if (valid && state->relative)
			x += state->x;
		break;
	default:
		valid = path_number(&state->parser, &y);
		if (valid && state->relative)
			y += state->y;
		break;
	}
	if (!valid)
		goto out_invalid;
	if (!path_line_to(state->path, x, y))
		return false;
	state->x = x;
	state->y = y;
	state->previous = command;
	return true;

out_invalid:
	archetypon_set_error(state->path->error, state->path->error_capacity,
			     "invalid SVG line command");
	return false;
}

static bool path_cubic_command(struct path_state *state, bool smooth)
{
	double x1 = state->x;
	double y1 = state->y;
	double x2;
	double y2;
	double x;
	double y;

	x2 = 0;
	y2 = 0;
	x = 0;
	y = 0;
	if (!smooth && !path_pair(state, &x1, &y1))
		goto out_invalid;
	if (smooth && (state->previous == 'c' || state->previous == 's')) {
		x1 = 2 * state->x - state->cubic_x;
		y1 = 2 * state->y - state->cubic_y;
	}
	if (!path_pair(state, &x2, &y2) || !path_pair(state, &x, &y))
		goto out_invalid;
	if (!path_cubic_to(state->path, state->x, state->y, x1, y1, x2, y2, x,
			   y))
		return false;
	state->x = x;
	state->y = y;
	state->cubic_x = x2;
	state->cubic_y = y2;
	state->previous = smooth ? 's' : 'c';
	return true;

out_invalid:
	archetypon_set_error(state->path->error, state->path->error_capacity,
			     "invalid SVG cubic command");
	return false;
}

static bool path_quadratic_command(struct path_state *state, bool smooth)
{
	double x1 = state->x;
	double y1 = state->y;
	double x;
	double y;

	x = 0;
	y = 0;
	if (!smooth && !path_pair(state, &x1, &y1))
		goto out_invalid;
	if (smooth && (state->previous == 'q' || state->previous == 't')) {
		x1 = 2 * state->x - state->quadratic_x;
		y1 = 2 * state->y - state->quadratic_y;
	}
	if (!path_pair(state, &x, &y))
		goto out_invalid;
	if (!path_quadratic_to(state->path, state->x, state->y, x1, y1, x, y))
		return false;
	state->x = x;
	state->y = y;
	state->quadratic_x = x1;
	state->quadratic_y = y1;
	state->previous = smooth ? 't' : 'q';
	return true;

out_invalid:
	archetypon_set_error(state->path->error, state->path->error_capacity,
			     "invalid SVG quadratic command");
	return false;
}

static bool path_arc_command(struct path_state *state)
{
	double values[7];
	size_t i;
	double x;
	double y;

	for (i = 0; i < ARRAY_SIZE(values); i++) {
		if (!path_number(&state->parser, &values[i]))
			goto out_invalid;
	}
	if ((values[3] != 0 && values[3] != 1) ||
	    (values[4] != 0 && values[4] != 1))
		goto out_invalid;
	x = values[5] + (state->relative ? state->x : 0);
	y = values[6] + (state->relative ? state->y : 0);
	if (!path_arc_to(state->path, state->x, state->y, values[0], values[1],
			 values[2], values[3] != 0, values[4] != 0, x, y))
		return false;
	state->x = x;
	state->y = y;
	state->previous = 'a';
	return true;

out_invalid:
	archetypon_set_error(state->path->error, state->path->error_capacity,
			     "invalid SVG arc command");
	return false;
}

static bool path_close_command(struct path_state *state)
{
	if (state->path->contour_count == 0) {
		archetypon_set_error(state->path->error,
				     state->path->error_capacity,
				     "SVG close command has no contour");
		return false;
	}
	state->path->contours[state->path->contour_count - 1].closed = true;
	state->x = state->start_x;
	state->y = state->start_y;
	state->previous = 'z';
	state->command = 0;
	return true;
}

static bool path_dispatch(struct path_state *state)
{
	char command = (char)tolower((unsigned char)state->command);

	switch (command) {
	case 'm':
		return path_move_command(state);
	case 'l':
	case 'h':
	case 'v':
		return path_line_command(state, command);
	case 'c':
	case 's':
		return path_cubic_command(state, command == 's');
	case 'q':
	case 't':
		return path_quadratic_command(state, command == 't');
	case 'a':
		return path_arc_command(state);
	case 'z':
		return path_close_command(state);
	default:
		archetypon_set_error(state->path->error,
				     state->path->error_capacity,
				     "unsupported SVG path command '%c'",
				     state->command);
		return false;
	}
}

static bool parse_path_data(struct slice data, struct matrix matrix,
			    struct path *path, char *error,
			    size_t error_capacity)
{
	struct path_state state = { .parser = { data.begin, data.end },
				    .path = path };

	memset(path, 0, sizeof(*path));
	path->matrix = matrix;
	path->error = error;
	path->error_capacity = error_capacity;
	for (;;) {
		path_skip_separators(&state.parser);
		if (state.parser.cursor >= state.parser.end)
			return true;
		if (isalpha((unsigned char)*state.parser.cursor))
			state.command = *state.parser.cursor++;
		if (state.command == 0) {
			archetypon_set_error(error, error_capacity,
					     "SVG path is missing a command");
			return false;
		}
		state.relative = islower((unsigned char)state.command);
		if (!path_dispatch(&state))
			return false;
	}
}

static void path_free(struct path *path)
{
	free(path->points);
	free(path->contours);
	memset(path, 0, sizeof(*path));
}

static s32 compare_intersections(const void *left, const void *right)
{
	const struct intersection *a = (const struct intersection *)left;
	const struct intersection *b = (const struct intersection *)right;

	if (a->x < b->x)
		return -1;
	if (a->x > b->x)
		return 1;
	return b->winding - a->winding;
}

static void composite_pixel(u8 *pixel, struct color color, u8 alpha)
{
	u32 inverse = 255 - alpha;

	pixel[0] = (u8)(((u32)color.r * alpha + (u32)pixel[0] * inverse + 127) /
			255);
	pixel[1] = (u8)(((u32)color.g * alpha + (u32)pixel[1] * inverse + 127) /
			255);
	pixel[2] = (u8)(((u32)color.b * alpha + (u32)pixel[2] * inverse + 127) /
			255);
	pixel[3] = (u8)(alpha + ((u32)pixel[3] * inverse + 127) / 255);
}

static u8 effective_alpha(struct color color, double opacity)
{
	double alpha = color.a * opacity;

	if (alpha < 0)
		alpha = 0;
	if (alpha > 255)
		alpha = 255;
	return (u8)lround(alpha);
}

static void composite_span(struct archetypon_image *surface, s32 row,
			   double start, double end, struct color color,
			   u8 alpha)
{
	double bounded_start;
	double bounded_end;
	s32 first;
	s32 last;
	s32 column;

	if (!isfinite(start) || !isfinite(end) || end < 0 ||
	    start >= surface->width) {
		return;
	}
	bounded_start = fmax(0, start);
	bounded_end = fmin(surface->width, end);
	first = (s32)ceil(bounded_start - 0.5);
	last = (s32)floor(bounded_end - 0.5);
	if (first < 0)
		first = 0;
	if (last >= surface->width)
		last = surface->width - 1;
	for (column = first; column <= last; column++) {
		size_t offset = ((size_t)row * (size_t)surface->width +
				 (size_t)column) *
				4;
		composite_pixel(surface->pixels + offset, color, alpha);
	}
}

static struct color gradient_color(const struct style *style, double x,
				   double y)
{
	const struct linear_gradient *gradient = style->gradient;
	struct point sample = matrix_point(style->gradient_inverse, x, y);
	double dx = style->gradient_end.x - style->gradient_start.x;
	double dy = style->gradient_end.y - style->gradient_start.y;
	double length = dx * dx + dy * dy;
	double t = length > 1e-20
			   ? ((sample.x - style->gradient_start.x) * dx +
			      (sample.y - style->gradient_start.y) * dy) /
				     length
			   : 0;
	size_t index;

	if (t < 0)
		t = 0;
	if (t > 1)
		t = 1;
	if (!gradient || gradient->stop_count == 0)
		return color_rgba(0, 0, 0, 0);
	if (t <= gradient->stops[0].offset)
		return gradient->stops[0].color;
	for (index = 1; index < gradient->stop_count; index++) {
		const struct gradient_stop *a = &gradient->stops[index - 1];
		const struct gradient_stop *b = &gradient->stops[index];
		if (t <= b->offset) {
			double q = b->offset > a->offset
					   ? (t - a->offset) /
						     (b->offset - a->offset)
					   : 1;
			double alpha =
				a->color.a + q * (b->color.a - a->color.a);
			struct color result =
				color_rgba(0, 0, 0, (u8)lround(alpha));

			if (alpha > 0) {
				result.r = (u8)lround(
					(a->color.r * a->color.a +
					 q * (b->color.r * b->color.a -
					      a->color.r * a->color.a)) /
					alpha);
				result.g = (u8)lround(
					(a->color.g * a->color.a +
					 q * (b->color.g * b->color.a -
					      a->color.g * a->color.a)) /
					alpha);
				result.b = (u8)lround(
					(a->color.b * a->color.a +
					 q * (b->color.b * b->color.a -
					      a->color.b * a->color.a)) /
					alpha);
			}
			return result;
		}
	}
	return gradient->stops[gradient->stop_count - 1].color;
}

static void composite_gradient_span(struct archetypon_image *surface, s32 row,
				    double start, double end,
				    const struct style *style, u8 alpha)
{
	s32 first = (s32)fmax(0, ceil(start - 0.5));
	s32 last = (s32)fmin(surface->width - 1, floor(end - 0.5));
	s32 column;
	for (column = first; column <= last; column++) {
		struct color color =
			gradient_color(style, column + 0.5, row + 0.5);
		u8 effective = (u8)(((u32)alpha * color.a + 127) / 255);
		size_t offset = ((size_t)row * surface->width + column) * 4;
		color.a = 255;
		composite_pixel(surface->pixels + offset, color, effective);
	}
}

static size_t fill_intersections(const struct path *path, double y,
				 struct intersection *intersections)
{
	size_t count = 0;
	size_t contour_index;

	for (contour_index = 0; contour_index < path->contour_count;
	     contour_index++) {
		const struct contour *contour = &path->contours[contour_index];
		size_t i;

		if (contour->count < 2)
			continue;
		for (i = 0; i < contour->count; i++) {
			struct point start = path->points[contour->start + i];
			struct point end =
				path->points[contour->start +
					     ((i + 1) % contour->count)];
			double ratio;

			if (!((start.y <= y && end.y > y) ||
			      (end.y <= y && start.y > y)))
				continue;
			ratio = (y - start.y) / (end.y - start.y);
			intersections[count].x =
				start.x + ratio * (end.x - start.x);
			intersections[count].winding = end.y > start.y ? 1 : -1;
			count++;
		}
	}
	return count;
}

static void fill_row(struct archetypon_image *surface, s32 row,
		     struct intersection *intersections, size_t count,
		     const struct style *style, u8 alpha)
{
	s32 winding = 0;
	double start = 0;
	size_t i;

	qsort(intersections, count, sizeof(*intersections),
	      compare_intersections);
	if (style->fill_rule_evenodd) {
		for (i = 0; i + 1 < count; i += 2)
			if (style->gradient)
				composite_gradient_span(
					surface, row, intersections[i].x,
					intersections[i + 1].x, style, alpha);
			else
				composite_span(surface, row, intersections[i].x,
					       intersections[i + 1].x,
					       style->fill, alpha);
		return;
	}
	for (i = 0; i < count; i++) {
		s32 previous = winding;

		winding += intersections[i].winding;
		if (previous == 0 && winding != 0)
			start = intersections[i].x;
		else if (previous != 0 && winding == 0) {
			if (style->gradient)
				composite_gradient_span(surface, row, start,
							intersections[i].x,
							style, alpha);
			else
				composite_span(surface, row, start,
					       intersections[i].x, style->fill,
					       alpha);
		}
	}
}

static bool fill_rows(const struct archetypon_image *surface,
		      const struct path *path, s32 *first, s32 *last)
{
	double minimum = INFINITY;
	double maximum = -INFINITY;
	size_t i;

	for (i = 0; i < path->point_count; i++) {
		minimum = fmin(minimum, path->points[i].y);
		maximum = fmax(maximum, path->points[i].y);
	}
	if (!isfinite(minimum) || !isfinite(maximum) || maximum < 0 ||
	    minimum >= surface->height)
		return false;
	*first = (s32)fmax(0, floor(fmin(minimum, surface->height - 1)));
	*last = (s32)fmin(surface->height - 1, ceil(fmax(0, maximum)));
	return true;
}

static bool consume_render_work(size_t *remaining, size_t count,
				size_t repetitions, char *error,
				size_t error_capacity)
{
	if (repetitions != 0 && count > *remaining / repetitions) {
		archetypon_set_error(error, error_capacity,
				     "SVG render exceeds the work limit");
		return false;
	}
	*remaining -= count * repetitions;
	return true;
}

static size_t sort_work_factor(size_t count)
{
	size_t factor = 0;

	while (count > 1) {
		count = (count + 1) / 2;
		factor++;
	}
	return factor == 0 ? 1 : factor;
}

static bool draw_fill(struct archetypon_image *surface, const struct path *path,
		      const struct style *style, size_t *work_remaining,
		      char *error, size_t error_capacity)
{
	struct intersection *intersections;
	s32 first;
	s32 last;
	s32 row;
	u8 alpha;

	if (style->fill.none || path->point_count < 3)
		return true;
	alpha = effective_alpha(style->fill,
				style->opacity * style->fill_opacity);
	if (alpha == 0)
		return true;
	if (!fill_rows(surface, path, &first, &last))
		return true;
	if (!consume_render_work(work_remaining,
				path->point_count *
				sort_work_factor(path->point_count),
				(size_t)(last - first + 1), error,
				error_capacity))
		return false;
	intersections = malloc(path->point_count * sizeof(*intersections));
	if (!intersections) {
		archetypon_set_error(error, error_capacity,
				     "out of memory rasterizing SVG");
		return false;
	}
	for (row = first; row <= last; row++) {
		size_t count =
			fill_intersections(path, row + 0.5, intersections);

		if (count >= 2)
			fill_row(surface, row, intersections, count, style,
				 alpha);
	}
	free(intersections);
	return true;
}

struct stroke_mask {
	double radius;
	s32 left;
	s32 top;
	s32 right;
	s32 bottom;
	s32 width;
	u8 *data;
};

struct stroke_segment {
	struct point start;
	struct point end;
	double dx;
	double dy;
	double length_squared;
	s32 left;
	s32 top;
	s32 right;
	s32 bottom;
	bool start_cap;
	bool end_cap;
};

static int stroke_bounds(const struct archetypon_image *surface,
			 const struct path *path, struct stroke_mask *mask, double padding,
			 char *error, size_t capacity)
{
	double min_x = INFINITY;
	double min_y = INFINITY;
	double max_x = -INFINITY;
	double max_y = -INFINITY;
	size_t i;

	for (i = 0; i < path->point_count; i++) {
		min_x = fmin(min_x, path->points[i].x);
		min_y = fmin(min_y, path->points[i].y);
		max_x = fmax(max_x, path->points[i].x);
		max_y = fmax(max_y, path->points[i].y);
	}
	if (!isfinite(padding) || !isfinite(min_x) || !isfinite(min_y) ||
	    !isfinite(max_x) || !isfinite(max_y)) {
		archetypon_set_error(error, capacity,
				     "non-finite SVG stroke geometry");
		return -1;
	}
	if (max_x + padding < 0 || max_y + padding < 0 ||
	    min_x - padding >= surface->width ||
	    min_y - padding >= surface->height)
		return 0;
	mask->left = (s32)fmax(0,
		floor(fmin(min_x - padding - 1, surface->width - 1)));
	mask->top = (s32)fmax(0,
		floor(fmin(min_y - padding - 1, surface->height - 1)));
	mask->right = (s32)fmin(surface->width - 1,
				ceil(fmax(0, max_x + padding + 1)));
	mask->bottom = (s32)fmin(surface->height - 1,
				 ceil(fmax(0, max_y + padding + 1)));
	return mask->right >= mask->left && mask->bottom >= mask->top;
}

static bool stroke_mask_alloc(struct stroke_mask *mask, char *error,
			      size_t capacity)
{
	size_t size;
	s32 height;

	mask->width = mask->right - mask->left + 1;
	height = mask->bottom - mask->top + 1;
	if (!archetypon_multiply_size((size_t)mask->width, (size_t)height,
				      &size))
		goto out_error;
	mask->data = calloc(size, 1);
	if (mask->data)
		return true;

out_error:
	archetypon_set_error(error, capacity,
			     "out of memory rasterizing stroke");
	return false;
}

static int stroke_segment(struct stroke_segment *segment,
			  const struct contour *contour,
			  const struct path *path, size_t index,
			  const struct stroke_mask *mask)
{
	size_t count = contour->closed ? contour->count : contour->count - 1;

	segment->start = path->points[contour->start + index];
	segment->end =
		path->points[contour->start + (index + 1) % contour->count];
	segment->dx = segment->end.x - segment->start.x;
	segment->dy = segment->end.y - segment->start.y;
	segment->length_squared =
		segment->dx * segment->dx + segment->dy * segment->dy;
	if (segment->length_squared < 1e-20)
		return 0;
	if (!isfinite(segment->length_squared))
		return -1;
	segment->left = (s32)fmax(mask->left,
		floor(fmin(fmin(segment->start.x, segment->end.x) -
					       mask->radius - 1,
				       (double)mask->right)));
	segment->top = (s32)fmax(mask->top,
		floor(fmin(fmin(segment->start.y, segment->end.y) -
					      mask->radius - 1,
				      (double)mask->bottom)));
	segment->right = (s32)fmin(mask->right,
				   ceil(fmax(segment->start.x, segment->end.x) +
					mask->radius + 1));
	segment->bottom = (s32)fmin(mask->bottom,
		ceil(fmax(segment->start.y, segment->end.y) +
				   mask->radius + 1));
	segment->start_cap = index == 0 && !contour->closed;
	segment->end_cap = index + 1 == count && !contour->closed;
	return 1;
}

static bool stroke_pixel(const struct stroke_segment *segment,
			 const struct stroke_mask *mask, s32 x, s32 y,
			 s32 line_cap, s32 line_join)
{
	double px = x + 0.5;
	double py = y + 0.5;
	double projection = ((px - segment->start.x) * segment->dx +
			     (py - segment->start.y) * segment->dy) /
			    segment->length_squared;
	double dx;
	double dy;

	if (line_join != 1 &&
	    ((!segment->start_cap && projection < 0) ||
	     (!segment->end_cap && projection > 1)))
		return false;
	if (line_cap == 2) {
		double extension = mask->radius / sqrt(segment->length_squared);

		if ((segment->start_cap && projection < -extension) ||
		    (segment->end_cap && projection > 1 + extension))
			return false;
	} else if (line_cap == 0 && ((segment->start_cap && projection < 0) ||
				     (segment->end_cap && projection > 1)))
		return false;
	projection = fmax(0, fmin(1, projection));
	dx = px - (segment->start.x + projection * segment->dx);
	dy = py - (segment->start.y + projection * segment->dy);
	return dx * dx + dy * dy <= mask->radius * mask->radius;
}

static void raster_stroke_segment(const struct stroke_segment *segment,
				  struct stroke_mask *mask, s32 line_cap,
				  s32 line_join)
{
	s32 y;

	for (y = segment->top; y <= segment->bottom; y++) {
		size_t row;
		s32 x;

		row = (size_t)(y - mask->top) * mask->width;
		for (x = segment->left; x <= segment->right; x++) {
			if (stroke_pixel(segment, mask, x, y, line_cap, line_join))
				mask->data[row + x - mask->left] = 1;
		}
	}
}

static double triangle_edge(struct point a, struct point b,
			    double x, double y)
{
	return (x - a.x) * (b.y - a.y) - (y - a.y) * (b.x - a.x);
}

static bool raster_stroke_triangle(struct stroke_mask *mask, struct point a,
				   struct point b, struct point c,
				   size_t *work_remaining, char *error,
				   size_t error_capacity)
{
	s32 left = (s32)fmax(mask->left,
			       floor(fmin(a.x, fmin(b.x, c.x))));
	s32 top = (s32)fmax(mask->top,
			      floor(fmin(a.y, fmin(b.y, c.y))));
	s32 right = (s32)fmin(mask->right,
				ceil(fmax(a.x, fmax(b.x, c.x))));
	s32 bottom = (s32)fmin(mask->bottom,
				 ceil(fmax(a.y, fmax(b.y, c.y))));
	s32 y;

	if (right < left || bottom < top)
		return true;
	if (!consume_render_work(work_remaining, (size_t)(right - left + 1),
				 (size_t)(bottom - top + 1), error,
				 error_capacity))
		return false;
	for (y = top; y <= bottom; y++) {
		s32 x;
		size_t row = (size_t)(y - mask->top) * mask->width;

		for (x = left; x <= right; x++) {
			double e1 = triangle_edge(a, b, x + 0.5, y + 0.5);
			double e2 = triangle_edge(b, c, x + 0.5, y + 0.5);
			double e3 = triangle_edge(c, a, x + 0.5, y + 0.5);

			if ((e1 >= 0 && e2 >= 0 && e3 >= 0) ||
			    (e1 <= 0 && e2 <= 0 && e3 <= 0))
				mask->data[row + x - mask->left] = 1;
		}
	}
	return true;
}

static bool raster_stroke_join(struct stroke_mask *mask, struct point before,
			       struct point point, struct point after,
			       s32 line_join, double miter_limit,
			       size_t *work_remaining, char *error,
			       size_t error_capacity)
{
	double ax = point.x - before.x;
	double ay = point.y - before.y;
	double bx = after.x - point.x;
	double by = after.y - point.y;
	double al = hypot(ax, ay);
	double bl = hypot(bx, by);
	double cross;
	double dot;
	double side;
	struct point p1;
	struct point p2;

	if (al < 1e-10 || bl < 1e-10)
		return true;
	ax /= al;
	ay /= al;
	bx /= bl;
	by /= bl;
	cross = ax * by - ay * bx;
	dot = ax * bx + ay * by;
	if (fabs(cross) < 1e-10 || dot < -0.999999)
		return true;
	side = cross > 0 ? 1 : -1;
	p1 = (struct point){ point.x + side * ay * mask->radius,
				   point.y - side * ax * mask->radius };
	p2 = (struct point){ point.x + side * by * mask->radius,
				   point.y - side * bx * mask->radius };
	if (line_join == 0) {
		double ratio = sqrt(2 / (1 + dot));

		if (ratio <= miter_limit) {
			double factor = side * mask->radius / (1 + dot);
			struct point miter = {
				point.x + factor * (ay + by),
				point.y - factor * (ax + bx)
			};

			return raster_stroke_triangle(mask, p1, miter, point,
						      work_remaining, error,
						      error_capacity) &&
			       raster_stroke_triangle(mask, miter, p2, point,
						      work_remaining, error,
						      error_capacity);
		}
	}
	return raster_stroke_triangle(mask, p1, p2, point, work_remaining,
				      error, error_capacity);
}

static bool raster_dashed_contour(const struct contour *contour,
				  const struct path *path,
				  struct stroke_mask *mask,
				  const struct style *style, double scale,
				  size_t *work_remaining, char *error,
				  size_t error_capacity)
{
	size_t segment_count = contour->closed ? contour->count :
						 contour->count - 1;
	size_t pattern_count = style->dash_count *
			       (style->dash_count % 2 ? 2 : 1);
	double period = 0;
	double phase;
	double remaining;
	size_t pattern = 0;
	size_t i;

	for (i = 0; i < pattern_count; i++)
		period += style->dashes[i % style->dash_count] * scale;
	if (period <= 0 || !isfinite(period))
		return true;
	phase = fmod(style->dash_offset * scale, period);
	if (phase < 0)
		phase += period;
	while (phase >= style->dashes[pattern % style->dash_count] * scale) {
		double length = style->dashes[pattern % style->dash_count] * scale;

		if (length > 0)
			phase -= length;
		pattern = (pattern + 1) % pattern_count;
	}
	remaining = style->dashes[pattern % style->dash_count] * scale - phase;
	for (i = 0; i < segment_count; i++) {
		struct point start = path->points[contour->start + i];
		struct point end = path->points[contour->start +
					 (i + 1) % contour->count];
		double dx = end.x - start.x;
		double dy = end.y - start.y;
		double length = hypot(dx, dy);
		double position = 0;

		if (!isfinite(length))
			goto out_nonfinite;
		while (position < length) {
			double take;

			while (remaining <= 1e-12) {
				pattern = (pattern + 1) % pattern_count;
				remaining = style->dashes[pattern %
						style->dash_count] * scale;
			}
			take = fmin(remaining, length - position);
			if (pattern % 2 == 0 && take > 1e-12) {
				struct stroke_segment dash = { 0 };
				double begin = position / length;
				double finish = (position + take) / length;
				size_t width;
				size_t height;

				dash.start = (struct point){ start.x + begin * dx,
							     start.y + begin * dy };
				dash.end = (struct point){ start.x + finish * dx,
							   start.y + finish * dy };
				dash.dx = dash.end.x - dash.start.x;
				dash.dy = dash.end.y - dash.start.y;
				dash.length_squared = dash.dx * dash.dx +
						      dash.dy * dash.dy;
				dash.left = (s32)fmax(mask->left,
					floor(fmin(dash.start.x, dash.end.x) -
					      mask->radius - 1));
				dash.top = (s32)fmax(mask->top,
					floor(fmin(dash.start.y, dash.end.y) -
					      mask->radius - 1));
				dash.right = (s32)fmin(mask->right,
					ceil(fmax(dash.start.x, dash.end.x) +
					     mask->radius + 1));
				dash.bottom = (s32)fmin(mask->bottom,
					ceil(fmax(dash.start.y, dash.end.y) +
					     mask->radius + 1));
				dash.start_cap = true;
				dash.end_cap = true;
				if (dash.right >= dash.left && dash.bottom >= dash.top) {
					width = (size_t)(dash.right - dash.left + 1);
					height = (size_t)(dash.bottom - dash.top + 1);
					if (!consume_render_work(work_remaining, width,
							 height, error,
							 error_capacity))
						return false;
					raster_stroke_segment(&dash, mask,
							      style->line_cap, 1);
				}
			}
			position += take;
			remaining -= take;
		}
	}
	return true;

out_nonfinite:
	archetypon_set_error(error, error_capacity,
			     "non-finite SVG stroke segment");
	return false;
}

static bool raster_stroke_contour(const struct contour *contour,
				  const struct path *path,
				  struct stroke_mask *mask, s32 line_cap,
				  s32 line_join, double miter_limit,
				  size_t *work_remaining, char *error,
				  size_t error_capacity)
{
	size_t count = contour->closed ? contour->count : contour->count - 1;
	size_t i;

	for (i = 0; i < count; i++) {
		struct stroke_segment segment;
		int status = stroke_segment(&segment, contour, path, i, mask);

		if (status < 0) {
			archetypon_set_error(error, error_capacity,
					     "non-finite SVG stroke segment");
			return false;
		}
		if (status > 0) {
			size_t width = (size_t)(segment.right - segment.left + 1);
			size_t height = (size_t)(segment.bottom - segment.top + 1);

			if (!consume_render_work(work_remaining, width, height,
						 error, error_capacity))
				return false;
			raster_stroke_segment(&segment, mask, line_cap, line_join);
		}
	}
	if (line_join != 1 && contour->count > 2) {
		size_t first = contour->closed ? 0 : 1;
		size_t last = contour->closed ? contour->count : contour->count - 1;

		for (i = first; i < last; i++) {
			struct point before = path->points[contour->start +
				(i + contour->count - 1) % contour->count];
			struct point point = path->points[contour->start + i];
			struct point after = path->points[contour->start +
				(i + 1) % contour->count];

			if (!raster_stroke_join(mask, before, point, after,
						line_join, miter_limit,
						work_remaining, error,
						error_capacity))
				return false;
		}
	}

	return true;
}

static void composite_stroke(struct archetypon_image *surface,
			     const struct stroke_mask *mask, struct color color,
			     u8 alpha)
{
	s32 y;

	for (y = mask->top; y <= mask->bottom; y++) {
		s32 x;

		for (x = mask->left; x <= mask->right; x++) {
			if (!mask->data[(size_t)(y - mask->top) * mask->width +
					x - mask->left])
				continue;
			composite_pixel(surface->pixels +
					((size_t)y * surface->width + x) * 4,
				color, alpha);
		}
	}
}

static bool draw_stroke(struct archetypon_image *surface,
			const struct path *path, const struct style *style,
			struct matrix matrix, size_t *work_remaining, char *error,
			size_t error_capacity)
{
	struct stroke_mask mask = { .radius = style->stroke_width *
					      matrix_scale(matrix) / 2 };
	size_t contour_index;
	u8 alpha;
	int bounds;
	bool ok = false;

	if (style->stroke.none || style->stroke_width <= 0 ||
	    path->point_count < 2 || mask.radius <= 0)
		return true;
	alpha = effective_alpha(style->stroke,
				style->opacity * style->stroke_opacity);
	if (alpha == 0)
		return true;
	bounds = stroke_bounds(surface, path, &mask,
		style->line_join == 0 && style->dash_count == 0 ?
		mask.radius * fmin(style->miter_limit, 2048) : mask.radius,
		error, error_capacity);
	if (bounds <= 0)
		return bounds == 0;
	if (!stroke_mask_alloc(&mask, error, error_capacity))
		return false;
	for (contour_index = 0; contour_index < path->contour_count;
	     contour_index++) {
		const struct contour *contour = &path->contours[contour_index];

		if (style->dash_count > 0) {
			if (!raster_dashed_contour(contour, path, &mask, style,
					     matrix_scale(matrix), work_remaining,
					     error, error_capacity))
				goto out_free;
		} else if (!raster_stroke_contour(contour, path, &mask,
						  style->line_cap,
						  style->line_join,
						  style->miter_limit,
						  work_remaining, error,
						  error_capacity))
			goto out_free;
	}
	composite_stroke(surface, &mask, style->stroke, alpha);
	ok = true;

out_free:
	free(mask.data);
	return ok;
}

static bool draw_path(struct archetypon_image *surface, const struct path *path,
		      const struct style *style, struct matrix matrix,
		      size_t *work_remaining, char *error,
		      size_t error_capacity)
{
	return draw_fill(surface, path, style, work_remaining, error,
			 error_capacity) &&
	       draw_stroke(surface, path, style, matrix, work_remaining, error,
			   error_capacity);
}

static bool required_length(const struct tag *tag, const char *name,
			    double *value, char *error, size_t error_capacity)
{
	struct slice attribute;

	if (!attribute_find(tag, name, &attribute) ||
	    !parse_length(attribute, value)) {
		archetypon_set_error(error, error_capacity,
				     "SVG <%.*s> needs a valid %s",
				     (s32)(tag->name.end - tag->name.begin),
				     tag->name.begin, name);
		return false;
	}
	return true;
}

static bool optional_length(const struct tag *tag, const char *name,
			    double fallback, double *value, char *error,
			    size_t error_capacity)
{
	struct slice attribute;
	*value = fallback;
	if (attribute_find(tag, name, &attribute) &&
	    !parse_length(attribute, value)) {
		archetypon_set_error(error, error_capacity, "invalid SVG %s",
				     name);
		return false;
	}
	return true;
}

struct rectangle_geometry {
	double x;
	double y;
	double width;
	double height;
	double rx;
	double ry;
};

struct ellipse_geometry {
	double cx;
	double cy;
	double rx;
	double ry;
};

static bool build_data_path(const struct tag *tag, struct path *path)
{
	struct slice data;

	if (!attribute_find(tag, "d", &data)) {
		archetypon_set_error(path->error, path->error_capacity,
				     "SVG <path> is missing d");
		return false;
	}
	return parse_path_data(data, path->matrix, path, path->error,
			       path->error_capacity);
}

static bool build_line_path(const struct tag *tag, struct path *path)
{
	double x1;
	double y1;
	double x2;
	double y2;

	if (!optional_length(tag, "x1", 0, &x1, path->error,
			     path->error_capacity) ||
	    !optional_length(tag, "y1", 0, &y1, path->error,
			     path->error_capacity) ||
	    !optional_length(tag, "x2", 0, &x2, path->error,
			     path->error_capacity) ||
	    !optional_length(tag, "y2", 0, &y2, path->error,
			     path->error_capacity))
		return false;
	return path_begin_contour(path, x1, y1) && path_line_to(path, x2, y2);
}

static bool rectangle_geometry(const struct tag *tag, struct path *path,
			       struct rectangle_geometry *geometry)
{
	if (!optional_length(tag, "x", 0, &geometry->x, path->error,
			     path->error_capacity) ||
	    !optional_length(tag, "y", 0, &geometry->y, path->error,
			     path->error_capacity) ||
	    !required_length(tag, "width", &geometry->width, path->error,
			     path->error_capacity) ||
	    !required_length(tag, "height", &geometry->height, path->error,
			     path->error_capacity) ||
	    !optional_length(tag, "rx", 0, &geometry->rx, path->error,
			     path->error_capacity) ||
	    !optional_length(tag, "ry", 0, &geometry->ry, path->error,
			     path->error_capacity))
		return false;
	if (geometry->width < 0 || geometry->height < 0 || geometry->rx < 0 ||
	    geometry->ry < 0) {
		archetypon_set_error(path->error, path->error_capacity,
				     "negative SVG rectangle size");
		return false;
	}
	if (geometry->rx == 0 && geometry->ry != 0)
		geometry->rx = geometry->ry;
	if (geometry->ry == 0 && geometry->rx != 0)
		geometry->ry = geometry->rx;
	geometry->rx = fmin(geometry->rx, geometry->width / 2);
	geometry->ry = fmin(geometry->ry, geometry->height / 2);
	return true;
}

static bool build_rectangle_path(const struct tag *tag, struct path *path)
{
	struct rectangle_geometry geometry;
	double x;
	double y;
	double width;
	double height;
	double rx;
	double ry;

	if (!rectangle_geometry(tag, path, &geometry))
		return false;
	x = geometry.x;
	y = geometry.y;
	width = geometry.width;
	height = geometry.height;
	rx = geometry.rx;
	ry = geometry.ry;
	if (!path_begin_contour(path, x + rx, y) ||
	    !path_line_to(path, x + width - rx, y))
		return false;
	if (rx > 0 && !path_arc_to(path, x + width - rx, y, rx, ry, 0, 0, 1,
				   x + width, y + ry))
		return false;
	if (!path_line_to(path, x + width, y + height - ry))
		return false;
	if (rx > 0 && !path_arc_to(path, x + width, y + height - ry, rx, ry, 0,
				   0, 1, x + width - rx, y + height))
		return false;
	if (!path_line_to(path, x + rx, y + height))
		return false;
	if (rx > 0 && !path_arc_to(path, x + rx, y + height, rx, ry, 0, 0, 1,
				   x, y + height - ry))
		return false;
	if (!path_line_to(path, x, y + ry))
		return false;
	if (rx > 0 && !path_arc_to(path, x, y + ry, rx, ry, 0, 0, 1, x + rx, y))
		return false;
	path->contours[path->contour_count - 1].closed = true;
	return true;
}

static bool ellipse_geometry(const struct tag *tag, struct slice name,
			     struct path *path,
			     struct ellipse_geometry *geometry)
{
	if (!optional_length(tag, "cx", 0, &geometry->cx, path->error,
			     path->error_capacity) ||
	    !optional_length(tag, "cy", 0, &geometry->cy, path->error,
			     path->error_capacity))
		return false;
	if (slice_equal(name, "circle")) {
		if (!required_length(tag, "r", &geometry->rx, path->error,
				     path->error_capacity))
			return false;
		geometry->ry = geometry->rx;
	} else if (!required_length(tag, "rx", &geometry->rx, path->error,
				    path->error_capacity) ||
		   !required_length(tag, "ry", &geometry->ry, path->error,
				    path->error_capacity)) {
		return false;
	}
	if (geometry->rx < 0 || geometry->ry < 0) {
		archetypon_set_error(path->error, path->error_capacity,
				     "negative SVG ellipse radius");
		return false;
	}
	return true;
}

static s32 ellipse_segment_count(const struct ellipse_geometry *geometry,
				 struct path *path)
{
	double radius;
	double estimate;
	s32 segments;

	radius = fmax(geometry->rx, geometry->ry) * matrix_scale(path->matrix);
	estimate = 2 * PI * sqrt(fmax(radius, 1.0));
	if (!isfinite(estimate)) {
		archetypon_set_error(path->error, path->error_capacity,
				     "non-finite SVG ellipse geometry");
		return -1;
	}
	segments = estimate > 4096 ? 4096 : (s32)ceil(estimate);
	if (segments < 16)
		segments = 16;
	return segments;
}

static bool build_ellipse_path(const struct tag *tag, struct slice name,
			       struct path *path)
{
	struct ellipse_geometry geometry;
	s32 segments;
	s32 i;

	if (!ellipse_geometry(tag, name, path, &geometry))
		return false;
	segments = ellipse_segment_count(&geometry, path);
	if (segments < 0 ||
	    !path_begin_contour(path, geometry.cx + geometry.rx, geometry.cy))
		return false;
	for (i = 1; i < segments; i++) {
		double angle = 2 * PI * i / segments;

		if (!path_line_to(path, geometry.cx + cos(angle) * geometry.rx,
				  geometry.cy + sin(angle) * geometry.ry))
			return false;
	}
	path->contours[path->contour_count - 1].closed = true;
	return true;
}

static bool build_polygon_path(const struct tag *tag, struct slice name,
			       struct path *path)
{
	struct slice points;
	struct path_parser parser;
	double x;
	double y;

	if (!attribute_find(tag, "points", &points)) {
		archetypon_set_error(path->error, path->error_capacity,
				     "SVG polygon is missing points");
		return false;
	}
	parser = (struct path_parser){ points.begin, points.end };
	if (!path_number(&parser, &x) || !path_number(&parser, &y) ||
	    !path_begin_contour(path, x, y))
		goto invalid;
	while (path_has_number(&parser)) {
		if (!path_number(&parser, &x) || !path_number(&parser, &y) ||
		    !path_line_to(path, x, y))
			goto invalid;
	}
	path_skip_separators(&parser);
	if (parser.cursor != parser.end)
		goto invalid;
	if (slice_equal(name, "polygon"))
		path->contours[path->contour_count - 1].closed = true;
	return true;

invalid:
	archetypon_set_error(path->error, path->error_capacity,
			     "invalid SVG polygon points");
	return false;
}

static bool build_shape_path(const struct tag *tag, struct slice name,
			     struct matrix matrix, struct path *path,
			     char *error, size_t error_capacity)
{
	memset(path, 0, sizeof(*path));
	path->matrix = matrix;
	path->error = error;
	path->error_capacity = error_capacity;

	if (slice_equal(name, "path"))
		return build_data_path(tag, path);
	if (slice_equal(name, "line"))
		return build_line_path(tag, path);
	if (slice_equal(name, "rect"))
		return build_rectangle_path(tag, path);
	if (slice_equal(name, "circle") || slice_equal(name, "ellipse"))
		return build_ellipse_path(tag, name, path);
	if (slice_equal(name, "polyline") || slice_equal(name, "polygon"))
		return build_polygon_path(tag, name, path);
	archetypon_set_error(error, error_capacity,
			     "internal unsupported shape");
	return false;
}

static bool tag_is_shape(struct slice name)
{
	return slice_equal(name, "path") || slice_equal(name, "rect") ||
	       slice_equal(name, "circle") || slice_equal(name, "ellipse") ||
	       slice_equal(name, "line") || slice_equal(name, "polyline") ||
	       slice_equal(name, "polygon");
}

static bool tag_is_unsupported(struct slice name)
{
	static const char *const unsupported[] = {
		"text",		  "tspan",  "image",   "use",
		"radialGradient", "filter", "pattern", "foreignObject"};
	size_t index;

	for (index = 0; index < ARRAY_SIZE(unsupported); index++) {
		if (slice_equal(name, unsupported[index]))
			return true;
	}
	return false;
}

enum aspect_alignment {
	ASPECT_MIN,
	ASPECT_MID,
	ASPECT_MAX
};

struct svg_geometry {
	double view_x;
	double view_y;
	double view_width;
	double view_height;
	double intrinsic_width;
	double intrinsic_height;
	enum aspect_alignment align_x;
	enum aspect_alignment align_y;
	bool aspect_none;
	bool aspect_slice;
};

#define SVG_MAX_EFFECTS 256
#define SVG_MAX_EFFECT_SHAPES 64
struct effect_shape {
	struct tag tag;
	struct slice name;
	struct context context;
};
struct svg_effect {
	struct slice id;
	struct tag tag;
	struct slice name;
	struct context context;
	bool mask;
	bool region_object_bbox;
	double region_x;
	double region_y;
	double region_width;
	double region_height;
	bool region_x_percent;
	bool region_y_percent;
	bool region_width_percent;
	bool region_height_percent;
	bool luminance;
	struct matrix transform;
	struct effect_shape *shapes;
	size_t shape_count;
	size_t shape_capacity;
};

#define SVG_MAX_CSS_RULES 1024
struct css_rule {
	struct slice selector;
	struct slice declarations;
	s32 specificity;
};

enum scene_command_kind { SCENE_DRAW, SCENE_PUSH, SCENE_POP };

struct scene_command {
	enum scene_command_kind kind;
	size_t shape;
	struct context context;
};

struct compiled_shape {
	struct tag tag;
	struct slice name;
	struct context context;
};

struct archetypon_svg_document {
	char *source;
	size_t length;
	struct svg_geometry geometry;
	struct compiled_shape *shapes;
	size_t shape_count;
	size_t shape_capacity;
	struct scene_command *commands;
	size_t command_count;
	size_t command_capacity;
	struct linear_gradient *gradients;
	size_t gradient_count;
	size_t gradient_capacity;
	struct svg_effect *effects;
	size_t effect_count;
	size_t effect_capacity;
	struct css_rule *css_rules;
	size_t css_rule_count;
	size_t css_rule_capacity;
	size_t scene_bytes;
};

static bool scene_reserve(struct archetypon_svg_document *document,
			  void **items, size_t item_size, size_t count,
			  size_t *capacity, size_t initial, char *error,
			  size_t error_capacity)
{
	size_t next;
	void *resized;

	if (count < *capacity)
		return true;
	next = *capacity ? *capacity * 2 : initial;
	if (next <= *capacity ||
	    (next - *capacity) >
		    (SVG_MAX_SCENE_BYTES - document->scene_bytes) / item_size) {
		archetypon_set_error(error, error_capacity,
				     "SVG scene exceeds %d bytes",
				     SVG_MAX_SCENE_BYTES);
		return false;
	}
	resized = realloc(*items, next * item_size);
	if (!resized) {
		archetypon_set_error(error, error_capacity,
				     "out of memory compiling SVG scene");
		return false;
	}
	document->scene_bytes += (next - *capacity) * item_size;
	*items = resized;
	*capacity = next;
	return true;
}

static bool parse_preserve_aspect_ratio(struct slice value,
					struct svg_geometry *geometry)
{
	char text[32];
	char alignment[9];
	char mode[6] = { 0 };
	char extra[2];
	size_t length;
	int fields;

	value = slice_trim(value);
	length = (size_t)(value.end - value.begin);
	if (length == 0 || length >= sizeof(text))
		return false;
	memcpy(text, value.begin, length);
	text[length] = 0;
	fields = sscanf(text, "%8s %5s %1s", alignment, mode, extra);
	if (fields < 1 || fields > 2)
		return false;
	if (strcmp(alignment, "none") == 0) {
		if (fields != 1)
			return false;
		geometry->aspect_none = true;
		return true;
	}
	if (strlen(alignment) != 8 || alignment[0] != 'x' ||
	    alignment[4] != 'Y')
		return false;
	if (memcmp(alignment + 1, "Min", 3) == 0)
		geometry->align_x = ASPECT_MIN;
	else if (memcmp(alignment + 1, "Mid", 3) == 0)
		geometry->align_x = ASPECT_MID;
	else if (memcmp(alignment + 1, "Max", 3) == 0)
		geometry->align_x = ASPECT_MAX;
	else
		return false;
	if (memcmp(alignment + 5, "Min", 3) == 0)
		geometry->align_y = ASPECT_MIN;
	else if (memcmp(alignment + 5, "Mid", 3) == 0)
		geometry->align_y = ASPECT_MID;
	else if (memcmp(alignment + 5, "Max", 3) == 0)
		geometry->align_y = ASPECT_MAX;
	else
		return false;
	if (fields == 2) {
		if (strcmp(mode, "slice") == 0)
			geometry->aspect_slice = true;
		else if (strcmp(mode, "meet") != 0)
			return false;
	}
	return true;
}

static int read_svg_geometry(const struct tag *tag,
			     struct svg_geometry *geometry, char *error,
			     size_t error_capacity)
{
	static const char missing_geometry[] =
		"SVG needs a positive viewBox or width and height";
	struct slice value;
	struct slice viewbox = { 0 };
	double width = 0;
	double height = 0;
	bool width_percent = false;
	bool height_percent = false;
	bool has_width;
	bool has_height;
	bool has_viewbox;

	memset(geometry, 0, sizeof(*geometry));
	geometry->align_x = ASPECT_MID;
	geometry->align_y = ASPECT_MID;
	has_viewbox = attribute_find(tag, "viewBox", &viewbox);
	has_width = attribute_find(tag, "width", &value);
	if (has_width) {
		bool valid = parse_length(value, &width);

		if (!valid)
			width_percent = parse_percentage(value, &width);
		if ((!valid && !width_percent) || width <= 0) {
			archetypon_set_error(error, error_capacity,
					     "invalid SVG width");
			return -1;
		}
	}
	has_height = attribute_find(tag, "height", &value);
	if (has_height) {
		bool valid = parse_length(value, &height);

		if (!valid)
			height_percent = parse_percentage(value, &height);
		if ((!valid && !height_percent) || height <= 0) {
			archetypon_set_error(error, error_capacity,
					     "invalid SVG height");
			return -1;
		}
	}
	if (has_viewbox) {
		double numbers[4];

		if (!parse_number_list(viewbox, numbers, 4) || numbers[2] <= 0 ||
		    numbers[3] <= 0) {
			archetypon_set_error(error, error_capacity,
					     "invalid SVG viewBox");
			return -1;
		}
		geometry->view_x = numbers[0];
		geometry->view_y = numbers[1];
		geometry->view_width = numbers[2];
		geometry->view_height = numbers[3];
	} else if (has_width && has_height && !width_percent &&
		   !height_percent) {
		geometry->view_width = width;
		geometry->view_height = height;
	} else {
		archetypon_set_error(error, error_capacity, missing_geometry);
		return -1;
	}
	if (has_width && has_height && !width_percent && !height_percent) {
		geometry->intrinsic_width = width;
		geometry->intrinsic_height = height;
	} else if (has_width && !width_percent && has_viewbox) {
		geometry->intrinsic_width = width;
		geometry->intrinsic_height =
			width * geometry->view_height / geometry->view_width;
	} else if (has_height && !height_percent && has_viewbox) {
		geometry->intrinsic_width =
			height * geometry->view_width / geometry->view_height;
		geometry->intrinsic_height = height;
	} else {
		geometry->intrinsic_width = geometry->view_width;
		geometry->intrinsic_height = geometry->view_height;
	}
	if (attribute_find(tag, "preserveAspectRatio", &value) &&
	    !parse_preserve_aspect_ratio(value, geometry)) {
		archetypon_set_error(error, error_capacity,
				     "invalid SVG preserveAspectRatio");
		return -1;
	}
	return 0;
}

static bool validate_svg_document(const char *source, size_t length,
				  char *error, size_t error_capacity)
{
	struct slice stack[SVG_MAX_DEPTH];
	const char *cursor = source;
	const char *end = source + length;
	s32 depth = 0;
	size_t element_count = 0;
	bool saw_root = false;
	bool root_closed = false;

	while (cursor < end) {
		const char *text = cursor;
		struct tag tag;
		int special;

		while (cursor < end && *cursor != '<')
			cursor++;
		if (depth == 0) {
			while (text < cursor) {
				if (!isspace((unsigned char)*text++)) {
					archetypon_set_error(error, error_capacity,
							     "text outside the SVG root");
					return false;
				}
			}
		}
		if (cursor == end)
			break;
		special = skip_special_tag(&cursor, end, error, error_capacity);
		if (special < 0)
			return false;
		if (special > 0)
			continue;
		if (!parse_tag(cursor + 1, end, &tag, &cursor, error,
			       error_capacity))
			return false;
		if (root_closed) {
			archetypon_set_error(error, error_capacity,
					     "SVG contains elements after the root closes");
			return false;
		}
		if (tag.closing) {
			if (depth == 0 || !slice_same(tag.name, stack[depth - 1])) {
				archetypon_set_error(error, error_capacity,
						     "SVG contains mismatched closing tags");
				return false;
			}
			depth--;
			if (depth == 0)
				root_closed = true;
			continue;
		}
		if (++element_count > SVG_MAX_ELEMENTS) {
			archetypon_set_error(error, error_capacity,
					     "SVG exceeds the %d element limit",
					     SVG_MAX_ELEMENTS);
			return false;
		}
		if (!saw_root) {
			if (!slice_equal(local_name(tag.name), "svg")) {
				archetypon_set_error(error, error_capacity,
						     "the first SVG element must be <svg>");
				return false;
			}
			saw_root = true;
		}
		if (tag.self_closing) {
			if (depth == 0)
				root_closed = true;
			continue;
		}
		if (depth >= SVG_MAX_DEPTH) {
			archetypon_set_error(error, error_capacity,
					     "SVG nesting exceeds %d elements",
					     SVG_MAX_DEPTH);
			return false;
		}
		stack[depth++] = tag.name;
	}
	if (!saw_root) {
		archetypon_set_error(error, error_capacity,
				     "input contains no <svg> element");
		return false;
	}
	if (!root_closed || depth != 0) {
		archetypon_set_error(error, error_capacity,
				     "SVG contains unclosed elements");
		return false;
	}
	return true;
}

static bool find_svg_geometry(const char *source, size_t length,
			      struct svg_geometry *geometry, char *error,
			      size_t error_capacity)
{
	static const char first_element[] =
		"the first SVG element must be <svg>";
	const char *cursor = source;
	const char *end = source + length;
	struct tag tag;

	while (next_tag(&cursor, end, &tag, error, error_capacity)) {
		struct slice name = local_name(tag.name);

		if (tag.closing)
			continue;
		if (!slice_equal(name, "svg")) {
			archetypon_set_error(error, error_capacity,
					     first_element);
			return false;
		}
		return read_svg_geometry(&tag, geometry, error,
					 error_capacity) == 0;
	}
	if (error[0] != 0)
		return false;
	archetypon_set_error(error, error_capacity,
			     "input contains no <svg> element");
	return false;
}

struct render_state {
	struct archetypon_image surface;
	struct archetypon_svg_document *document;
	const struct archetypon_svg_document *resources;
	struct context stack[SVG_MAX_DEPTH];
	const char *cursor;
	const char *end;
	char *error;
	size_t error_capacity;
	s32 depth;
	size_t total_points;
	size_t work_remaining;
	size_t temporary_bytes;
	bool found_svg;
	bool root_closed;
};

struct channel_sum {
	u32 red;
	u32 green;
	u32 blue;
	u32 alpha;
};

static double alignment_offset(double extra, enum aspect_alignment alignment)
{
	if (alignment == ASPECT_MIN)
		return 0;
	if (alignment == ASPECT_MAX)
		return extra;
	return extra / 2;
}

static void initialize_viewport(struct render_state *state,
				const struct svg_geometry *geometry)
{
	struct matrix viewport;
	double scale_x = state->surface.width / geometry->view_width;
	double scale_y = state->surface.height / geometry->view_height;

	if (!geometry->aspect_none) {
		double scale = geometry->aspect_slice ?
			fmax(scale_x, scale_y) : fmin(scale_x, scale_y);
		scale_x = scale;
		scale_y = scale;
	}
	viewport.a = scale_x;
	viewport.b = 0;
	viewport.c = 0;
	viewport.d = scale_y;
	viewport.e = alignment_offset(state->surface.width -
				      geometry->view_width * scale_x,
				      geometry->align_x) -
		     geometry->view_x * scale_x;
	viewport.f = alignment_offset(state->surface.height -
				      geometry->view_height * scale_y,
				      geometry->align_y) -
		     geometry->view_y * scale_y;
	state->stack[0].matrix = viewport;
	state->stack[0].style = style_default();
	state->stack[0].name = (struct slice){ 0 };
	state->stack[0].own_opacity = 1;
	state->stack[0].render = true;
}

static int initialize_render_state_geometry(struct render_state *state,
				   const char *source, size_t length,
				   const struct svg_geometry *geometry,
				   s32 output_width, s32 output_height,
				   char *error, size_t error_capacity)
{
	size_t pixel_count;

	memset(state, 0, sizeof(*state));
	state->cursor = source;
	state->end = source + length;
	state->error = error;
	state->error_capacity = error_capacity;
	state->depth = 1;
	state->work_remaining = SVG_MAX_RENDER_WORK;
	state->surface.width = output_width * SUPERSAMPLE;
	state->surface.height = output_height * SUPERSAMPLE;
	if (!archetypon_multiply_size((size_t)state->surface.width,
				      (size_t)state->surface.height,
				      &pixel_count) ||
	    pixel_count > SVG_MAX_SURFACE_PIXELS) {
		archetypon_set_error(error, error_capacity,
				     "SVG render surface exceeds %d pixels",
				     SVG_MAX_SURFACE_PIXELS);
		return -1;
	}
	pixel_count *= 4;
	state->surface.pixels = calloc(pixel_count, 1);
	if (!state->surface.pixels) {
		archetypon_set_error(error, error_capacity,
				     "out of memory creating canvas");
		return -1;
	}
	initialize_viewport(state, geometry);
	return 0;
}

static bool append_scene_command(struct archetypon_svg_document *,
				 enum scene_command_kind, size_t,
				 const struct context *, char *, size_t);

static int close_render_tag(struct render_state *state, struct slice name)
{
	if (state->depth <= 1) {
		archetypon_set_error(state->error, state->error_capacity,
				     "SVG contains an unexpected closing tag");
		return -1;
	}
	if (!slice_same(name, state->stack[state->depth - 1].name)) {
		archetypon_set_error(state->error, state->error_capacity,
				     "SVG contains mismatched closing tags");
		return -1;
	}
	if (state->document && state->stack[state->depth - 1].isolate &&
	    !append_scene_command(state->document, SCENE_POP, 0, NULL,
				  state->error, state->error_capacity))
		return -1;
	state->depth--;
	if (state->depth == 1)
		state->root_closed = true;
	return 0;
}

static const struct svg_effect *find_effect(const struct render_state *state,
					    struct slice id, bool mask)
{
	if (!id.begin)
		return NULL;
	for (size_t i = 0;
	     state->resources && i < state->resources->effect_count; i++)
		if (state->resources->effects[i].mask == mask &&
		    slice_same(id, state->resources->effects[i].id))
			return &state->resources->effects[i];
	return NULL;
}

static bool make_context_css(const struct tag *, struct slice,
			     const struct context *, struct context *,
			     const struct archetypon_svg_document *, char *,
			     size_t);

static void apply_mask_region(const struct render_state *state,
			      const struct svg_effect *effect,
			      const struct context *context,
			      const struct archetypon_image *source,
			      struct archetypon_image *mask)
{
	double left = INFINITY;
	double top = INFINITY;
	double right = -INFINITY;
	double bottom = -INFINITY;
	s32 x;
	s32 y;

	if (!effect->mask)
		return;
	if (effect->region_object_bbox) {
		for (y = 0; y < source->height; y++) {
			for (x = 0; x < source->width; x++) {
				if (source->pixels[((size_t)y * source->width +
						    x) * 4 +
						   3]) {
					left = fmin(left, x);
					top = fmin(top, y);
					right = fmax(right, x + 1);
					bottom = fmax(bottom, y + 1);
				}
			}
		}
		if (!isfinite(left)) {
			memset(mask->pixels, 0,
			       (size_t)mask->width * mask->height * 4);
			return;
		}
		{
			double width = right - left;
			double height = bottom - top;

			right = left +
				(effect->region_x + effect->region_width) *
					width;
			bottom = top +
				 (effect->region_y + effect->region_height) *
					 height;
			left += effect->region_x * width;
			top += effect->region_y * height;
		}
	} else {
		double x_value =
			effect->region_x_percent
				? state->resources->geometry.view_x +
					  effect->region_x *
						  state->resources->geometry
							  .view_width
				: effect->region_x;
		double y_value =
			effect->region_y_percent
				? state->resources->geometry.view_y +
					  effect->region_y *
						  state->resources->geometry
							  .view_height
				: effect->region_y;
		double width =
			effect->region_width_percent
				? effect->region_width *
					  state->resources->geometry.view_width
				: effect->region_width;
		double height =
			effect->region_height_percent
				? effect->region_height *
					  state->resources->geometry.view_height
				: effect->region_height;
		struct point first =
			matrix_point(context->matrix, x_value, y_value);
		struct point second = matrix_point(
			context->matrix, x_value + width, y_value + height);

		left = fmin(first.x, second.x);
		top = fmin(first.y, second.y);
		right = fmax(first.x, second.x);
		bottom = fmax(first.y, second.y);
	}
	for (y = 0; y < mask->height; y++) {
		for (x = 0; x < mask->width; x++) {
			if (x + 0.5 < left || x + 0.5 >= right ||
			    y + 0.5 < top || y + 0.5 >= bottom)
				memset(mask->pixels + ((size_t)y * mask->width +
						       x) * 4,
				       0, 4);
		}
	}
}

static bool render_effect_surface(struct render_state *state,
				  const struct svg_effect *effect,
				  const struct context *shape_context,
				  const struct archetypon_image *source_layer,
				  struct archetypon_image *mask)
{
	size_t bytes = (size_t)state->surface.width * state->surface.height * 4;
	struct context parent = {0};
	struct context container;
	size_t index;

	mask->width = state->surface.width;
	mask->height = state->surface.height;
	mask->pixels = calloc(bytes, 1);
	if (!mask->pixels) {
		archetypon_set_error(state->error, state->error_capacity,
				     "out of memory rendering SVG clip/mask");
		return false;
	}
	parent.matrix = shape_context->matrix;
	parent.style = style_default();
	parent.own_opacity = 1;
	parent.render = true;
	container = effect->context;
	container.matrix = matrix_multiply(parent.matrix, container.matrix);
	if (!container.render || container.style.hidden)
		return true;
	for (index = 0; index < effect->shape_count; index++) {
		const struct effect_shape *item = &effect->shapes[index];
		struct context context;
		struct path path;

		context = item->context;
		context.matrix =
			matrix_multiply(shape_context->matrix, context.matrix);
		if (!context.render || context.style.hidden)
			continue;
		if (effect->mask && context.style.fill_url.begin) {
			archetypon_set_error(state->error,
					     state->error_capacity,
					     "paint servers inside SVG masks "
					     "are not supported");
			goto out_error;
		}
		if (!build_shape_path(&item->tag, item->name, context.matrix,
				      &path, state->error,
				      state->error_capacity))
			goto out_error;
		if (!effect->mask) {
			context.style.fill = color_rgba(255, 255, 255, 255);
			context.style.fill.none = false;
			context.style.stroke.none = true;
			context.style.opacity = 1;
			context.style.fill_opacity = 1;
			context.style.fill_rule_evenodd =
				context.style.clip_rule_evenodd;
		}
		if (!draw_path(mask, &path, &context.style, context.matrix,
			       &state->work_remaining, state->error,
			       state->error_capacity)) {
			path_free(&path);
			goto out_error;
		}
		path_free(&path);
	}
	apply_mask_region(state, effect, shape_context, source_layer, mask);
	return true;

out_error:
	archetypon_image_free(mask);
	return false;
}

static u32 mask_coverage(const struct archetypon_image *surface,
			 const struct svg_effect *effect, size_t index)
{
	const u8 *pixel;

	if (!surface || !surface->pixels)
		return 255;
	pixel = surface->pixels + index * 4;
	if (effect->luminance)
		return ((u32)pixel[0] * 54 + (u32)pixel[1] * 183 +
			(u32)pixel[2] * 19) /
		       256;
	return pixel[3];
}

static void composite_effect_layer(struct archetypon_image *target,
				   const struct archetypon_image *layer,
				   const struct archetypon_image *clip,
				   const struct archetypon_image *mask,
				   const struct svg_effect *mask_effect,
				   double opacity)
{
	size_t pixels = (size_t)target->width * target->height;
	size_t index;

	for (index = 0; index < pixels; index++) {
		u32 clip_coverage = clip && clip->pixels
					    ? clip->pixels[index * 4 + 3]
					    : 255;
		u32 coverage = (clip_coverage * mask_coverage(mask, mask_effect,
							      index) +
				127) /
			       255;
		coverage = (u32)lround(coverage * opacity);
		u8 *destination;
		const u8 *source;
		u32 alpha;
		u32 inverse;

		if (!coverage)
			continue;
		destination = target->pixels + index * 4;
		source = layer->pixels + index * 4;
		alpha = (u32)source[3] * coverage / 255;
		inverse = 255 - alpha;
		destination[0] = (u8)(((u32)source[0] * coverage +
				       (u32)destination[0] * inverse + 127) /
				      255);
		destination[1] = (u8)(((u32)source[1] * coverage +
				       (u32)destination[1] * inverse + 127) /
				      255);
		destination[2] = (u8)(((u32)source[2] * coverage +
				       (u32)destination[2] * inverse + 127) /
				      255);
		destination[3] = (u8)(alpha + ((u32)destination[3] * inverse +
					       127) / 255);
	}
}

static bool append_scene_command(struct archetypon_svg_document *document,
				 enum scene_command_kind kind, size_t shape,
				 const struct context *context, char *error,
				 size_t error_capacity)
{
	struct scene_command command = {0};

	if (!scene_reserve(document, (void **)&document->commands,
			   sizeof(*document->commands), document->command_count,
			   &document->command_capacity, 32, error,
			   error_capacity))
		return false;
	command.kind = kind;
	command.shape = shape;
	if (context)
		command.context = *context;
	document->commands[document->command_count++] = command;
	return true;
}

static int render_shape(struct render_state *state, const struct tag *tag,
			struct slice name, const struct context *context)
{
	struct path path;
	struct context local_context;
	struct archetypon_image original = {0};
	struct archetypon_image layer = {0};
	struct archetypon_image clip_surface = {0};
	struct archetypon_image mask_surface = {0};
	const struct svg_effect *clip_effect = NULL;
	const struct svg_effect *mask_effect = NULL;
	size_t effect_bytes = 0;
	int status = -1;

	if (state->document) {
		if (!scene_reserve(state->document,
				   (void **)&state->document->shapes,
				   sizeof(*state->document->shapes),
				   state->document->shape_count,
				   &state->document->shape_capacity, 32,
				   state->error, state->error_capacity))
			return -1;
		state->document->shapes[state->document->shape_count] =
			(struct compiled_shape){*tag, name, *context};
		if (!append_scene_command(state->document, SCENE_DRAW,
					  state->document->shape_count, NULL,
					  state->error, state->error_capacity))
			return -1;
		state->document->shape_count++;
		return 0;
	}

	local_context = *context;
	context = &local_context;
	if (!build_shape_path(tag, name, context->matrix, &path, state->error,
			      state->error_capacity))
		goto out_free_path;
	if (context->style.fill_url.begin) {
		const struct linear_gradient *gradient = NULL;
		struct style *style = &local_context.style;
		size_t i;
		for (i = 0;
		     state->resources && i < state->resources->gradient_count;
		     i++)
			if (slice_same(style->fill_url,
				       state->resources->gradients[i].id)) {
				gradient = &state->resources->gradients[i];
				break;
			}
		if (!gradient) {
			archetypon_set_error(
				state->error, state->error_capacity,
				"paint servers are not supported for missing "
				"SVG gradient references");
			goto out_free_path;
		}
		if (gradient->stop_count == 0) {
			archetypon_set_error(state->error,
					     state->error_capacity,
					     "SVG gradient has no stops");
			goto out_free_path;
		}
		style->gradient = gradient;
		if (gradient->user_space) {
			double x1 =
				gradient->x1_percent
					? state->resources->geometry.view_x +
						  gradient->x1 *
							  state->resources
								  ->geometry
								  .view_width
					: gradient->x1;
			double y1 =
				gradient->y1_percent
					? state->resources->geometry.view_y +
						  gradient->y1 *
							  state->resources
								  ->geometry
								  .view_height
					: gradient->y1;
			double x2 =
				gradient->x2_percent
					? state->resources->geometry.view_x +
						  gradient->x2 *
							  state->resources
								  ->geometry
								  .view_width
					: gradient->x2;
			double y2 =
				gradient->y2_percent
					? state->resources->geometry.view_y +
						  gradient->y2 *
							  state->resources
								  ->geometry
								  .view_height
					: gradient->y2;
			struct matrix gradient_to_device = matrix_multiply(
				context->matrix, gradient->transform);

			style->gradient_start = (struct point){x1, y1};
			style->gradient_end = (struct point){x2, y2};
			if (!matrix_inverse(gradient_to_device,
					    &style->gradient_inverse)) {
				style->fill.none = true;
				style->gradient = NULL;
			}
		} else {
			struct path bounds_path;
			double min_x = INFINITY;
			double min_y = INFINITY;
			double max_x = -INFINITY;
			double max_y = -INFINITY;
			struct matrix box;
			struct matrix gradient_to_device;

			if (!build_shape_path(tag, name, matrix_identity(),
					      &bounds_path, state->error,
					      state->error_capacity))
				goto out_free_path;
			for (i = 0; i < bounds_path.point_count; i++) {
				min_x = fmin(min_x, bounds_path.points[i].x);
				min_y = fmin(min_y, bounds_path.points[i].y);
				max_x = fmax(max_x, bounds_path.points[i].x);
				max_y = fmax(max_y, bounds_path.points[i].y);
			}
			path_free(&bounds_path);
			box = (struct matrix){max_x - min_x, 0,	    0,
					      max_y - min_y, min_x, min_y};
			gradient_to_device = matrix_multiply(
				context->matrix,
				matrix_multiply(box, gradient->transform));
			style->gradient_start =
				(struct point){gradient->x1, gradient->y1};
			style->gradient_end =
				(struct point){gradient->x2, gradient->y2};
			if (!matrix_inverse(gradient_to_device,
					    &style->gradient_inverse)) {
				style->fill.none = true;
				style->gradient = NULL;
			}
		}
	}
	if (path.point_count > SVG_MAX_TOTAL_POINTS - state->total_points) {
		archetypon_set_error(state->error, state->error_capacity,
				     "SVG exceeds the total point limit");
		goto out_free_path;
	}
	state->total_points += path.point_count;
	clip_effect = find_effect(state, context->style.clip_url, false);
	mask_effect = find_effect(state, context->style.mask_url, true);
	if ((context->style.clip_url.begin && !clip_effect) ||
	    (context->style.mask_url.begin && !mask_effect)) {
		archetypon_set_error(state->error, state->error_capacity,
				     "SVG clip/mask references a missing id");
		goto out_free_path;
	}
	if (clip_effect || mask_effect) {
		size_t bytes = (size_t)state->surface.width *
			       state->surface.height * 4;
		size_t temporary_count =
			1 + (clip_effect != NULL) + (mask_effect != NULL);

		if (bytes > (size_t)SVG_MAX_SURFACE_PIXELS * 4 ||
		    temporary_count > SIZE_MAX / bytes ||
		    bytes * temporary_count >
			    (size_t)SVG_MAX_SURFACE_PIXELS * 16 -
				    state->temporary_bytes) {
			archetypon_set_error(state->error,
					     state->error_capacity,
					     "SVG clip/mask temporary surfaces "
					     "exceed memory limit");
			goto out_free_path;
		}
		effect_bytes = bytes * temporary_count;
		state->temporary_bytes += effect_bytes;
		original = state->surface;
		layer.width = original.width;
		layer.height = original.height;
		layer.pixels = calloc(bytes, 1);
		if (!layer.pixels) {
			archetypon_set_error(
				state->error, state->error_capacity,
				"out of memory rendering SVG clip/mask layer");
			goto out_restore;
		}
		state->surface = layer;
	}
	if (!draw_path(&state->surface, &path, &context->style, context->matrix,
		       &state->work_remaining, state->error,
		       state->error_capacity))
		goto out_restore;
	if (clip_effect || mask_effect) {
		size_t pixels = (size_t)original.width * original.height;

		state->surface = original;
		if ((clip_effect &&
		     !render_effect_surface(state, clip_effect, context, &layer,
					    &clip_surface)) ||
		    (mask_effect &&
		     !render_effect_surface(state, mask_effect, context, &layer,
					    &mask_surface)) ||
		    !consume_render_work(&state->work_remaining, pixels, 1,
					 state->error, state->error_capacity))
			goto out_restore;
		composite_effect_layer(&original, &layer, &clip_surface,
				       &mask_surface, mask_effect, 1);
	}
	status = 0;
out_restore:
	if (original.pixels)
		state->surface = original;
	archetypon_image_free(&layer);
	archetypon_image_free(&clip_surface);
	archetypon_image_free(&mask_surface);
	state->temporary_bytes -= effect_bytes;

out_free_path:
	path_free(&path);
	return status;
}

static int render_element(struct render_state *state, const struct tag *tag,
			  struct slice name, struct context *context)
{
	static const char nested_svg[] =
		"nested <svg> elements are not supported";
	static const char unsupported_element[] =
		"SVG <%.*s> is not supported by the minimal renderer";
	bool shape = tag_is_shape(name);
	bool svg = slice_equal(name, "svg");

	if (svg && state->found_svg) {
		archetypon_set_error(state->error, state->error_capacity,
				     nested_svg);
		return -1;
	}
	if (svg) {
		state->found_svg = true;
		return 0;
	}
	if (tag_is_unsupported(name)) {
		archetypon_set_error(state->error, state->error_capacity,
				     unsupported_element,
				     (s32)(name.end - name.begin), name.begin);
		return -1;
	}
	if (slice_equal(name, "defs") || slice_equal(name, "metadata") ||
	    slice_equal(name, "title") || slice_equal(name, "desc") ||
	    slice_equal(name, "linearGradient") || slice_equal(name, "stop") ||
	    slice_equal(name, "clipPath") || slice_equal(name, "mask") ||
	    slice_equal(name, "style")) {
		context->render = false;
		return 0;
	}
	if (shape && context->render && !context->style.hidden)
		return render_shape(state, tag, name, context);
	if (shape || slice_equal(name, "g") || slice_equal(name, "a") ||
	    !context->render)
		return 0;
	if (tag->name.begin != name.begin) {
		context->render = false;
		return 0;
	}
	archetypon_set_error(state->error, state->error_capacity,
			     "unsupported SVG element <%.*s>",
			     (s32)(name.end - name.begin), name.begin);
	return -1;
}

static bool make_context_css(const struct tag *, struct slice,
			     const struct context *, struct context *,
			     const struct archetypon_svg_document *, char *,
			     size_t);

static int open_render_tag(struct render_state *state, const struct tag *tag,
			   struct slice name)
{
	struct context context;

	if (!state->found_svg && !slice_equal(name, "svg")) {
		archetypon_set_error(state->error, state->error_capacity,
				     "the first SVG element must be <svg>");
		return -1;
	}
	if (state->depth >= SVG_MAX_DEPTH) {
		archetypon_set_error(state->error, state->error_capacity,
				     "SVG nesting exceeds %d elements",
				     SVG_MAX_DEPTH);
		return -1;
	}
	if (!make_context_css(
		    tag, name, &state->stack[state->depth - 1], &context,
		    state->document ? state->document : state->resources,
		    state->error, state->error_capacity))
		return -1;
	context.name = tag->name;
	if (render_element(state, tag, name, &context))
		return -1;
	context.isolate =
		state->document && context.render && !tag->self_closing &&
		(slice_equal(name, "svg") || slice_equal(name, "g") ||
		 slice_equal(name, "a")) &&
		(context.style.clip_url.begin || context.style.mask_url.begin ||
		 context.own_opacity != 1);
	if (context.isolate &&
	    !append_scene_command(state->document, SCENE_PUSH, 0, &context,
				  state->error, state->error_capacity))
		return -1;
	if (!tag->self_closing)
		state->stack[state->depth++] = context;
	else if (slice_equal(name, "svg"))
		state->root_closed = true;
	return 0;
}

static int render_document(struct render_state *state)
{
	static const char after_root[] =
		"SVG contains elements after the root closes";

	while (1) {
		struct tag tag;
		struct slice name;
		s32 next;

		next = next_tag(&state->cursor, state->end, &tag, state->error,
				state->error_capacity);
		if (!next)
			break;
		name = local_name(tag.name);
		if (state->root_closed) {
			archetypon_set_error(state->error,
					     state->error_capacity, after_root);
			return -1;
		}
		if (tag.closing && close_render_tag(state, tag.name))
			return -1;
		if (!tag.closing && open_render_tag(state, &tag, name))
			return -1;
	}
	if (state->error[0])
		return -1;
	if (!state->found_svg) {
		archetypon_set_error(state->error, state->error_capacity,
				     "input contains no <svg> element");
		return -1;
	}
	if (!state->root_closed || state->depth != 1) {
		archetypon_set_error(state->error, state->error_capacity,
				     "SVG contains unclosed elements");
		return -1;
	}
	return 0;
}

static bool gradient_coordinate(const struct tag *tag, const char *name,
				double fallback, bool fallback_percent,
				double *number, bool *percent)
{
	struct slice value;
	if (!attribute_find(tag, name, &value)) {
		*number = fallback;
		*percent = fallback_percent;
		return true;
	}
	if (parse_percentage(value, number)) {
		*number /= 100.0;
		*percent = true;
		return true;
	}
	*percent = false;
	return parse_length(value, number);
}

static bool css_selector_matches(struct slice selector, const struct tag *tag,
				 struct slice name);

static bool apply_stop_declarations_raw(struct slice declarations,
					struct color *color, bool *current,
					struct color *current_color,
					double *opacity)
{
	const char *cursor = declarations.begin;

	while (cursor < declarations.end) {
		struct slice name;
		struct slice value;

		while (cursor < declarations.end &&
		       (isspace((unsigned char)*cursor) || *cursor == ';'))
			cursor++;
		name.begin = cursor;
		while (cursor < declarations.end && *cursor != ':' &&
		       *cursor != ';')
			cursor++;
		name.end = cursor;
		if (cursor == declarations.end || *cursor != ':') {
			while (cursor < declarations.end && *cursor != ';')
				cursor++;
			continue;
		}
		cursor++;
		value.begin = cursor;
		while (cursor < declarations.end && *cursor != ';')
			cursor++;
		value.end = cursor;
		name = slice_trim(name);
		value = slice_trim(value);
		if (slice_equal_ci(name, "color")) {
			if (!parse_color(value, *current_color, current_color))
				return false;
		} else if (slice_equal_ci(name, "stop-color")) {
			if (slice_equal_ci(value, "currentcolor")) {
				*current = true;
			} else {
				if (!parse_color(value, *current_color, color))
					return false;
				*current = false;
			}
		} else if (slice_equal_ci(name, "stop-opacity") &&
			   !parse_opacity(value, opacity)) {
			return false;
		}
	}
	return true;
}

static bool apply_stop_declarations(struct slice declarations,
				    struct color *color, bool *current,
				    struct color *current_color,
				    double *opacity, char *error,
				    size_t error_capacity)
{
	char *storage;
	struct slice clean;
	bool result;

	if (!css_without_comments(declarations, &storage, &clean, error,
				  error_capacity))
		return false;
	result = apply_stop_declarations_raw(clean, color, current,
					     current_color, opacity);
	free(storage);
	return result;
}

static bool parse_stop(struct archetypon_svg_document *document,
		       const struct tag *tag, struct linear_gradient *gradient,
		       struct color inherited_color, char *error,
		       size_t capacity)
{
	struct slice value;
	struct slice name = local_name(tag->name);
	struct color color = color_rgba(0, 0, 0, 255);
	struct color current_color = inherited_color;
	double offset = 0;
	double opacity = 1;
	bool current = false;
	s32 specificity;
	size_t index;

	if (gradient->stop_count == SVG_MAX_STOPS) {
		archetypon_set_error(error, capacity,
				     "SVG gradient exceeds stop limit");
		return false;
	}
	if (!scene_reserve(document, (void **)&gradient->stops,
			   sizeof(*gradient->stops), gradient->stop_count,
			   &gradient->stop_capacity, 4, error, capacity))
		return false;
	if (attribute_find(tag, "offset", &value)) {
		if (parse_percentage(value, &offset))
			offset /= 100.0;
		else if (!parse_opacity(value, &offset))
			goto invalid;
	}
	if (attribute_find(tag, "color", &value) &&
	    !parse_color(value, current_color, &current_color))
		goto invalid;
	if (attribute_find(tag, "stop-color", &value)) {
		if (slice_equal_ci(slice_trim(value), "currentcolor"))
			current = true;
		else if (!parse_color(value, current_color, &color))
			goto invalid;
	}
	if (attribute_find(tag, "stop-opacity", &value) &&
	    !parse_opacity(value, &opacity))
		goto invalid;
	for (specificity = 1; specificity <= 100; specificity *= 10) {
		for (index = 0; index < document->css_rule_count; index++) {
			const struct css_rule *rule =
				&document->css_rules[index];

			if (rule->specificity == specificity &&
			    css_selector_matches(rule->selector, tag, name) &&
			    !apply_stop_declarations(rule->declarations, &color,
						     &current, &current_color,
						     &opacity, error, capacity))
				goto invalid;
		}
	}
	if (attribute_find(tag, "style", &value) &&
	    !apply_stop_declarations(value, &color, &current, &current_color,
				     &opacity, error, capacity))
		goto invalid;
	if (current)
		color = current_color;
	if (offset < 0)
		offset = 0;
	if (offset > 1)
		offset = 1;
	if (gradient->stop_count &&
	    offset < gradient->stops[gradient->stop_count - 1].offset)
		offset = gradient->stops[gradient->stop_count - 1].offset;
	color.a = (u8)lround(color.a * opacity);
	gradient->stops[gradient->stop_count++] =
		(struct gradient_stop){offset, color};
	return true;

invalid:
	archetypon_set_error(error, capacity, "invalid SVG gradient stop");
	return false;
}

static bool resolve_gradient(struct archetypon_svg_document *document,
			     size_t index, char *error, size_t capacity)
{
	struct linear_gradient *gradient = &document->gradients[index];
	struct linear_gradient *base;
	struct slice reference;
	size_t base_index;

	if (gradient->resolve_state == 2)
		return true;
	if (gradient->resolve_state == 1) {
		archetypon_set_error(error, capacity,
				     "SVG gradient reference cycle");
		return false;
	}
	gradient->resolve_state = 1;
	if (!gradient->href.begin) {
		gradient->resolve_state = 2;
		return true;
	}
	reference = slice_trim(gradient->href);
	if (reference.begin < reference.end && *reference.begin == '#')
		reference.begin++;
	for (base_index = 0; base_index < document->gradient_count;
	     base_index++)
		if (slice_same(reference, document->gradients[base_index].id))
			break;
	if (base_index == document->gradient_count) {
		archetypon_set_error(error, capacity,
				     "SVG gradient references a missing id");
		return false;
	}
	if (!resolve_gradient(document, base_index, error, capacity))
		return false;
	base = &document->gradients[base_index];
	if (!gradient->has_x1) {
		gradient->x1 = base->x1;
		gradient->x1_percent = base->x1_percent;
	}
	if (!gradient->has_y1) {
		gradient->y1 = base->y1;
		gradient->y1_percent = base->y1_percent;
	}
	if (!gradient->has_x2) {
		gradient->x2 = base->x2;
		gradient->x2_percent = base->x2_percent;
	}
	if (!gradient->has_y2) {
		gradient->y2 = base->y2;
		gradient->y2_percent = base->y2_percent;
	}
	if (!gradient->has_units)
		gradient->user_space = base->user_space;
	if (!gradient->has_transform)
		gradient->transform = base->transform;
	if (gradient->stop_count == 0) {
		while (gradient->stop_capacity < base->stop_count) {
			if (!scene_reserve(document, (void **)&gradient->stops,
					   sizeof(*gradient->stops),
					   gradient->stop_capacity,
					   &gradient->stop_capacity, 4, error,
					   capacity))
				return false;
		}
		gradient->stop_count = base->stop_count;
		memcpy(gradient->stops, base->stops,
		       base->stop_count * sizeof(*gradient->stops));
	}
	gradient->resolve_state = 2;
	return true;
}

static bool compile_gradients(struct archetypon_svg_document *document,
			      char *error, size_t capacity)
{
	const char *cursor = document->source, *end = cursor + document->length;
	struct context stack[SVG_MAX_DEPTH];
	struct linear_gradient *active = NULL;
	s32 gradient_depth = 0, depth = 0;

	memset(stack, 0, sizeof(stack));
	stack[0].matrix = matrix_identity();
	stack[0].style = style_default();
	stack[0].own_opacity = 1;
	stack[0].render = true;
	while (1) {
		struct tag tag;
		struct slice name;
		struct context context;
		int next = next_tag(&cursor, end, &tag, error, capacity);
		if (!next)
			break;
		name = local_name(tag.name);
		if (tag.closing) {
			if (active && depth == gradient_depth)
				active = NULL;
			depth--;
			continue;
		}
		if (depth + 1 >= SVG_MAX_DEPTH ||
		    !make_context_css(&tag, name, &stack[depth], &context,
				      document, error, capacity))
			return false;
		depth++;
		if (slice_equal(name, "linearGradient")) {
			struct slice id, value;
			if (!attribute_find(&tag, "id", &id) ||
			    id.begin == id.end) {
				archetypon_set_error(
					error, capacity,
					"SVG linearGradient is missing id");
				return false;
			}
			if (document->gradient_count == SVG_MAX_GRADIENTS) {
				archetypon_set_error(
					error, capacity,
					"SVG exceeds gradient limit");
				return false;
			}
			if (!scene_reserve(document,
					   (void **)&document->gradients,
					   sizeof(*document->gradients),
					   document->gradient_count,
					   &document->gradient_capacity, 4,
					   error, capacity))
				return false;
			active = &document->gradients
					  [document->gradient_count++];
			memset(active, 0, sizeof(*active));
			active->id = id;
			active->x2 = 1;
			active->x2_percent = true;
			active->transform = matrix_identity();
			active->current_color = context.style.current_color;
			gradient_depth = depth;
			active->has_x1 = attribute_find(&tag, "x1", &value);
			active->has_y1 = attribute_find(&tag, "y1", &value);
			active->has_x2 = attribute_find(&tag, "x2", &value);
			active->has_y2 = attribute_find(&tag, "y2", &value);
			if (!gradient_coordinate(&tag, "x1", 0, true,
						 &active->x1,
						 &active->x1_percent) ||
			    !gradient_coordinate(&tag, "y1", 0, true,
						 &active->y1,
						 &active->y1_percent) ||
			    !gradient_coordinate(&tag, "x2", 1, true,
						 &active->x2,
						 &active->x2_percent) ||
			    !gradient_coordinate(&tag, "y2", 0, true,
						 &active->y2,
						 &active->y2_percent)) {
				archetypon_set_error(
					error, capacity,
					"invalid SVG gradient coordinate");
				return false;
			}
			if (attribute_find(&tag, "gradientUnits", &value)) {
				active->has_units = true;
				if (slice_equal(value, "userSpaceOnUse"))
					active->user_space = true;
				else if (!slice_equal(value,
						      "objectBoundingBox")) {
					archetypon_set_error(error, capacity,
							     "unsupported SVG "
							     "gradientUnits");
					return false;
				}
			}
			if (attribute_find(&tag, "spreadMethod", &value) &&
			    !slice_equal(value, "pad")) {
				archetypon_set_error(error, capacity,
						     "unsupported SVG gradient "
						     "spreadMethod");
				return false;
			}
			if (attribute_find(&tag, "gradientTransform", &value)) {
				active->has_transform = true;
				if (!parse_transform(value, &active->transform,
						     error, capacity))
					return false;
			}
			{
				struct slice href;
				if (attribute_find(&tag, "href", &href) ||
				    attribute_find(&tag, "xlink:href", &href))
					active->href = href;
			}
		} else if (active && slice_equal(name, "stop")) {
			if (!parse_stop(document, &tag, active,
					active->current_color, error, capacity))
				return false;
		}
		if (!tag.self_closing)
			stack[depth] = context;
		if (tag.self_closing) {
			if (active && depth == gradient_depth)
				active = NULL;
			depth--;
		}
	}
	if (error[0])
		return false;
	for (size_t index = 0; index < document->gradient_count; index++)
		if (!resolve_gradient(document, index, error, capacity))
			return false;
	return true;
}

static bool compile_effects(struct archetypon_svg_document *document,
			    char *error, size_t capacity)
{
	const char *cursor = document->source;
	const char *end = cursor + document->length;
	struct context stack[SVG_MAX_DEPTH];
	struct svg_effect *active = NULL;
	s32 depth = 0;
	s32 effect_depth = 0;

	memset(stack, 0, sizeof(stack));
	stack[0].matrix = matrix_identity();
	stack[0].style = style_default();
	stack[0].own_opacity = 1;
	stack[0].render = true;
	while (1) {
		struct tag tag;
		struct slice name;
		struct slice value;
		struct context context;
		int next = next_tag(&cursor, end, &tag, error, capacity);

		if (!next)
			break;
		name = local_name(tag.name);
		if (tag.closing) {
			if (active && depth == effect_depth)
				active = NULL;
			depth--;
			continue;
		}
		if (depth + 1 >= SVG_MAX_DEPTH ||
		    !make_context_css(&tag, name, &stack[depth], &context,
				      document, error, capacity))
			return false;
		context.style.opacity *= stack[depth].style.opacity;
		depth++;
		if (slice_equal(name, "clipPath") ||
		    slice_equal(name, "mask")) {
			struct slice id;

			if (active) {
				archetypon_set_error(error, capacity,
						     "nested SVG clip/mask is "
						     "not supported");
				return false;
			}
			if (!attribute_find(&tag, "id", &id) ||
			    id.begin == id.end) {
				archetypon_set_error(
					error, capacity,
					"SVG clipPath or mask is missing id");
				return false;
			}
			if (document->effect_count == SVG_MAX_EFFECTS ||
			    !scene_reserve(document,
					   (void **)&document->effects,
					   sizeof(*document->effects),
					   document->effect_count,
					   &document->effect_capacity, 4, error,
					   capacity))
				return false;
			active = &document->effects[document->effect_count++];
			memset(active, 0, sizeof(*active));
			active->id = id;
			active->tag = tag;
			active->name = name;
			active->context = context;
			active->mask = slice_equal(name, "mask");
			active->transform = matrix_identity();
			effect_depth = depth;
			if (active->mask) {
				active->luminance = true;
				active->region_object_bbox = true;
				if (attribute_find(&tag, "maskUnits", &value)) {
					if (slice_equal(value,
							"userSpaceOnUse"))
						active->region_object_bbox =
							false;
					else if (!slice_equal(
							 value,
							 "objectBoundingBox")) {
						archetypon_set_error(
							error, capacity,
							"unsupported SVG "
							"maskUnits");
						return false;
					}
				}
				if (!gradient_coordinate(
					    &tag, "x", -0.1, true,
					    &active->region_x,
					    &active->region_x_percent) ||
				    !gradient_coordinate(
					    &tag, "y", -0.1, true,
					    &active->region_y,
					    &active->region_y_percent) ||
				    !gradient_coordinate(
					    &tag, "width", 1.2, true,
					    &active->region_width,
					    &active->region_width_percent) ||
				    !gradient_coordinate(
					    &tag, "height", 1.2, true,
					    &active->region_height,
					    &active->region_height_percent) ||
				    active->region_width <= 0 ||
				    active->region_height <= 0) {
					archetypon_set_error(
						error, capacity,
						"invalid SVG mask region");
					return false;
				}
				if (attribute_find(&tag, "mask-type", &value)) {
					if (slice_equal(value, "alpha"))
						active->luminance = false;
					else if (!slice_equal(value,
							      "luminance")) {
						archetypon_set_error(
							error, capacity,
							"unsupported SVG "
							"mask-type");
						return false;
					}
				}
			}
			if (attribute_find(&tag,
					   active->mask ? "maskContentUnits"
							: "clipPathUnits",
					   &value) &&
			    !slice_equal(value, "userSpaceOnUse")) {
				archetypon_set_error(
					error, capacity,
					"unsupported objectBoundingBox "
					"clip/mask content");
				return false;
			}
		} else if (active && tag_is_shape(name)) {
			if (active->shape_count == SVG_MAX_EFFECT_SHAPES ||
			    !scene_reserve(document, (void **)&active->shapes,
					   sizeof(*active->shapes),
					   active->shape_count,
					   &active->shape_capacity, 4, error,
					   capacity))
				return false;
			active->shapes[active->shape_count++] =
				(struct effect_shape){tag, name, context};
		} else if (active && !slice_equal(name, "g") &&
			   !slice_equal(name, "title") &&
			   !slice_equal(name, "desc")) {
			archetypon_set_error(
				error, capacity,
				"unsupported element in SVG clip/mask");
			return false;
		}
		if (!tag.self_closing)
			stack[depth] = context;
		else {
			if (active && depth == effect_depth)
				active = NULL;
			depth--;
		}
	}
	return error[0] == 0;
}

static bool css_add_rules(struct archetypon_svg_document *document,
			  struct slice selectors, struct slice declarations,
			  char *error, size_t capacity)
{
	const char *p = selectors.begin;
	while (p < selectors.end) {
		struct slice q;
		q.begin = p;
		while (p < selectors.end && *p != ',')
			p++;
		q.end = p;
		q = slice_trim(q);
		if (p < selectors.end)
			p++;
		if (q.begin == q.end)
			continue;
		s32 spec = 1;
		if (*q.begin == '#')
			spec = 100;
		else if (*q.begin == '.')
			spec = 10;
		for (const char *x = q.begin + (spec > 1); x < q.end; x++)
			if (!(isalnum((unsigned char)*x) || *x == '_' ||
			      *x == '-')) {
				archetypon_set_error(
					error, capacity,
					"unsupported SVG CSS selector");
				return false;
			}
		if (document->css_rule_count == SVG_MAX_CSS_RULES) {
			archetypon_set_error(error, capacity,
					     "SVG exceeds CSS rule limit");
			return false;
		}
		if (!scene_reserve(document, (void **)&document->css_rules,
				   sizeof(*document->css_rules),
				   document->css_rule_count,
				   &document->css_rule_capacity, 8, error,
				   capacity))
			return false;
		document->css_rules[document->css_rule_count++] =
			(struct css_rule){q, declarations, spec};
	}
	return true;
}
enum css_media_policy {
	CSS_MEDIA_UNSUPPORTED,
	CSS_MEDIA_APPLY,
	CSS_MEDIA_SKIP
};

static enum css_media_policy css_media_policy(struct slice query)
{
	char normalized[64];
	size_t length = 0;
	const char *cursor;

	for (cursor = query.begin; cursor < query.end; cursor++) {
		if (isspace((unsigned char)*cursor))
			continue;
		if (length + 1 == sizeof(normalized))
			return CSS_MEDIA_UNSUPPORTED;
		normalized[length++] = (char)tolower((unsigned char)*cursor);
	}
	normalized[length] = 0;
	if (strcmp(normalized, "all") == 0 ||
	    strcmp(normalized, "screen") == 0 ||
	    strcmp(normalized, "(prefers-color-scheme:light)") == 0)
		return CSS_MEDIA_APPLY;
	if (strcmp(normalized, "print") == 0 ||
	    strcmp(normalized, "(prefers-color-scheme:dark)") == 0)
		return CSS_MEDIA_SKIP;
	return CSS_MEDIA_UNSUPPORTED;
}

static bool parse_css_range(struct archetypon_svg_document *document,
			    const char *p, const char *end, char *error,
			    size_t capacity)
{
	while (p < end) {
		while (p < end && isspace((unsigned char)*p))
			p++;
		if (p == end)
			break;
		if (end - p >= 2 && p[0] == '/' && p[1] == '*') {
			const char *q = p + 2;
			while (end - q >= 2 && !(q[0] == '*' && q[1] == '/'))
				q++;
			if (end - q < 2) {
				archetypon_set_error(
					error, capacity,
					"unterminated SVG CSS comment");
				return false;
			}
			p = q + 2;
			continue;
		}
		struct slice head = {p, p};
		while (head.end < end && *head.end != '{')
			head.end++;
		if (head.end == end) {
			if (slice_trim(head).begin != slice_trim(head).end) {
				archetypon_set_error(error, capacity,
						     "malformed SVG CSS");
				return false;
			}
			break;
		}
		const char *body = head.end + 1, *q = body;
		int depth = 1;
		while (q < end && depth) {
			if (*q == '{')
				depth++;
			else if (*q == '}')
				depth--;
			q++;
		}
		if (depth) {
			archetypon_set_error(error, capacity,
					     "unterminated SVG CSS rule");
			return false;
		}
		struct slice trimmed = slice_trim(head);
		if (trimmed.begin < trimmed.end && *trimmed.begin == '@') {
			if ((size_t)(trimmed.end - trimmed.begin) >= 6 &&
			    memcmp(trimmed.begin, "@media", 6) == 0) {
				struct slice query = {trimmed.begin + 6,
						      trimmed.end};
				enum css_media_policy policy =
					css_media_policy(query);

				if (policy == CSS_MEDIA_UNSUPPORTED) {
					archetypon_set_error(error, capacity,
							     "unsupported SVG "
							     "CSS media query");
					return false;
				}
				if (policy == CSS_MEDIA_APPLY &&
				    !parse_css_range(document, body, q - 1,
						     error, capacity))
					return false;
			} else {
				archetypon_set_error(
					error, capacity,
					"unsupported SVG CSS at-rule");
				return false;
			}
		} else if (!css_add_rules(document, trimmed,
					  (struct slice){body, q - 1}, error,
					  capacity))
			return false;
		p = q;
	}
	return true;
}
static bool compile_css(struct archetypon_svg_document *document, char *error,
			size_t capacity)
{
	const char *cursor = document->source;
	const char *end = cursor + document->length;
	struct tag tag;

	while (next_tag(&cursor, end, &tag, error, capacity)) {
		struct slice name = local_name(tag.name);

		if (!tag.closing && !tag.self_closing &&
		    slice_equal(name, "style")) {
			const char *content = cursor;
			struct tag closing;

			if (!next_tag(&cursor, end, &closing, error,
				      capacity) ||
			    !closing.closing ||
			    !slice_equal(local_name(closing.name), "style")) {
				archetypon_set_error(
					error, capacity,
					"unterminated SVG style element");
				return false;
			}
			if (!parse_css_range(document, content,
					     closing.name.begin - 2, error,
					     capacity))
				return false;
		}
	}
	return error[0] == 0;
}
static bool css_selector_matches(struct slice selector, const struct tag *tag,
				 struct slice name)
{
	struct slice value;
	if (*selector.begin == '#')
		return attribute_find(tag, "id", &value) &&
		       slice_same(
			       (struct slice){selector.begin + 1, selector.end},
			       value);
	if (*selector.begin == '.') {
		if (!attribute_find(tag, "class", &value))
			return false;
		const char *p = value.begin;
		while (p < value.end) {
			while (p < value.end && isspace((unsigned char)*p))
				p++;
			struct slice token = {p, p};
			while (token.end < value.end &&
			       !isspace((unsigned char)*token.end))
				token.end++;
			p = token.end;
			if (slice_same((struct slice){selector.begin + 1,
						      selector.end},
				       token))
				return true;
		}
		return false;
	}
	return slice_same(selector, name);
}
static bool make_context_css(const struct tag *tag, struct slice name,
			     const struct context *parent,
			     struct context *context,
			     const struct archetypon_svg_document *document,
			     char *error, size_t capacity)
{
	struct style style = parent->style;
	struct slice value;

	style.clip_url = (struct slice){0};
	style.mask_url = (struct slice){0};
	style.opacity = 1;
	struct matrix local = matrix_identity();
	double own_opacity = 1;
	s32 specificity;
	size_t index;

	if (!apply_presentation_attributes(tag, &style, &own_opacity, error,
					   capacity))
		return false;
	for (specificity = 1; specificity <= 100; specificity *= 10) {
		for (index = 0; document && index < document->css_rule_count;
		     index++) {
			const struct css_rule *rule =
				&document->css_rules[index];

			if (rule->specificity == specificity &&
			    css_selector_matches(rule->selector, tag, name) &&
			    !apply_style_attribute(rule->declarations, &style,
						   &own_opacity, error,
						   capacity))
				return false;
		}
	}
	if (attribute_find(tag, "style", &value) &&
	    !apply_style_attribute(value, &style, &own_opacity, error,
				   capacity))
		return false;
	resolve_current_color(&style);
	style.opacity = own_opacity;
	*context = *parent;
	context->style = style;
	context->own_opacity = own_opacity;
	if (attribute_find(tag, "transform", &value) &&
	    !parse_transform(value, &local, error, capacity))
		return false;
	context->matrix = matrix_multiply(parent->matrix, local);
	if (context->style.display_none)
		context->render = false;
	return true;
}

static int compile_document_scene(struct archetypon_svg_document *document,
				  char *error, size_t error_capacity)
{
	struct render_state state;

	memset(&state, 0, sizeof(state));
	if (!compile_css(document, error, error_capacity))
		return -1;
	if (!compile_gradients(document, error, error_capacity))
		return -1;
	if (!compile_effects(document, error, error_capacity))
		return -1;
	state.cursor = document->source;
	state.end = document->source + document->length;
	state.error = error;
	state.error_capacity = error_capacity;
	state.depth = 1;
	state.work_remaining = SVG_MAX_RENDER_WORK;
	state.document = document;
	state.stack[0].matrix = matrix_identity();
	state.stack[0].style = style_default();
	state.stack[0].own_opacity = 1;
	state.stack[0].render = true;
	return render_document(&state);
}

static int render_scene_commands(struct render_state *state,
				 const struct archetypon_svg_document *document,
				 struct matrix viewport, size_t *position,
				 bool nested)
{
	while (*position < document->command_count) {
		const struct scene_command *command =
			&document->commands[(*position)++];

		if (command->kind == SCENE_POP)
			return nested ? 0 : -1;
		if (command->kind == SCENE_DRAW) {
			const struct compiled_shape *shape =
				&document->shapes[command->shape];
			struct context context = shape->context;

			context.matrix =
				matrix_multiply(viewport, context.matrix);
			if (render_shape(state, &shape->tag, shape->name,
					 &context))
				return -1;
			continue;
		}
		if (command->kind == SCENE_PUSH) {
			struct context context = command->context;
			struct archetypon_image original = state->surface;
			struct archetypon_image layer = {0};
			struct archetypon_image clip = {0};
			struct archetypon_image mask = {0};
			const struct svg_effect *clip_effect;
			const struct svg_effect *mask_effect;
			size_t bytes =
				(size_t)original.width * original.height * 4;
			size_t temporary_count;
			size_t pixels =
				(size_t)original.width * original.height;
			int status = -1;

			context.matrix =
				matrix_multiply(viewport, context.matrix);
			clip_effect = find_effect(state, context.style.clip_url,
						  false);
			mask_effect = find_effect(state, context.style.mask_url,
						  true);
			if ((context.style.clip_url.begin && !clip_effect) ||
			    (context.style.mask_url.begin && !mask_effect)) {
				archetypon_set_error(state->error,
						     state->error_capacity,
						     "SVG clip/mask references "
						     "a missing id");
				return -1;
			}
			temporary_count = 1 + (clip_effect != NULL) +
					  (mask_effect != NULL);
			if (temporary_count > SIZE_MAX / bytes ||
			    bytes * temporary_count >
				    (size_t)SVG_MAX_SURFACE_PIXELS * 16 -
					    state->temporary_bytes) {
				archetypon_set_error(state->error,
						     state->error_capacity,
						     "SVG nested effects "
						     "exceed memory limit");
				return -1;
			}
			state->temporary_bytes += bytes * temporary_count;
			layer.width = original.width;
			layer.height = original.height;
			layer.pixels = calloc(bytes, 1);
			if (!layer.pixels) {
				archetypon_set_error(
					state->error, state->error_capacity,
					"out of memory rendering SVG group");
				goto out_group;
			}
			state->surface = layer;
			if (render_scene_commands(state, document, viewport,
						  position, true))
				goto out_group;
			state->surface = original;
			if ((clip_effect &&
			     !render_effect_surface(state, clip_effect,
						    &context, &layer, &clip)) ||
			    (mask_effect &&
			     !render_effect_surface(state, mask_effect,
						    &context, &layer, &mask)) ||
			    !consume_render_work(&state->work_remaining, pixels,
						 1, state->error,
						 state->error_capacity))
				goto out_group;
			composite_effect_layer(&original, &layer, &clip, &mask,
					       mask_effect,
					       context.own_opacity);
			status = 0;

		out_group:
			state->surface = original;
			archetypon_image_free(&layer);
			archetypon_image_free(&clip);
			archetypon_image_free(&mask);
			state->temporary_bytes -= bytes * temporary_count;
			if (status)
				return -1;
		}
	}
	if (nested) {
		archetypon_set_error(state->error, state->error_capacity,
				     "SVG scene has an unclosed group");
		return -1;
	}
	return 0;
}

static int render_compiled_scene(struct render_state *state,
				 const struct archetypon_svg_document *document)
{
	size_t position = 0;

	state->resources = document;
	return render_scene_commands(state, document, state->stack[0].matrix,
				     &position, false);
}

static struct channel_sum sample_output_pixel(const struct render_state *state,
					      s32 x, s32 y)
{
	struct channel_sum sum = { 0 };
	s32 sy;

	for (sy = 0; sy < SUPERSAMPLE; sy++) {
		s32 sx;

		for (sx = 0; sx < SUPERSAMPLE; sx++) {
			const u8 *pixel;
			size_t offset;

			offset = (size_t)(y * SUPERSAMPLE + sy) *
				 state->surface.width + x * SUPERSAMPLE + sx;
			pixel = state->surface.pixels + offset * 4;
			sum.red += pixel[0];
			sum.green += pixel[1];
			sum.blue += pixel[2];
			sum.alpha += pixel[3];
		}
	}
	return sum;
}

static void store_output_pixel(u8 *destination, struct channel_sum sum)
{
	u32 red;
	u32 green;
	u32 blue;
	u32 alpha;

	red = (sum.red + 2) / 4;
	green = (sum.green + 2) / 4;
	blue = (sum.blue + 2) / 4;
	alpha = (sum.alpha + 2) / 4;
	if (alpha > 0) {
		red = (red * 255 + alpha / 2) / alpha;
		green = (green * 255 + alpha / 2) / alpha;
		blue = (blue * 255 + alpha / 2) / alpha;
		destination[0] = (u8)(red > 255 ? 255 : red);
		destination[1] = (u8)(green > 255 ? 255 : green);
		destination[2] = (u8)(blue > 255 ? 255 : blue);
	} else {
		destination[0] = 0;
		destination[1] = 0;
		destination[2] = 0;
	}
	destination[3] = (u8)alpha;
}

static int downsample_surface(const struct render_state *state,
			      struct archetypon_image *image, s32 width,
			      s32 height)
{
	size_t pixel_count;
	s32 y;

	image->width = width;
	image->height = height;
	if (!archetypon_multiply_size((size_t)width, (size_t)height,
				      &pixel_count) ||
	    !archetypon_multiply_size(pixel_count, 4, &pixel_count)) {
		archetypon_set_error(state->error, state->error_capacity,
				     "output dimensions are too large");
		return -1;
	}
	image->pixels = malloc(pixel_count);
	if (!image->pixels) {
		archetypon_set_error(state->error, state->error_capacity,
				     "out of memory downsampling SVG");
		return -1;
	}
	for (y = 0; y < height; y++) {
		s32 x;

		for (x = 0; x < width; x++) {
			struct channel_sum sum;
			u8 *destination;

			sum = sample_output_pixel(state, x, y);
			destination = image->pixels +
				      ((size_t)y * width + x) * 4;
			store_output_pixel(destination, sum);
		}
	}
	return 0;
}

static void svg_plan_take_image(struct archetypon_svg_plan *plan,
				struct archetypon_image *image);

int archetypon_svg_render(const char *source, size_t length, s32 output_width,
			  s32 output_height, struct archetypon_image *image,
			  char *error, size_t error_capacity)
{
	struct archetypon_svg_document *document;
	struct archetypon_svg_plan *plan;

	if (!source || !image || !error || error_capacity == 0)
		return -1;
	memset(image, 0, sizeof(*image));
	document = archetypon_svg_document_create(source, length, error,
						  error_capacity);
	if (!document)
		return -1;
	plan = archetypon_svg_plan_create(document, output_width, output_height,
					 error, error_capacity);
	archetypon_svg_document_free(document);
	if (!plan)
		return -1;
	svg_plan_take_image(plan, image);
	archetypon_svg_plan_release(plan);
	return 0;
}

int archetypon_svg_canvas_size(const char *source, size_t length, double *width,
			       double *height, char *error,
			       size_t error_capacity)
{
	struct archetypon_svg_document *document;
	int status;

	if (!source || !width || !height || !error || error_capacity == 0)
		return -1;
	document = archetypon_svg_document_create(source, length, error,
						  error_capacity);
	if (!document)
		return -1;
	status = archetypon_svg_document_canvas_size(document, width, height);
	archetypon_svg_document_free(document);
	return status;
}

struct archetypon_svg_plan {
	atomic_uint references;
	struct archetypon_image image;
	size_t cost;
};

static void svg_plan_take_image(struct archetypon_svg_plan *plan,
				struct archetypon_image *image)
{
	*image = plan->image;
	memset(&plan->image, 0, sizeof(plan->image));
}

static atomic_size_t svg_document_compiles;
static atomic_size_t svg_plan_compiles;

struct archetypon_svg_document *archetypon_svg_document_create(
	const char *source, size_t length, char *error, size_t error_capacity)
{
	struct archetypon_svg_document *document;

	if (!source || !error || error_capacity == 0)
		return NULL;
	error[0] = 0;
	if (length > SVG_MAX_SOURCE_BYTES) {
		archetypon_set_error(error, error_capacity,
				     "SVG source exceeds %d bytes",
				     SVG_MAX_SOURCE_BYTES);
		return NULL;
	}
	if (!validate_svg_document(source, length, error, error_capacity))
		return NULL;
	document = calloc(1, sizeof(*document));
	if (!document) {
		archetypon_set_error(error, error_capacity,
				     "out of memory creating SVG document");
		return NULL;
	}
	document->source = malloc(length ? length : 1);
	if (!document->source) {
		free(document);
		archetypon_set_error(error, error_capacity,
				     "out of memory copying SVG document");
		return NULL;
	}
	memcpy(document->source, source, length);
	document->length = length;
	if (!find_svg_geometry(document->source, length, &document->geometry,
			       error, error_capacity) ||
	    compile_document_scene(document, error, error_capacity)) {
		archetypon_svg_document_free(document);
		return NULL;
	}
	atomic_fetch_add_explicit(&svg_document_compiles, 1,
				  memory_order_relaxed);
	return document;
}

void archetypon_svg_document_free(struct archetypon_svg_document *document)
{
	if (document) {
		size_t index;

		for (index = 0; index < document->gradient_count; index++)
			free(document->gradients[index].stops);
		for (index = 0; index < document->effect_count; index++)
			free(document->effects[index].shapes);
		free(document->shapes);
		free(document->commands);
		free(document->gradients);
		free(document->effects);
		free(document->css_rules);
		free(document->source);
		free(document);
	}
}

int archetypon_svg_document_canvas_size(
	const struct archetypon_svg_document *document, double *width,
	double *height)
{
	if (!document || !width || !height)
		return -1;
	*width = document->geometry.intrinsic_width;
	*height = document->geometry.intrinsic_height;
	return 0;
}

const u8 *archetypon_svg_document_source(
	const struct archetypon_svg_document *document)
{
	return document ? (const u8 *)document->source : NULL;
}

size_t archetypon_svg_document_source_length(
	const struct archetypon_svg_document *document)
{
	return document ? document->length : 0;
}

struct archetypon_svg_plan *archetypon_svg_plan_create(
	const struct archetypon_svg_document *document, s32 output_width,
	s32 output_height, char *error, size_t error_capacity)
{
	struct archetypon_svg_plan *plan;
	struct render_state state;
	int status;

	if (!document || !error || error_capacity == 0)
		return NULL;
	error[0] = 0;
	if (output_width <= 0 || output_height <= 0) {
		archetypon_set_error(error, error_capacity,
				     "invalid output dimensions");
		return NULL;
	}
	if (output_width > SVG_MAX_OUTPUT_DIMENSION ||
	    output_height > SVG_MAX_OUTPUT_DIMENSION) {
		archetypon_set_error(error, error_capacity,
				     "SVG output dimension exceeds %d pixels",
				     SVG_MAX_OUTPUT_DIMENSION);
		return NULL;
	}
	plan = calloc(1, sizeof(*plan));
	if (!plan) {
		archetypon_set_error(error, error_capacity,
				     "out of memory creating SVG plan");
		return NULL;
	}
	if (initialize_render_state_geometry(&state, document->source,
					     document->length,
					     &document->geometry,
					     output_width, output_height,
					     error, error_capacity)) {
		free(plan);
		return NULL;
	}
	status = render_compiled_scene(&state, document);
	if (!status)
		status = downsample_surface(&state, &plan->image, output_width,
					    output_height);
	archetypon_image_free(&state.surface);
	if (status) {
		archetypon_image_free(&plan->image);
		free(plan);
		return NULL;
	}
	atomic_init(&plan->references, 1);
	plan->cost = sizeof(*plan) + (size_t)output_width *
		     (size_t)output_height * 4;
	atomic_fetch_add_explicit(&svg_plan_compiles, 1, memory_order_relaxed);
	return plan;
}

struct archetypon_svg_plan *archetypon_svg_plan_retain(
	struct archetypon_svg_plan *plan)
{
	unsigned previous;

	if (!plan)
		return NULL;
	previous = atomic_fetch_add_explicit(&plan->references, 1,
					     memory_order_relaxed);
	if (previous == UINT_MAX) {
		atomic_fetch_sub_explicit(&plan->references, 1,
					  memory_order_relaxed);
		return NULL;
	}
	return plan;
}

void archetypon_svg_plan_release(struct archetypon_svg_plan *plan)
{
	if (!plan || atomic_fetch_sub_explicit(&plan->references, 1,
					      memory_order_acq_rel) != 1)
		return;
	archetypon_image_free(&plan->image);
	free(plan);
}

const u8 *archetypon_svg_plan_pixels(const struct archetypon_svg_plan *plan)
{
	return plan ? plan->image.pixels : NULL;
}

s32 archetypon_svg_plan_width(const struct archetypon_svg_plan *plan)
{
	return plan ? plan->image.width : 0;
}

s32 archetypon_svg_plan_height(const struct archetypon_svg_plan *plan)
{
	return plan ? plan->image.height : 0;
}

size_t archetypon_svg_plan_cost(const struct archetypon_svg_plan *plan)
{
	return plan ? plan->cost : 0;
}

void archetypon_svg_counters_reset(void)
{
	atomic_store_explicit(&svg_document_compiles, 0, memory_order_relaxed);
	atomic_store_explicit(&svg_plan_compiles, 0, memory_order_relaxed);
}

size_t archetypon_svg_document_compile_count(void)
{
	return atomic_load_explicit(&svg_document_compiles, memory_order_relaxed);
}

size_t archetypon_svg_plan_compile_count(void)
{
	return atomic_load_explicit(&svg_plan_compiles, memory_order_relaxed);
}
