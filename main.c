#include "archetypon.h"

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define INPUT_LIMIT (32U * 1024U * 1024U)
#define SOURCE_EDGE 2048

enum output_format {
	OUTPUT_SVG = 1U << 0,
	OUTPUT_PNG = 1U << 1,
	OUTPUT_WEBP = 1U << 2,
	OUTPUT_ICO = 1U << 3,
	OUTPUT_ALL = OUTPUT_SVG | OUTPUT_PNG | OUTPUT_WEBP | OUTPUT_ICO
};

struct asset_job {
	struct archetypon_buffer input;
	struct archetypon_buffer optimized;
	struct archetypon_buffer ico_pngs[3];
	struct archetypon_image source_image;
	double width;
	double height;
	uint32_t outputs;
	char error[512];
};

static int read_file(const char *path, struct archetypon_buffer *buffer,
		     char *error, size_t error_capacity)
{
	FILE *file;
	long length;

	file = fopen(path, "rb");
	if (!file) {
		snprintf(error, error_capacity, "cannot open '%s': %s", path,
			 strerror(errno));
		return -1;
	}
	if (fseek(file, 0, SEEK_END) != 0) {
		snprintf(error, error_capacity, "cannot seek '%s'", path);
		goto out_close;
	}
	length = ftell(file);
	if (length < 0 || fseek(file, 0, SEEK_SET) != 0) {
		snprintf(error, error_capacity, "cannot measure '%s'", path);
		goto out_close;
	}
	if ((uint64_t)length > INPUT_LIMIT) {
		snprintf(error, error_capacity, "SVG exceeds the 32 MiB limit");
		goto out_close;
	}
	buffer->data = malloc((size_t)length + 1);
	if (!buffer->data) {
		snprintf(error, error_capacity, "out of memory reading '%s'",
			 path);
		goto out_close;
	}
	if (length > 0 &&
	    fread(buffer->data, 1, (size_t)length, file) != (size_t)length) {
		archetypon_buffer_free(buffer);
		snprintf(error, error_capacity, "cannot read '%s'", path);
		goto out_close;
	}
	if (fclose(file) != 0) {
		archetypon_buffer_free(buffer);
		snprintf(error, error_capacity, "cannot close '%s'", path);
		return -1;
	}
	buffer->length = (size_t)length;
	buffer->capacity = (size_t)length + 1;
	buffer->data[buffer->length] = 0;
	return 0;

out_close:
	fclose(file);
	return -1;
}

static int write_file(const char *path, const void *data, size_t length,
		      char *error, size_t error_capacity)
{
	FILE *file;

	file = fopen(path, "wb");
	if (!file) {
		snprintf(error, error_capacity, "cannot create '%s': %s", path,
			 strerror(errno));
		return -1;
	}
	if (length > 0 && fwrite(data, 1, length, file) != length) {
		snprintf(error, error_capacity, "cannot write '%s'", path);
		fclose(file);
		return -1;
	}
	if (fclose(file) != 0) {
		snprintf(error, error_capacity, "cannot finish '%s'", path);
		return -1;
	}
	return 0;
}

static int ensure_directory(const char *path, char *error,
			    size_t error_capacity)
{
	struct stat status;

	if (mkdir(path, 0755) == 0)
		return 0;
	if (errno == EEXIST && stat(path, &status) == 0 &&
	    S_ISDIR(status.st_mode))
		return 0;
	snprintf(error, error_capacity, "cannot create directory '%s': %s",
		 path, strerror(errno));
	return -1;
}

static int prepare_source_image(struct asset_job *job, const char *path)
{
	int32_t width;
	int32_t height;

	if (read_file(path, &job->input, job->error, sizeof(job->error)) ||
	    archetypon_svg_canvas_size((const char *)job->input.data,
				       job->input.length, &job->width,
				       &job->height, job->error,
				       sizeof(job->error)))
		return -1;
	if (job->width >= job->height) {
		width = SOURCE_EDGE;
		height =
			(int32_t)lround(SOURCE_EDGE * job->height / job->width);
	} else {
		height = SOURCE_EDGE;
		width = (int32_t)lround(SOURCE_EDGE * job->width / job->height);
	}
	if (width < 1)
		width = 1;
	if (height < 1)
		height = 1;
	if (archetypon_svg_render((const char *)job->input.data,
				  job->input.length, width, height,
				  &job->source_image, job->error, sizeof(job->error)))
		return -1;
	return archetypon_svg_optimize(job->input.data, job->input.length,
				       &job->optimized, job->error,
				       sizeof(job->error));
}

