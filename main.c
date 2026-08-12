#include "archetypon.h"

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define INPUT_LIMIT (32U * 1024U * 1024U)
#define MASTER_EDGE 2048

enum {
  OUTPUT_SVG = 1U << 0,
  OUTPUT_PNG = 1U << 1,
  OUTPUT_WEBP = 1U << 2,
  OUTPUT_ICO = 1U << 3,
  OUTPUT_ALL = OUTPUT_SVG | OUTPUT_PNG | OUTPUT_WEBP | OUTPUT_ICO
};

static int32_t read_file(const char *path, ArchetyponBuffer *buffer,
                         char *error, size_t error_capacity) {
  FILE *file;
  long length;

  file = fopen(path, "rb");
  if (file == NULL) {
    snprintf(error, error_capacity, "cannot open '%s': %s", path,
             strerror(errno));
    return 0;
  }
  if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 ||
      fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    snprintf(error, error_capacity, "cannot measure '%s'", path);
    return 0;
  }
  if ((uint64_t)length > INPUT_LIMIT) {
    fclose(file);
    snprintf(error, error_capacity, "SVG exceeds the 32 MiB limit");
    return 0;
  }

  buffer->data = (uint8_t *)malloc((size_t)length + 1);
  if (buffer->data == NULL) {
    fclose(file);
    snprintf(error, error_capacity, "out of memory reading '%s'", path);
    return 0;
  }
  if (length > 0 &&
      fread(buffer->data, 1, (size_t)length, file) != (size_t)length) {
    fclose(file);
    archetypon_buffer_free(buffer);
    snprintf(error, error_capacity, "cannot read '%s'", path);
    return 0;
  }
  fclose(file);
  buffer->length = (size_t)length;
  buffer->capacity = (size_t)length + 1;
  buffer->data[buffer->length] = 0;
  return 1;
}

static int32_t write_file(const char *path, const void *data, size_t length,
                          char *error, size_t error_capacity) {
  FILE *file = fopen(path, "wb");
  if (file == NULL) {
    snprintf(error, error_capacity, "cannot create '%s': %s", path,
             strerror(errno));
    return 0;
  }
  if (length > 0 && fwrite(data, 1, length, file) != length) {
    fclose(file);
    snprintf(error, error_capacity, "cannot write '%s'", path);
    return 0;
  }
  if (fclose(file) != 0) {
    snprintf(error, error_capacity, "cannot finish '%s'", path);
    return 0;
  }
  return 1;
}

static int32_t ensure_directory(const char *path, char *error,
                                size_t error_capacity) {
  struct stat status;

  if (mkdir(path, S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH) == 0) {
    return 1;
  }
  if (errno == EEXIST && stat(path, &status) == 0 && S_ISDIR(status.st_mode)) {
    return 1;
  }
  snprintf(error, error_capacity, "cannot create directory '%s': %s", path,
           strerror(errno));
  return 0;
}

