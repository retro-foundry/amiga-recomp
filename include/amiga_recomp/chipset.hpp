// The Amiga custom chips: Agnus, Denise and Paula, plus the two CIAs
// (AMIGA_RECOMP.md 17-25).
//
// The CPU is recompiled; the hardware is implemented. Everything here is
// asynchronous and driven by one master clock, so raster effects, Copper
// waits, blitter completion and CIA timers all line up with translated code
// executing between them.
#pragma once

#include <array>
#include <functional>
#include <vector>

#include "amiga_recomp/custom_registers.hpp"
#include "amiga_recomp/guest_memory.hpp"
#include "amiga_recomp/runtime.hpp"
#include "amiga_recomp/types.hpp"

namespace arecomp {

// --- timing --------------------------------------------------------------
// The master tick is one 68000 clock cycle. A colour clock is two of those,
// and one colour clock is two lores pixels.
constexpr u32 kColourClocksPerLinePal = 227;
constexpr u32 kColourClocksPerLineNtsc = 227;
constexpr u32 kLinesPerFramePal = 312;
constexpr u32 kLinesPerFrameNtsc = 262;
constexpr u32 kLoresPixelsPerColourClock = 2;
constexpr u32 kCiaTicksPerColourClock = 5;   // the CIA runs at colour clock / 5

// Native display geometry. The framebuffer is larger than the nominal 320x256
// so that overscan and widescreen extension have somewhere to go.
constexpr u32 kLoresPixelsPerLine = kColourClocksPerLinePal * kLoresPixelsPerColourClock;
constexpr u32 kMaxFramebufferWidth = kLoresPixelsPerLine;   // 454
constexpr u32 kMaxFramebufferHeight = kLinesPerFramePal;

enum class VideoStandard : u8 { Pal, Ntsc };

// --- widescreen ----------------------------------------------------------
//
// Level 1 of AMIGA_RECOMP.md 37: widen the fetch and display windows so the
// hardware shows more of the game's own bitplane memory.
//
// This only produces correct picture when the game's bitmap is wider than its
// display window, which is true of most side-scrollers: they render a buffer
// a few columns wider than the screen and scroll inside it with BPLCON1 and a
// negative BPL1MOD. Those spare columns are already drawn every frame and are
// simply never displayed. Widening reveals them.
//
// `slack_words()` on the display reports how much room the game's own modulo
// leaves, so a port can ask for what is actually there rather than guessing.
struct WidescreenConfig {
    bool enabled = false;
    // Extra lores pixels to reveal on each side. Rounded down to whole
    // fetch blocks (16 lores pixels in lores, 16 in hires).
    u32 extra_left = 0;
    u32 extra_right = 0;
    // Keep the row stride constant by subtracting the extra fetch from the
    // modulo. Correct whenever the game's bitmap is wider than its display
    // window; without it the picture shears by one row per line.
    bool compensate_modulo = true;
};

struct ChipsetConfig {
    VideoStandard standard = VideoStandard::Pal;
    WidescreenConfig widescreen;
    // Collect a register write log for debugging (AMIGA_RECOMP.md 33).
    bool log_register_writes = false;
};

struct RegisterWrite {
    MasterTick time = 0;
    u32 offset = 0;
    u16 value = 0;
    u16 vpos = 0;
    u16 hpos = 0;
    bool from_copper = false;
};

// One rendered frame: 12-bit Amiga colours expanded to 32-bit RGBA.
struct Framebuffer {
    u32 width = 0;
    u32 height = 0;
    std::vector<u32> pixels;

    void resize(u32 w, u32 h) {
        width = w;
        height = h;
        pixels.assign(static_cast<std::size_t>(w) * h, 0xff000000u);
    }
    [[nodiscard]] u32* line(u32 y) { return pixels.data() + static_cast<std::size_t>(y) * width; }
};

class Chipset final : public HardwareModel, public MmioDevice {
public:
    Chipset(GuestMemory& memory, Runtime& runtime, ChipsetConfig config = {});

    // --- MmioDevice ------------------------------------------------------
    u8 read8(u32 addr) override;
    u16 read16(u32 addr) override;
    void write8(u32 addr, u8 value) override;
    void write16(u32 addr, u16 value) override;

