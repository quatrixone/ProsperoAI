// SPDX-License-Identifier: GPL-3.0-or-later

#include "image_codec.hpp"

#include <cassert>
#include <cstdio>
#include <string>

int main()
{
    using namespace prospero_image;

    EncodedFormat format{};
    unsigned width = 0;
    unsigned height = 0;
    assert(inspect_file("tests/fixtures/one-pixel.png", &format, &width, &height));
    assert(format == EncodedFormat::Png && width == 1 && height == 1);

    DecodedImage png{};
    assert(decode_file("tests/fixtures/one-pixel.png", &png));
    assert(png.pixels && png.width == 1 && png.height == 1 && png.channels == 4);
    const std::string normalized_path = "build/tests/qwen-image-codec-normalized.png";
    std::remove(normalized_path.c_str());
    std::FILE *normalized = std::fopen(normalized_path.c_str(), "wbx");
    assert(normalized && write_png(normalized, png));
    assert(std::ftell(normalized) > 0);
    assert(std::fclose(normalized) == 0);
    free_image(&png);
    assert(inspect_file(normalized_path.c_str(), &format, &width, &height));
    assert(format == EncodedFormat::Png && width == 1 && height == 1);

    assert(inspect_file("tests/fixtures/one-pixel.jpg", &format, &width, &height));
    assert(format == EncodedFormat::Jpeg && width == 1 && height == 1);
    DecodedImage jpeg{};
    assert(decode_file("tests/fixtures/one-pixel.jpg", &jpeg));
    assert(jpeg.pixels && jpeg.channels == 3);
    free_image(&jpeg);

    DecodedImage gray_alpha{};
    assert(decode_file_rgba("tests/fixtures/gray-alpha.png", &gray_alpha));
    assert(gray_alpha.pixels && gray_alpha.channels == 4);
    assert(gray_alpha.pixels[0] == 0x66 && gray_alpha.pixels[1] == 0x66 &&
           gray_alpha.pixels[2] == 0x66 && gray_alpha.pixels[3] == 0x33);
    free_image(&gray_alpha);

    for (const char *invalid : {"tests/fixtures/truncated.png", "tests/fixtures/bmp-as-png.png",
                                "tests/fixtures/zero-width.png", "tests/fixtures/wide-2049.png"})
        assert(!inspect_file(invalid, &format, &width, &height));
}
