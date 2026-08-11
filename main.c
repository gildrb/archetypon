#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef float f32;
typedef double f64;

#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define SVG_MAX_DEPTH 128
#define SVG_MAX_POINTS 2000000
#define SUPERSAMPLE 2
#define PI 3.14159265358979323846

typedef struct {
  u8 *data;
  size_t length;
  size_t capacity;
} Buffer;

typedef struct {
  const char *begin;
  const char *end;
} Slice;

typedef struct {
  i32 width;
  i32 height;
  u8 *pixels;
} Image;

typedef struct {
  f64 x;
  f64 y;
} Point;

typedef struct {
  f64 a;
  f64 b;
  f64 c;
  f64 d;
  f64 e;
  f64 f;
} Matrix;

typedef struct {
  size_t start;
  size_t count;
  i32 closed;
} Contour;

typedef struct {
  Point *points;
  size_t point_count;
  size_t point_capacity;
  Contour *contours;
  size_t contour_count;
  size_t contour_capacity;
  Matrix matrix;
  char *error;
  size_t error_capacity;
} Path;

typedef struct {
  u8 r;
  u8 g;
  u8 b;
  u8 a;
  i32 none;
} Color;

typedef struct {
  Color fill;
  Color stroke;
  Color current_color;
  f64 opacity;
  f64 fill_opacity;
  f64 stroke_opacity;
  f64 stroke_width;
  i32 fill_rule_evenodd;
  i32 line_cap;
  i32 line_join;
  i32 hidden;
} Style;

typedef struct {
  Matrix matrix;
  Style style;
  i32 render;
} Context;

typedef struct {
  Slice name;
  const char *attributes;
  const char *end;
  i32 closing;
  i32 self_closing;
} Tag;

typedef struct {
  f64 x;
  i32 winding;
} Intersection;

typedef struct {
  Buffer *buffer;
  u64 bits;
  i32 bit_count;
  i32 failed;
} BitWriter;

static i32 set_error(char *error, size_t capacity, const char *format, ...) {
  va_list arguments;
  if (error != NULL && capacity > 0) {
    va_start(arguments, format);
    vsnprintf(error, capacity, format, arguments);
    va_end(arguments);
  }
  return 0;
}

static i32 checked_multiply(size_t left, size_t right, size_t *result) {
  if (left != 0 && right > SIZE_MAX / left) {
    return 0;
  }
  *result = left * right;
  return 1;
}

static i32 buffer_reserve(Buffer *buffer, size_t extra) {
  size_t required;
  size_t capacity;
  u8 *data;

  if (extra > SIZE_MAX - buffer->length) {
    return 0;
  }
  required = buffer->length + extra;
  if (required <= buffer->capacity) {
    return 1;
  }

  capacity = buffer->capacity == 0 ? 256 : buffer->capacity;
  while (capacity < required) {
    if (capacity > SIZE_MAX / 2) {
      capacity = required;
      break;
    }
    capacity *= 2;
  }

  data = (u8 *)realloc(buffer->data, capacity);
  if (data == NULL) {
    return 0;
  }
  buffer->data = data;
  buffer->capacity = capacity;
  return 1;
}

static i32 buffer_append(Buffer *buffer, const void *data, size_t length) {
  if (length == 0) {
    return 1;
  }
  if (!buffer_reserve(buffer, length)) {
    return 0;
  }
  memcpy(buffer->data + buffer->length, data, length);
  buffer->length += length;
  return 1;
}

static i32 buffer_u8(Buffer *buffer, u8 value) {
  return buffer_append(buffer, &value, 1);
}

static i32 buffer_u16le(Buffer *buffer, u16 value) {
  u8 bytes[2] = {(u8)value, (u8)(value >> 8)};
  return buffer_append(buffer, bytes, sizeof(bytes));
}

static i32 buffer_u32le(Buffer *buffer, u32 value) {
  u8 bytes[4] = {(u8)value, (u8)(value >> 8), (u8)(value >> 16),
                 (u8)(value >> 24)};
  return buffer_append(buffer, bytes, sizeof(bytes));
}

static i32 buffer_u32be(Buffer *buffer, u32 value) {
  u8 bytes[4] = {(u8)(value >> 24), (u8)(value >> 16), (u8)(value >> 8),
                 (u8)value};
  return buffer_append(buffer, bytes, sizeof(bytes));
}

static void buffer_free(Buffer *buffer) {
  free(buffer->data);
  memset(buffer, 0, sizeof(*buffer));
}

static i32 read_file(const char *path, Buffer *buffer, char *error,
                     size_t error_capacity) {
  FILE *file;
  long length;

  file = fopen(path, "rb");
  if (file == NULL) {
    return set_error(error, error_capacity, "cannot open '%s': %s", path,
                     strerror(errno));
  }
  if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 ||
      fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return set_error(error, error_capacity, "cannot measure '%s'", path);
  }
  if ((u64)length > 32U * 1024U * 1024U) {
    fclose(file);
    return set_error(error, error_capacity, "SVG exceeds the 32 MiB limit");
  }
  if (!buffer_reserve(buffer, (size_t)length + 1)) {
    fclose(file);
    return set_error(error, error_capacity, "out of memory reading '%s'", path);
  }
  if (length > 0 && fread(buffer->data, 1, (size_t)length, file) != (size_t)length) {
    fclose(file);
    return set_error(error, error_capacity, "cannot read '%s'", path);
  }
  fclose(file);
  buffer->length = (size_t)length;
  buffer->data[buffer->length] = 0;
  return 1;
}

static i32 write_file(const char *path, const void *data, size_t length,
                      char *error, size_t error_capacity) {
  FILE *file = fopen(path, "wb");
  if (file == NULL) {
    return set_error(error, error_capacity, "cannot create '%s': %s", path,
                     strerror(errno));
  }
  if (length > 0 && fwrite(data, 1, length, file) != length) {
    fclose(file);
    return set_error(error, error_capacity, "cannot write '%s'", path);
  }
  if (fclose(file) != 0) {
    return set_error(error, error_capacity, "cannot finish '%s'", path);
  }
  return 1;
}

static i32 ensure_directory(const char *path, char *error,
                            size_t error_capacity) {
  struct stat status;
  if (mkdir(path, S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH) == 0) {
    return 1;
  }
  if (errno == EEXIST && stat(path, &status) == 0 && S_ISDIR(status.st_mode)) {
    return 1;
  }
  return set_error(error, error_capacity, "cannot create directory '%s': %s",
                   path, strerror(errno));
}

static Slice slice_trim(Slice value) {
  while (value.begin < value.end && isspace((unsigned char)*value.begin)) {
    value.begin++;
  }
  while (value.end > value.begin && isspace((unsigned char)value.end[-1])) {
    value.end--;
  }
  return value;
}

static i32 slice_equal(Slice value, const char *text) {
  size_t length = strlen(text);
  return (size_t)(value.end - value.begin) == length &&
         memcmp(value.begin, text, length) == 0;
}

static i32 slice_equal_ci(Slice value, const char *text) {
  size_t index;
  size_t length = strlen(text);
  if ((size_t)(value.end - value.begin) != length) {
    return 0;
  }
  for (index = 0; index < length; index++) {
    if (tolower((unsigned char)value.begin[index]) !=
        tolower((unsigned char)text[index])) {
      return 0;
    }
  }
  return 1;
}

static Slice local_name(Slice name) {
  const char *cursor;
  for (cursor = name.begin; cursor < name.end; cursor++) {
    if (*cursor == ':') {
      name.begin = cursor + 1;
    }
  }
  return name;
}

static i32 attribute_find(const Tag *tag, const char *wanted, Slice *value) {
  const char *cursor = tag->attributes;
  size_t wanted_length = strlen(wanted);

  while (cursor < tag->end) {
    const char *name_begin;
    const char *name_end;
    char quote;

    while (cursor < tag->end &&
           (isspace((unsigned char)*cursor) || *cursor == '/')) {
      cursor++;
    }
    if (cursor >= tag->end) {
      break;
    }
    name_begin = cursor;
    while (cursor < tag->end && !isspace((unsigned char)*cursor) &&
           *cursor != '=' && *cursor != '/') {
      cursor++;
    }
    name_end = cursor;
    while (cursor < tag->end && isspace((unsigned char)*cursor)) {
      cursor++;
    }
    if (cursor >= tag->end || *cursor != '=') {
      while (cursor < tag->end && !isspace((unsigned char)*cursor)) {
        cursor++;
      }
      continue;
    }
    cursor++;
    while (cursor < tag->end && isspace((unsigned char)*cursor)) {
      cursor++;
    }
    if (cursor >= tag->end) {
      break;
    }
    quote = *cursor;
    if (quote == '\'' || quote == '"') {
      cursor++;
      value->begin = cursor;
      while (cursor < tag->end && *cursor != quote) {
        cursor++;
      }
    } else {
      value->begin = cursor;
      while (cursor < tag->end && !isspace((unsigned char)*cursor) &&
             *cursor != '/') {
        cursor++;
      }
    }
    value->end = cursor;
    if ((size_t)(name_end - name_begin) == wanted_length &&
        memcmp(name_begin, wanted, wanted_length) == 0) {
      return 1;
    }
    if ((quote == '\'' || quote == '"') && cursor < tag->end) {
      cursor++;
    }
  }
  return 0;
}

static i32 next_tag(const char **position, const char *end, Tag *tag,
                    char *error, size_t error_capacity) {
  const char *cursor = *position;

  while (cursor < end) {
    const char *close;
    const char *name_begin;
    char quote = 0;

    while (cursor < end && *cursor != '<') {
      cursor++;
    }
    if (cursor >= end) {
      *position = end;
      return 0;
    }
    if ((size_t)(end - cursor) >= 4 && memcmp(cursor, "<!--", 4) == 0) {
      close = strstr(cursor + 4, "-->");
      if (close == NULL || close >= end) {
        return set_error(error, error_capacity, "unterminated SVG comment");
      }
      cursor = close + 3;
      continue;
    }
    if ((size_t)(end - cursor) >= 2 &&
        (cursor[1] == '?' || cursor[1] == '!')) {
      close = strchr(cursor + 2, '>');
      if (close == NULL || close >= end) {
        return set_error(error, error_capacity, "unterminated SVG declaration");
      }
      cursor = close + 1;
      continue;
    }

    cursor++;
    memset(tag, 0, sizeof(*tag));
    if (cursor < end && *cursor == '/') {
      tag->closing = 1;
      cursor++;
    }
    while (cursor < end && isspace((unsigned char)*cursor)) {
      cursor++;
    }
    name_begin = cursor;
    while (cursor < end && !isspace((unsigned char)*cursor) &&
           *cursor != '/' && *cursor != '>') {
      cursor++;
    }
    if (cursor == name_begin) {
      return set_error(error, error_capacity, "invalid empty SVG tag");
    }
    tag->name.begin = name_begin;
    tag->name.end = cursor;
    tag->attributes = cursor;

    close = cursor;
    while (close < end) {
      if (quote != 0) {
        if (*close == quote) {
          quote = 0;
        }
      } else if (*close == '\'' || *close == '"') {
        quote = *close;
      } else if (*close == '>') {
        break;
      }
      close++;
    }
    if (close >= end) {
      return set_error(error, error_capacity, "unterminated SVG tag");
    }
    tag->end = close;
    cursor = close;
    while (cursor > tag->attributes && isspace((unsigned char)cursor[-1])) {
      cursor--;
    }
    tag->self_closing = cursor > tag->attributes && cursor[-1] == '/';
    *position = close + 1;
    return 1;
  }
  return 0;
}


static i32 parse_length(Slice value, f64 *result) {
  char *after;
  value = slice_trim(value);
  if (value.begin == value.end) {
    return 0;
  }
  errno = 0;
  *result = strtod(value.begin, &after);
  if (after == value.begin || after > value.end || errno == ERANGE ||
      !isfinite(*result)) {
    return 0;
  }
  while (after < value.end && isspace((unsigned char)*after)) {
    after++;
  }
  if ((size_t)(value.end - after) == 2 && memcmp(after, "px", 2) == 0) {
    after += 2;
  }
  return after == value.end;
}