static int prepare_directories(struct asset_job *job)
{
	if ((job->outputs & OUTPUT_SVG) &&
	    (ensure_directory("svg", job->error, sizeof(job->error)) ||
	     write_file("svg/original.svg", job->input.data, job->input.length,
			job->error, sizeof(job->error)) ||
	     write_file("svg/optimized.svg", job->optimized.data,
			job->optimized.length, job->error, sizeof(job->error))))
		return -1;
	if ((job->outputs & OUTPUT_PNG) &&
	    ensure_directory("png", job->error, sizeof(job->error)))
		return -1;
	if ((job->outputs & OUTPUT_WEBP) &&
	    ensure_directory("webp", job->error, sizeof(job->error)))
		return -1;
	if (!(job->outputs & OUTPUT_ICO))
		return 0;
	if (ensure_directory("favicon", job->error, sizeof(job->error)))
		return -1;
	if (job->outputs != OUTPUT_ALL)
		return 0;
	return write_file("favicon/favicon.svg", job->optimized.data,
			  job->optimized.length, job->error,
			  sizeof(job->error));
}

static bool favicon_size(int32_t size)
{
	return size == 16 || size == 32 || size == 48;
}

static void retain_ico_png(struct asset_job *job, int32_t size,
			   struct archetypon_buffer *png)
{
	static const int32_t sizes[] = { 16, 32, 48 };
	size_t i;

	for (i = 0; i < ARRAY_SIZE(sizes); i++) {
		if (size != sizes[i])
			continue;
		job->ico_pngs[i] = *png;
		memset(png, 0, sizeof(*png));
		return;
	}
}

static int write_png_asset(struct asset_job *job, int32_t size)
{
	struct archetypon_image image = { 0 };
	struct archetypon_buffer png = { 0 };
	char path[64];
	int status = -1;

	if (archetypon_image_resize(&job->source_image, size, job->width, job->height,
				    &image, job->error, sizeof(job->error)) ||
	    archetypon_png_encode(&image, &png, job->error, sizeof(job->error)))
		goto out;
	if (job->outputs & OUTPUT_PNG) {
		snprintf(path, sizeof(path), "png/%d.png", size);
		if (write_file(path, png.data, png.length, job->error,
			       sizeof(job->error)))
			goto out;
	}
	if (job->outputs == OUTPUT_ALL && (size == 16 || size == 32)) {
		snprintf(path, sizeof(path), "favicon/favicon-%d.png", size);
		if (write_file(path, png.data, png.length, job->error,
			       sizeof(job->error)))
			goto out;
	}
	if (job->outputs & OUTPUT_ICO)
		retain_ico_png(job, size, &png);
	status = 0;

out:
	archetypon_image_free(&image);
	archetypon_buffer_free(&png);
	return status;
}

static int write_png_assets(struct asset_job *job)
{
	static const int32_t sizes[] = { 16,  32,  48,	 64,  128,
					 256, 512, 1024, 2048 };
	size_t i;

	if (!(job->outputs & (OUTPUT_PNG | OUTPUT_ICO)))
		return 0;
	for (i = 0; i < ARRAY_SIZE(sizes); i++) {
		if (!(job->outputs & OUTPUT_PNG) && !favicon_size(sizes[i]))
			continue;
		if (write_png_asset(job, sizes[i]))
			return -1;
	}
	return 0;
}

static int write_ico_asset(struct asset_job *job)
{
	struct archetypon_buffer ico = { 0 };
	int status = 0;

	if (!(job->outputs & OUTPUT_ICO))
		return 0;
	if (archetypon_ico_encode(job->ico_pngs, &ico, job->error,
				  sizeof(job->error)) ||
	    write_file("favicon/favicon.ico", ico.data, ico.length, job->error,
		       sizeof(job->error)))
		status = -1;
	archetypon_buffer_free(&ico);
	return status;
}

