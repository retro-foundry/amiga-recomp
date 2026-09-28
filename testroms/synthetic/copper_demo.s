* copper_demo.s
*
* An end-to-end demonstration: 68000 code that programs the Amiga custom chips
* the way a real game does, then gets statically recompiled and run natively.
*
* It draws a bitmap wider than its display window, exactly as a scrolling game
* does, so the widescreen extension has real content to reveal. The Copper
* changes the background colour down the screen so the raster is obviously
* being produced by the hardware rather than blitted once.
*
* Display window : 256 lores pixels, at 145..401
* Bitmap         : 320 lores pixels, 40 bytes per row
* Spare columns  : 32 pixels each side, drawn but never displayed
*
*   python tools/asm68k.py testroms/synthetic/copper_demo.s \
*       -o testroms/synthetic/copper_demo.bin --org 0x1000

        org     $1000

CUSTOM   equ     $00dff000
HALT     equ     $00f00000
BITPLANE equ     $00020000

DIWSTRT  equ     $00dff08e
DIWSTOP  equ     $00dff090
DDFSTRT  equ     $00dff092
DDFSTOP  equ     $00dff094
DMACON   equ     $00dff096
BPLCON0  equ     $00dff100
BPLCON1  equ     $00dff102
BPL1MOD  equ     $00dff108
BPL1PTH  equ     $00dff0e0
COP1LCH  equ     $00dff080
COPJMP1  equ     $00dff088
COLOR00  equ     $00dff180
COLOR01  equ     $00dff182
VHPOSR   equ     $00dff006

start:
        bsr     draw_bitmap
        bsr     setup_display
        bsr     wait_frames

        move.w  #1,HALT
        stop    #$2700

* --------------------------------------------------------------------------
* Fill the bitmap: solid margins either side of a striped middle, so it is
* obvious which columns came from where.
* --------------------------------------------------------------------------
draw_bitmap:
        lea     BITPLANE,a0
        move.w  #199,d7                 ; 200 rows
row_loop:
        move.w  #$ffff,(a0)+            ; left margin, 32 pixels
        move.w  #$ffff,(a0)+
        moveq   #15,d6                  ; 16 words of visible area
mid_loop:
        move.w  #$8181,(a0)+
        dbra    d6,mid_loop
        move.w  #$ffff,(a0)+            ; right margin, 32 pixels
        move.w  #$ffff,(a0)+
        dbra    d7,row_loop
        rts

* --------------------------------------------------------------------------
* Program the display: a 256 pixel window into a 320 pixel bitmap, with the
* pointer two words in and the modulo taking up the difference. This is the
* standard arrangement for a game that scrolls horizontally.
* --------------------------------------------------------------------------
setup_display:
        move.w  #$2c91,DIWSTRT          ; window at lines 44..244, x 145..401
        move.w  #$f491,DIWSTOP
        move.w  #$0040,DDFSTRT          ; 16 words of fetch, lined up with it
        move.w  #$00b8,DDFSTOP
        move.w  #$1000,BPLCON0          ; one bitplane, lores
        move.w  #$0000,BPLCON1
        move.w  #8,BPL1MOD              ; 40 byte rows, 32 bytes fetched

        move.l  #BITPLANE+4,BPL1PTH     ; start two words into each row
        move.w  #$0000,COLOR00
        move.w  #$0fff,COLOR01

        move.l  #copperlist,COP1LCH
        move.w  #0,COPJMP1              ; strobe: start the copper at the list.
*                                       ; Without this it carries on from
*                                       ; wherever its program counter was and
*                                       ; interprets whatever it finds as copper
*                                       ; instructions, which on real hardware
*                                       ; stomps the registers just as it does
*                                       ; here.
        move.w  #$8380,DMACON           ; master, bitplane and copper dma
        rts

* --------------------------------------------------------------------------
* Wait for four vertical blanks by watching the beam counter, which is what a
* game without interrupts enabled would do.
* --------------------------------------------------------------------------
wait_frames:
        moveq   #4,d5
frame_loop:
        bsr     wait_vblank
        subq.w  #1,d5
        bne     frame_loop
        rts

wait_vblank:
        move.w  VHPOSR,d0               ; wait until the beam leaves line 0
        and.w   #$ff00,d0
        beq     wait_vblank
still_in_frame:
        move.w  VHPOSR,d0               ; then until it comes back round
        and.w   #$ff00,d0
        bne     still_in_frame
        rts

* --------------------------------------------------------------------------
* The Copper list. It lives in chip ram because the program itself is loaded
* there, so the Copper reads it in place with no copying.
* --------------------------------------------------------------------------
        even
copperlist:
        dc.w    $0180,$0002             ; colour 0 at the top of the frame
        dc.w    $3801,$fffe             ; wait for line $38
        dc.w    $0180,$0204
        dc.w    $5001,$fffe
        dc.w    $0180,$0406
        dc.w    $6801,$fffe
        dc.w    $0180,$0608
        dc.w    $8001,$fffe
        dc.w    $0180,$080a
        dc.w    $9801,$fffe
        dc.w    $0180,$0a0c
        dc.w    $b001,$fffe
        dc.w    $0180,$0c0e
        dc.w    $c801,$fffe
        dc.w    $0180,$0e0f
        dc.w    $ffff,$fffe             ; end of list