static i32 parse_number_list(Slice value, f64 *numbers, size_t count) {
  size_t index;
  const char *cursor = value.begin;
  for (index = 0; index < count; index++) {
    char *after;
    while (cursor < value.end &&
           (isspace((unsigned char)*cursor) || *cursor == ',')) {
      cursor++;
    }
    if (cursor >= value.end) {
      return 0;
    }
    errno = 0;
    numbers[index] = strtod(cursor, &after);
    if (after == cursor || after > value.end || errno == ERANGE ||
        !isfinite(numbers[index])) {
      return 0;
    }
    cursor = after;
  }
  while (cursor < value.end &&
         (isspace((unsigned char)*cursor) || *cursor == ',')) {
    cursor++;
  }
  return cursor == value.end;
}

static Matrix matrix_identity(void) {
  Matrix matrix = {1, 0, 0, 1, 0, 0};
  return matrix;
}

static Matrix matrix_multiply(Matrix left, Matrix right) {
  Matrix result;
  result.a = left.a * right.a + left.c * right.b;
  result.b = left.b * right.a + left.d * right.b;
  result.c = left.a * right.c + left.c * right.d;
  result.d = left.b * right.c + left.d * right.d;
  result.e = left.a * right.e + left.c * right.f + left.e;
  result.f = left.b * right.e + left.d * right.f + left.f;
  return result;
}

static Point matrix_point(Matrix matrix, f64 x, f64 y) {
  Point point;
  point.x = matrix.a * x + matrix.c * y + matrix.e;
  point.y = matrix.b * x + matrix.d * y + matrix.f;
  return point;
}

static f64 matrix_scale(Matrix matrix) {
  return sqrt((matrix.a * matrix.a + matrix.b * matrix.b +
               matrix.c * matrix.c + matrix.d * matrix.d) /
              2.0);
}

static i32 parse_transform(Slice value, Matrix *result, char *error,
                           size_t error_capacity) {
  const char *cursor = value.begin;
  Matrix matrix = matrix_identity();

  while (cursor < value.end) {
    const char *name_begin;
    Slice arguments;
    f64 values[6];
    size_t count = 0;
    Matrix transform = matrix_identity();

    while (cursor < value.end &&
           (isspace((unsigned char)*cursor) || *cursor == ',')) {
      cursor++;
    }
    if (cursor == value.end) {
      break;
    }
    name_begin = cursor;
    while (cursor < value.end && isalpha((unsigned char)*cursor)) {
      cursor++;
    }
    while (cursor < value.end && isspace((unsigned char)*cursor)) {
      cursor++;
    }
    if (cursor >= value.end || *cursor != '(') {
      return set_error(error, error_capacity, "invalid SVG transform");
    }
    cursor++;
    arguments.begin = cursor;
    while (cursor < value.end && *cursor != ')') {
      cursor++;
    }
    if (cursor >= value.end) {
      return set_error(error, error_capacity, "unterminated SVG transform");
    }
    arguments.end = cursor;
    cursor++;

    {
      const char *number_cursor = arguments.begin;
      while (number_cursor < arguments.end) {
        char *after;
        while (number_cursor < arguments.end &&
               (isspace((unsigned char)*number_cursor) ||
                *number_cursor == ',')) {
          number_cursor++;
        }
        if (number_cursor == arguments.end) {
          break;
        }
        if (count == ARRAY_COUNT(values)) {
          return set_error(error, error_capacity,
                           "too many SVG transform arguments");
        }
        values[count] = strtod(number_cursor, &after);
        if (after == number_cursor || after > arguments.end ||
            !isfinite(values[count])) {
          return set_error(error, error_capacity,
                           "invalid SVG transform number");
        }
        count++;
        number_cursor = after;
      }
    }

    if ((size_t)(cursor - 1 - name_begin) >= 6 &&
        (size_t)(strchr(name_begin, '(') - name_begin) == 6 &&
        memcmp(name_begin, "matrix", 6) == 0) {
      if (count != 6) {
        return set_error(error, error_capacity, "matrix() needs 6 values");
      }
      transform = (Matrix){values[0], values[1], values[2],
                           values[3], values[4], values[5]};
    } else if ((size_t)(strchr(name_begin, '(') - name_begin) == 9 &&
               memcmp(name_begin, "translate", 9) == 0) {
      if (count < 1 || count > 2) {
        return set_error(error, error_capacity,
                         "translate() needs 1 or 2 values");
      }
      transform.e = values[0];
      transform.f = count == 2 ? values[1] : 0;
    } else if ((size_t)(strchr(name_begin, '(') - name_begin) == 5 &&
               memcmp(name_begin, "scale", 5) == 0) {
      if (count < 1 || count > 2) {
        return set_error(error, error_capacity, "scale() needs 1 or 2 values");
      }
      transform.a = values[0];
      transform.d = count == 2 ? values[1] : values[0];
    } else if ((size_t)(strchr(name_begin, '(') - name_begin) == 6 &&
               memcmp(name_begin, "rotate", 6) == 0) {
      f64 angle;
      if (count != 1 && count != 3) {
        return set_error(error, error_capacity,
                         "rotate() needs 1 or 3 values");
      }
      angle = values[0] * PI / 180.0;
      transform.a = cos(angle);
      transform.b = sin(angle);
      transform.c = -sin(angle);
      transform.d = cos(angle);
      if (count == 3) {
        Matrix before = matrix_identity();
        Matrix after = matrix_identity();
        before.e = values[1];
        before.f = values[2];
        after.e = -values[1];
        after.f = -values[2];
        transform = matrix_multiply(before, matrix_multiply(transform, after));
      }
    } else if ((size_t)(strchr(name_begin, '(') - name_begin) == 5 &&
               memcmp(name_begin, "skewX", 5) == 0) {
      if (count != 1) {
        return set_error(error, error_capacity, "skewX() needs 1 value");
      }
      transform.c = tan(values[0] * PI / 180.0);
    } else if ((size_t)(strchr(name_begin, '(') - name_begin) == 5 &&
               memcmp(name_begin, "skewY", 5) == 0) {
      if (count != 1) {
        return set_error(error, error_capacity, "skewY() needs 1 value");
      }
      transform.b = tan(values[0] * PI / 180.0);
    } else {
      return set_error(error, error_capacity, "unsupported SVG transform");
    }
    matrix = matrix_multiply(matrix, transform);
  }

  *result = matrix;
  return 1;
}

static Color color_rgba(u8 red, u8 green, u8 blue, u8 alpha) {
  Color color = {red, green, blue, alpha, 0};
  return color;
}

static i32 hex_value(char character) {
  if (character >= '0' && character <= '9') {
    return character - '0';
  }
  character = (char)tolower((unsigned char)character);
  if (character >= 'a' && character <= 'f') {
    return character - 'a' + 10;
  }
  return -1;
}

static i32 parse_color(Slice value, Color current_color, Color *result) {
  char text[128];
  size_t length;
  size_t index;
  struct NamedColor {
    const char *name;
    u8 red;
    u8 green;
    u8 blue;
  };
  static const struct NamedColor named[] = {
      {"aqua", 0, 255, 255},       {"black", 0, 0, 0},
      {"blue", 0, 0, 255},        {"fuchsia", 255, 0, 255},
      {"gray", 128, 128, 128},    {"green", 0, 128, 0},
      {"lime", 0, 255, 0},        {"maroon", 128, 0, 0},
      {"navy", 0, 0, 128},        {"olive", 128, 128, 0},
      {"orange", 255, 165, 0},    {"purple", 128, 0, 128},
      {"red", 255, 0, 0},         {"silver", 192, 192, 192},
      {"teal", 0, 128, 128},      {"white", 255, 255, 255},
      {"yellow", 255, 255, 0},
  };

  value = slice_trim(value);
  length = (size_t)(value.end - value.begin);
  if (length == 0 || length >= sizeof(text)) {
    return 0;
  }
  for (index = 0; index < length; index++) {
    text[index] = (char)tolower((unsigned char)value.begin[index]);
  }
  text[length] = 0;

  if (strcmp(text, "none") == 0) {
    *result = color_rgba(0, 0, 0, 0);
    result->none = 1;
    return 1;
  }
  if (strcmp(text, "transparent") == 0) {
    *result = color_rgba(0, 0, 0, 0);
    return 1;
  }
  if (strcmp(text, "currentcolor") == 0) {
    *result = current_color;
    return 1;
  }
  if (text[0] == '#') {
    i32 digits[8];
    if (length != 4 && length != 5 && length != 7 && length != 9) {
      return 0;
    }
    for (index = 1; index < length; index++) {
      digits[index - 1] = hex_value(text[index]);
      if (digits[index - 1] < 0) {
        return 0;
      }
    }
    if (length == 4 || length == 5) {
      *result = color_rgba((u8)(digits[0] * 17), (u8)(digits[1] * 17),
                           (u8)(digits[2] * 17),
                           length == 5 ? (u8)(digits[3] * 17) : 255);
    } else {
      *result = color_rgba((u8)(digits[0] * 16 + digits[1]),
                           (u8)(digits[2] * 16 + digits[3]),
                           (u8)(digits[4] * 16 + digits[5]),
                           length == 9 ? (u8)(digits[6] * 16 + digits[7])
                                       : 255);
    }
    return 1;
  }
  if (strncmp(text, "rgb(", 4) == 0 || strncmp(text, "rgba(", 5) == 0) {
    f64 red;
    f64 green;
    f64 blue;
    f64 alpha = 1;
    i32 parsed;
    if (text[3] == 'a') {
      parsed = sscanf(text, "rgba(%lf,%lf,%lf,%lf)", &red, &green, &blue,
                      &alpha);
      if (parsed != 4) {
        return 0;
      }
    } else {
      parsed = sscanf(text, "rgb(%lf,%lf,%lf)", &red, &green, &blue);
      if (parsed != 3) {
        return 0;
      }
    }
    if (red < 0 || red > 255 || green < 0 || green > 255 || blue < 0 ||
        blue > 255 || alpha < 0 || alpha > 1) {
      return 0;
    }
    *result = color_rgba((u8)lround(red), (u8)lround(green),
                         (u8)lround(blue), (u8)lround(alpha * 255));
    return 1;
  }
  for (index = 0; index < ARRAY_COUNT(named); index++) {
    if (strcmp(text, named[index].name) == 0) {
      *result = color_rgba(named[index].red, named[index].green,
                           named[index].blue, 255);
      return 1;
    }
  }
  return 0;
}

static i32 parse_opacity(Slice value, f64 *result) {
  char *after;
  f64 number;
  value = slice_trim(value);
  if (value.begin == value.end) {
    return 0;
  }
  number = strtod(value.begin, &after);
  if (after == value.begin || after > value.end || !isfinite(number)) {
    return 0;
  }
  if (after < value.end && *after == '%') {
    number /= 100.0;
    after++;
  }
  while (after < value.end && isspace((unsigned char)*after)) {
    after++;
  }
  if (after != value.end) {
    return 0;
  }
  if (number < 0) {
    number = 0;
  }
  if (number > 1) {
    number = 1;
  }
  *result = number;
  return 1;
}

static Style style_default(void) {
  Style style;
  memset(&style, 0, sizeof(style));
  style.fill = color_rgba(0, 0, 0, 255);
  style.stroke = color_rgba(0, 0, 0, 0);
  style.stroke.none = 1;
  style.current_color = color_rgba(0, 0, 0, 255);
  style.opacity = 1;
  style.fill_opacity = 1;
  style.stroke_opacity = 1;
  style.stroke_width = 1;
  return style;
}

