// Custom chip register map, centred on $DFF000 (AMIGA_RECOMP.md 18, AMIGA.md).
//
// Offsets only. The behaviour lives in the chipset, because these are not
// storage locations: reads and writes have side effects, and DMACON, INTENA,
// INTREQ and ADKCON use bit-15 set/clear semantics (rule 12).
#pragma once

#include "amiga_recomp/types.hpp"

namespace arecomp {

constexpr u32 kCustomBase = 0x00dff000;
constexpr u32 kCustomSize = 0x00001000;
constexpr u32 kCiaBase = 0x00bf0000;
constexpr u32 kCiaSize = 0x00010000;

namespace reg {

// --- read ---------------------------------------------------------------
enum : u32 {
    DMACONR = 0x002,
    VPOSR   = 0x004,
    VHPOSR  = 0x006,
    DSKDATR = 0x008,
    JOY0DAT = 0x00a,
    JOY1DAT = 0x00c,
    CLXDAT  = 0x00e,
    ADKCONR = 0x010,
    POT0DAT = 0x012,
    POT1DAT = 0x014,
    POTGOR  = 0x016,
    SERDATR = 0x018,
    DSKBYTR = 0x01a,
    INTENAR = 0x01c,
    INTREQR = 0x01e,
};

// --- write --------------------------------------------------------------
enum : u32 {
    DSKPTH  = 0x020,
    DSKPTL  = 0x022,
    DSKLEN  = 0x024,
    DSKDAT  = 0x026,
    REFPTR  = 0x028,
    VPOSW   = 0x02a,
    VHPOSW  = 0x02c,
    COPCON  = 0x02e,
    SERDAT  = 0x030,
    SERPER  = 0x032,
    POTGO   = 0x034,
    JOYTEST = 0x036,
    STREQU  = 0x038,
    STRVBL  = 0x03a,
    STRHOR  = 0x03c,
    STRLONG = 0x03e,

    BLTCON0 = 0x040,
    BLTCON1 = 0x042,
    BLTAFWM = 0x044,
    BLTALWM = 0x046,
    BLTCPTH = 0x048,
    BLTCPTL = 0x04a,
    BLTBPTH = 0x04c,
    BLTBPTL = 0x04e,
    BLTAPTH = 0x050,
    BLTAPTL = 0x052,
    BLTDPTH = 0x054,
    BLTDPTL = 0x056,
    BLTSIZE = 0x058,
    BLTCON0L = 0x05a,   // ECS
    BLTSIZV = 0x05c,    // ECS
    BLTSIZH = 0x05e,    // ECS
    BLTCMOD = 0x060,
    BLTBMOD = 0x062,
    BLTAMOD = 0x064,
    BLTDMOD = 0x066,
    BLTCDAT = 0x070,
    BLTBDAT = 0x072,
    BLTADAT = 0x074,
    DSKSYNC = 0x07e,

    COP1LCH = 0x080,
    COP1LCL = 0x082,
    COP2LCH = 0x084,
    COP2LCL = 0x086,
    COPJMP1 = 0x088,
    COPJMP2 = 0x08a,
    COPINS  = 0x08c,
    DIWSTRT = 0x08e,
    DIWSTOP = 0x090,
    DDFSTRT = 0x092,
    DDFSTOP = 0x094,
    DMACON  = 0x096,
    CLXCON  = 0x098,
    INTENA  = 0x09a,
    INTREQ  = 0x09c,
    ADKCON  = 0x09e,

    AUD0LCH = 0x0a0,    // channels are 16 bytes apart
    AUD0LCL = 0x0a2,
    AUD0LEN = 0x0a4,
    AUD0PER = 0x0a6,
    AUD0VOL = 0x0a8,
    AUD0DAT = 0x0aa,

    BPL1PTH = 0x0e0,    // bitplane pointers are 4 bytes apart
    BPL1PTL = 0x0e2,

