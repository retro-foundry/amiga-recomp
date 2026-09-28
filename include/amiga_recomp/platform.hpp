// The host presentation layer (AMIGA_RECOMP.md 25, and the platform backend
// in the section 3 layout).
//
// The chipset produces frames and consumes input; this is what puts one on a
// screen and reads the other from a keyboard. It is deliberately the only part
// of the runtime that knows a window exists, so the guest machine stays
// deterministic and testable without one.
#pragma once

#include <memory>
#include <string>

#include "amiga_recomp/chipset.hpp"

namespace arecomp {

struct PlatformConfig {
    std::string title = "amiga-recomp";
    // Integer scale applied to the framebuffer when opening the window. The
    // window is resizable afterwards and the picture is fitted to it.
    unsigned scale = 2;
    bool fullscreen = false;
    // Amiga pixels are not square. A PAL lores pixel is about 1.07 times as
    // wide as it is tall on a 4:3 display; the framebuffer already holds two
    // columns per lores pixel, so this corrects the remainder.
    float pixel_aspect = 1.0667f;
};

class Platform {
public:
    virtual ~Platform() = default;

    // Show a frame. Called from the chipset's frame callback.
    virtual void present(const Framebuffer& frame) = 0;

    // Pump host events and update the chipset's input state. Returns false
    // once the user has asked to quit.
    virtual bool poll(Chipset& chipset) = 0;

    [[nodiscard]] virtual bool should_quit() const = 0;

    // True for a backend that actually opens a window.
    [[nodiscard]] virtual bool is_visible() const { return false; }
};

// Creates the best backend available: SDL when it was compiled in, otherwise a
// headless one that counts frames and never blocks. A port therefore behaves
// sensibly in a test harness and in front of a person without changing code.
std::unique_ptr<Platform> create_platform(const PlatformConfig& config);

// True when this build has a windowing backend at all.
bool platform_has_display();

} // namespace arecomp