static i32 style_property(Style *style, Slice name, Slice value,
                          f64 *own_opacity, char *error,
                          size_t error_capacity) {
  f64 number;
  if (slice_equal_ci(name, "color")) {
    if (!parse_color(value, style->current_color, &style->current_color)) {
      return set_error(error, error_capacity, "unsupported SVG color");
    }
  } else if (slice_equal_ci(name, "fill")) {
    if (!parse_color(value, style->current_color, &style->fill)) {
      return set_error(error, error_capacity,
                       "unsupported SVG fill (paint servers are not supported)");
    }
  } else if (slice_equal_ci(name, "stroke")) {
    if (!parse_color(value, style->current_color, &style->stroke)) {
      return set_error(error, error_capacity,
                       "unsupported SVG stroke (paint servers are not supported)");
    }
  } else if (slice_equal_ci(name, "opacity")) {
    if (!parse_opacity(value, own_opacity)) {
      return set_error(error, error_capacity, "invalid SVG opacity");
    }
  } else if (slice_equal_ci(name, "fill-opacity")) {
    if (!parse_opacity(value, &style->fill_opacity)) {
      return set_error(error, error_capacity, "invalid SVG fill-opacity");
    }
  } else if (slice_equal_ci(name, "stroke-opacity")) {
    if (!parse_opacity(value, &style->stroke_opacity)) {
      return set_error(error, error_capacity, "invalid SVG stroke-opacity");
    }
  } else if (slice_equal_ci(name, "stroke-width")) {
    if (!parse_length(value, &number) || number < 0) {
      return set_error(error, error_capacity, "invalid SVG stroke-width");
    }
    style->stroke_width = number;
  } else if (slice_equal_ci(name, "fill-rule")) {
    if (slice_equal_ci(slice_trim(value), "evenodd")) {
      style->fill_rule_evenodd = 1;
    } else if (slice_equal_ci(slice_trim(value), "nonzero")) {
      style->fill_rule_evenodd = 0;
    } else {
      return set_error(error, error_capacity, "unsupported SVG fill-rule");
    }
  } else if (slice_equal_ci(name, "stroke-linecap")) {
    if (slice_equal_ci(slice_trim(value), "butt")) {
      style->line_cap = 0;
    } else if (slice_equal_ci(slice_trim(value), "round")) {
      style->line_cap = 1;
    } else if (slice_equal_ci(slice_trim(value), "square")) {
      style->line_cap = 2;
    } else {
      return set_error(error, error_capacity, "unsupported SVG stroke-linecap");
    }
  } else if (slice_equal_ci(name, "stroke-linejoin")) {
    if (slice_equal_ci(slice_trim(value), "miter")) {
      style->line_join = 0;
    } else if (slice_equal_ci(slice_trim(value), "round")) {
      style->line_join = 1;
    } else if (slice_equal_ci(slice_trim(value), "bevel")) {
      style->line_join = 2;
    } else {
      return set_error(error, error_capacity,
                       "unsupported SVG stroke-linejoin");
    }
  } else if (slice_equal_ci(name, "display")) {
    if (slice_equal_ci(slice_trim(value), "none")) {
      style->hidden = 1;
    }
  } else if (slice_equal_ci(name, "visibility")) {
    if (slice_equal_ci(slice_trim(value), "hidden") ||
        slice_equal_ci(slice_trim(value), "collapse")) {
      style->hidden = 1;
    } else if (slice_equal_ci(slice_trim(value), "visible")) {
      style->hidden = 0;
    }
  } else if (slice_equal_ci(name, "stroke-dasharray") &&
             !slice_equal_ci(slice_trim(value), "none")) {
    return set_error(error, error_capacity,
                     "SVG dashed strokes are not supported");
  } else if ((slice_equal_ci(name, "clip-path") ||
              slice_equal_ci(name, "mask") ||
              slice_equal_ci(name, "filter")) &&
             !slice_equal_ci(slice_trim(value), "none")) {
    return set_error(error, error_capacity,
                     "SVG clipping, masks, and filters are not supported");
  }
  return 1;
}

static i32 apply_style(const Tag *tag, const Style *parent, Style *style,
                       char *error, size_t error_capacity) {
  static const char *properties[] = {
      "color",          "fill",           "stroke",       "opacity",
      "fill-opacity",   "stroke-opacity", "stroke-width", "fill-rule",
      "stroke-linecap", "stroke-linejoin", "display",      "visibility",
      "stroke-dasharray", "clip-path",     "mask",         "filter"};
  size_t index;
  Slice value;
  f64 own_opacity = 1;

  *style = *parent;
  for (index = 0; index < ARRAY_COUNT(properties); index++) {
    if (attribute_find(tag, properties[index], &value)) {
      Slice name = {properties[index], properties[index] + strlen(properties[index])};
      if (!style_property(style, name, value, &own_opacity, error,
                          error_capacity)) {
        return 0;
      }
    }
  }

  if (attribute_find(tag, "style", &value)) {
    const char *cursor = value.begin;
    while (cursor < value.end) {
      Slice name;
      Slice property_value;
      while (cursor < value.end &&
             (isspace((unsigned char)*cursor) || *cursor == ';')) {
        cursor++;
      }
      name.begin = cursor;
      while (cursor < value.end && *cursor != ':' && *cursor != ';') {
        cursor++;
      }
      name.end = cursor;
      if (cursor >= value.end || *cursor != ':') {
        while (cursor < value.end && *cursor != ';') {
          cursor++;
        }
        continue;
      }
      cursor++;
      property_value.begin = cursor;
      while (cursor < value.end && *cursor != ';') {
        cursor++;
      }
      property_value.end = cursor;
      if (!style_property(style, slice_trim(name), slice_trim(property_value),
                          &own_opacity, error, error_capacity)) {
        return 0;
      }
    }
  }
  style->opacity = parent->opacity * own_opacity;
  return 1;
}

static i32 make_context(const Tag *tag, const Context *parent, Context *context,
                        char *error, size_t error_capacity) {
  Slice value;
  Matrix local = matrix_identity();
  *context = *parent;
  if (!apply_style(tag, &parent->style, &context->style, error,
                   error_capacity)) {
    return 0;
  }
  if (attribute_find(tag, "transform", &value) &&
      !parse_transform(value, &local, error, error_capacity)) {
    return 0;
  }
  context->matrix = matrix_multiply(parent->matrix, local);
  if (context->style.hidden) {
    context->render = 0;
  }
  return 1;
}

static i32 path_reserve_points(Path *path, size_t extra) {
  size_t needed;
  size_t capacity;
  Point *points;
  if (extra > SVG_MAX_POINTS - path->point_count) {
    return set_error(path->error, path->error_capacity,
                     "SVG path exceeds the point limit");
  }
  needed = path->point_count + extra;
  if (needed <= path->point_capacity) {
    return 1;
  }
  capacity = path->point_capacity == 0 ? 128 : path->point_capacity;
  while (capacity < needed) {
    capacity *= 2;
  }
  points = (Point *)realloc(path->points, capacity * sizeof(*points));
  if (points == NULL) {
    return set_error(path->error, path->error_capacity,
                     "out of memory parsing SVG path");
  }
  path->points = points;
  path->point_capacity = capacity;
  return 1;
}

static i32 path_begin_contour(Path *path, f64 x, f64 y) {
  Contour *contours;
  size_t capacity;
  if (path->contour_count == path->contour_capacity) {
    capacity = path->contour_capacity == 0 ? 8 : path->contour_capacity * 2;
    contours =
        (Contour *)realloc(path->contours, capacity * sizeof(*contours));
    if (contours == NULL) {
      return set_error(path->error, path->error_capacity,
                       "out of memory parsing SVG contours");
    }
    path->contours = contours;
    path->contour_capacity = capacity;
  }
  if (!path_reserve_points(path, 1)) {
    return 0;
  }
  path->contours[path->contour_count] =
      (Contour){path->point_count, 1, 0};
  path->contour_count++;
  path->points[path->point_count++] = matrix_point(path->matrix, x, y);
  return 1;
}

static i32 path_line_to(Path *path, f64 x, f64 y) {
  Point point;
  Contour *contour;
  if (path->contour_count == 0) {
    return set_error(path->error, path->error_capacity,
                     "SVG path draws before its first move");
  }
  point = matrix_point(path->matrix, x, y);
  contour = &path->contours[path->contour_count - 1];
  if (contour->count > 0) {
    Point previous = path->points[path->point_count - 1];
    if (fabs(previous.x - point.x) < 1e-9 &&
        fabs(previous.y - point.y) < 1e-9) {
      return 1;
    }
  }
  if (!path_reserve_points(path, 1)) {
    return 0;
  }
  path->points[path->point_count++] = point;
  contour->count++;
  return 1;
}

static f64 point_line_distance_squared(Point point, Point start, Point end) {
  f64 dx = end.x - start.x;
  f64 dy = end.y - start.y;
  f64 cross;
  f64 length_squared = dx * dx + dy * dy;
  if (length_squared < 1e-20) {
    dx = point.x - start.x;
    dy = point.y - start.y;
    return dx * dx + dy * dy;
  }
  cross = (point.x - start.x) * dy - (point.y - start.y) * dx;
  return cross * cross / length_squared;
}

static i32 flatten_cubic_device(Path *path, Point p0, Point p1, Point p2,
                                Point p3, i32 depth) {
  Point p01;
  Point p12;
  Point p23;
  Point p012;
  Point p123;
  Point midpoint;
  const f64 tolerance_squared = 0.0625;

  if (depth >= 14 ||
      (point_line_distance_squared(p1, p0, p3) <= tolerance_squared &&
       point_line_distance_squared(p2, p0, p3) <= tolerance_squared)) {
    if (!path_reserve_points(path, 1)) {
      return 0;
    }
    path->points[path->point_count++] = p3;
    path->contours[path->contour_count - 1].count++;
    return 1;
  }

  p01 = (Point){(p0.x + p1.x) / 2, (p0.y + p1.y) / 2};
  p12 = (Point){(p1.x + p2.x) / 2, (p1.y + p2.y) / 2};
  p23 = (Point){(p2.x + p3.x) / 2, (p2.y + p3.y) / 2};
  p012 = (Point){(p01.x + p12.x) / 2, (p01.y + p12.y) / 2};
  p123 = (Point){(p12.x + p23.x) / 2, (p12.y + p23.y) / 2};
  midpoint = (Point){(p012.x + p123.x) / 2,
                     (p012.y + p123.y) / 2};
  return flatten_cubic_device(path, p0, p01, p012, midpoint, depth + 1) &&
         flatten_cubic_device(path, midpoint, p123, p23, p3, depth + 1);
}

static i32 path_cubic_to(Path *path, f64 x0, f64 y0, f64 x1, f64 y1,
                         f64 x2, f64 y2, f64 x3, f64 y3) {
  return flatten_cubic_device(path, matrix_point(path->matrix, x0, y0),
                              matrix_point(path->matrix, x1, y1),
                              matrix_point(path->matrix, x2, y2),
                              matrix_point(path->matrix, x3, y3), 0);
}

static i32 path_quadratic_to(Path *path, f64 x0, f64 y0, f64 x1, f64 y1,
                             f64 x2, f64 y2) {
  f64 c1x = x0 + (2.0 / 3.0) * (x1 - x0);
  f64 c1y = y0 + (2.0 / 3.0) * (y1 - y0);
  f64 c2x = x2 + (2.0 / 3.0) * (x1 - x2);
  f64 c2y = y2 + (2.0 / 3.0) * (y1 - y2);
  return path_cubic_to(path, x0, y0, c1x, c1y, c2x, c2y, x2, y2);
}

static f64 vector_angle(f64 ux, f64 uy, f64 vx, f64 vy) {
  f64 denominator = sqrt((ux * ux + uy * uy) * (vx * vx + vy * vy));
  f64 cosine;
  f64 angle;
  if (denominator < 1e-20) {
    return 0;
  }
  cosine = (ux * vx + uy * vy) / denominator;
  if (cosine < -1) {
    cosine = -1;
  }
  if (cosine > 1) {
    cosine = 1;
  }
  angle = acos(cosine);
  return ux * vy - uy * vx < 0 ? -angle : angle;
}

