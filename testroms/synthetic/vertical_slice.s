* vertical_slice.s
*
* The demonstration program from AMIGA_RECOMP.md 69: load values, modify RAM,
* call a subroutine, branch on flags, return, and halt through the harness
* sentinel. Its final guest state must be identical under the reference
* interpreter and under the generated native code.
*
* Assembled to testroms/synthetic/vertical_slice.bin at guest address $1000.
*   python tools/asm68k.py testroms/synthetic/vertical_slice.s \
*       -o testroms/synthetic/vertical_slice.bin --org 0x1000

        org     $1000

HALT    equ     $00f00000       ; a write here stops the harness
WORKSP  equ     $00002000       ; scratch RAM the program modifies

start:
        lea     WORKSP,a0       ; a0 = scratch area
        moveq   #0,d0           ; running total
        moveq   #10,d1          ; loop counter
        move.l  #$12345678,d2   ; a long to stash in RAM

        move.l  d2,(a0)         ; modify RAM
        move.w  #$beef,4(a0)    ; and again, at a displacement

* Sum 10..1 into d0, exercising a backward conditional branch.
sum_loop:
        add.w   d1,d0
        subq.w  #1,d1
        bne     sum_loop        ; d0 = 55

* Call a subroutine: the guest stack must be real for this to work.
        move.w  d0,d3
        bsr     double_it       ; d3 = 110
        move.w  d3,8(a0)

* Branch on a flag set by a comparison.
        cmpi.w  #110,d3
        beq     matched
        moveq   #-1,d4          ; not reached if the recomp is correct
        bra     finish
matched:
        moveq   #1,d4

finish:
* Exercise postincrement and predecrement addressing on the way out.
        lea     WORKSP,a1
        move.l  (a1)+,d5        ; d5 = $12345678, a1 = WORKSP+4
        move.w  -(a1),d6        ; d6 = $5678,     a1 = WORKSP+2

* Halt the harness. The sentinel sets the halt flag, which the dispatcher
* only tests between blocks, so STOP follows to end the block immediately.
* Both the interpreter and the generated code then stop at the same
* instruction, which is what makes the differential comparison meaningful.
        move.w  #1,HALT
        stop    #$2700

* d3 = d3 * 2, via the stack, so a return address really is popped.
double_it:
        add.w   d3,d3
        rts
