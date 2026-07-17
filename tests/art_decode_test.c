#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include <jpeglib.h>

#include "art_decode.h"

#include "check.h"

/* Real JPEGs are built with libjpeg, then mutated into the pathological inputs
 * the decoder must survive. Run under ASan to also catch leaks on the failure
 * paths, which is where they hide. */
static int write_jpeg(const char *path, int w, int h) {
	FILE *f = fopen(path, "wb");
	if (!f) return 0;

	struct jpeg_compress_struct cinfo;
	struct jpeg_error_mgr jerr;
	cinfo.err = jpeg_std_error(&jerr);
	jpeg_create_compress(&cinfo);
	jpeg_stdio_dest(&cinfo, f);
	cinfo.image_width = (JDIMENSION)w;
	cinfo.image_height = (JDIMENSION)h;
	cinfo.input_components = 3;
	cinfo.in_color_space = JCS_RGB;
	jpeg_set_defaults(&cinfo);
	jpeg_start_compress(&cinfo, TRUE);

	unsigned char *row = malloc((size_t)w * 3);
	memset(row, 0x80, (size_t)w * 3);
	while (cinfo.next_scanline < cinfo.image_height) {
		JSAMPROW r = row;
		jpeg_write_scanlines(&cinfo, &r, 1);
	}
	free(row);

	jpeg_finish_compress(&cinfo);
	jpeg_destroy_compress(&cinfo);
	fclose(f);
	return 1;
}

static unsigned char *slurp(const char *path, size_t *len) {
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	unsigned char *buf = malloc((size_t)n);
	if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
	fclose(f);
	if (buf) *len = (size_t)n;
	return buf;
}

static int dump(const char *path, const unsigned char *buf, size_t len) {
	FILE *f = fopen(path, "wb");
	if (!f) return 0;
	size_t w = fwrite(buf, 1, len, f);
	fclose(f);
	return w == len;
}

/* Layout after the FF C0 marker: length(2) precision(1) height(2) width(2). */
static int preset_sof_dims(unsigned char *buf, size_t len, int dim) {
	for (size_t i = 0; i + 8 < len; i++) {
		if (buf[i] == 0xFF && buf[i + 1] == 0xC0) {
			buf[i + 5] = (unsigned char)((dim >> 8) & 0xFF);
			buf[i + 6] = (unsigned char)(dim & 0xFF);
			buf[i + 7] = (unsigned char)((dim >> 8) & 0xFF);
			buf[i + 8] = (unsigned char)(dim & 0xFF);
			return 1;
		}
	}
	return 0;
}

