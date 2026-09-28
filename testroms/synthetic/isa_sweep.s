* isa_sweep.s
*
* Broad MC68000 coverage for differential testing (AMIGA_RECOMP.md 28.2).
*
* Every result is written to the scratch area so the interpreter-versus-native
* comparison covers guest memory as well as registers. Condition codes are
* captured after the interesting operations, because a wrong flag is the
* failure mode that otherwise hides until a game plays subtly wrong.
*
* Deliberately avoided: anything needing exception vectors (TRAP, CHK failure,
* divide by zero), and anything reading uninitialised memory.
*
*   python tools/asm68k.py testroms/synthetic/isa_sweep.s \
*       -o testroms/synthetic/isa_sweep.bin --org 0x1000

        org     $1000

HALT    equ     $00f00000
WORK    equ     $00003000       ; results land here
SRC     equ     $00002000       ; source data

start:
        lea     SRC,a0
        lea     WORK,a1

* ---------------------------------------------------------------- source data
        move.l  #$0f0f0f0f,(a0)
        move.l  #$12345678,4(a0)
        move.w  #$8000,8(a0)
        move.w  #$7fff,10(a0)
        move.l  #$fffffffe,12(a0)

* ------------------------------------------------- addressing mode coverage
        move.l  (a0),d0                 ; (an)
        move.l  d0,(a1)

        move.l  4(a0),d1                ; d16(an)
        move.l  d1,4(a1)

        moveq   #4,d2
        move.l  0(a0,d2.w),d3           ; d8(an,xn.w)
        move.l  d3,8(a1)

        moveq   #4,d2
        move.l  0(a0,d2.l),d3           ; d8(an,xn.l)
        move.l  d3,12(a1)

        move.l  SRC,d4                  ; absolute long
        move.l  d4,16(a1)

        movea.l a0,a2
        move.l  (a2)+,d5                ; (an)+
        move.l  d5,20(a1)
        move.l  -(a2),d6                ; -(an)
        move.l  d6,24(a1)

        move.l  a2,28(a1)               ; pointer must be back where it began

        move.w  pcdata(pc),d7           ; d16(pc)
        move.w  d7,32(a1)

* ------------------------------------------------------- arithmetic and flags
        move.w  8(a0),d0                ; $8000
        move.w  10(a0),d1               ; $7fff

        add.w   d1,d0                   ; $8000 + $7fff = $ffff, no overflow
        move.w  sr,d2                   ; capture flags before MOVE clobbers them
        move.w  d0,40(a1)
        move.w  d2,42(a1)

        move.w  10(a0),d0
        addq.w  #1,d0                   ; $7fff + 1 overflows
        move.w  sr,d2
        move.w  d0,44(a1)
        move.w  d2,46(a1)

        moveq   #0,d0
        subq.b  #1,d0                   ; 0 - 1 borrows
        move.w  sr,d2
        move.w  d0,48(a1)
        move.w  d2,50(a1)

        move.l  #$00010000,d0
        move.l  #$00000001,d1
        sub.l   d1,d0
        move.l  d0,52(a1)

        move.l  #$aaaa5555,d0
        and.l   #$ffff0000,d0
        move.l  d0,56(a1)
        move.l  #$aaaa5555,d0
        or.l    #$0000ffff,d0
        move.l  d0,60(a1)
        move.l  #$aaaa5555,d0
        eor.l   #$ffffffff,d0
        move.l  d0,64(a1)
        not.l   d0
        move.l  d0,68(a1)

        move.l  #$00000080,d0
        neg.b   d0
        move.l  d0,72(a1)
        move.l  #$00000001,d0
        neg.l   d0
        move.l  d0,76(a1)

* --------------------------------------------------------- extend arithmetic
        move.l  #$0000ffff,d0
        move.l  #$00000001,d1
        add.w   d1,d0                   ; sets X
        addx.w  d1,d0                   ; consumes X
        move.w  sr,d2
        move.w  d0,80(a1)
        move.w  d2,82(a1)

        move.l  #$00000000,d0
        move.l  #$00000001,d1
        sub.w   d1,d0                   ; sets X
        subx.w  d1,d0
        move.w  d0,84(a1)

* ---------------------------------------------------------------------- BCD
        move.w  #0,ccr                  ; clear X before a BCD chain
        move.l  #$00000028,d0
        move.l  #$00000014,d1
        abcd    d1,d0                   ; 28 + 14 = 42
        move.b  d0,88(a1)

        move.w  #0,ccr
        move.l  #$00000042,d0
        move.l  #$00000014,d1
        sbcd    d1,d0                   ; 42 - 14 = 28
        move.b  d0,89(a1)

        move.w  #0,ccr
        move.l  #$00000001,d0
        nbcd    d0
        move.b  d0,90(a1)

* ------------------------------------------------------- multiply and divide
        move.l  #$0000ffff,d0
        move.l  #$0000ffff,d1
        mulu    d1,d0
        move.l  d0,92(a1)

        move.l  #$0000ffff,d0
        move.l  #$0000ffff,d1
        muls    d1,d0                   ; -1 * -1
        move.l  d0,96(a1)

        move.l  #100,d0
        move.l  #7,d1
        divu    d1,d0
        move.l  d0,100(a1)

        move.l  #-100,d0
        move.l  #7,d1
        divs    d1,d0
        move.l  d0,104(a1)

        move.l  #$00010000,d0           ; quotient will not fit: V only
        move.l  #1,d1
        divu    d1,d0
        move.w  sr,d2
        move.l  d0,108(a1)              ; destination must be unchanged
        move.w  d2,112(a1)