    // --- HardwareModel ---------------------------------------------------
    void run_until(MasterTick target) override;
    [[nodiscard]] u8 pending_interrupt_level() const override { return pending_level_; }
    u32 acknowledge_interrupt(u8 level) override;
    [[nodiscard]] MasterTick next_event_time() const override;

    // Maps this chipset into the guest address space at $DFF000 and $BFxxxx.
    void map(GuestMemory& memory);

    // --- observation -----------------------------------------------------
    [[nodiscard]] const Framebuffer& framebuffer() const { return frame_; }
    [[nodiscard]] u64 frames_completed() const { return frames_; }
    [[nodiscard]] u32 vpos() const { return vpos_; }
    [[nodiscard]] u32 hpos() const { return hpos_; }
    [[nodiscard]] const std::vector<RegisterWrite>& register_log() const { return log_; }
    void clear_register_log() { log_.clear(); }

    // Raw register file, for tests and the debugger. Reading it is not the
    // same as a guest read, which may have side effects.
    [[nodiscard]] u16 peek_register(u32 offset) const { return regs_[(offset & 0x1fe) >> 1]; }

    // Called once per completed frame, for the platform backend.
    void set_frame_callback(std::function<void(const Framebuffer&)> fn) {
        on_frame_ = std::move(fn);
    }

    // --- machine state (AMIGA_RECOMP.md 39) --------------------------------
    // A snapshot is memory *and* machine state. Restoring guest RAM without
    // the custom chips leaves the Copper pointing at address zero, DMA and
    // interrupts disabled, and the display unprogrammed -- a machine the game
    // never expects to find itself on.
    bool save_state(const std::string& path) const;
    bool load_state(const std::string& path);

    // --- disk ------------------------------------------------------------
    // Insert a raw ADF. The chipset builds MFM track images from it on demand,
    // so a game's own trackloader reads it exactly as it would a real disk
    // (AMIGA_RECOMP.md 4.3).
    void insert_disk(std::vector<u8> adf);
    void eject_disk();
    [[nodiscard]] bool disk_inserted() const { return !disk_.image.empty(); }
    [[nodiscard]] u32 disk_cylinder() const { return disk_.cylinder; }
    [[nodiscard]] u64 disk_reads() const { return disk_.reads; }
    [[nodiscard]] u64 disk_index_pulses() const { return disk_.index_pulses; }

    // --- input -----------------------------------------------------------
    // Joystick/mouse port state, as JOY0DAT/JOY1DAT and the fire buttons in
    // CIA-A PRA (AMIGA.md, input).
    void set_joystick(unsigned port, bool up, bool down, bool left, bool right,
                      bool fire);
    // Relative mouse movement and buttons. Port 0 is where the mouse lives,
    // and a game reads JOY0DAT as a pair of counters rather than as
    // directions, so movement has to accumulate.
    void set_mouse(int dx, int dy, bool left_button, bool right_button);
    void set_key(u8 raw_code, bool pressed);

    // --- widescreen ------------------------------------------------------
    [[nodiscard]] WidescreenConfig& widescreen() { return config_.widescreen; }
    // How many extra words per line the game's own modulo leaves available,
    // per playfield. Negative means the game is already fetching everything
    // its bitmap holds, so widening would shear the picture.
    [[nodiscard]] int slack_words(unsigned playfield) const;

    // What the extension actually came to, in lores pixels, after rounding to
    // whole fetch blocks and clamping to the scan line. A request is not a
    // promise: a PAL line holds 454 lores pixels in total, and a game whose
    // display window already reaches the end of the line has nowhere to grow.
    [[nodiscard]] u32 applied_extra_left() const { return display_.extra_left_px; }
    [[nodiscard]] u32 applied_extra_right() const { return display_.extra_right_px; }

    // --- blitter ---------------------------------------------------------
    [[nodiscard]] bool blitter_busy() const { return blitter_.active; }

private:
    // --- register file ---------------------------------------------------
    u16 read_register(u32 offset);
    void write_register(u32 offset, u16 value, bool from_copper);

    // --- interrupts ------------------------------------------------------
    void update_interrupts();
    void raise_interrupt(u16 bits);