static int32_t create_assets(const char *input_path, uint32_t outputs) {
  static const int32_t png_sizes[] = {16,  32,  48,   64,  128,
                                      256, 512, 1024, 2048};
  static const int32_t webp_sizes[] = {256, 512, 1024};
  static const int32_t ico_sizes[] = {16, 32, 48};
  ArchetyponBuffer input = {0};
  ArchetyponBuffer optimized = {0};
  ArchetyponBuffer ico_pngs[3] = {{0}, {0}, {0}};
  ArchetyponBuffer ico = {0};
  ArchetyponImage master = {0};
  double aspect_width;
  double aspect_height;
  int32_t master_width;
  int32_t master_height;
  char error[512] = {0};
  size_t index;
  int32_t status = 1;

  if (!read_file(input_path, &input, error, sizeof(error)) ||
      !archetypon_svg_canvas_size((const char *)input.data, input.length,
                                  &aspect_width, &aspect_height, error,
                                  sizeof(error))) {
    fprintf(stderr, "archetypon: %s\n", error);
    status = 0;
    goto cleanup;
  }
  if (aspect_width >= aspect_height) {
    master_width = MASTER_EDGE;
    master_height = (int32_t)lround(MASTER_EDGE * aspect_height / aspect_width);
  } else {
    master_height = MASTER_EDGE;
    master_width = (int32_t)lround(MASTER_EDGE * aspect_width / aspect_height);
  }
  if (master_width < 1) {
    master_width = 1;
  }
  if (master_height < 1) {
    master_height = 1;
  }

  if (!archetypon_svg_render((const char *)input.data, input.length,
                             master_width, master_height, &master, error,
                             sizeof(error)) ||
      !archetypon_svg_optimize(input.data, input.length, &optimized, error,
                               sizeof(error))) {
    fprintf(stderr, "archetypon: %s\n", error);
    status = 0;
    goto cleanup;
  }
  if (((outputs & OUTPUT_SVG) != 0 &&
       (!ensure_directory("svg", error, sizeof(error)) ||
        !write_file("svg/original.svg", input.data, input.length, error,
                    sizeof(error)) ||
        !write_file("svg/optimized.svg", optimized.data, optimized.length,
                    error, sizeof(error)))) ||
      ((outputs & OUTPUT_PNG) != 0 &&
       !ensure_directory("png", error, sizeof(error))) ||
      ((outputs & OUTPUT_WEBP) != 0 &&
       !ensure_directory("webp", error, sizeof(error))) ||
      ((outputs & OUTPUT_ICO) != 0 &&
       (!ensure_directory("favicon", error, sizeof(error)) ||
        (outputs == OUTPUT_ALL &&
         !write_file("favicon/favicon.svg", optimized.data, optimized.length,
                     error, sizeof(error)))))) {
    fprintf(stderr, "archetypon: %s\n", error);
    status = 0;
    goto cleanup;
  }

  if ((outputs & (OUTPUT_PNG | OUTPUT_ICO)) != 0) {
    for (index = 0; index < ARRAY_COUNT(png_sizes); index++) {
      ArchetyponImage resized = {0};
      ArchetyponBuffer png = {0};
      char path[64];
      size_t ico_index;
      int32_t files_written = 1;
      int32_t favicon_size = png_sizes[index] == 16 || png_sizes[index] == 32 ||
                             png_sizes[index] == 48;

      if ((outputs & OUTPUT_PNG) == 0 && !favicon_size) {
        continue;
      }
      if (!archetypon_image_resize(&master, png_sizes[index], aspect_width,
                                   aspect_height, &resized, error,
                                   sizeof(error)) ||
          !archetypon_png_encode(&resized, &png, error, sizeof(error))) {
        archetypon_image_free(&resized);
        archetypon_buffer_free(&png);
        fprintf(stderr, "archetypon: %s\n", error);
        status = 0;
        goto cleanup;
      }
      if ((outputs & OUTPUT_PNG) != 0) {
        snprintf(path, sizeof(path), "png/%d.png", png_sizes[index]);
        files_written =
            write_file(path, png.data, png.length, error, sizeof(error));
      }
      if (files_written && outputs == OUTPUT_ALL &&
          (png_sizes[index] == 16 || png_sizes[index] == 32)) {
        snprintf(path, sizeof(path), "favicon/favicon-%d.png",
                 png_sizes[index]);
        files_written =
            write_file(path, png.data, png.length, error, sizeof(error));
      }
      if (!files_written) {
        archetypon_image_free(&resized);
        archetypon_buffer_free(&png);
        fprintf(stderr, "archetypon: %s\n", error);
        status = 0;
        goto cleanup;
      }
      if ((outputs & OUTPUT_ICO) != 0) {
        for (ico_index = 0; ico_index < ARRAY_COUNT(ico_sizes); ico_index++) {
          if (png_sizes[index] == ico_sizes[ico_index]) {
            ico_pngs[ico_index] = png;
            memset(&png, 0, sizeof(png));
          }
        }
      }
      archetypon_buffer_free(&png);
      archetypon_image_free(&resized);
    }
  }

  if ((outputs & OUTPUT_ICO) != 0 &&
      (!archetypon_ico_encode(ico_pngs, &ico, error, sizeof(error)) ||
       !write_file("favicon/favicon.ico", ico.data, ico.length, error,
                   sizeof(error)))) {
    fprintf(stderr, "archetypon: %s\n", error);
    status = 0;
    goto cleanup;
  }

  if ((outputs & OUTPUT_WEBP) != 0) {
    for (index = 0; index < ARRAY_COUNT(webp_sizes); index++) {
      ArchetyponImage resized = {0};
      ArchetyponBuffer webp = {0};
      char path[64];

      if (!archetypon_image_resize(&master, webp_sizes[index], aspect_width,
                                   aspect_height, &resized, error,
                                   sizeof(error)) ||
          !archetypon_webp_encode(&resized, &webp, error, sizeof(error))) {
        archetypon_image_free(&resized);
        archetypon_buffer_free(&webp);
        fprintf(stderr, "archetypon: %s\n", error);
        status = 0;
        goto cleanup;
      }
      snprintf(path, sizeof(path), "webp/%d.webp", webp_sizes[index]);
      if (!write_file(path, webp.data, webp.length, error, sizeof(error))) {
        archetypon_image_free(&resized);
        archetypon_buffer_free(&webp);
        fprintf(stderr, "archetypon: %s\n", error);
        status = 0;
        goto cleanup;
      }
      archetypon_buffer_free(&webp);
      archetypon_image_free(&resized);
    }
  }

  if (outputs == OUTPUT_ALL) {
    printf("Created SVG, PNG, WebP, and favicon assets in %s\n", ".");
  } else if (outputs == OUTPUT_SVG) {
    printf("Created SVG assets in %s\n", ".");
  } else if (outputs == OUTPUT_PNG) {
    printf("Created PNG assets in %s\n", ".");
  } else if (outputs == OUTPUT_WEBP) {
    printf("Created WebP assets in %s\n", ".");
  } else {
    printf("Created ICO asset in %s\n", ".");
  }
cleanup:
  for (index = 0; index < ARRAY_COUNT(ico_pngs); index++) {
    archetypon_buffer_free(&ico_pngs[index]);
  }
  archetypon_buffer_free(&ico);
  archetypon_buffer_free(&optimized);
  archetypon_buffer_free(&input);
  archetypon_image_free(&master);
  return status;
}

