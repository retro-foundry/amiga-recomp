// SDL2 presentation and input, plus the headless fallback.
//
// Compiled with a window when ARECOMP_WITH_SDL is on, and without one
// otherwise, so the same port source builds and runs either way.

#include "amiga_recomp/platform.hpp"

#include <algorithm>
#include <cstdio>
#include <vector>

#if ARECOMP_WITH_SDL
#include <SDL.h>
#endif

namespace arecomp {

namespace {

// Counts frames and never blocks. Used for tests, for replay harnesses, and
// for any build without SDL.
class HeadlessPlatform final : public Platform {
public:
    void present(const Framebuffer& frame) override {
        ++frames_;
        width_ = frame.width;
        height_ = frame.height;
    }
    bool poll(Chipset&) override { return true; }
    [[nodiscard]] bool should_quit() const override { return false; }

private:
    u64 frames_ = 0;
    u32 width_ = 0;
    u32 height_ = 0;
};

#if ARECOMP_WITH_SDL

class SdlPlatform final : public Platform {
public:
    explicit SdlPlatform(const PlatformConfig& config) : config_(config) {}

    ~SdlPlatform() override {
        for (SDL_GameController* pad : pads_) SDL_GameControllerClose(pad);
        if (texture_) SDL_DestroyTexture(texture_);
        if (renderer_) SDL_DestroyRenderer(renderer_);
        if (window_) SDL_DestroyWindow(window_);
        if (initialised_) SDL_Quit();
    }

    bool open() {
        // A missing controller subsystem is not a reason to refuse to start,
        // so it is asked for separately from the window.
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
            std::fprintf(stderr, "[platform] no controller support (%s); "
                                 "keyboard and mouse only\n",
                         SDL_GetError());
            if (SDL_Init(SDL_INIT_VIDEO) != 0) {
                std::fprintf(stderr, "[platform] SDL_Init failed: %s\n",
                             SDL_GetError());
                return false;
            }
        }
        initialised_ = true;

        for (int i = 0; i < SDL_NumJoysticks(); ++i) open_pad(i);

        // The window is sized once the first frame arrives and the real
        // display geometry is known, which a game can change at any time.
        window_ = SDL_CreateWindow(config_.title.c_str(), SDL_WINDOWPOS_CENTERED,
                                   SDL_WINDOWPOS_CENTERED, 640, 512,
                                   SDL_WINDOW_RESIZABLE |
                                       (config_.fullscreen
                                            ? SDL_WINDOW_FULLSCREEN_DESKTOP
                                            : 0u));
        if (!window_) {
            std::fprintf(stderr, "[platform] SDL_CreateWindow failed: %s\n",
                         SDL_GetError());
            return false;
        }

        renderer_ = SDL_CreateRenderer(
            window_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (!renderer_) renderer_ = SDL_CreateRenderer(window_, -1, 0);
        if (!renderer_) {
            std::fprintf(stderr, "[platform] SDL_CreateRenderer failed: %s\n",
                         SDL_GetError());
            return false;
        }
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
        return true;
    }

    void present(const Framebuffer& frame) override {
        if (!renderer_ || frame.width == 0 || frame.height == 0) return;

        if (!texture_ || texture_width_ != frame.width ||
            texture_height_ != frame.height) {
            if (texture_) SDL_DestroyTexture(texture_);
            texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888,
                                         SDL_TEXTUREACCESS_STREAMING,
                                         static_cast<int>(frame.width),
                                         static_cast<int>(frame.height));
            texture_width_ = frame.width;
            texture_height_ = frame.height;
            if (!sized_window_ && texture_) {
                sized_window_ = true;
                SDL_SetWindowSize(
                    window_,
                    static_cast<int>(frame.width * config_.scale / 2 *
                                     static_cast<unsigned>(config_.pixel_aspect * 2)),
                    static_cast<int>(frame.height * config_.scale));
            }
        }
        if (!texture_) return;

        SDL_UpdateTexture(texture_, nullptr, frame.pixels.data(),
                          static_cast<int>(frame.width * sizeof(u32)));

        int window_w = 0, window_h = 0;
        SDL_GetRendererOutputSize(renderer_, &window_w, &window_h);

