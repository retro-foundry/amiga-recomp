// Writing a framebuffer out as a file, for tests, screenshots and eyeballing
// what a port actually produced.
//
// PPM because it needs no library and any image viewer or converter reads it.
// A real port's platform backend presents frames; this is for looking at them.
#pragma once

#include <string>

#include "amiga_recomp/chipset.hpp"

namespace arecomp {

// Binary PPM (P6). Returns false if the file could not be written.
bool write_ppm(const Framebuffer& frame, const std::string& path);

// Amiga pixels are not square, and the framebuffer holds two columns per lores
// pixel, so a screenshot only looks right when it is scaled. Each pixel is
// repeated x_scale times horizontally and y_scale times vertically.
bool write_ppm_scaled(const Framebuffer& frame, const std::string& path,
                      unsigned x_scale, unsigned y_scale);

} // namespace arecomp
