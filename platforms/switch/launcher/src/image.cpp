#include "image.hpp"

#include <cstring>

#include <png.h>

namespace partyboard::launcher {

namespace {

Image downscale(const Image& src, int factor) {
    Image out;
    out.width = src.width / factor;
    out.height = src.height / factor;
    out.rgba.resize(size_t(out.width) * out.height * 4);
    const int area = factor * factor;
    for (int y = 0; y < out.height; ++y) {
        for (int x = 0; x < out.width; ++x) {
            unsigned sum[4] = {};
            for (int sy = 0; sy < factor; ++sy) {
                const uint8_t* row = src.rgba.data() + (size_t(y * factor + sy) * src.width + size_t(x) * factor) * 4;
                for (int sx = 0; sx < factor; ++sx) {
                    for (int c = 0; c < 4; ++c)
                        sum[c] += row[sx * 4 + c];
                }
            }
            uint8_t* dst = out.rgba.data() + (size_t(y) * out.width + x) * 4;
            for (int c = 0; c < 4; ++c)
                dst[c] = static_cast<uint8_t>(sum[c] / area);
        }
    }
    return out;
}

} // namespace

bool loadPng(const std::string& path, Image& out, int maxDimension) {
    png_image image;
    std::memset(&image, 0, sizeof(image));
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&image, path.c_str()))
        return false;
    if (image.width == 0 || image.height == 0 || image.width > 8192 || image.height > 8192) {
        png_image_free(&image);
        return false;
    }
    image.format = PNG_FORMAT_RGBA;

    Image loaded;
    loaded.width = static_cast<int>(image.width);
    loaded.height = static_cast<int>(image.height);
    loaded.rgba.resize(PNG_IMAGE_SIZE(image));
    if (!png_image_finish_read(&image, nullptr, loaded.rgba.data(), 0, nullptr)) {
        png_image_free(&image);
        return false;
    }

    int factor = 1;
    while (loaded.width / factor > maxDimension || loaded.height / factor > maxDimension)
        ++factor;
    out = factor > 1 ? downscale(loaded, factor) : std::move(loaded);
    return true;
}

bool writePng(const std::string& path, const Image& source) {
    png_image image;
    std::memset(&image, 0, sizeof(image));
    image.version = PNG_IMAGE_VERSION;
    image.width = static_cast<png_uint_32>(source.width);
    image.height = static_cast<png_uint_32>(source.height);
    image.format = PNG_FORMAT_RGBA;
    return png_image_write_to_file(&image, path.c_str(), 0, source.rgba.data(), 0, nullptr) != 0;
}

} // namespace partyboard::launcher