static int write_webp_asset(struct asset_job *job, int32_t size)
{
	struct archetypon_image image = { 0 };
	struct archetypon_buffer webp = { 0 };
	char path[64];
	int status = -1;

	if (archetypon_image_resize(&job->source_image, size, job->width, job->height,
				    &image, job->error, sizeof(job->error)) ||
	    archetypon_webp_encode(&image, &webp, job->error,
				   sizeof(job->error)))
		goto out;
	snprintf(path, sizeof(path), "webp/%d.webp", size);
	status = write_file(path, webp.data, webp.length, job->error,
			    sizeof(job->error));

out:
	archetypon_image_free(&image);
	archetypon_buffer_free(&webp);
	return status;
}

static int write_webp_assets(struct asset_job *job)
{
	static const int32_t sizes[] = { 256, 512, 1024 };
	size_t i;

	if (!(job->outputs & OUTPUT_WEBP))
		return 0;
	for (i = 0; i < ARRAY_SIZE(sizes); i++) {
		if (write_webp_asset(job, sizes[i]))
			return -1;
	}
	return 0;
}

static void release_job(struct asset_job *job)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(job->ico_pngs); i++)
		archetypon_buffer_free(&job->ico_pngs[i]);
	archetypon_buffer_free(&job->optimized);
	archetypon_buffer_free(&job->input);
	archetypon_image_free(&job->source_image);
}

static void print_created(uint32_t outputs)
{
	if (outputs == OUTPUT_ALL)
		printf("Created SVG, PNG, WebP, and favicon assets in .\n");
	else if (outputs == OUTPUT_SVG)
		printf("Created SVG assets in .\n");
	else if (outputs == OUTPUT_PNG)
		printf("Created PNG assets in .\n");
	else if (outputs == OUTPUT_WEBP)
		printf("Created WebP assets in .\n");
	else
		printf("Created ICO asset in .\n");
}

static int create_assets(const char *input_path, uint32_t outputs)
{
	struct asset_job job = { .outputs = outputs };
	int status = -1;

	if (prepare_source_image(&job, input_path) || prepare_directories(&job) ||
	    write_png_assets(&job) || write_ico_asset(&job) ||
	    write_webp_assets(&job)) {
		fprintf(stderr, "archetypon: %s\n", job.error);
		goto out;
	}
	print_created(outputs);
	status = 0;

out:
	release_job(&job);
	return status;
}

static void print_usage(FILE *stream)
{
	fprintf(stream, "Usage: archetypon create <file.svg>\n"
			"       archetypon create <format> <file.svg>\n"
			"       archetypon --help\n");
}

static void print_help(void)
{
	print_usage(stdout);
	printf("\n"
	       "Create every asset format by default, or select one format:\n"
	       "  svg       Create only svg/ assets\n"
	       "  png       Create only png/ assets\n"
	       "  webp      Create only webp/ assets\n"
	       "  ico       Create only favicon/favicon.ico\n");
}

static int parse_format(const char *text, uint32_t *outputs)
{
	if (strcmp(text, "svg") == 0)
		*outputs = OUTPUT_SVG;
	else if (strcmp(text, "png") == 0)
		*outputs = OUTPUT_PNG;
	else if (strcmp(text, "webp") == 0)
		*outputs = OUTPUT_WEBP;
	else if (strcmp(text, "ico") == 0)
		*outputs = OUTPUT_ICO;
	else
		return -1;
	return 0;
}

int main(int argc, char **argv)
{
	const char *input_path;
	uint32_t outputs = OUTPUT_ALL;

	if (argc == 2 &&
	    (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "--h") == 0)) {
		print_help();
		return 0;
	}
	if (argc == 3 && strcmp(argv[1], "create") == 0) {
		input_path = argv[2];
	} else if (argc == 4 && strcmp(argv[1], "create") == 0) {
		input_path = argv[3];
		if (parse_format(argv[2], &outputs)) {
			fprintf(stderr, "archetypon: unknown format '%s'\n",
				argv[2]);
			print_usage(stderr);
			return 2;
		}
	} else {
		print_usage(stderr);
		return 2;
	}
	return create_assets(input_path, outputs) ? 1 : 0;
}