static void print_usage(FILE *stream) {
  fprintf(stream, "Usage: archetypon create <file.svg>\n"
                  "       archetypon create <format> <file.svg>\n"
                  "       archetypon --help\n");
}

static void print_help(void) {
  print_usage(stdout);
  printf("\n"
         "Create every asset format by default, or select one format:\n"
         "svg    Create only svg/ assets\n"
         "png    Create only png/ assets\n"
         "webp   Create only webp/ assets\n"
         "ico    Create only favicon/favicon.ico\n");
}

int main(int argument_count, char **arguments) {
  const char *input_path;
  uint32_t outputs = OUTPUT_ALL;

  if (argument_count == 2 && strcmp(arguments[1], "--help") == 0) {
    print_help();
    return 0;
  }
  if (argument_count == 3 && strcmp(arguments[1], "create") == 0) {
    input_path = arguments[2];
  } else if (argument_count == 4 && strcmp(arguments[1], "create") == 0) {
    input_path = arguments[3];
    if (strcmp(arguments[2], "svg") == 0) {
      outputs = OUTPUT_SVG;
    } else if (strcmp(arguments[2], "png") == 0) {
      outputs = OUTPUT_PNG;
    } else if (strcmp(arguments[2], "webp") == 0) {
      outputs = OUTPUT_WEBP;
    } else if (strcmp(arguments[2], "ico") == 0) {
      outputs = OUTPUT_ICO;
    } else {
      fprintf(stderr, "archetypon: unknown format '%s'\n", arguments[2]);
      print_usage(stderr);
      return 2;
    }
  } else {
    print_usage(stderr);
    return 2;
  }
  return create_assets(input_path, outputs) ? 0 : 1;
}