        // Fit the picture to the window while keeping the Amiga's aspect
        // ratio. The framebuffer holds two columns per lores pixel, so the
        // content is half as wide as it looks.
        const float content_w =
            static_cast<float>(frame.width) * 0.5f * config_.pixel_aspect;
        const float content_h = static_cast<float>(frame.height);
        const float scale =
            std::min(static_cast<float>(window_w) / content_w,
                     static_cast<float>(window_h) / content_h);

        SDL_Rect destination;
        destination.w = static_cast<int>(content_w * scale);
        destination.h = static_cast<int>(content_h * scale);
        destination.x = (window_w - destination.w) / 2;
        destination.y = (window_h - destination.h) / 2;

        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
        SDL_RenderClear(renderer_);
        SDL_RenderCopy(renderer_, texture_, nullptr, &destination);
        SDL_RenderPresent(renderer_);
    }

    bool poll(Chipset& chipset) override {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
            case SDL_QUIT:
                quit_ = true;
                break;
            case SDL_KEYDOWN:
            case SDL_KEYUP: {
                const bool down = event.type == SDL_KEYDOWN;
                switch (event.key.keysym.scancode) {
                case SDL_SCANCODE_UP: up_ = down; break;
                case SDL_SCANCODE_DOWN: down_ = down; break;
                case SDL_SCANCODE_LEFT: left_ = down; break;
                case SDL_SCANCODE_RIGHT: right_ = down; break;
                case SDL_SCANCODE_SPACE:
                case SDL_SCANCODE_LCTRL: fire_ = down; break;
                case SDL_SCANCODE_ESCAPE:
                    if (down) quit_ = true;
                    break;
                case SDL_SCANCODE_F11:
                    if (down) toggle_fullscreen();
                    break;
                default:
                    break;
                }
                break;
            }
            case SDL_CONTROLLERDEVICEADDED:
                // `which` is a device index here and an instance id below;
                // SDL uses the same field for both.
                open_pad(event.cdevice.which);
                break;
            case SDL_CONTROLLERDEVICEREMOVED:
                close_pad(event.cdevice.which);
                break;
            case SDL_MOUSEMOTION:
                // Accumulated and handed over below, because several motion
                // events can arrive between frames.
                mouse_dx_ += event.motion.xrel;
                mouse_dy_ += event.motion.yrel;
                break;
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP: {
                const bool down = event.type == SDL_MOUSEBUTTONDOWN;
                if (event.button.button == SDL_BUTTON_LEFT) mouse_left_ = down;
                if (event.button.button == SDL_BUTTON_RIGHT) mouse_right_ = down;
                break;
            }
            default:
                break;
            }
        }

        // A gamepad drives the same port as the keyboard rather than a second
        // one, so either can be picked up at any moment and holding both is
        // not a conflict.
        bool up = up_, down = down_, left = left_, right = right_, fire = fire_;
        int pad_dx = 0, pad_dy = 0;
        bool pad_button = false;

        for (SDL_GameController* pad : pads_) {
            const auto held = [pad](SDL_GameControllerButton b) {
                return SDL_GameControllerGetButton(pad, b) != 0;
            };
            const auto stick = [pad](SDL_GameControllerAxis a) {
                return SDL_GameControllerGetAxis(pad, a);
            };

            // The d-pad and the left stick both steer, because which one a
            // player reaches for is theirs to decide.
            up = up || held(SDL_CONTROLLER_BUTTON_DPAD_UP) ||
                 stick(SDL_CONTROLLER_AXIS_LEFTY) < -kStickOn;
            down = down || held(SDL_CONTROLLER_BUTTON_DPAD_DOWN) ||
                   stick(SDL_CONTROLLER_AXIS_LEFTY) > kStickOn;
            left = left || held(SDL_CONTROLLER_BUTTON_DPAD_LEFT) ||
                   stick(SDL_CONTROLLER_AXIS_LEFTX) < -kStickOn;
            right = right || held(SDL_CONTROLLER_BUTTON_DPAD_RIGHT) ||
                    stick(SDL_CONTROLLER_AXIS_LEFTX) > kStickOn;

            // An Amiga stick has one button, so every face button and the
            // right shoulder and trigger all report as that one button.
            fire = fire || held(SDL_CONTROLLER_BUTTON_A) ||
                   held(SDL_CONTROLLER_BUTTON_B) ||
                   held(SDL_CONTROLLER_BUTTON_X) ||
                   held(SDL_CONTROLLER_BUTTON_Y) ||
                   held(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) ||
                   stick(SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > kTriggerOn;

            // The right stick moves the mouse and the left shoulder clicks it.
            // Without this a gamepad cannot reach a menu that wants a pointer,
            // which is most of them, and the game would be unreachable from
            // the controller it is played with.
            pad_dx += stick_to_mouse(stick(SDL_CONTROLLER_AXIS_RIGHTX));
            pad_dy += stick_to_mouse(stick(SDL_CONTROLLER_AXIS_RIGHTY));
            pad_button = pad_button ||
                         held(SDL_CONTROLLER_BUTTON_LEFTSHOULDER) ||
                         stick(SDL_CONTROLLER_AXIS_TRIGGERLEFT) > kTriggerOn;
        }

        // Port 1 is where a game looks for a joystick; port 0 is the mouse,
        // and plenty of games use it for their menus even when the game
        // itself is played on a stick.
        chipset.set_joystick(1, up, down, left, right, fire);
        chipset.set_mouse(mouse_dx_ + pad_dx, mouse_dy_ + pad_dy,
                          mouse_left_ || pad_button, mouse_right_);
        mouse_dx_ = 0;
        mouse_dy_ = 0;
        return !quit_;
    }

