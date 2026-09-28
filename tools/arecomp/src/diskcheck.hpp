// Verifying the emulated floppy against a real disk image.
#pragma once

#include <string>

#include "image.hpp"

namespace arecomp {

// Reads every track back through the emulated drive and compares it with the
// source image. Returns 0 when the round trip is lossless.
int disk_check(const std::string& path, int only_cylinder, bool verbose);

} // namespace arecomp