    // --- beam / scheduling ----------------------------------------------
    void step_colour_clock();
    void begin_line();
    void end_line();
    void begin_frame();

    // --- copper ----------------------------------------------------------
    void copper_reset(u32 address);
    void copper_step();

    // --- bitplanes / display --------------------------------------------
    void recompute_display_window();
    void fetch_bitplane_words();
    void shift_out_pixels();
    void render_sprites_for_line();
    void latch_sprite_dma();

    // --- blitter ---------------------------------------------------------
    void blitter_start();
    void blitter_run();
    u16 blitter_minterm(u16 a, u16 b, u16 c) const;

    // --- audio / cia -----------------------------------------------------
    void audio_step();
    void cia_step();

    // --- disk ------------------------------------------------------------
    void disk_write_ciab_prb(u8 value);
    [[nodiscard]] u8 disk_status_bits() const;
    void disk_start_dma();
    void disk_step();
    void disk_build_track();
    void disk_index_pulse(u32 colour_clocks);

    GuestMemory& memory_;
    Runtime& runtime_;
    ChipsetConfig config_;

    std::array<u16, 256> regs_{};   // $DFF000..$DFF1FF, word indexed

    // Beam position.
    u32 hpos_ = 0;                  // colour clocks within the line
    u32 vpos_ = 0;
    u32 lines_per_frame_ = kLinesPerFramePal;
    u32 colour_clocks_per_line_ = kColourClocksPerLinePal;
    MasterTick now_ = 0;            // master time already simulated
    u64 frames_ = 0;
    u32 cia_divider_ = 0;

    // Interrupts.
    u8 pending_level_ = 0;

    // Copper.
    struct Copper {
        u32 pc = 0;
        bool enabled = false;
        bool waiting = false;
        u16 wait_vpos = 0;
        u16 wait_hpos = 0;
        u16 wait_mask_v = 0;
        u16 wait_mask_h = 0;
        bool skip_next = false;
        u32 cycle = 0;              // colour clocks until the next fetch
    } copper_;

    // Display state for the current line.
    struct Display {
        u32 diw_start_x = 129;      // lores pixels
        u32 diw_stop_x = 449;
        u32 diw_start_y = 44;
        u32 diw_stop_y = 244;
        u32 ddf_start = 0x38;       // colour clocks
        u32 ddf_stop = 0xd0;
        u32 fetch_words = 0;        // words per plane per line
        bool hires = false;
        u32 planes = 0;
        bool ham = false;
        bool dual_playfield = false;
        u32 scroll_pf1 = 0;         // lores pixels of delay from BPLCON1
        u32 scroll_pf2 = 0;
        bool fetching = false;
        u32 fetched_words = 0;
        u32 next_fetch_hpos = 0;
        u32 out_x = 0;              // framebuffer column being written

        // The rectangle actually rendered, fixed at the start of each frame
        // so the framebuffer does not resize under a mid-frame DIW change.
        // With widescreen enabled this is wider than the game's own window.
        u32 view_x0 = 0;
        u32 view_y0 = 0;
        u32 view_w = 0;
        u32 view_h = 0;
        // Extra words fetched per plane per line because of the widened
        // window, and the modulo correction that keeps the row stride intact.
        u32 extra_words = 0;
        // The extension actually applied, in lores pixels. Rounded to whole
        // fetch blocks so that the fetch window and the view agree; asking for
        // 40 extra pixels in lores gets 32.
        u32 extra_left_px = 0;
        u32 extra_right_px = 0;
        // Words gained on the left. The bitplane pointers must walk back by
        // this much each frame, or the left margin shows the columns that
        // belong on the right.
        u32 extra_left_words = 0;
    } display_;

    std::array<u32, 8> bpl_pointer_{};   // working pointers, reloaded per frame
    std::array<u64, 8> shift_{};         // per-plane pixel shift registers
    std::array<u32, 8> shift_count_{};
    // The pixel most recently shifted out, so a lores pixel can cover both of
    // its hires halves without consuming twice.
    u32 last_index_ = 0;
    bool last_index_valid_ = false;
    u16 ham_previous_ = 0;               // hold-and-modify running colour