int main(void) {
	char dir[64];
	snprintf(dir, sizeof(dir), "/tmp/art_decode_test_%d", (int)getpid());
	mkdir(dir, 0700);

	char valid[128], liar[128], trunc[128], garbage[128], png_path[128], gone[128];
	snprintf(valid,   sizeof(valid),   "%s/valid.jpg",   dir);
	snprintf(liar,    sizeof(liar),    "%s/liar.jpg",    dir);
	snprintf(trunc,   sizeof(trunc),   "%s/trunc.jpg",   dir);
	snprintf(garbage, sizeof(garbage), "%s/garbage.jpg", dir);
	snprintf(png_path,sizeof(png_path),"%s/valid.png",   dir);
	snprintf(gone,    sizeof(gone),    "%s/nope.jpg",    dir);

	CHECK(write_jpeg(valid, 16, 16), "a real JPEG can be built to decode against");
	{
		art_decode_info ai;
		cairo_surface_t *cs = art_decode_image(valid, &ai);
		CHECK(cs != NULL, "a valid JPEG decodes to a surface");
		CHECK(ai.status == ART_DECODE_OK, "a valid JPEG reports success");
		CHECK(ai.width == 16 && ai.height == 16, "the reported dimensions are the real ones");
		CHECK(cairo_image_surface_get_width(cs) == 16, "the surface is as wide as the image");
		CHECK(cairo_image_surface_get_height(cs) == 16, "the surface is as tall as the image");
		cairo_surface_destroy(cs);
	}

	/* The header lies about its size: past the cap, but under libjpeg's own
	 * limit, so only the cap catches it. This is the heap-overflow vector. */
	{
		size_t n = 0;
		unsigned char *buf = slurp(valid, &n);
		CHECK(buf != NULL, "the valid JPEG can be read back for mutation");
		CHECK(preset_sof_dims(buf, n, 16384), "the size fields in the header can be overwritten");
		CHECK(dump(liar, buf, n), "the mutated JPEG can be written out");
		free(buf);

		art_decode_info ai;
		cairo_surface_t *cs = art_decode_image(liar, &ai);
		CHECK(cs == NULL, "an oversized declared size is refused rather than allocated");
		CHECK(ai.status == ART_DECODE_BAD_DIMENSIONS, "the refusal names the dimensions as the reason");
		CHECK(ai.width == 16384 && ai.height == 16384, "the declared dimensions are reported back, not the real ones");
	}

	{
		size_t n = 0;
		unsigned char *buf = slurp(valid, &n);
		CHECK(buf != NULL && n > 40, "the valid JPEG is long enough to truncate");
		CHECK(dump(trunc, buf, 40), "a truncated JPEG can be written out");
		free(buf);

		art_decode_info ai;
		cairo_surface_t *cs = art_decode_image(trunc, &ai);
		CHECK(cs == NULL, "a stream that ends mid-image decodes to nothing");
		CHECK(ai.status == ART_DECODE_DECODE_FAILED, "a truncated stream reports a decode failure");
	}

	{
		const char *junk = "this is definitely not a JPEG file\n";
		CHECK(dump(garbage, (const unsigned char *)junk, strlen(junk)), "non-image bytes can be written under an image name");
		art_decode_info ai;
		cairo_surface_t *cs = art_decode_image(garbage, &ai);
		CHECK(cs == NULL, "non-image bytes decode to nothing");
		CHECK(ai.status == ART_DECODE_DECODE_FAILED, "non-image bytes report a decode failure");
	}

	{
		art_decode_info ai;
		CHECK(art_decode_image(gone, &ai) == NULL, "a path that does not exist decodes to nothing");
		CHECK(ai.status == ART_DECODE_OPEN_FAILED, "a missing file reports an open failure");
		CHECK(art_decode_image(NULL, &ai) == NULL, "a NULL path is survivable");
		CHECK(ai.status == ART_DECODE_OPEN_FAILED, "a NULL path reports an open failure");
		CHECK(art_decode_image("", &ai) == NULL, "an empty path is survivable");
		CHECK(ai.status == ART_DECODE_OPEN_FAILED, "an empty path reports an open failure");
	}

	{
		cairo_surface_t *cs = art_decode_image(valid, NULL);
		CHECK(cs != NULL, "a caller that wants no info still gets the surface");
		cairo_surface_destroy(cs);
		CHECK(art_decode_image(garbage, NULL) == NULL, "a NULL info is survivable on the failure path too");
	}

	{
		cairo_surface_t *src = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 8, 8);
		CHECK(cairo_surface_write_to_png(src, png_path) == CAIRO_STATUS_SUCCESS, "a real PNG can be built to decode against");
		cairo_surface_destroy(src);

		art_decode_info ai;
		cairo_surface_t *cs = art_decode_image(png_path, &ai);
		CHECK(cs != NULL, "the dispatcher decodes a PNG as well as a JPEG");
		CHECK(ai.status == ART_DECODE_OK, "a valid PNG reports success");
		CHECK(ai.width == 8 && ai.height == 8, "a PNG reports its real dimensions");
		cairo_surface_destroy(cs);
	}

	/* An extension can lie, which is what a URL-derived guess produces. The
	 * decoder must follow the bytes, not the name. */
	{
		size_t n = 0;
		char png_as_jpg[128], jpg_as_png[128];
		snprintf(png_as_jpg, sizeof(png_as_jpg), "%s/png_as.jpg", dir);
		snprintf(jpg_as_png, sizeof(jpg_as_png), "%s/jpg_as.png", dir);

		unsigned char *png = slurp(png_path, &n);
		CHECK(png != NULL, "the PNG can be read back to rename");
		CHECK(dump(png_as_jpg, png, n), "the PNG can be written out under a JPEG name");
		free(png);

		art_decode_info ai;
		cairo_surface_t *cs = art_decode_image(png_as_jpg, &ai);
		CHECK(cs != NULL, "a PNG named as a JPEG still decodes, because content is sniffed");
		CHECK(ai.status == ART_DECODE_OK, "a misnamed PNG reports success");
		CHECK(ai.width == 8 && ai.height == 8, "a misnamed PNG reports its real dimensions");
		cairo_surface_destroy(cs);

		unsigned char *jpg = slurp(valid, &n);
		CHECK(jpg != NULL, "the JPEG can be read back to rename");
		CHECK(dump(jpg_as_png, jpg, n), "the JPEG can be written out under a PNG name");
		free(jpg);

		cs = art_decode_image(jpg_as_png, &ai);
		CHECK(cs != NULL, "a JPEG named as a PNG still decodes, so sniffing works both ways");
		CHECK(ai.status == ART_DECODE_OK, "a misnamed JPEG reports success");
		CHECK(ai.width == 16 && ai.height == 16, "a misnamed JPEG reports its real dimensions");
		cairo_surface_destroy(cs);

		unlink(png_as_jpg);
		unlink(jpg_as_png);
	}

	unlink(valid); unlink(liar); unlink(trunc);
	unlink(garbage); unlink(png_path);
	rmdir(dir);

	CHECK_PASS("test-art_decode");
}
