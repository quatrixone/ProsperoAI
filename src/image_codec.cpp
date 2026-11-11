// SPDX-License-Identifier: GPL-3.0-or-later

#include "image_codec.hpp"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <cstdio>
#include <cstring>

namespace prospero_image
{
namespace
{

constexpr unsigned MaxDimension = 2048;
constexpr unsigned char PngSignature[] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};

bool read_format(std::FILE *file, EncodedFormat *format)
{
    if (!file || !format)
        return false;
    unsigned char signature[sizeof(PngSignature)]{};
    const std::size_t count = std::fread(signature, 1, sizeof(signature), file);
    if (count >= sizeof(PngSignature) &&
        std::memcmp(signature, PngSignature, sizeof(PngSignature)) == 0)
        *format = EncodedFormat::Png;
    else if (count >= 2 && signature[0] == 0xff && signature[1] == 0xd8)
        *format = EncodedFormat::Jpeg;
    else
        return false;
    return std::fseek(file, 0, SEEK_SET) == 0;
}

bool read_dimensions(std::FILE *file, unsigned *width, unsigned *height, int *channels)
{
    int decoded_width = 0;
    int decoded_height = 0;
    int decoded_channels = 0;
    if (!stbi_info_from_file(file, &decoded_width, &decoded_height, &decoded_channels) ||
        decoded_width < 1 || decoded_height < 1 || decoded_width > static_cast<int>(MaxDimension) ||
        decoded_height > static_cast<int>(MaxDimension) || decoded_channels < 1 ||
        decoded_channels > 4 || std::fseek(file, 0, SEEK_SET) != 0)
        return false;
    *width = static_cast<unsigned>(decoded_width);
    *height = static_cast<unsigned>(decoded_height);
    *channels = decoded_channels;
    return true;
}

bool decode_file_with_channels(const char *path, DecodedImage *image, int output_channels)
{
    if (!image)
        return false;
    *image = {};
    if (!path)
        return false;
    std::FILE *file = std::fopen(path, "rb");
    if (!file)
        return false;
    EncodedFormat format{};
    unsigned width = 0;
    unsigned height = 0;
    int channels = 0;
    const bool header_valid =
        read_format(file, &format) && read_dimensions(file, &width, &height, &channels);
    int decoded_width = 0;
    int decoded_height = 0;
    int decoded_channels = 0;
    unsigned char *pixels = header_valid
                                ? stbi_load_from_file(file, &decoded_width, &decoded_height,
                                                      &decoded_channels, output_channels)
                                : nullptr;
    const bool closed = std::fclose(file) == 0;
    if (!pixels || !closed || decoded_width != static_cast<int>(width) ||
        decoded_height != static_cast<int>(height) || decoded_channels != channels)
    {
        stbi_image_free(pixels);
        return false;
    }
    image->width = width;
    image->height = height;
    image->channels = static_cast<unsigned>(output_channels == 0 ? channels : output_channels);
    image->pixels = pixels;
    return true;
}

struct FileWriter
{
    std::FILE *file;
    bool ok = true;
};

void write_bytes(void *context, void *data, int size)
{
    auto *writer = static_cast<FileWriter *>(context);
    if (size < 0 || !writer->ok ||
        std::fwrite(data, 1, static_cast<std::size_t>(size), writer->file) !=
            static_cast<std::size_t>(size))
        writer->ok = false;
}

} // namespace

bool inspect_file(const char *path, EncodedFormat *format, unsigned *width, unsigned *height)
{
    if (!path || !format || !width || !height)
        return false;
    std::FILE *file = std::fopen(path, "rb");
    if (!file)
        return false;
    EncodedFormat detected{};
    unsigned image_width = 0;
    unsigned image_height = 0;
    int channels = 0;
    bool valid = read_format(file, &detected) &&
                 read_dimensions(file, &image_width, &image_height, &channels);
    if (valid)
    {
        int decoded_width = 0;
        int decoded_height = 0;
        int decoded_channels = 0;
        unsigned char *pixels =
            stbi_load_from_file(file, &decoded_width, &decoded_height, &decoded_channels, 0);
        valid = pixels && decoded_width == static_cast<int>(image_width) &&
                decoded_height == static_cast<int>(image_height) && decoded_channels == channels;
        stbi_image_free(pixels);
    }
    const bool closed = std::fclose(file) == 0;
    if (!valid || !closed)
        return false;
    *format = detected;
    *width = image_width;
    *height = image_height;
    return true;
}

bool decode_file(const char *path, DecodedImage *image)
{
    return decode_file_with_channels(path, image, 0);
}

bool decode_file_rgba(const char *path, DecodedImage *image)
{
    return decode_file_with_channels(path, image, 4);
}

void free_image(DecodedImage *image)
{
    if (!image)
        return;
    stbi_image_free(image->pixels);
    *image = {};
}

bool write_png(std::FILE *file, const DecodedImage &image)
{
    if (!file || !image.pixels || image.width == 0 || image.height == 0 ||
        image.width > MaxDimension || image.height > MaxDimension || image.channels < 1 ||
        image.channels > 4)
        return false;
    FileWriter writer{file};
    const int result = stbi_write_png_to_func(write_bytes, &writer, static_cast<int>(image.width),
                                              static_cast<int>(image.height),
                                              static_cast<int>(image.channels), image.pixels, 0);
    return result != 0 && writer.ok;
}

} // namespace prospero_image