static i32 path_arc_to(Path *path, f64 x0, f64 y0, f64 rx, f64 ry,
                       f64 rotation, i32 large_arc, i32 sweep, f64 x1,
                       f64 y1) {
  f64 phi;
  f64 cosine;
  f64 sine;
  f64 dx;
  f64 dy;
  f64 px;
  f64 py;
  f64 lambda;
  f64 numerator;
  f64 denominator;
  f64 factor;
  f64 cxp;
  f64 cyp;
  f64 cx;
  f64 cy;
  f64 start_angle;
  f64 delta_angle;
  f64 radius_pixels;
  i32 segments;
  i32 index;

  rx = fabs(rx);
  ry = fabs(ry);
  if (rx < 1e-12 || ry < 1e-12 ||
      (fabs(x0 - x1) < 1e-12 && fabs(y0 - y1) < 1e-12)) {
    return path_line_to(path, x1, y1);
  }

  phi = fmod(rotation, 360.0) * PI / 180.0;
  cosine = cos(phi);
  sine = sin(phi);
  dx = (x0 - x1) / 2;
  dy = (y0 - y1) / 2;
  px = cosine * dx + sine * dy;
  py = -sine * dx + cosine * dy;
  lambda = px * px / (rx * rx) + py * py / (ry * ry);
  if (lambda > 1) {
    f64 scale = sqrt(lambda);
    rx *= scale;
    ry *= scale;
  }

  numerator = rx * rx * ry * ry - rx * rx * py * py - ry * ry * px * px;
  denominator = rx * rx * py * py + ry * ry * px * px;
  factor = denominator <= 0 ? 0 : sqrt(fmax(0, numerator / denominator));
  if (large_arc == sweep) {
    factor = -factor;
  }
  cxp = factor * (rx * py / ry);
  cyp = factor * (-ry * px / rx);
  cx = cosine * cxp - sine * cyp + (x0 + x1) / 2;
  cy = sine * cxp + cosine * cyp + (y0 + y1) / 2;

  start_angle = vector_angle(1, 0, (px - cxp) / rx, (py - cyp) / ry);
  delta_angle = vector_angle((px - cxp) / rx, (py - cyp) / ry,
                             (-px - cxp) / rx, (-py - cyp) / ry);
  if (!sweep && delta_angle > 0) {
    delta_angle -= 2 * PI;
  } else if (sweep && delta_angle < 0) {
    delta_angle += 2 * PI;
  }

  radius_pixels = fmax(rx, ry) * matrix_scale(path->matrix);
  segments = (i32)ceil(fabs(delta_angle) * sqrt(fmax(radius_pixels, 1.0)));
  if (segments < 4) {
    segments = 4;
  }
  if (segments > 4096) {
    segments = 4096;
  }
  for (index = 1; index <= segments; index++) {
    f64 angle = start_angle + delta_angle * index / segments;
    f64 unit_x = cos(angle);
    f64 unit_y = sin(angle);
    f64 x = cx + cosine * rx * unit_x - sine * ry * unit_y;
    f64 y = cy + sine * rx * unit_x + cosine * ry * unit_y;
    if (!path_line_to(path, x, y)) {
      return 0;
    }
  }
  return 1;
}

typedef struct {
  const char *cursor;
  const char *end;
} PathParser;

static void path_skip_separators(PathParser *parser) {
  while (parser->cursor < parser->end &&
         (isspace((unsigned char)*parser->cursor) || *parser->cursor == ',')) {
    parser->cursor++;
  }
}

static i32 path_has_number(PathParser *parser) {
  const char *cursor = parser->cursor;
  while (cursor < parser->end &&
         (isspace((unsigned char)*cursor) || *cursor == ',')) {
    cursor++;
  }
  return cursor < parser->end &&
         (*cursor == '+' || *cursor == '-' || *cursor == '.' ||
          isdigit((unsigned char)*cursor));
}

static i32 path_number(PathParser *parser, f64 *number) {
  char *after;
  path_skip_separators(parser);
  if (parser->cursor >= parser->end) {
    return 0;
  }
  errno = 0;
  *number = strtod(parser->cursor, &after);
  if (after == parser->cursor || after > parser->end || errno == ERANGE ||
      !isfinite(*number)) {
    return 0;
  }
  parser->cursor = after;
  return 1;
}

static i32 parse_path_data(Slice data, Matrix matrix, Path *path, char *error,
                           size_t error_capacity) {
  PathParser parser = {data.begin, data.end};
  char command = 0;
  f64 x = 0;
  f64 y = 0;
  f64 start_x = 0;
  f64 start_y = 0;
  f64 cubic_control_x = 0;
  f64 cubic_control_y = 0;
  f64 quadratic_control_x = 0;
  f64 quadratic_control_y = 0;
  char previous_command = 0;

  memset(path, 0, sizeof(*path));
  path->matrix = matrix;
  path->error = error;
  path->error_capacity = error_capacity;

  while (1) {
    i32 relative;
    path_skip_separators(&parser);
    if (parser.cursor >= parser.end) {
      break;
    }
    if (isalpha((unsigned char)*parser.cursor)) {
      command = *parser.cursor++;
    } else if (command == 0) {
      return set_error(error, error_capacity, "SVG path is missing a command");
    }
    relative = islower((unsigned char)command);

    switch (tolower((unsigned char)command)) {
    case 'm': {
      f64 next_x;
      f64 next_y;
      if (!path_number(&parser, &next_x) || !path_number(&parser, &next_y)) {
        return set_error(error, error_capacity, "invalid SVG move command");
      }
      if (relative) {
        next_x += x;
        next_y += y;
      }
      x = next_x;
      y = next_y;
      start_x = x;
      start_y = y;
      if (!path_begin_contour(path, x, y)) {
        return 0;
      }
      command = relative ? 'l' : 'L';
      previous_command = 'm';
      break;
    }
    case 'l': {
      f64 next_x;
      f64 next_y;
      if (!path_number(&parser, &next_x) || !path_number(&parser, &next_y)) {
        return set_error(error, error_capacity, "invalid SVG line command");
      }
      if (relative) {
        next_x += x;
        next_y += y;
      }
      if (!path_line_to(path, next_x, next_y)) {
        return 0;
      }
      x = next_x;
      y = next_y;
      previous_command = 'l';
      break;
    }
    case 'h': {
      f64 next_x;
      if (!path_number(&parser, &next_x)) {
        return set_error(error, error_capacity,
                         "invalid SVG horizontal line command");
      }
      if (relative) {
        next_x += x;
      }
      if (!path_line_to(path, next_x, y)) {
        return 0;
      }
      x = next_x;
      previous_command = 'h';
      break;
    }
    case 'v': {
      f64 next_y;
      if (!path_number(&parser, &next_y)) {
        return set_error(error, error_capacity,
                         "invalid SVG vertical line command");
      }
      if (relative) {
        next_y += y;
      }
      if (!path_line_to(path, x, next_y)) {
        return 0;
      }
      y = next_y;
      previous_command = 'v';
      break;
    }
    case 'c':
    case 's': {
      i32 smooth = tolower((unsigned char)command) == 's';
      f64 x1 = x;
      f64 y1 = y;
      f64 x2;
      f64 y2;
      f64 next_x;
      f64 next_y;
      if (smooth) {
        if (previous_command == 'c' || previous_command == 's') {
          x1 = 2 * x - cubic_control_x;
          y1 = 2 * y - cubic_control_y;
        }
      } else if (!path_number(&parser, &x1) ||
                 !path_number(&parser, &y1)) {
        return set_error(error, error_capacity,
                         "invalid SVG cubic command");
      }
      if (!path_number(&parser, &x2) || !path_number(&parser, &y2) ||
          !path_number(&parser, &next_x) || !path_number(&parser, &next_y)) {
        return set_error(error, error_capacity,
                         "invalid SVG cubic command");
      }
      if (relative) {
        if (!smooth) {
          x1 += x;
          y1 += y;
        }
        x2 += x;
        y2 += y;
        next_x += x;
        next_y += y;
      }
      if (!path_cubic_to(path, x, y, x1, y1, x2, y2, next_x, next_y)) {
        return 0;
      }
      x = next_x;
      y = next_y;
      cubic_control_x = x2;
      cubic_control_y = y2;
      previous_command = smooth ? 's' : 'c';
      break;
    }
    case 'q':
    case 't': {
      i32 smooth = tolower((unsigned char)command) == 't';
      f64 x1 = x;
      f64 y1 = y;
      f64 next_x;
      f64 next_y;
      if (smooth) {
        if (previous_command == 'q' || previous_command == 't') {
          x1 = 2 * x - quadratic_control_x;
          y1 = 2 * y - quadratic_control_y;
        }
      } else if (!path_number(&parser, &x1) ||
                 !path_number(&parser, &y1)) {
        return set_error(error, error_capacity,
                         "invalid SVG quadratic command");
      }
      if (!path_number(&parser, &next_x) || !path_number(&parser, &next_y)) {
        return set_error(error, error_capacity,
                         "invalid SVG quadratic command");
      }
      if (relative) {
        if (!smooth) {
          x1 += x;
          y1 += y;
        }
        next_x += x;
        next_y += y;
      }
      if (!path_quadratic_to(path, x, y, x1, y1, next_x, next_y)) {
        return 0;
      }
      x = next_x;
      y = next_y;
      quadratic_control_x = x1;
      quadratic_control_y = y1;
      previous_command = smooth ? 't' : 'q';
      break;
    }
    case 'a': {
      f64 rx;
      f64 ry;
      f64 rotation;
      f64 large_arc;
      f64 sweep;
      f64 next_x;
      f64 next_y;
      if (!path_number(&parser, &rx) || !path_number(&parser, &ry) ||
          !path_number(&parser, &rotation) ||
          !path_number(&parser, &large_arc) ||
          !path_number(&parser, &sweep) || !path_number(&parser, &next_x) ||
          !path_number(&parser, &next_y) ||
          (large_arc != 0 && large_arc != 1) || (sweep != 0 && sweep != 1)) {
        return set_error(error, error_capacity, "invalid SVG arc command");
      }
      if (relative) {
        next_x += x;
        next_y += y;
      }
      if (!path_arc_to(path, x, y, rx, ry, rotation, (i32)large_arc,
                       (i32)sweep, next_x, next_y)) {
        return 0;
      }
      x = next_x;
      y = next_y;
      previous_command = 'a';
      break;
    }
    case 'z':
      if (path->contour_count == 0) {
        return set_error(error, error_capacity,
                         "SVG close command has no contour");
      }
      path->contours[path->contour_count - 1].closed = 1;
      x = start_x;
      y = start_y;
      previous_command = 'z';
      command = 0;
      break;
    default:
      return set_error(error, error_capacity,
                       "unsupported SVG path command '%c'", command);
    }

  }
  return 1;
}

static void path_free(Path *path) {
  free(path->points);
  free(path->contours);
  memset(path, 0, sizeof(*path));
}

static i32 compare_intersections(const void *left, const void *right) {
  const Intersection *a = (const Intersection *)left;
  const Intersection *b = (const Intersection *)right;
  if (a->x < b->x) {
    return -1;
  }
  if (a->x > b->x) {
    return 1;
  }
  return b->winding - a->winding;
}

static void composite_pixel(u8 *pixel, Color color, u8 alpha) {
  u32 inverse = 255 - alpha;
  pixel[0] = (u8)(((u32)color.r * alpha + (u32)pixel[0] * inverse + 127) / 255);
  pixel[1] = (u8)(((u32)color.g * alpha + (u32)pixel[1] * inverse + 127) / 255);
  pixel[2] = (u8)(((u32)color.b * alpha + (u32)pixel[2] * inverse + 127) / 255);
  pixel[3] = (u8)(alpha + ((u32)pixel[3] * inverse + 127) / 255);
}

static u8 effective_alpha(Color color, f64 opacity) {
  f64 alpha = color.a * opacity;
  if (alpha < 0) {
    alpha = 0;
  }
  if (alpha > 255) {
    alpha = 255;
  }
  return (u8)lround(alpha);
}