    // Sprites.
    struct Sprite {
        u32 pointer = 0;
        u16 pos = 0;
        u16 ctl = 0;
        u16 data_a = 0;
        u16 data_b = 0;
        bool armed = false;
        u32 start_line = 0;
        u32 stop_line = 0;
        u32 x = 0;
        bool attached = false;
    };
    std::array<Sprite, 8> sprites_{};

    // Blitter.
    struct Blitter {
        bool active = false;
        u32 width = 0;     // words per row
        u32 height = 0;
        u32 x = 0, y = 0;
        u32 pt[4]{};       // A, B, C, D
        s16 mod[4]{};
        u16 dat[4]{};
        u16 hold_a = 0, hold_b = 0;
        u32 a_shift = 0, b_shift = 0;
        bool descending = false;
        bool fill_enabled = false;
        bool fill_carry = false;
        bool line_mode = false;
        bool any_nonzero = false;
        u64 cycles_remaining = 0;
    } blitter_;

    // Audio channels: enough state to keep DMA and interrupts honest even
    // where sample output is not consumed.
    struct AudioChannel {
        u32 location = 0;
        u32 pointer = 0;
        u16 length = 1;
        u32 words_left = 0;
        u16 period = 1;
        u16 volume = 0;
        s32 counter = 0;
        u16 sample = 0;
        bool dma_on = false;
        s8 output = 0;
    };
    std::array<AudioChannel, 4> audio_{};

    // CIAs. Only what games actually use: timers, the interrupt they raise,
    // and the port bits carrying joystick fire and the keyboard line.
    struct Cia {
        u8 pra = 0xff, prb = 0xff;
        u8 ddra = 0, ddrb = 0;
        u16 timer_a = 0xffff, timer_b = 0xffff;
        u16 latch_a = 0xffff, latch_b = 0xffff;
        u8 cra = 0, crb = 0;
        u8 icr_mask = 0;
        u8 icr_data = 0;
        u32 tod = 0;
        bool irq = false;
    };
    Cia cia_a_{};
    Cia cia_b_{};

    // Keyboard. The serial register holds one code at a time, so the rest
    // queue behind it until the game reads what is there.
    std::vector<u8> key_queue_;
    u8 kbd_sdr_ = 0;
    bool key_in_flight_ = false;
    void deliver_next_key();

    u8 cia_read(Cia& cia, u32 reg, bool is_a);
    void cia_write(Cia& cia, u32 reg, u8 value, bool is_a);

    // Disk. The image is the raw ADF; `track` is the MFM encoding of the
    // currently selected cylinder and head, rebuilt when the head moves.
    struct Disk {
        std::vector<u8> image;
        std::vector<u8> track;     // MFM bytes for the selected track
        u32 track_index = 0xffffffffu;   // which track `track` holds
        u32 cylinder = 0;
        u32 side = 0;
        bool motor = false;
        bool selected = false;
        bool step_line = true;     // /STEP is active low
        // /CHNG is asserted when a disk is swapped and stays asserted until
        // the head steps with a disk in the drive. Loaders step in a loop
        // waiting for exactly that, so getting it wrong hangs them.
        bool change_flag = true;
        bool direction_out = false;
        // DMA in progress.
        bool dma_active = false;
        u32 words_remaining = 0;
        u32 position = 0;          // byte position within the track
        u32 length_written = 0;    // DSKLEN is written twice to arm a transfer
        u16 last_length = 0;
        u64 reads = 0;
        // A spinning drive pulses INDEX once per revolution, which reaches the
        // guest as CIA-B's FLG interrupt. Loaders time their reads against it,
        // so without it they sit in a timeout loop and give up.
        u64 index_countdown = 0;
        u64 index_pulses = 0;
    } disk_;

    // Input.
    u16 joy_dat_[2] = {0, 0};
    bool joy_fire_[2] = {false, false};
    u8 mouse_x_ = 0, mouse_y_ = 0;      // the port 0 quadrature counters
    bool mouse_right_ = false;
    void refresh_fire_buttons();

    Framebuffer frame_;
    std::function<void(const Framebuffer&)> on_frame_;
    std::vector<RegisterWrite> log_;
};

} // namespace arecomp
