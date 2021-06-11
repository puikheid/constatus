// (C) 2017-2020 by folkert van heusden, released under AGPL v3.0
#pragma once
#include "config.h"
#include "gen.h"
#include <stdint.h>
#include <stdio.h>
#include <turbojpeg.h>

class meta;

void read_PNG_file_rgba(FILE *fh, int *w, int *h, uint8_t **pixels);
void write_PNG_file(FILE *fh, int ncols, int nrows, unsigned char *pixels);

void load_PBM_file(FILE *const fh, int *const w, int *const h, uint8_t **out);

class myjpeg
{
private:
	tjhandle jpegDecompressor, jpegCompressor;

public:
	myjpeg();
	virtual ~myjpeg();

	bool write_JPEG_memory(const meta *const m, const int ncols, const int nrows, const int quality, const uint8_t *const pixels, uint8_t **out, size_t *out_len);
	bool read_JPEG_memory(unsigned char *in, int n_bytes_in, int *w, int *h, unsigned char **pixels);

	void rgb_to_i420(const uint8_t *const in, const int width, const int height, uint8_t **const out);
	void i420_to_rgb(const uint8_t *const in, const int width, const int height, uint8_t **const out);
};

extern thread_local myjpeg my_jpeg;