static void composite_span(Image *surface, i32 row, f64 start, f64 end,
                           Color color, u8 alpha) {
  i32 first = (i32)ceil(start - 0.5);
  i32 last = (i32)floor(end - 0.5);
  i32 column;
  if (first < 0) {
    first = 0;
  }
  if (last >= surface->width) {
    last = surface->width - 1;
  }
  for (column = first; column <= last; column++) {
    size_t offset =
        ((size_t)row * (size_t)surface->width + (size_t)column) * 4;
    composite_pixel(surface->pixels + offset, color, alpha);
  }
}

static i32 draw_fill(Image *surface, const Path *path, const Style *style,
                     char *error, size_t error_capacity) {
  size_t maximum_intersections = path->point_count + path->contour_count;
  Intersection *intersections;
  f64 minimum_y = INFINITY;
  f64 maximum_y = -INFINITY;
  i32 first_row;
  i32 last_row;
  i32 row;
  size_t contour_index;
  u8 alpha;

  if (style->fill.none || path->point_count < 3) {
    return 1;
  }
  alpha = effective_alpha(style->fill,
                          style->opacity * style->fill_opacity);
  if (alpha == 0) {
    return 1;
  }
  intersections = (Intersection *)malloc(maximum_intersections *
                                          sizeof(*intersections));
  if (intersections == NULL) {
    return set_error(error, error_capacity, "out of memory rasterizing SVG");
  }
  for (contour_index = 0; contour_index < path->point_count; contour_index++) {
    minimum_y = fmin(minimum_y, path->points[contour_index].y);
    maximum_y = fmax(maximum_y, path->points[contour_index].y);
  }
  first_row = (i32)fmax(0, floor(minimum_y));
  last_row = (i32)fmin(surface->height - 1, ceil(maximum_y));

  for (row = first_row; row <= last_row; row++) {
    f64 scan_y = row + 0.5;
    size_t count = 0;
    size_t index;
    for (contour_index = 0; contour_index < path->contour_count;
         contour_index++) {
      const Contour *contour = &path->contours[contour_index];
      size_t point_index;
      if (contour->count < 2) {
        continue;
      }
      for (point_index = 0; point_index < contour->count; point_index++) {
        Point start = path->points[contour->start + point_index];
        Point end = path->points[contour->start +
                                 ((point_index + 1) % contour->count)];
        if ((start.y <= scan_y && end.y > scan_y) ||
            (end.y <= scan_y && start.y > scan_y)) {
          f64 ratio = (scan_y - start.y) / (end.y - start.y);
          intersections[count].x = start.x + ratio * (end.x - start.x);
          intersections[count].winding = end.y > start.y ? 1 : -1;
          count++;
        }
      }
    }
    if (count < 2) {
      continue;
    }
    qsort(intersections, count, sizeof(*intersections),
          compare_intersections);

    if (style->fill_rule_evenodd) {
      for (index = 0; index + 1 < count; index += 2) {
        composite_span(surface, row, intersections[index].x,
                       intersections[index + 1].x, style->fill, alpha);
      }
    } else {
      i32 winding = 0;
      f64 span_start = 0;
      for (index = 0; index < count; index++) {
        i32 previous = winding;
        winding += intersections[index].winding;
        if (previous == 0 && winding != 0) {
          span_start = intersections[index].x;
        } else if (previous != 0 && winding == 0) {
          composite_span(surface, row, span_start, intersections[index].x,
                         style->fill, alpha);
        }
      }
    }
  }
  free(intersections);
  return 1;
}

static i32 draw_stroke(Image *surface, const Path *path, const Style *style,
                       Matrix matrix, char *error, size_t error_capacity) {
  f64 radius;
  f64 minimum_x = INFINITY;
  f64 minimum_y = INFINITY;
  f64 maximum_x = -INFINITY;
  f64 maximum_y = -INFINITY;
  i32 left;
  i32 top;
  i32 right;
  i32 bottom;
  i32 mask_width;
  i32 mask_height;
  size_t mask_size;
  u8 *mask;
  size_t contour_index;
  u8 alpha;

  if (style->stroke.none || style->stroke_width <= 0 ||
      path->point_count < 2) {
    return 1;
  }
  alpha = effective_alpha(style->stroke,
                          style->opacity * style->stroke_opacity);
  if (alpha == 0) {
    return 1;
  }
  radius = style->stroke_width * matrix_scale(matrix) / 2;
  if (radius <= 0) {
    return 1;
  }
  for (contour_index = 0; contour_index < path->point_count; contour_index++) {
    minimum_x = fmin(minimum_x, path->points[contour_index].x);
    minimum_y = fmin(minimum_y, path->points[contour_index].y);
    maximum_x = fmax(maximum_x, path->points[contour_index].x);
    maximum_y = fmax(maximum_y, path->points[contour_index].y);
  }
  left = (i32)fmax(0, floor(minimum_x - radius - 1));
  top = (i32)fmax(0, floor(minimum_y - radius - 1));
  right = (i32)fmin(surface->width - 1, ceil(maximum_x + radius + 1));
  bottom = (i32)fmin(surface->height - 1, ceil(maximum_y + radius + 1));
  if (right < left || bottom < top) {
    return 1;
  }
  mask_width = right - left + 1;
  mask_height = bottom - top + 1;
  if (!checked_multiply((size_t)mask_width, (size_t)mask_height,
                        &mask_size)) {
    return set_error(error, error_capacity, "SVG stroke is too large");
  }
  mask = (u8 *)calloc(mask_size, 1);
  if (mask == NULL) {
    return set_error(error, error_capacity, "out of memory rasterizing stroke");
  }

  for (contour_index = 0; contour_index < path->contour_count;
       contour_index++) {
    const Contour *contour = &path->contours[contour_index];
    size_t segment_count = contour->closed ? contour->count : contour->count - 1;
    size_t segment_index;
    for (segment_index = 0; segment_index < segment_count; segment_index++) {
      Point start = path->points[contour->start + segment_index];
      Point end = path->points[contour->start +
                               ((segment_index + 1) % contour->count)];
      f64 dx = end.x - start.x;
      f64 dy = end.y - start.y;
      f64 length_squared = dx * dx + dy * dy;
      i32 segment_left;
      i32 segment_top;
      i32 segment_right;
      i32 segment_bottom;
      i32 row;
      if (length_squared < 1e-20) {
        continue;
      }
      segment_left = (i32)fmax(left, floor(fmin(start.x, end.x) - radius - 1));
      segment_top = (i32)fmax(top, floor(fmin(start.y, end.y) - radius - 1));
      segment_right =
          (i32)fmin(right, ceil(fmax(start.x, end.x) + radius + 1));
      segment_bottom =
          (i32)fmin(bottom, ceil(fmax(start.y, end.y) + radius + 1));
      for (row = segment_top; row <= segment_bottom; row++) {
        i32 column;
        for (column = segment_left; column <= segment_right; column++) {
          f64 px = column + 0.5;
          f64 py = row + 0.5;
          f64 projection = ((px - start.x) * dx + (py - start.y) * dy) /
                           length_squared;
          f64 distance_x;
          f64 distance_y;
          i32 endpoint_start = segment_index == 0 && !contour->closed;
          i32 endpoint_end = segment_index + 1 == segment_count &&
                             !contour->closed;

          if (style->line_cap == 2) {
            f64 extension = radius / sqrt(length_squared);
            if (endpoint_start && projection < -extension) {
              continue;
            }
            if (endpoint_end && projection > 1 + extension) {
              continue;
            }
          } else if (style->line_cap == 0) {
            if ((endpoint_start && projection < 0) ||
                (endpoint_end && projection > 1)) {
              continue;
            }
          }
          if (projection < 0) {
            projection = 0;
          }
          if (projection > 1) {
            projection = 1;
          }
          distance_x = px - (start.x + projection * dx);
          distance_y = py - (start.y + projection * dy);
          if (distance_x * distance_x + distance_y * distance_y <=
              radius * radius) {
            mask[(size_t)(row - top) * mask_width + (column - left)] = 1;
          }
        }
      }
    }
  }

  {
    i32 row;
    for (row = top; row <= bottom; row++) {
      i32 column;
      for (column = left; column <= right; column++) {
        if (mask[(size_t)(row - top) * mask_width + (column - left)] != 0) {
          composite_pixel(surface->pixels +
                              ((size_t)row * surface->width + column) * 4,
                          style->stroke, alpha);
        }
      }
    }
  }
  free(mask);
  return 1;
}

static i32 draw_path(Image *surface, const Path *path, const Style *style,
                     Matrix matrix, char *error, size_t error_capacity) {
  return draw_fill(surface, path, style, error, error_capacity) &&
         draw_stroke(surface, path, style, matrix, error, error_capacity);
}

static i32 required_length(const Tag *tag, const char *name, f64 *value,
                           char *error, size_t error_capacity) {
  Slice attribute;
  if (!attribute_find(tag, name, &attribute) ||
      !parse_length(attribute, value)) {
    return set_error(error, error_capacity,
                     "SVG <%.*s> needs a valid %s", (i32)(tag->name.end - tag->name.begin),
                     tag->name.begin, name);
  }
  return 1;
}

static i32 optional_length(const Tag *tag, const char *name, f64 fallback,
                           f64 *value, char *error, size_t error_capacity) {
  Slice attribute;
  *value = fallback;
  if (attribute_find(tag, name, &attribute) &&
      !parse_length(attribute, value)) {
    return set_error(error, error_capacity, "invalid SVG %s", name);
  }
  return 1;
}

