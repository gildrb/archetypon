#include "internal.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>

#define SVG_MAX_DEPTH 128
#define SVG_MAX_ELEMENTS 100000
#define SVG_MAX_ATTRIBUTES 256
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

struct style {
	struct color fill;
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
	struct color *color;

	if (slice_equal_ci(name, "color"))
		color = &style->current_color;
	else if (slice_equal_ci(name, "fill"))
		color = &style->fill;
	else if (slice_equal_ci(name, "stroke"))
		color = &style->stroke;
	else
		return false;
	if (parse_color(value, style->current_color, color))
		return true;
	archetypon_set_error(error, error_capacity,
			     unsupported_paint);
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
	if (slice_equal_ci(name, "fill-rule")) {
		if (slice_equal_ci(slice_trim(value), "evenodd"))
			style->fill_rule_evenodd = true;
		else if (slice_equal_ci(slice_trim(value), "nonzero"))
			style->fill_rule_evenodd = false;
		else
			goto out_unsupported;
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
	if ((slice_equal_ci(name, "clip-path") ||
	     slice_equal_ci(name, "mask") || slice_equal_ci(name, "filter")) &&
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

static bool apply_style_attribute(struct slice value, struct style *style,
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

static bool apply_style(const struct tag *tag, const struct style *parent,
			struct style *style, double *element_opacity,
			char *error, size_t error_capacity)
{
	struct slice value;
	double own_opacity = 1;

	*style = *parent;
	if (!apply_presentation_attributes(tag, style, &own_opacity, error,
					   error_capacity))
		return false;
	if (attribute_find(tag, "style", &value) &&
	    !apply_style_attribute(value, style, &own_opacity, error,
				   error_capacity))
		return false;
	style->opacity = parent->opacity * own_opacity;
	*element_opacity = own_opacity;
	return true;
}

static bool make_context(const struct tag *tag, const struct context *parent,
			 struct context *context, char *error,
			 size_t error_capacity)
{
	struct slice value;
	struct matrix local = matrix_identity();
	*context = *parent;
	if (!apply_style(tag, &parent->style, &context->style,
			 &context->own_opacity, error, error_capacity))
		return false;
	if (attribute_find(tag, "transform", &value) &&
	    !parse_transform(value, &local, error, error_capacity))
		return false;
	context->matrix = matrix_multiply(parent->matrix, local);
	if (context->style.display_none)
		context->render = false;
	return true;
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
			composite_span(surface, row, intersections[i].x,
				       intersections[i + 1].x, style->fill,
				       alpha);
		return;
	}
	for (i = 0; i < count; i++) {
		s32 previous = winding;

		winding += intersections[i].winding;
		if (previous == 0 && winding != 0)
			start = intersections[i].x;
		else if (previous != 0 && winding == 0)
			composite_span(surface, row, start,
				       intersections[i].x, style->fill, alpha);
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
	static const char * const unsupported[] = {
		"text",		  "tspan",	    "image",	     "use",
		"linearGradient", "radialGradient", "filter",	     "mask",
		"clipPath",	  "pattern",	    "foreignObject", "style"
	};
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
	double width = 0;
	double height = 0;
	bool has_width = attribute_find(tag, "width", &value);
	bool has_height;
	bool has_viewbox;

	memset(geometry, 0, sizeof(*geometry));
	geometry->align_x = ASPECT_MID;
	geometry->align_y = ASPECT_MID;
	if (has_width && (!parse_length(value, &width) || width <= 0)) {
		archetypon_set_error(error, error_capacity,
				     "invalid SVG width");
		return -1;
	}
	has_height = attribute_find(tag, "height", &value);
	if (has_height && (!parse_length(value, &height) || height <= 0)) {
		archetypon_set_error(error, error_capacity,
				     "invalid SVG height");
		return -1;
	}
	has_viewbox = attribute_find(tag, "viewBox", &value);
	if (has_viewbox) {
		double numbers[4];

		if (!parse_number_list(value, numbers, 4) || numbers[2] <= 0 ||
		    numbers[3] <= 0) {
			archetypon_set_error(error, error_capacity,
					     "invalid SVG viewBox");
			return -1;
		}
		geometry->view_x = numbers[0];
		geometry->view_y = numbers[1];
		geometry->view_width = numbers[2];
		geometry->view_height = numbers[3];
	} else if (has_width && has_height) {
		geometry->view_width = width;
		geometry->view_height = height;
	} else {
		archetypon_set_error(error, error_capacity, missing_geometry);
		return -1;
	}
	geometry->intrinsic_width = has_width && has_height ?
		width : geometry->view_width;
	geometry->intrinsic_height = has_width && has_height ?
		height : geometry->view_height;
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
	struct context stack[SVG_MAX_DEPTH];
	const char *cursor;
	const char *end;
	char *error;
	size_t error_capacity;
	s32 depth;
	size_t total_points;
	size_t work_remaining;
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

static int initialize_render_state(struct render_state *state,
				   const char *source, size_t length,
				   s32 output_width, s32 output_height,
				   char *error, size_t error_capacity)
{
	struct svg_geometry geometry;
	size_t pixel_count;

	memset(state, 0, sizeof(*state));
	state->cursor = source;
	state->end = source + length;
	state->error = error;
	state->error_capacity = error_capacity;
	state->depth = 1;
	state->work_remaining = SVG_MAX_RENDER_WORK;
	if (!validate_svg_document(source, length, error, error_capacity) ||
	    !find_svg_geometry(source, length, &geometry, error,
			       error_capacity))
		return -1;
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
	initialize_viewport(state, &geometry);
	return 0;
}

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
	state->depth--;
	if (state->depth == 1)
		state->root_closed = true;
	return 0;
}

static int render_shape(struct render_state *state, const struct tag *tag,
			struct slice name, const struct context *context)
{
	struct path path;
	int status = -1;

	if (!build_shape_path(tag, name, context->matrix, &path, state->error,
			      state->error_capacity))
		goto out_free_path;
	if (path.point_count > SVG_MAX_TOTAL_POINTS - state->total_points) {
		archetypon_set_error(state->error, state->error_capacity,
				     "SVG exceeds the total point limit");
		goto out_free_path;
	}
	state->total_points += path.point_count;
	if (!draw_path(&state->surface, &path, &context->style, context->matrix,
		       &state->work_remaining, state->error,
		       state->error_capacity))
		goto out_free_path;
	status = 0;

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
	    slice_equal(name, "title") || slice_equal(name, "desc")) {
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
	if (!make_context(tag, &state->stack[state->depth - 1], &context,
			  state->error, state->error_capacity))
		return -1;
	context.name = tag->name;
	if ((slice_equal(name, "svg") || slice_equal(name, "g") ||
	     slice_equal(name, "a")) && context.own_opacity != 1) {
		archetypon_set_error(state->error, state->error_capacity,
				     "SVG container opacity is not supported");
		return -1;
	}
	if (render_element(state, tag, name, &context))
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

int archetypon_svg_render(const char *source, size_t length, s32 output_width,
			  s32 output_height, struct archetypon_image *image,
			  char *error, size_t error_capacity)
{
	struct render_state state;
	int status;

	if (!source || !image || !error || error_capacity == 0)
		return -1;
	memset(image, 0, sizeof(*image));
	error[0] = 0;
	if (output_width <= 0 || output_height <= 0) {
		archetypon_set_error(error, error_capacity,
				     "invalid output dimensions");
		return -1;
	}
	if (output_width > SVG_MAX_OUTPUT_DIMENSION ||
	    output_height > SVG_MAX_OUTPUT_DIMENSION) {
		archetypon_set_error(error, error_capacity,
				     "SVG output dimension exceeds %d pixels",
				     SVG_MAX_OUTPUT_DIMENSION);
		return -1;
	}
	if (initialize_render_state(&state, source, length, output_width,
				    output_height, error, error_capacity))
		return -1;
	status = render_document(&state);
	if (!status)
		status = downsample_surface(&state, image,
					    output_width, output_height);
	archetypon_image_free(&state.surface);
	return status;
}

int archetypon_svg_canvas_size(const char *source, size_t length, double *width,
			       double *height, char *error,
			       size_t error_capacity)
{
	struct svg_geometry geometry;

	if (!source || !width || !height ||
	    !error || error_capacity == 0)
		return -1;
	error[0] = 0;
	if (!validate_svg_document(source, length, error, error_capacity) ||
	    !find_svg_geometry(source, length, &geometry, error, error_capacity))
		return -1;
	*width = geometry.intrinsic_width;
	*height = geometry.intrinsic_height;
	return 0;
}
