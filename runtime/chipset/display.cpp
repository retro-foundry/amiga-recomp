// Bitplanes, palette, sprites and the display window (AMIGA_RECOMP.md 20, 21).
//
// The beam walks the line one colour clock at a time and pixels are produced
// as it goes, so a Copper write that lands mid-line changes the colour of the
// pixels after it and not the ones before. That is what raster bars, split
// palettes and mid-screen mode changes depend on.
//
// Known approximation: bitplane DMA is modelled as a fetch every eight colour
// clocks across the fetch window rather than at Agnus's exact DMA slots, and
// the fetch-to-display pipeline delay is taken up by the shift registers
// rather than reproduced cycle for cycle. Geometry, scrolling and modulo
// behaviour are exact; a demo that counts individual DMA slots is not.

#include <algorithm>

#include "amiga_recomp/chipset.hpp"

namespace arecomp {

namespace {

constexpr u32 kDmaAddressMask = 0x001ffffe;

// Amiga colour registers are 12-bit: 0x0RGB. Scaling each nibble by 17 maps
// 0..15 onto 0..255 without drifting away from white.
u32 expand_colour(u16 value) {
    const u32 r = (value >> 8) & 0x0f;
    const u32 g = (value >> 4) & 0x0f;
    const u32 b = value & 0x0f;
    return 0xff000000u | (r * 17u << 16) | (g * 17u << 8) | (b * 17u);
}

} // namespace

void Chipset::recompute_display_window() {
    const u16 bplcon0 = regs_[reg::BPLCON0 >> 1];
    const u16 bplcon1 = regs_[reg::BPLCON1 >> 1];
    const u16 diwstrt = regs_[reg::DIWSTRT >> 1];
    const u16 diwstop = regs_[reg::DIWSTOP >> 1];
    const u16 ddfstrt = regs_[reg::DDFSTRT >> 1];
    const u16 ddfstop = regs_[reg::DDFSTOP >> 1];

    display_.hires = (bplcon0 & BPLCON0_HIRES) != 0;
    display_.planes = (bplcon0 & BPLCON0_BPU_MASK) >> BPLCON0_BPU_SHIFT;
    display_.ham = (bplcon0 & BPLCON0_HAM) != 0;
    display_.dual_playfield = (bplcon0 & BPLCON0_DBLPF) != 0;
    display_.scroll_pf1 = bplcon1 & 0x0f;
    display_.scroll_pf2 = (bplcon1 >> 4) & 0x0f;

    // Display window. Horizontal values are lores pixels; DIWSTOP's high bit
    // is implied, and its vertical stop rolls over past 255.
    display_.diw_start_x = diwstrt & 0xff;
    display_.diw_stop_x = (diwstop & 0xff) | 0x100;
    display_.diw_start_y = (diwstrt >> 8) & 0xff;
    const u32 vstop = (diwstop >> 8) & 0xff;
    display_.diw_stop_y = vstop >= 0x80 ? vstop : vstop + 0x100;

    // Fetch window, in colour clocks.
    u32 ddf_start = ddfstrt & 0xfc;
    u32 ddf_stop = ddfstop & 0xfc;

    // Widescreen extension: pull the fetch window outwards by whole fetch
    // blocks, so the hardware reads the columns the game already renders
    // outside its display window (docs/widescreen.md).
    // A fetch block is 8 colour clocks in lores and 4 in hires, and each one
    // yields one word per plane: 16 lores pixels, or 16 hires pixels which
    // cover 8 lores positions.
    const u32 block = display_.hires ? 4u : 8u;
    const u32 block_lores_px = display_.hires ? 8u : 16u;
    u32 extra_blocks = 0;
    display_.extra_left_px = 0;
    display_.extra_right_px = 0;
    if (config_.widescreen.enabled) {
        u32 left_blocks = config_.widescreen.extra_left / block_lores_px;
        const u32 right_blocks = config_.widescreen.extra_right / block_lores_px;

        // Never fetch before the start of the line.
        left_blocks = std::min(left_blocks, ddf_start / block);
        ddf_start -= left_blocks * block;
        ddf_stop = std::min(ddf_stop + right_blocks * block,
                            colour_clocks_per_line_ - block);

        extra_blocks = left_blocks + right_blocks;
        // The view must be extended by exactly what the fetch window gained,
        // or the picture slides sideways relative to the extra columns.
        display_.extra_left_px = left_blocks * block_lores_px;
        display_.extra_right_px = right_blocks * block_lores_px;
    }

    display_.ddf_start = ddf_start;
    display_.ddf_stop = ddf_stop;

    if (ddf_stop >= ddf_start) {
        display_.fetch_words = display_.hires ? ((ddf_stop - ddf_start) / 4) + 2
                                              : ((ddf_stop - ddf_start) / 8) + 1;
    } else {
        display_.fetch_words = 0;
    }
    // One extra word per plane per block, in both lores and hires.
    display_.extra_words = extra_blocks;
}

void Chipset::fetch_bitplane_words() {
    if (display_.planes == 0 || display_.fetch_words == 0) return;
    if (vpos_ < display_.diw_start_y || vpos_ >= display_.diw_stop_y) return;
    if (hpos_ < display_.ddf_start || hpos_ > display_.ddf_stop) return;

    const u32 step = display_.hires ? 4u : 8u;
    if (((hpos_ - display_.ddf_start) % step) != 0) return;
    if (display_.fetched_words >= display_.fetch_words) return;

    for (u32 plane = 0; plane < display_.planes; ++plane) {
        const u16 word = memory_.peek16(bpl_pointer_[plane]);
        bpl_pointer_[plane] = (bpl_pointer_[plane] + 2) & kDmaAddressMask;
        // BPLxDAT is visible to the guest and to the Copper.
        regs_[(reg::BPL1DAT + plane * 2) >> 1] = word;
        shift_[plane] = (shift_[plane] << 16) | word;
        shift_count_[plane] += 16;
    }
    ++display_.fetched_words;
    display_.fetching = true;
}

void Chipset::shift_out_pixels() {
    if (vpos_ < display_.view_y0 || vpos_ >= display_.view_y0 + display_.view_h) return;
    if (frame_.pixels.empty()) return;

    const u32 y = vpos_ - display_.view_y0;
    u32* row = frame_.line(y);
    const u32 fb_width = frame_.width;

    const u32 background = expand_colour(regs_[reg::COLOR00 >> 1]);

    // Work in hires units throughout: four per colour clock. A lores pixel
    // occupies two of them and consumes one bitplane bit; a hires pixel
    // occupies one and consumes one.
    for (u32 i = 0; i < 4; ++i) {
        const u32 hires_x = hpos_ * 4 + i;
        const u32 lores_x = hires_x / 2;

        if (lores_x < display_.view_x0) continue;
        const u32 fb_x = (lores_x - display_.view_x0) * 2 + (hires_x & 1);
        if (fb_x >= fb_width) continue;

        const bool inside_diw = lores_x >= display_.diw_start_x &&
                                lores_x < display_.diw_stop_x;
        const bool widened = config_.widescreen.enabled && !inside_diw;

        u32 colour = background;
        u32 index = 0;
        bool have_pixel = false;

        // In lores the same bitplane pixel covers both halves, so it is only
        // shifted out on the first of them.
        const bool new_pixel = display_.hires || ((hires_x & 1) == 0);

        if ((inside_diw || widened) && display_.planes > 0) {
            if (new_pixel) {
                for (u32 plane = 0; plane < display_.planes; ++plane) {
                    if (shift_count_[plane] == 0) continue;
                    // The scroll delay is per playfield: even-numbered planes
                    // are playfield one, odd ones playfield two. Delaying means
                    // reading further back in the shift register, towards data
                    // fetched earlier, which is why the index goes up.
                    const u32 scroll = ((plane & 1) == 0) ? display_.scroll_pf1
                                                          : display_.scroll_pf2;
                    const u32 bit_index = shift_count_[plane] - 1 + scroll;
                    // Beyond the register is data that was never fetched: the
                    // pixels a scroll pulls in at the left edge before the
                    // game's extra fetch word arrives.
                    const u32 bit = bit_index < 64
                                        ? ((shift_[plane] >> bit_index) & 1u)
                                        : 0u;
                    index |= bit << plane;
                    have_pixel = true;
                }
                for (u32 plane = 0; plane < display_.planes; ++plane)
                    if (shift_count_[plane] > 0) --shift_count_[plane];
                last_index_ = index;
                last_index_valid_ = have_pixel;
            } else {
                index = last_index_;
                have_pixel = last_index_valid_;
            }
        }

        if (have_pixel) {
            if (display_.ham && display_.planes >= 6) {
                // HAM: the top two bits select hold-and-modify behaviour.
                const u32 control = (index >> 4) & 3;
                const u32 value = index & 0x0f;
                if (lores_x == display_.view_x0) ham_previous_ = regs_[reg::COLOR00 >> 1];
                u16 rgb = ham_previous_;
                switch (control) {
                case 0: rgb = regs_[(reg::COLOR00 + (index & 0x0f) * 2) >> 1]; break;
                case 1: rgb = static_cast<u16>((rgb & 0x0ff0) | value); break;        // blue
                case 2: rgb = static_cast<u16>((rgb & 0x00ff) | (value << 8)); break; // red
                default: rgb = static_cast<u16>((rgb & 0x0f0f) | (value << 4)); break;// green
                }
                ham_previous_ = rgb;
                colour = expand_colour(rgb);
            } else if (display_.dual_playfield) {
                // Playfield one takes the odd planes, playfield two the even.
                u32 pf1 = 0, pf2 = 0;
                for (u32 plane = 0; plane < display_.planes; ++plane) {
                    const u32 bit = (index >> plane) & 1u;
                    if ((plane & 1) == 0) pf1 |= bit << (plane / 2);
                    else pf2 |= bit << (plane / 2);
                }
                const bool pf2_in_front = (regs_[reg::BPLCON2 >> 1] & 0x0040) != 0;
                if (pf2_in_front) index = pf2 ? (pf2 + 8) : pf1;
                else index = pf1 ? pf1 : (pf2 ? pf2 + 8 : 0);
                colour = expand_colour(regs_[(reg::COLOR00 + (index & 31) * 2) >> 1]);
            } else {
                colour = expand_colour(regs_[(reg::COLOR00 + (index & 31) * 2) >> 1]);
            }
        }

        row[fb_x] = colour;
    }

    // Sprites are drawn over the playfield the beam has just produced.
    render_sprites_for_line();
}

void Chipset::render_sprites_for_line() {
    const u16 dmacon = regs_[reg::DMACON >> 1];
    if (!(dmacon & DMAF_MASTER) || !(dmacon & DMAF_SPRITE)) return;
    if (vpos_ < display_.view_y0 || vpos_ >= display_.view_y0 + display_.view_h) return;
    if (frame_.pixels.empty()) return;

    const u32 y = vpos_ - display_.view_y0;
    u32* row = frame_.line(y);

    for (u32 i = 0; i < 8; ++i) {
        const Sprite& sprite = sprites_[i];
        if (!sprite.armed) continue;
        if (vpos_ < sprite.start_line || vpos_ >= sprite.stop_line) continue;

        // A sprite covers 16 lores pixels starting at its horizontal position.
        for (u32 bit = 0; bit < 16; ++bit) {
            const u32 lores_x = sprite.x + bit;
            if (lores_x < display_.view_x0) continue;
            const u32 fb_x = (lores_x - display_.view_x0) * 2;
            if (fb_x + 1 >= frame_.width) continue;
            // Only draw where the beam has already been this colour clock.
            if (lores_x > hpos_ * kLoresPixelsPerColourClock + 1) continue;

            const u32 shift = 15 - bit;
            const u32 low = (sprite.data_a >> shift) & 1u;
            const u32 high = (sprite.data_b >> shift) & 1u;
            const u32 value = (high << 1) | low;
            if (value == 0) continue;   // transparent

            // Sprite pairs share four colours, starting at COLOR16.
            const u32 pair = i / 2;
            const u32 colour_index = 16 + pair * 4 + value;
            const u32 colour = expand_colour(regs_[(reg::COLOR00 + colour_index * 2) >> 1]);
            row[fb_x] = colour;
            row[fb_x + 1] = colour;
        }
    }
}

void Chipset::latch_sprite_dma() {
    const u16 dmacon = regs_[reg::DMACON >> 1];
    if (!(dmacon & DMAF_MASTER) || !(dmacon & DMAF_SPRITE)) return;

    for (u32 i = 0; i < 8; ++i) {
        Sprite& sprite = sprites_[i];
        if (sprite.pointer == 0) continue;

        if (vpos_ == sprite.stop_line) sprite.armed = false;

        if (sprite.armed && vpos_ > sprite.start_line && vpos_ < sprite.stop_line) {
            // An armed sprite fetches two data words per line.
            sprite.data_a = memory_.peek16(sprite.pointer);
            sprite.data_b = memory_.peek16(sprite.pointer + 2);
            sprite.pointer = (sprite.pointer + 4) & kDmaAddressMask;
            continue;
        }

        if (!sprite.armed) {
            // Otherwise it reads the next control word pair, which says where
            // the next sprite image begins. An all-zero pair ends the list.
            const u16 pos = memory_.peek16(sprite.pointer);
            const u16 ctl = memory_.peek16(sprite.pointer + 2);
            if (pos == 0 && ctl == 0) continue;

            sprite.pos = pos;
            sprite.ctl = ctl;
            sprite.start_line = ((ctl & 0x04) << 6) | ((pos >> 8) & 0xff);
            sprite.stop_line = ((ctl & 0x02) << 7) | ((ctl >> 8) & 0xff);
            sprite.x = ((pos & 0xff) << 1) | (ctl & 1);
            sprite.attached = (ctl & 0x80) != 0;
            sprite.pointer = (sprite.pointer + 4) & kDmaAddressMask;

            if (vpos_ + 1 >= sprite.start_line && sprite.stop_line > sprite.start_line)
                sprite.armed = true;
        }
    }
}

} // namespace arecomp