static i32 build_shape_path(const Tag *tag, Slice name, Matrix matrix,
                            Path *path, char *error, size_t error_capacity) {
  memset(path, 0, sizeof(*path));
  path->matrix = matrix;
  path->error = error;
  path->error_capacity = error_capacity;

  if (slice_equal(name, "path")) {
    Slice data;
    if (!attribute_find(tag, "d", &data)) {
      return set_error(error, error_capacity, "SVG <path> is missing d");
    }
    return parse_path_data(data, matrix, path, error, error_capacity);
  }
  if (slice_equal(name, "line")) {
    f64 x1;
    f64 y1;
    f64 x2;
    f64 y2;
    if (!optional_length(tag, "x1", 0, &x1, error, error_capacity) ||
        !optional_length(tag, "y1", 0, &y1, error, error_capacity) ||
        !optional_length(tag, "x2", 0, &x2, error, error_capacity) ||
        !optional_length(tag, "y2", 0, &y2, error, error_capacity) ||
        !path_begin_contour(path, x1, y1) || !path_line_to(path, x2, y2)) {
      return 0;
    }
    return 1;
  }
  if (slice_equal(name, "rect")) {
    f64 x;
    f64 y;
    f64 width;
    f64 height;
    f64 rx;
    f64 ry;
    if (!optional_length(tag, "x", 0, &x, error, error_capacity) ||
        !optional_length(tag, "y", 0, &y, error, error_capacity) ||
        !required_length(tag, "width", &width, error, error_capacity) ||
        !required_length(tag, "height", &height, error, error_capacity) ||
        !optional_length(tag, "rx", 0, &rx, error, error_capacity) ||
        !optional_length(tag, "ry", 0, &ry, error, error_capacity)) {
      return 0;
    }
    if (width < 0 || height < 0 || rx < 0 || ry < 0) {
      return set_error(error, error_capacity, "negative SVG rectangle size");
    }
    if (rx == 0 && ry != 0) {
      rx = ry;
    }
    if (ry == 0 && rx != 0) {
      ry = rx;
    }
    rx = fmin(rx, width / 2);
    ry = fmin(ry, height / 2);
    if (!path_begin_contour(path, x + rx, y) ||
        !path_line_to(path, x + width - rx, y)) {
      return 0;
    }
    if (rx > 0 && !path_arc_to(path, x + width - rx, y, rx, ry, 0, 0, 1,
                               x + width, y + ry)) {
      return 0;
    }
    if (!path_line_to(path, x + width, y + height - ry)) {
      return 0;
    }
    if (rx > 0 &&
        !path_arc_to(path, x + width, y + height - ry, rx, ry, 0, 0, 1,
                     x + width - rx, y + height)) {
      return 0;
    }
    if (!path_line_to(path, x + rx, y + height)) {
      return 0;
    }
    if (rx > 0 &&
        !path_arc_to(path, x + rx, y + height, rx, ry, 0, 0, 1, x,
                     y + height - ry)) {
      return 0;
    }
    if (!path_line_to(path, x, y + ry)) {
      return 0;
    }
    if (rx > 0 && !path_arc_to(path, x, y + ry, rx, ry, 0, 0, 1, x + rx, y)) {
      return 0;
    }
    path->contours[path->contour_count - 1].closed = 1;
    return 1;
  }
  if (slice_equal(name, "circle") || slice_equal(name, "ellipse")) {
    f64 cx;
    f64 cy;
    f64 rx;
    f64 ry;
    f64 pixel_radius;
    i32 segments;
    i32 index;
    if (!optional_length(tag, "cx", 0, &cx, error, error_capacity) ||
        !optional_length(tag, "cy", 0, &cy, error, error_capacity)) {
      return 0;
    }
    if (slice_equal(name, "circle")) {
      if (!required_length(tag, "r", &rx, error, error_capacity)) {
        return 0;
      }
      ry = rx;
    } else if (!required_length(tag, "rx", &rx, error, error_capacity) ||
               !required_length(tag, "ry", &ry, error, error_capacity)) {
      return 0;
    }
    if (rx < 0 || ry < 0) {
      return set_error(error, error_capacity, "negative SVG ellipse radius");
    }
    pixel_radius = fmax(rx, ry) * matrix_scale(matrix);
    segments = (i32)ceil(2 * PI * sqrt(fmax(pixel_radius, 1.0)));
    if (segments < 16) {
      segments = 16;
    }
    if (segments > 4096) {
      segments = 4096;
    }
    if (!path_begin_contour(path, cx + rx, cy)) {
      return 0;
    }
    for (index = 1; index < segments; index++) {
      f64 angle = 2 * PI * index / segments;
      if (!path_line_to(path, cx + cos(angle) * rx, cy + sin(angle) * ry)) {
        return 0;
      }
    }
    path->contours[path->contour_count - 1].closed = 1;
    return 1;
  }
  if (slice_equal(name, "polyline") || slice_equal(name, "polygon")) {
    Slice points;
    PathParser parser;
    f64 x;
    f64 y;
    if (!attribute_find(tag, "points", &points)) {
      return set_error(error, error_capacity, "SVG polygon is missing points");
    }
    parser = (PathParser){points.begin, points.end};
    if (!path_number(&parser, &x) || !path_number(&parser, &y) ||
        !path_begin_contour(path, x, y)) {
      return set_error(error, error_capacity, "invalid SVG polygon points");
    }
    while (path_has_number(&parser)) {
      if (!path_number(&parser, &x) || !path_number(&parser, &y) ||
          !path_line_to(path, x, y)) {
        return set_error(error, error_capacity, "invalid SVG polygon points");
      }
    }
    path_skip_separators(&parser);
    if (parser.cursor != parser.end) {
      return set_error(error, error_capacity, "invalid SVG polygon points");
    }
    if (slice_equal(name, "polygon")) {
      path->contours[path->contour_count - 1].closed = 1;
    }
    return 1;
  }
  return set_error(error, error_capacity, "internal unsupported shape");
}

static i32 tag_is_shape(Slice name) {
  return slice_equal(name, "path") || slice_equal(name, "rect") ||
         slice_equal(name, "circle") || slice_equal(name, "ellipse") ||
         slice_equal(name, "line") || slice_equal(name, "polyline") ||
         slice_equal(name, "polygon");
}

static i32 tag_is_unsupported(Slice name) {
  static const char *unsupported[] = {
      "text",      "tspan",        "image",      "use",
      "linearGradient", "radialGradient", "filter",     "mask",
      "clipPath",  "pattern",      "foreignObject", "style"};
  size_t index;
  for (index = 0; index < ARRAY_COUNT(unsupported); index++) {
    if (slice_equal(name, unsupported[index])) {
      return 1;
    }
  }
  return 0;
}

static i32 find_svg_viewbox(const char *source, size_t length, f64 *view_x,
                            f64 *view_y, f64 *view_width, f64 *view_height,
                            char *error, size_t error_capacity) {
  const char *cursor = source;
  const char *end = source + length;
  Tag tag;
  Slice value;
  f64 width = 0;
  f64 height = 0;

  while (next_tag(&cursor, end, &tag, error, error_capacity)) {
    Slice name = local_name(tag.name);
    if (tag.closing) {
      continue;
    }
    if (!slice_equal(name, "svg")) {
      return set_error(error, error_capacity,
                       "the first SVG element must be <svg>");
    }
    if (attribute_find(&tag, "viewBox", &value)) {
      f64 numbers[4];
      if (!parse_number_list(value, numbers, 4) || numbers[2] <= 0 ||
          numbers[3] <= 0) {
        return set_error(error, error_capacity, "invalid SVG viewBox");
      }
      *view_x = numbers[0];
      *view_y = numbers[1];
      *view_width = numbers[2];
      *view_height = numbers[3];
      return 1;
    }
    if (!attribute_find(&tag, "width", &value) || !parse_length(value, &width) ||
        !attribute_find(&tag, "height", &value) ||
        !parse_length(value, &height) || width <= 0 || height <= 0) {
      return set_error(error, error_capacity,
                       "SVG needs a positive viewBox or width and height");
    }
    *view_x = 0;
    *view_y = 0;
    *view_width = width;
    *view_height = height;
    return 1;
  }
  if (error[0] != 0) {
    return 0;
  }
  return set_error(error, error_capacity, "input contains no <svg> element");
}

static i32 render_svg(const char *source, size_t length, Image *image,
                      f64 *aspect_width, f64 *aspect_height, char *error,
                      size_t error_capacity) {
  f64 view_x;
  f64 view_y;
  f64 view_width;
  f64 view_height;
  i32 output_width;
  i32 output_height;
  i32 surface_width;
  i32 surface_height;
  size_t pixel_count;
  Image surface;
  Matrix viewport;
  f64 scale;
  Context stack[SVG_MAX_DEPTH];
  i32 depth = 1;
  const char *cursor = source;
  const char *end = source + length;
  Tag tag;
  i32 found_svg = 0;

  memset(image, 0, sizeof(*image));
  error[0] = 0;
  if (!find_svg_viewbox(source, length, &view_x, &view_y, &view_width,
                        &view_height, error, error_capacity)) {
    return 0;
  }
  *aspect_width = view_width;
  *aspect_height = view_height;
  if (view_width >= view_height) {
    output_width = 2048;
    output_height = (i32)lround(2048 * view_height / view_width);
  } else {
    output_height = 2048;
    output_width = (i32)lround(2048 * view_width / view_height);
  }
  if (output_width < 1) {
    output_width = 1;
  }
  if (output_height < 1) {
    output_height = 1;
  }
  surface_width = output_width * SUPERSAMPLE;
  surface_height = output_height * SUPERSAMPLE;
  if (!checked_multiply((size_t)surface_width, (size_t)surface_height,
                        &pixel_count) ||
      !checked_multiply(pixel_count, 4, &pixel_count)) {
    return set_error(error, error_capacity, "SVG dimensions are too large");
  }
  surface = (Image){surface_width, surface_height,
                    (u8 *)calloc(pixel_count, 1)};
  if (surface.pixels == NULL) {
    return set_error(error, error_capacity, "out of memory creating canvas");
  }

  scale = fmin(surface_width / view_width, surface_height / view_height);
  viewport = (Matrix){scale,
                      0,
                      0,
                      scale,
                      (surface_width - view_width * scale) / 2 - view_x * scale,
                      (surface_height - view_height * scale) / 2 - view_y * scale};
  stack[0].matrix = viewport;
  stack[0].style = style_default();
  stack[0].render = 1;

  while (1) {
    i32 next = next_tag(&cursor, end, &tag, error, error_capacity);
    Slice name;
    Context context;
    if (!next) {
      if (error[0] != 0) {
        free(surface.pixels);
        return 0;
      }
      break;
    }
    name = local_name(tag.name);
    if (tag.closing) {
      if (depth > 1) {
        depth--;
      }
      continue;
    }
    if (!found_svg && !slice_equal(name, "svg")) {
      free(surface.pixels);
      return set_error(error, error_capacity,
                       "the first SVG element must be <svg>");
    }
    if (depth >= SVG_MAX_DEPTH) {
      free(surface.pixels);
      return set_error(error, error_capacity,
                       "SVG nesting exceeds %d elements", SVG_MAX_DEPTH);
    }
    if (!make_context(&tag, &stack[depth - 1], &context, error,
                      error_capacity)) {
      free(surface.pixels);
      return 0;
    }

    if (slice_equal(name, "svg")) {
      if (found_svg) {
        free(surface.pixels);
        return set_error(error, error_capacity,
                         "nested <svg> elements are not supported");
      }
      found_svg = 1;
    } else if (tag_is_unsupported(name)) {
      free(surface.pixels);
      return set_error(error, error_capacity,
                       "SVG <%.*s> is not supported by the minimal renderer",
                       (i32)(name.end - name.begin), name.begin);
    } else if (slice_equal(name, "defs") || slice_equal(name, "metadata") ||
               slice_equal(name, "title") || slice_equal(name, "desc")) {
      context.render = 0;
    } else if (tag_is_shape(name) && context.render) {
      Path path;
      if (!build_shape_path(&tag, name, context.matrix, &path, error,
                            error_capacity)) {
        path_free(&path);
        free(surface.pixels);
        return 0;
      }
      if (!draw_path(&surface, &path, &context.style, context.matrix, error,
                     error_capacity)) {
        path_free(&path);
        free(surface.pixels);
        return 0;
      }
      path_free(&path);
    } else if (!slice_equal(name, "g") && !slice_equal(name, "a") &&
               !tag_is_shape(name) && context.render) {
      /* Namespaced editor metadata is harmless; unknown SVG elements are not. */
      if (tag.name.begin == name.begin) {
        free(surface.pixels);
        return set_error(error, error_capacity,
                         "unsupported SVG element <%.*s>",
                         (i32)(name.end - name.begin), name.begin);
      }
      context.render = 0;
    }

    if (!tag.self_closing) {
      stack[depth++] = context;
    }
  }
  if (!found_svg) {
    free(surface.pixels);
    return set_error(error, error_capacity, "input contains no <svg> element");
  }

  image->width = output_width;
  image->height = output_height;
  if (!checked_multiply((size_t)output_width, (size_t)output_height,
                        &pixel_count) ||
      !checked_multiply(pixel_count, 4, &pixel_count)) {
    free(surface.pixels);
    return set_error(error, error_capacity, "output dimensions are too large");
  }
  image->pixels = (u8 *)malloc(pixel_count);
  if (image->pixels == NULL) {
    free(surface.pixels);
    return set_error(error, error_capacity, "out of memory downsampling SVG");
  }

  {
    i32 y;
    for (y = 0; y < output_height; y++) {
      i32 x;
      for (x = 0; x < output_width; x++) {
        u32 red = 0;
        u32 green = 0;
        u32 blue = 0;
        u32 alpha = 0;
        i32 sy;
        u8 *destination = image->pixels +
                          ((size_t)y * output_width + x) * 4;
        for (sy = 0; sy < SUPERSAMPLE; sy++) {
          i32 sx;
          for (sx = 0; sx < SUPERSAMPLE; sx++) {
            const u8 *source_pixel =
                surface.pixels +
                ((size_t)(y * SUPERSAMPLE + sy) * surface_width +
                 x * SUPERSAMPLE + sx) *
                    4;
            red += source_pixel[0];
            green += source_pixel[1];
            blue += source_pixel[2];
            alpha += source_pixel[3];
          }
        }
        red = (red + 2) / 4;
        green = (green + 2) / 4;
        blue = (blue + 2) / 4;
        alpha = (alpha + 2) / 4;
        if (alpha > 0) {
          u32 straight_red = (red * 255 + alpha / 2) / alpha;
          u32 straight_green = (green * 255 + alpha / 2) / alpha;
          u32 straight_blue = (blue * 255 + alpha / 2) / alpha;
          destination[0] = (u8)(straight_red > 255 ? 255 : straight_red);
          destination[1] = (u8)(straight_green > 255 ? 255 : straight_green);
          destination[2] = (u8)(straight_blue > 255 ? 255 : straight_blue);
        } else {
          destination[0] = destination[1] = destination[2] = 0;
        }
        destination[3] = (u8)alpha;
      }
    }
  }
  free(surface.pixels);
  return 1;
}

