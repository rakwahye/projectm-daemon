// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rakwahye
/** @file art_decode.c
 * @brief PNG and JPEG decode to a cairo surface.
 *
 * Art arrives from untrusted URLs, so declared dimensions are clamped
 * before any byte-count math to keep allocations bounded. */

#include "art_decode.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <setjmp.h>
#include <jpeglib.h>

/* Upper bound on a decoded art dimension. Art arrives from untrusted
 * remote URLs, so the header's width/height are clamped before any
 * byte-count math to keep stride*height inside int and well under
 * cairo's surface limits. */
#define ART_MAX_DIM 8192

struct jpeg_err_mgr {
	struct jpeg_error_mgr base;
	jmp_buf jb;
};

static void jpeg_error_exit(j_common_ptr cinfo) {
	struct jpeg_err_mgr *e = (struct jpeg_err_mgr *)cinfo->err;
	longjmp(e->jb, 1);
}

/* Swallow libjpeg's own warning/trace output failure is reported
 * through the return value, not stderr. */
static void jpeg_no_message(j_common_ptr cinfo) { (void)cinfo; }

/* Decode JPEG file to a Cairo image surface (ARGB32). Returns NULL on
 * error and reports the outcome and dimensions through info. */
static cairo_surface_t *decode_jpeg(const char *path, art_decode_info *info) {
	info->status = ART_DECODE_DECODE_FAILED;
	info->width = info->height = 0;

	FILE *f = fopen(path, "rb");
	if (!f) { info->status = ART_DECODE_OPEN_FAILED; return NULL; }

	struct jpeg_decompress_struct cinfo;
	struct jpeg_err_mgr err;
	/* Declared before setjmp so the single cleanup path can release
	 * them. libjpeg longjmps on any decode error and locals first set
	 * after setjmp would otherwise leak. */
	unsigned char *data = NULL;
	unsigned char *row = NULL;
	cairo_surface_t *cs = NULL;
	int w = 0, h = 0, stride = 0;

	cinfo.err = jpeg_std_error(&err.base);
	err.base.error_exit = jpeg_error_exit;
	err.base.output_message = jpeg_no_message;

	if (setjmp(err.jb)) goto done;

	jpeg_create_decompress(&cinfo);
	jpeg_stdio_src(&cinfo, f);
	jpeg_read_header(&cinfo, TRUE);
	cinfo.out_color_space = JCS_RGB;
	jpeg_start_decompress(&cinfo);

	w = (int)cinfo.output_width;
	h = (int)cinfo.output_height;
	info->width = w;
	info->height = h;
	/* Bound dimensions before the stride*h allocation. */
	if (w <= 0 || h <= 0 || w > ART_MAX_DIM || h > ART_MAX_DIM) {
		info->status = ART_DECODE_BAD_DIMENSIONS;
		goto done;
	}
	stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, w);
	if (stride <= 0) goto done;

	data = malloc((size_t)stride * (size_t)h);
	row = malloc((size_t)w * 3);
	if (!data || !row) { info->status = ART_DECODE_OOM; goto done; }

	while ((int)cinfo.output_scanline < h) {
		int y = cinfo.output_scanline;
		jpeg_read_scanlines(&cinfo, &row, 1);
		uint32_t *dst = (uint32_t *)(data + (size_t)y * (size_t)stride);
		for (int x = 0; x < w; x++) {
			uint8_t r = row[x*3+0], g = row[x*3+1], b = row[x*3+2];
			dst[x] = (0xFFu << 24) | (r << 16) | (g << 8) | b;
		}
	}

	jpeg_finish_decompress(&cinfo);

	cs = cairo_image_surface_create_for_data(
		data, CAIRO_FORMAT_ARGB32, w, h, stride);
	if (cairo_surface_status(cs) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(cs);
		cs = NULL;
		goto done;
	}
	/* Hand ownership of `data` to cairo via destroy callback. If the hook
	 * can't be attached, free `data` ourselves. */
	static const cairo_user_data_key_t key = {0};
	if (cairo_surface_set_user_data(cs, &key, data, free) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(cs);
		cs = NULL;
		goto done;
	}
	data = NULL;
	info->status = ART_DECODE_OK;

done:
	free(row);
	free(data);
	jpeg_destroy_decompress(&cinfo);
	fclose(f);
	return cs;
}

static cairo_surface_t *load_png(const char *path, art_decode_info *info) {
	cairo_surface_t *cs = cairo_image_surface_create_from_png(path);
	if (cairo_surface_status(cs) == CAIRO_STATUS_SUCCESS) {
		info->status = ART_DECODE_OK;
		info->width = cairo_image_surface_get_width(cs);
		info->height = cairo_image_surface_get_height(cs);
		return cs;
	}
	cairo_surface_destroy(cs);
	return NULL;
}

typedef enum { ART_FORMAT_UNKNOWN, ART_FORMAT_PNG, ART_FORMAT_JPEG } img_kind;

/* Identify the format from the leading bytes, not the file name. */
static img_kind sniff_image(const char *path, int *open_ok) {
	*open_ok = 0;
	FILE *f = fopen(path, "rb");
	if (!f) return ART_FORMAT_UNKNOWN;
	*open_ok = 1;
	unsigned char m[8];
	size_t n = fread(m, 1, sizeof(m), f);
	fclose(f);
	if (n >= 8 && m[0] == 0x89 && m[1] == 'P' && m[2] == 'N' && m[3] == 'G' &&
		m[4] == 0x0D && m[5] == 0x0A && m[6] == 0x1A && m[7] == 0x0A)
		return ART_FORMAT_PNG;
	if (n >= 3 && m[0] == 0xFF && m[1] == 0xD8 && m[2] == 0xFF)
		return ART_FORMAT_JPEG;
	return ART_FORMAT_UNKNOWN;
}

/* Dispatch by file content. */
cairo_surface_t *art_decode_image(const char *path, art_decode_info *info) {
	art_decode_info scratch;
	if (!info) info = &scratch;
	info->status = ART_DECODE_DECODE_FAILED;
	info->width = info->height = 0;

	if (!path || !path[0]) { info->status = ART_DECODE_OPEN_FAILED; return NULL; }

	int open_ok = 0;
	img_kind kind = sniff_image(path, &open_ok);
	if (!open_ok) { info->status = ART_DECODE_OPEN_FAILED; return NULL; }

	if (kind == ART_FORMAT_PNG) return load_png(path, info);
	if (kind == ART_FORMAT_JPEG) return decode_jpeg(path, info);

	/* Unrecognized signature. Try both decoders as a last resort. */
	cairo_surface_t *cs = load_png(path, info);
	if (cs) return cs;
	return decode_jpeg(path, info);
}