* -------------------------------------------------------- shifts and rotates
        move.l  #$00000040,d0
        asl.b   #1,d0                   ; sign changes: V set
        move.w  sr,d2
        move.w  d0,116(a1)
        move.w  d2,118(a1)

        move.l  #$80000000,d0
        asr.l   #4,d0
        move.l  d0,120(a1)

        move.l  #$00008001,d0
        lsr.w   #1,d0
        move.w  d0,124(a1)

        move.l  #$00000081,d0
        rol.b   #1,d0
        move.b  d0,126(a1)
        move.l  #$00000081,d0
        ror.b   #1,d0
        move.b  d0,127(a1)

        move.w  #0,ccr
        move.l  #$00008000,d0
        roxl.w  #1,d0                   ; MSB into X
        move.w  d0,128(a1)
        roxl.w  #1,d0                   ; X back in at the bottom
        move.w  d0,130(a1)

        moveq   #20,d1                  ; a count larger than the operand
        move.l  #$00000001,d0
        lsl.w   d1,d0
        move.w  sr,d2
        move.w  d0,132(a1)
        move.w  d2,134(a1)

        move.w  #$1234,(a0)
        asl     (a0)                    ; memory form: one bit, one word
        move.w  (a0),136(a1)

* ------------------------------------------------------------ bit operations
        move.l  #$00000000,d0
        bset    #3,d0
        bset    #31,d0
        move.l  d0,140(a1)
        bclr    #3,d0
        move.l  d0,144(a1)
        bchg    #31,d0
        move.l  d0,148(a1)
        moveq   #5,d1
        bset    d1,d0                   ; dynamic bit number
        move.l  d0,152(a1)

        move.b  #$00,(a0)
        bset    #7,(a0)                 ; byte-sized on memory
        move.b  (a0),156(a1)
        btst    #7,(a0)
        move.w  sr,d2
        move.w  d2,158(a1)

* ----------------------------------------------------------- misc data moves
        move.l  #$ffff0080,d0
        ext.w   d0
        move.l  d0,160(a1)
        move.l  #$0000ff00,d0
        ext.l   d0
        move.l  d0,164(a1)

        move.l  #$12345678,d0
        swap    d0
        move.l  d0,168(a1)

        move.l  #$11111111,d0
        move.l  #$22222222,d1
        exg     d0,d1
        move.l  d0,172(a1)
        move.l  d1,176(a1)

        move.l  #$aabbccdd,d0
        movep.l d0,0(a0)                ; alternate bytes
        move.l  (a0),180(a1)
        move.l  4(a0),184(a1)
        movep.l 0(a0),d1
        move.l  d1,188(a1)

* ----------------------------------------------------------------- Scc, DBcc
        moveq   #0,d0
        cmpi.w  #0,d0
        seq     d1                      ; both Scc first: MOVE would reset Z
        sne     d2
        move.b  d1,192(a1)
        move.b  d2,193(a1)

        moveq   #3,d0
        moveq   #0,d1
dbloop:
        addq.w  #1,d1
        dbra    d0,dbloop
        move.w  d1,194(a1)              ; 4 iterations
        move.w  d0,196(a1)              ; counter ends at -1

* --------------------------------------------------------------------- MOVEM
        movem.l d0-d3/a0-a1,-(a7)
        moveq   #0,d0
        moveq   #0,d1
        moveq   #0,d2
        moveq   #0,d3
        movem.l (a7)+,d0-d3/a0-a1
        move.l  d0,200(a1)
        move.l  d3,204(a1)

        movem.w d0-d1,208(a1)           ; word form, to a fixed address

* ----------------------------------------------------------- CMPM and CMPA
        lea     SRC,a2
        lea     SRC,a3
        cmpm.l  (a2)+,(a3)+
        move.w  sr,d2
        move.w  d2,212(a1)

        lea     SRC,a2
        cmpa.l  a2,a0
        move.w  sr,d2
        move.w  d2,214(a1)

* ------------------------------------------------------- subroutine and frame
        moveq   #7,d0
        bsr     triple
        move.w  d0,216(a1)              ; 21

        link    a6,#-16
        move.l  a6,218(a1)
        move.l  #$cafebabe,-4(a6)
        move.l  -4(a6),222(a1)
        unlk    a6

        pea     SRC
        move.l  (a7)+,226(a1)

        lea     jump_table,a2
        jsr     (a2)                    ; indirect call
        move.w  d0,230(a1)

* --------------------------------------------------------------------- done
        move.w  #1,HALT
        stop    #$2700

* ------------------------------------------------------------- subroutines
triple:
        move.w  d0,d1
        add.w   d0,d0
        add.w   d1,d0
        rts

jump_table:
        moveq   #99,d0
        rts

pcdata:
        dc.w    $c0de
        dc.w    0
