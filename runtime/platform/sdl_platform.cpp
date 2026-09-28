// SDL2 presentation and input, plus the headless fallback.
//
// Compiled with a window when ARECOMP_WITH_SDL is on, and without one
// otherwise, so the same port source builds and runs either way.

#include "amiga_recomp/platform.hpp"

#include <cstdio>

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
        if (texture_) SDL_DestroyTexture(texture_);
        if (renderer_) SDL_DestroyRenderer(renderer_);
        if (window_) SDL_DestroyWindow(window_);
        if (initialised_) SDL_Quit();
    }

    bool open() {
        if (SDL_Init(SDL_INIT_VIDEO) != 0) {
            std::fprintf(stderr, "[platform] SDL_Init failed: %s\n", SDL_GetError());
            return false;
        }
        initialised_ = true;

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
            default:
                break;
            }
        }

        // Port 1 is where a game looks for a joystick; port 0 is the mouse.
        chipset.set_joystick(1, up_, down_, left_, right_, fire_);
        return !quit_;
    }

    [[nodiscard]] bool should_quit() const override { return quit_; }
    [[nodiscard]] bool is_visible() const override { return true; }

private:
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