    [[nodiscard]] bool should_quit() const override { return quit_; }
    [[nodiscard]] bool is_visible() const override { return true; }

private:
    // Past these an axis counts as pushed. A stick rests near zero but not at
    // it, so a threshold this side of half travel keeps a worn stick from
    // steering on its own.
    static constexpr Sint16 kStickOn = 12000;
    static constexpr Sint16 kTriggerOn = 16000;

    // An axis reading becomes mouse movement for this frame. The divisor sets
    // the top speed, at about ten pixels a frame with the stick held over.
    static int stick_to_mouse(Sint16 value) {
        if (value > -kStickOn && value < kStickOn) return 0;
        return value / 3000;
    }

    void open_pad(int device_index) {
        if (!SDL_IsGameController(device_index)) return;   // a wheel, or a HID
        SDL_GameController* pad = SDL_GameControllerOpen(device_index);
        if (!pad) {
            std::fprintf(stderr, "[platform] could not open controller %d: %s\n",
                         device_index, SDL_GetError());
            return;
        }
        // SDL hands back the same handle for a device already open, and takes
        // another reference for it. Devices present at start-up also arrive a
        // second time as added events, so without this every press on one
        // would count twice.
        for (SDL_GameController* seen : pads_) {
            if (seen == pad) {
                SDL_GameControllerClose(pad);   // drop the extra reference
                return;
            }
        }
        pads_.push_back(pad);
        const char* name = SDL_GameControllerName(pad);
        std::fprintf(stderr, "[platform] controller: %s\n", name ? name : "unnamed");
    }

    void close_pad(SDL_JoystickID which) {
        for (auto it = pads_.begin(); it != pads_.end(); ++it) {
            SDL_Joystick* stick = SDL_GameControllerGetJoystick(*it);
            if (stick && SDL_JoystickInstanceID(stick) == which) {
                SDL_GameControllerClose(*it);
                pads_.erase(it);
                return;
            }
        }
    }

    void toggle_fullscreen() {
        fullscreen_ = !fullscreen_;
        SDL_SetWindowFullscreen(window_,
                                fullscreen_ ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    }

    PlatformConfig config_;
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture* texture_ = nullptr;
    u32 texture_width_ = 0;
    u32 texture_height_ = 0;
    bool initialised_ = false;
    bool sized_window_ = false;
    bool fullscreen_ = false;
    bool quit_ = false;
    bool up_ = false, down_ = false, left_ = false, right_ = false, fire_ = false;
    int mouse_dx_ = 0, mouse_dy_ = 0;
    bool mouse_left_ = false, mouse_right_ = false;
    std::vector<SDL_GameController*> pads_;
};

#endif // ARECOMP_WITH_SDL

} // namespace

bool platform_has_display() {
#if ARECOMP_WITH_SDL
    return true;
#else
    return false;
#endif
}

std::unique_ptr<Platform> create_platform(const PlatformConfig& config) {
#if ARECOMP_WITH_SDL
    auto sdl = std::make_unique<SdlPlatform>(config);
    if (sdl->open()) return sdl;
    std::fprintf(stderr, "[platform] falling back to headless\n");
#else
    (void)config;
#endif
    return std::make_unique<HeadlessPlatform>();
}

} // namespace arecomp