    BPLCON0 = 0x100,
    BPLCON1 = 0x102,
    BPLCON2 = 0x104,
    BPLCON3 = 0x106,    // ECS/AGA
    BPL1MOD = 0x108,
    BPL2MOD = 0x10a,

    BPL1DAT = 0x110,    // 2 bytes apart

    SPR0PTH = 0x120,    // 4 bytes apart
    SPR0PTL = 0x122,

    SPR0POS = 0x140,    // 8 bytes apart
    SPR0CTL = 0x142,
    SPR0DATA = 0x144,
    SPR0DATB = 0x146,

    COLOR00 = 0x180,    // 32 entries, 2 bytes apart
};

} // namespace reg

// --- DMACON bits ---------------------------------------------------------
enum : u16 {
    DMAF_SETCLR = 0x8000,
    DMAF_BLTDONE = 0x4000,   // read-only in DMACONR
    DMAF_BLTNZERO = 0x2000,  // read-only in DMACONR
    DMAF_BLITHOG = 0x0400,
    DMAF_MASTER = 0x0200,
    DMAF_RASTER = 0x0100,    // bitplanes
    DMAF_COPPER = 0x0080,
    DMAF_BLITTER = 0x0040,
    DMAF_SPRITE = 0x0020,
    DMAF_DISK = 0x0010,
    DMAF_AUD3 = 0x0008,
    DMAF_AUD2 = 0x0004,
    DMAF_AUD1 = 0x0002,
    DMAF_AUD0 = 0x0001,
    // Every DMA channel plus the master enable. DMACON's writable bits run
    // from AUD0 up to BLTPRI, which is what the set/clear mask uses.
    DMAF_ALL = 0x03ff,
    DMAF_WRITABLE = 0x07ff,
};

// --- INTENA / INTREQ bits ------------------------------------------------
enum : u16 {
    INTF_SETCLR = 0x8000,
    INTF_INTEN = 0x4000,     // master interrupt enable
    INTF_EXTER = 0x2000,     // level 6
    INTF_DSKSYN = 0x1000,    // level 5
    INTF_RBF = 0x0800,       // level 5
    INTF_AUD3 = 0x0400,      // level 4
    INTF_AUD2 = 0x0200,
    INTF_AUD1 = 0x0100,
    INTF_AUD0 = 0x0080,
    INTF_BLIT = 0x0040,      // level 3
    INTF_VERTB = 0x0020,     // level 3
    INTF_COPER = 0x0010,     // level 3
    INTF_PORTS = 0x0008,     // level 2
    INTF_SOFT = 0x0004,      // level 1
    INTF_DSKBLK = 0x0002,    // level 1
    INTF_TBE = 0x0001,       // level 1
};

// The 68000 interrupt level each INTREQ bit asserts.
constexpr u8 interrupt_level_for_bit(unsigned bit) noexcept {
    switch (bit) {
    case 0: case 1: case 2: return 1;
    case 3: return 2;
    case 4: case 5: case 6: return 3;
    case 7: case 8: case 9: case 10: return 4;
    case 11: case 12: return 5;
    case 13: return 6;
    default: return 0;
    }
}

// --- BPLCON0 bits --------------------------------------------------------
enum : u16 {
    BPLCON0_HIRES = 0x8000,
    BPLCON0_BPU_MASK = 0x7000,
    BPLCON0_BPU_SHIFT = 12,
    BPLCON0_HAM = 0x0800,
    BPLCON0_DBLPF = 0x0400,
    BPLCON0_COLOR = 0x0200,   // composite colour enable
    BPLCON0_GAUD = 0x0100,
    BPLCON0_LPEN = 0x0008,
    BPLCON0_LACE = 0x0004,
    BPLCON0_ERSY = 0x0002,
};

// Register name for hardware logging (AMIGA_RECOMP.md 33). Returns nullptr
// for offsets with no documented name, which is itself worth logging.
const char* custom_register_name(u32 offset) noexcept;

} // namespace arecomp