static void image_free(Image *image) {
  free(image->pixels);
  memset(image, 0, sizeof(*image));
}

static i32 image_resize(const Image *source, i32 maximum_dimension,
                        f64 aspect_width, f64 aspect_height, Image *result,
                        char *error, size_t error_capacity) {
  size_t bytes;
  i32 y;
  if (aspect_width >= aspect_height) {
    result->width = maximum_dimension;
    result->height = (i32)lround(maximum_dimension * aspect_height / aspect_width);
  } else {
    result->height = maximum_dimension;
    result->width = (i32)lround(maximum_dimension * aspect_width / aspect_height);
  }
  if (result->width < 1) {
    result->width = 1;
  }
  if (result->height < 1) {
    result->height = 1;
  }
  if (!checked_multiply((size_t)result->width, (size_t)result->height,
                        &bytes) ||
      !checked_multiply(bytes, 4, &bytes)) {
    return set_error(error, error_capacity, "image size overflow");
  }
  result->pixels = (u8 *)malloc(bytes);
  if (result->pixels == NULL) {
    return set_error(error, error_capacity, "out of memory resizing image");
  }

  for (y = 0; y < result->height; y++) {
    f64 source_y = ((y + 0.5) * source->height / result->height) - 0.5;
    i32 y0 = (i32)floor(source_y);
    i32 y1;
    f64 fy = source_y - y0;
    i32 x;
    if (y0 < 0) {
      y0 = 0;
      fy = 0;
    }
    y1 = y0 + 1;
    if (y1 >= source->height) {
      y1 = source->height - 1;
    }
    for (x = 0; x < result->width; x++) {
      f64 source_x = ((x + 0.5) * source->width / result->width) - 0.5;
      i32 x0 = (i32)floor(source_x);
      i32 x1;
      f64 fx = source_x - x0;
      f64 weights[4];
      const u8 *pixels[4];
      f64 alpha = 0;
      f64 premultiplied[3] = {0, 0, 0};
      i32 sample;
      u8 *destination;
      if (x0 < 0) {
        x0 = 0;
        fx = 0;
      }
      x1 = x0 + 1;
      if (x1 >= source->width) {
        x1 = source->width - 1;
      }
      weights[0] = (1 - fx) * (1 - fy);
      weights[1] = fx * (1 - fy);
      weights[2] = (1 - fx) * fy;
      weights[3] = fx * fy;
      pixels[0] = source->pixels + ((size_t)y0 * source->width + x0) * 4;
      pixels[1] = source->pixels + ((size_t)y0 * source->width + x1) * 4;
      pixels[2] = source->pixels + ((size_t)y1 * source->width + x0) * 4;
      pixels[3] = source->pixels + ((size_t)y1 * source->width + x1) * 4;
      for (sample = 0; sample < 4; sample++) {
        f64 sample_alpha = pixels[sample][3] / 255.0;
        alpha += sample_alpha * weights[sample];
        premultiplied[0] += pixels[sample][0] * sample_alpha * weights[sample];
        premultiplied[1] += pixels[sample][1] * sample_alpha * weights[sample];
        premultiplied[2] += pixels[sample][2] * sample_alpha * weights[sample];
      }
      destination = result->pixels + ((size_t)y * result->width + x) * 4;
      if (alpha > 1e-12) {
        destination[0] = (u8)lround(fmin(255, premultiplied[0] / alpha));
        destination[1] = (u8)lround(fmin(255, premultiplied[1] / alpha));
        destination[2] = (u8)lround(fmin(255, premultiplied[2] / alpha));
      } else {
        destination[0] = destination[1] = destination[2] = 0;
      }
      destination[3] = (u8)lround(fmin(255, alpha * 255));
    }
  }
  return 1;
}

static u32 crc32_update(u32 crc, const u8 *data, size_t length) {
  size_t index;
  crc = ~crc;
  for (index = 0; index < length; index++) {
    i32 bit;
    crc ^= data[index];
    for (bit = 0; bit < 8; bit++) {
      crc = (crc >> 1) ^ (0xedb88320U & (u32)-(i32)(crc & 1));
    }
  }
  return ~crc;
}

static i32 png_chunk(Buffer *png, const char type[4], const u8 *data,
                     size_t length) {
  u32 crc;
  if (length > UINT32_MAX || !buffer_u32be(png, (u32)length) ||
      !buffer_append(png, type, 4) || !buffer_append(png, data, length)) {
    return 0;
  }
  crc = crc32_update(0, (const u8 *)type, 4);
  crc = crc32_update(crc, data, length);
  return buffer_u32be(png, crc);
}

static i32 encode_png(const Image *image, Buffer *png, char *error,
                      size_t error_capacity) {
  static const u8 signature[8] = {137, 80, 78, 71, 13, 10, 26, 10};
  Buffer raw = {0};
  Buffer zlib = {0};
  Buffer header = {0};
  size_t row_bytes;
  size_t raw_size;
  i32 row;
  u32 adler_a = 1;
  u32 adler_b = 0;
  size_t offset;
  i32 ok = 0;

  memset(png, 0, sizeof(*png));
  if (!checked_multiply((size_t)image->width, 4, &row_bytes) ||
      row_bytes > SIZE_MAX - 1 ||
      !checked_multiply(row_bytes + 1, (size_t)image->height, &raw_size) ||
      !buffer_reserve(&raw, raw_size)) {
    set_error(error, error_capacity, "PNG is too large");
    goto cleanup;
  }
  for (row = 0; row < image->height; row++) {
    if (!buffer_u8(&raw, 0) ||
        !buffer_append(&raw,
                       image->pixels + (size_t)row * image->width * 4,
                       row_bytes)) {
      set_error(error, error_capacity, "out of memory encoding PNG");
      goto cleanup;
    }
  }
  if (!buffer_u8(&zlib, 0x78) || !buffer_u8(&zlib, 0x01)) {
    set_error(error, error_capacity, "out of memory encoding PNG");
    goto cleanup;
  }
  offset = 0;
  while (offset < raw.length) {
    size_t block_length = raw.length - offset;
    u16 length16;
    if (block_length > 65535) {
      block_length = 65535;
    }
    length16 = (u16)block_length;
    if (!buffer_u8(&zlib, offset + block_length == raw.length ? 1 : 0) ||
        !buffer_u16le(&zlib, length16) ||
        !buffer_u16le(&zlib, (u16)~length16) ||
        !buffer_append(&zlib, raw.data + offset, block_length)) {
      set_error(error, error_capacity, "out of memory encoding PNG");
      goto cleanup;
    }
    offset += block_length;
  }
  for (offset = 0; offset < raw.length; offset++) {
    adler_a = (adler_a + raw.data[offset]) % 65521;
    adler_b = (adler_b + adler_a) % 65521;
  }
  if (!buffer_u32be(&zlib, (adler_b << 16) | adler_a) ||
      !buffer_u32be(&header, (u32)image->width) ||
      !buffer_u32be(&header, (u32)image->height) ||
      !buffer_u8(&header, 8) || !buffer_u8(&header, 6) ||
      !buffer_u8(&header, 0) || !buffer_u8(&header, 0) ||
      !buffer_u8(&header, 0) || !buffer_append(png, signature, 8) ||
      !png_chunk(png, "IHDR", header.data, header.length) ||
      !png_chunk(png, "IDAT", zlib.data, zlib.length) ||
      !png_chunk(png, "IEND", NULL, 0)) {
    set_error(error, error_capacity, "out of memory encoding PNG");
    goto cleanup;
  }
  ok = 1;

cleanup:
  buffer_free(&raw);
  buffer_free(&zlib);
  buffer_free(&header);
  if (!ok) {
    buffer_free(png);
  }
  return ok;
}

static void bit_writer_put(BitWriter *writer, u32 value, i32 count) {
  if (writer->failed || count < 0 || count > 24) {
    writer->failed = 1;
    return;
  }
  writer->bits |= ((u64)value & ((1ULL << count) - 1)) << writer->bit_count;
  writer->bit_count += count;
  while (writer->bit_count >= 8) {
    if (!buffer_u8(writer->buffer, (u8)writer->bits)) {
      writer->failed = 1;
      return;
    }
    writer->bits >>= 8;
    writer->bit_count -= 8;
  }
}

static void bit_writer_finish(BitWriter *writer) {
  if (!writer->failed && writer->bit_count > 0) {
    if (!buffer_u8(writer->buffer, (u8)writer->bits)) {
      writer->failed = 1;
    }
    writer->bits = 0;
    writer->bit_count = 0;
  }
}

static u32 reverse_bits(u32 value, i32 length) {
  u32 result = 0;
  i32 index;
  for (index = 0; index < length; index++) {
    result = (result << 1) | ((value >> index) & 1U);
  }
  return result;
}

static i32 build_huffman_codes(const u8 *lengths, size_t count, u16 *codes) {
  u32 length_counts[16] = {0};
  u32 next_code[16] = {0};
  u32 code = 0;
  i32 bits;
  size_t symbol;
  for (symbol = 0; symbol < count; symbol++) {
    if (lengths[symbol] > 15) {
      return 0;
    }
    if (lengths[symbol] != 0) {
      length_counts[lengths[symbol]]++;
    }
  }
  for (bits = 1; bits <= 15; bits++) {
    code = (code + length_counts[bits - 1]) << 1;
    next_code[bits] = code;
  }
  for (symbol = 0; symbol < count; symbol++) {
    i32 length = lengths[symbol];
    codes[symbol] = length == 0
                        ? 0
                        : (u16)reverse_bits(next_code[length]++, length);
  }
  return 1;
}

static void webp_simple_code(BitWriter *writer, u8 symbol) {
  bit_writer_put(writer, 1, 1);
  bit_writer_put(writer, 0, 1);
  if (symbol <= 1) {
    bit_writer_put(writer, 0, 1);
    bit_writer_put(writer, symbol, 1);
  } else {
    bit_writer_put(writer, 1, 1);
    bit_writer_put(writer, symbol, 8);
  }
}

static i32 webp_fixed_code(BitWriter *writer, size_t alphabet_size,
                           u8 *lengths, u16 *codes) {
  static const i32 order[19] = {17, 18, 0, 1, 2, 3, 4, 5, 16, 6,
                                7,  8,  9, 10, 11, 12, 13, 14, 15};
  u8 code_length_lengths[19] = {0};
  u16 code_length_codes[19] = {0};
  size_t symbol;
  i32 index;

  memset(lengths, 0, alphabet_size);
  if (alphabet_size == 280) {
    for (symbol = 0; symbol < 232; symbol++) {
      lengths[symbol] = 8;
    }
    for (; symbol < alphabet_size; symbol++) {
      lengths[symbol] = 9;
    }
  } else if (alphabet_size == 256) {
    memset(lengths, 8, alphabet_size);
  } else {
    return 0;
  }
  if (!build_huffman_codes(lengths, alphabet_size, codes)) {
    return 0;
  }

  code_length_lengths[0] = 1;
  code_length_lengths[8] = 2;
  code_length_lengths[9] = 2;
  if (!build_huffman_codes(code_length_lengths, 19, code_length_codes)) {
    return 0;
  }
  bit_writer_put(writer, 0, 1);
  bit_writer_put(writer, 9, 4);
  for (index = 0; index < 13; index++) {
    bit_writer_put(writer, code_length_lengths[order[index]], 3);
  }
  bit_writer_put(writer, 0, 1);
  for (symbol = 0; symbol < alphabet_size; symbol++) {
    u8 length = lengths[symbol];
    bit_writer_put(writer, code_length_codes[length],
                   code_length_lengths[length]);
  }
  return !writer->failed;
}

