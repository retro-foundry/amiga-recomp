#include "amiga_recomp/ppm.hpp"

#include <cstdio>
#include <vector>

namespace arecomp {

bool write_ppm_scaled(const Framebuffer& frame, const std::string& path,
                      unsigned x_scale, unsigned y_scale) {
    if (frame.width == 0 || frame.height == 0) return false;
    if (x_scale == 0) x_scale = 1;
    if (y_scale == 0) y_scale = 1;

    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) return false;

    std::fprintf(file, "P6\n%u %u\n255\n", frame.width * x_scale,
                 frame.height * y_scale);

    std::vector<unsigned char> row(static_cast<std::size_t>(frame.width) * x_scale * 3);
    for (u32 y = 0; y < frame.height; ++y) {
        const u32* source =
            frame.pixels.data() + static_cast<std::size_t>(y) * frame.width;
        std::size_t out = 0;
        for (u32 x = 0; x < frame.width; ++x) {
            const u32 pixel = source[x];
            for (unsigned repeat = 0; repeat < x_scale; ++repeat) {
                row[out++] = static_cast<unsigned char>((pixel >> 16) & 0xff);
                row[out++] = static_cast<unsigned char>((pixel >> 8) & 0xff);
                row[out++] = static_cast<unsigned char>(pixel & 0xff);
            }
        }
        for (unsigned repeat = 0; repeat < y_scale; ++repeat)
            std::fwrite(row.data(), 1, row.size(), file);
    }

    const bool ok = std::ferror(file) == 0;
    std::fclose(file);
    return ok;
}

bool write_ppm(const Framebuffer& frame, const std::string& path) {
    return write_ppm_scaled(frame, path, 1, 1);
}

} // namespace arecomp
