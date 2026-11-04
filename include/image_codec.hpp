// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdio>

namespace prospero_image
{

enum class EncodedFormat
{
    Png,
    Jpeg,
};

struct DecodedImage
{
    unsigned width = 0;
    unsigned height = 0;
    unsigned channels = 0;
    unsigned char *pixels = nullptr;
};

bool inspect_file(const char *path, EncodedFormat *format, unsigned *width, unsigned *height);
// Clears the output first; call free_image() before reusing a previously decoded result.
bool decode_file(const char *path, DecodedImage *image);
bool decode_file_rgba(const char *path, DecodedImage *image);
void free_image(DecodedImage *image);
bool write_png(std::FILE *file, const DecodedImage &image);

} // namespace prospero_image