static i32 encode_webp(const Image *image, Buffer *webp, char *error,
                       size_t error_capacity) {
  Buffer payload = {0};
  BitWriter writer;
  u8 green_lengths[280];
  u16 green_codes[280];
  u8 channel_lengths[256];
  u16 channel_codes[256];
  size_t pixel_count;
  size_t index;
  i32 uses_alpha = 0;
  i32 ok = 0;

  memset(webp, 0, sizeof(*webp));
  if (image->width < 1 || image->height < 1 || image->width > 16384 ||
      image->height > 16384 ||
      !checked_multiply((size_t)image->width, (size_t)image->height,
                        &pixel_count)) {
    return set_error(error, error_capacity, "invalid WebP dimensions");
  }
  for (index = 0; index < pixel_count; index++) {
    if (image->pixels[index * 4 + 3] != 255) {
      uses_alpha = 1;
      break;
    }
  }
  if (!buffer_u8(&payload, 0x2f)) {
    return set_error(error, error_capacity, "out of memory encoding WebP");
  }
  writer = (BitWriter){&payload, 0, 0, 0};
  bit_writer_put(&writer, (u32)(image->width - 1), 14);
  bit_writer_put(&writer, (u32)(image->height - 1), 14);
  bit_writer_put(&writer, (u32)uses_alpha, 1);
  bit_writer_put(&writer, 0, 3);
  bit_writer_put(&writer, 0, 1);
  bit_writer_put(&writer, 0, 1);
  bit_writer_put(&writer, 0, 1);

  if (!webp_fixed_code(&writer, 280, green_lengths, green_codes) ||
      !webp_fixed_code(&writer, 256, channel_lengths, channel_codes) ||
      !webp_fixed_code(&writer, 256, channel_lengths, channel_codes) ||
      !webp_fixed_code(&writer, 256, channel_lengths, channel_codes)) {
    set_error(error, error_capacity, "cannot build WebP prefix codes");
    goto cleanup;
  }
  webp_simple_code(&writer, 0);

  for (index = 0; index < pixel_count; index++) {
    const u8 *pixel = image->pixels + index * 4;
    u8 green = pixel[1];
    u8 red = pixel[0];
    u8 blue = pixel[2];
    u8 alpha = pixel[3];
    bit_writer_put(&writer, green_codes[green], green_lengths[green]);
    bit_writer_put(&writer, channel_codes[red], channel_lengths[red]);
    bit_writer_put(&writer, channel_codes[blue], channel_lengths[blue]);
    bit_writer_put(&writer, channel_codes[alpha], channel_lengths[alpha]);
  }
  bit_writer_finish(&writer);
  if (writer.failed || payload.length > UINT32_MAX ||
      !buffer_append(webp, "RIFF", 4) ||
      !buffer_u32le(webp, (u32)(4 + 8 + payload.length +
                               (payload.length & 1))) ||
      !buffer_append(webp, "WEBP", 4) || !buffer_append(webp, "VP8L", 4) ||
      !buffer_u32le(webp, (u32)payload.length) ||
      !buffer_append(webp, payload.data, payload.length) ||
      ((payload.length & 1) && !buffer_u8(webp, 0))) {
    set_error(error, error_capacity, "out of memory encoding WebP");
    goto cleanup;
  }
  ok = 1;

cleanup:
  buffer_free(&payload);
  if (!ok) {
    buffer_free(webp);
  }
  return ok;
}

static i32 encode_ico(Buffer pngs[3], Buffer *ico, char *error,
                      size_t error_capacity) {
  size_t index;
  u32 offset = 6 + 3 * 16;
  memset(ico, 0, sizeof(*ico));
  if (!buffer_u16le(ico, 0) || !buffer_u16le(ico, 1) ||
      !buffer_u16le(ico, 3)) {
    goto memory_error;
  }
  for (index = 0; index < 3; index++) {
    u32 width;
    u32 height;
    if (pngs[index].length < 24) {
      buffer_free(ico);
      return set_error(error, error_capacity,
                       "invalid PNG while encoding favicon.ico");
    }
    width = ((u32)pngs[index].data[16] << 24) |
            ((u32)pngs[index].data[17] << 16) |
            ((u32)pngs[index].data[18] << 8) | pngs[index].data[19];
    height = ((u32)pngs[index].data[20] << 24) |
             ((u32)pngs[index].data[21] << 16) |
             ((u32)pngs[index].data[22] << 8) | pngs[index].data[23];
    if (pngs[index].length > UINT32_MAX || width > 256 || height > 256 ||
        !buffer_u8(ico, width == 256 ? 0 : (u8)width) ||
        !buffer_u8(ico, height == 256 ? 0 : (u8)height) ||
        !buffer_u8(ico, 0) || !buffer_u8(ico, 0) ||
        !buffer_u16le(ico, 1) || !buffer_u16le(ico, 32) ||
        !buffer_u32le(ico, (u32)pngs[index].length) ||
        !buffer_u32le(ico, offset)) {
      goto memory_error;
    }
    offset += (u32)pngs[index].length;
  }
  for (index = 0; index < 3; index++) {
    if (!buffer_append(ico, pngs[index].data, pngs[index].length)) {
      goto memory_error;
    }
  }
  return 1;

memory_error:
  buffer_free(ico);
  return set_error(error, error_capacity, "out of memory encoding favicon.ico");
}

static i32 optimize_svg(const u8 *source, size_t length, Buffer *optimized,
                        char *error, size_t error_capacity) {
  size_t index = 0;
  memset(optimized, 0, sizeof(*optimized));
  while (index < length) {
    if (index + 4 <= length && memcmp(source + index, "<!--", 4) == 0) {
      size_t end = index + 4;
      while (end + 3 <= length && memcmp(source + end, "-->", 3) != 0) {
        end++;
      }
      if (end + 3 > length) {
        buffer_free(optimized);
        return set_error(error, error_capacity, "unterminated SVG comment");
      }
      index = end + 3;
      continue;
    }
    if (isspace(source[index])) {
      size_t whitespace_start = index;
      size_t next;
      while (index < length && isspace(source[index])) {
        index++;
      }
      next = index;
      if (optimized->length > 0 && optimized->data[optimized->length - 1] == '>' &&
          next < length && source[next] == '<') {
        continue;
      }
      if (!buffer_append(optimized, source + whitespace_start,
                         index - whitespace_start)) {
        goto memory_error;
      }
      continue;
    }
    if (!buffer_u8(optimized, source[index++])) {
      goto memory_error;
    }
  }
  while (optimized->length > 0 &&
         isspace(optimized->data[optimized->length - 1])) {
    optimized->length--;
  }
  if (!buffer_u8(optimized, '\n')) {
    goto memory_error;
  }
  return 1;

memory_error:
  buffer_free(optimized);
  return set_error(error, error_capacity, "out of memory optimizing SVG");
}

static i32 create_assets(const char *input_path) {
  static const i32 png_sizes[] = {16, 32, 48, 64, 128, 256, 512, 1024, 2048};
  static const i32 webp_sizes[] = {256, 512, 1024};
  static const i32 ico_sizes[] = {16, 32, 48};
  Buffer input = {0};
  Buffer optimized = {0};
  Buffer ico_pngs[3] = {{0}, {0}, {0}};
  Buffer ico = {0};
  Image master = {0};
  f64 aspect_width;
  f64 aspect_height;
  char error[512] = {0};
  size_t index;
  i32 status = 1;

  if (!read_file(input_path, &input, error, sizeof(error)) ||
      !render_svg((const char *)input.data, input.length, &master,
                  &aspect_width, &aspect_height, error, sizeof(error)) ||
      !optimize_svg(input.data, input.length, &optimized, error,
                    sizeof(error))) {
    fprintf(stderr, "archetypon: %s\n", error);
    status = 0;
    goto cleanup;
  }
  if (!ensure_directory("svg", error, sizeof(error)) ||
      !ensure_directory("png", error, sizeof(error)) ||
      !ensure_directory("webp", error, sizeof(error)) ||
      !ensure_directory("favicon", error, sizeof(error)) ||
      !write_file("svg/original.svg", input.data, input.length, error,
                  sizeof(error)) ||
      !write_file("svg/optimized.svg", optimized.data, optimized.length,
                  error, sizeof(error)) ||
      !write_file("favicon/favicon.svg", optimized.data, optimized.length,
                  error, sizeof(error))) {
    fprintf(stderr, "archetypon: %s\n", error);
    status = 0;
    goto cleanup;
  }

  for (index = 0; index < ARRAY_COUNT(png_sizes); index++) {
    Image resized = {0};
    Buffer png = {0};
    char path[64];
    size_t ico_index;
    i32 files_written;
    if (!image_resize(&master, png_sizes[index], aspect_width, aspect_height,
                      &resized, error, sizeof(error)) ||
        !encode_png(&resized, &png, error, sizeof(error))) {
      image_free(&resized);
      buffer_free(&png);
      fprintf(stderr, "archetypon: %s\n", error);
      status = 0;
      goto cleanup;
    }
    snprintf(path, sizeof(path), "png/%d.png", png_sizes[index]);
    files_written =
        write_file(path, png.data, png.length, error, sizeof(error));
    if (files_written &&
        (png_sizes[index] == 16 || png_sizes[index] == 32)) {
      snprintf(path, sizeof(path), "favicon/favicon-%d.png", png_sizes[index]);
      files_written =
          write_file(path, png.data, png.length, error, sizeof(error));
    }
    if (!files_written) {
      image_free(&resized);
      buffer_free(&png);
      fprintf(stderr, "archetypon: %s\n", error);
      status = 0;
      goto cleanup;
    }
    for (ico_index = 0; ico_index < ARRAY_COUNT(ico_sizes); ico_index++) {
      if (png_sizes[index] == ico_sizes[ico_index]) {
        ico_pngs[ico_index] = png;
        memset(&png, 0, sizeof(png));
      }
    }
    buffer_free(&png);
    image_free(&resized);
  }

  if (!encode_ico(ico_pngs, &ico, error, sizeof(error)) ||
      !write_file("favicon/favicon.ico", ico.data, ico.length, error,
                  sizeof(error))) {
    fprintf(stderr, "archetypon: %s\n", error);
    status = 0;
    goto cleanup;
  }

  for (index = 0; index < ARRAY_COUNT(webp_sizes); index++) {
    Image resized = {0};
    Buffer webp = {0};
    char path[64];
    if (!image_resize(&master, webp_sizes[index], aspect_width, aspect_height,
                      &resized, error, sizeof(error)) ||
        !encode_webp(&resized, &webp, error, sizeof(error))) {
      image_free(&resized);
      buffer_free(&webp);
      fprintf(stderr, "archetypon: %s\n", error);
      status = 0;
      goto cleanup;
    }
    snprintf(path, sizeof(path), "webp/%d.webp", webp_sizes[index]);
    if (!write_file(path, webp.data, webp.length, error, sizeof(error))) {
      image_free(&resized);
      buffer_free(&webp);
      fprintf(stderr, "archetypon: %s\n", error);
      status = 0;
      goto cleanup;
    }
    buffer_free(&webp);
    image_free(&resized);
  }

  printf("Created SVG, PNG, WebP, and favicon assets in %s\n", ".");

cleanup:
  for (index = 0; index < ARRAY_COUNT(ico_pngs); index++) {
    buffer_free(&ico_pngs[index]);
  }
  buffer_free(&ico);
  buffer_free(&optimized);
  buffer_free(&input);
  image_free(&master);
  return status;
}

static void print_usage(FILE *stream) {
  fprintf(stream,
          "Usage: archetypon create <file.svg>\n"
          "       archetypon --help\n");
}

int main(int argument_count, char **arguments) {
  if (argument_count == 2 && strcmp(arguments[1], "--help") == 0) {
    print_usage(stdout);
    return 0;
  }
  if (argument_count != 3 || strcmp(arguments[1], "create") != 0) {
    print_usage(stderr);
    return 2;
  }
  return create_assets(arguments[2]) ? 0 : 1;
}
