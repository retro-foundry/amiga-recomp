#!/usr/bin/env python3
"""A small MC68000 assembler for this project's test programs.

Not a general-purpose assembler. It covers the instructions the synthetic test
programs and examples in testroms/ and examples/ need, and it fails loudly on
anything it does not understand rather than emitting something plausible.

    python tools/asm68k.py input.s -o output.bin [--org 0x1000] [--listing]

Supported syntax:
    label:              a label, column 0
    ; comment           or a whole-line * comment
    org $1000           set the assembly origin
    dc.b/dc.w/dc.l      literal data
    ds.b/ds.w/ds.l      reserved (zeroed) space
    even                align to a word boundary

Operands:
    d0-d7  a0-a7/sp     registers
    (a0) (a0)+ -(a0)    indirect forms
    d(a0)  d(a0,d1.w)   displacement and index
    $1234  $1234.w      absolute (long unless .w is given)
    #imm                immediate
    label               absolute or branch target
"""

import argparse
import re
import sys

DATA_REG = re.compile(r'^d([0-7])$', re.I)
ADDR_REG = re.compile(r'^(?:a([0-7])|sp)$', re.I)
INDIRECT = re.compile(r'^\((?:a([0-7])|sp)\)$', re.I)
POSTINC = re.compile(r'^\((?:a([0-7])|sp)\)\+$', re.I)
PREDEC = re.compile(r'^-\((?:a([0-7])|sp)\)$', re.I)
DISP = re.compile(r'^(.+?)\((?:a([0-7])|sp)\)$', re.I)
INDEX = re.compile(r'^(.*?)\((?:a([0-7])|sp),\s*([da])([0-7])(?:\.([wl]))?\)$', re.I)
IMMEDIATE = re.compile(r'^#(.+)$')
ABS_W = re.compile(r'^(.+)\.w$', re.I)
PC_DISP = re.compile(r'^(.+?)\(pc\)$', re.I)
PC_INDEX = re.compile(r'^(.*?)\(pc,\s*([da])([0-7])(?:\.([wl]))?\)$', re.I)


class AsmError(Exception):
    pass


class Operand:
    """A decoded operand, in the mode/register form the 68000 encodes."""

    def __init__(self, mode, reg, ext=None, kind='', value=0):
        self.mode = mode
        self.reg = reg
        self.ext = ext or []      # extension words, as ints
        self.kind = kind
        self.value = value

    @property
    def is_dreg(self):
        return self.kind == 'd'

    @property
    def is_areg(self):
        return self.kind == 'a'


SIZE_BITS = {'b': 0, 'w': 1, 'l': 2}
MOVE_SIZE_BITS = {'b': 1, 'w': 3, 'l': 2}

CONDITIONS = {
    't': 0, 'f': 1, 'hi': 2, 'ls': 3, 'cc': 4, 'hs': 4, 'cs': 5, 'lo': 5,
    'ne': 6, 'eq': 7, 'vc': 8, 'vs': 9, 'pl': 10, 'mi': 11, 'ge': 12,
    'lt': 13, 'gt': 14, 'le': 15,
}


class Assembler:
    def __init__(self, org=0x1000):
        self.org = org
        self.pc = org
        self.labels = {}
        self.output = bytearray()
        self.listing = []

    # -- helpers ---------------------------------------------------------
    def emit_word(self, value):
        self.output += bytes(((value >> 8) & 0xff, value & 0xff))
        self.pc += 2

    def emit_long(self, value):
        self.emit_word((value >> 16) & 0xffff)
        self.emit_word(value & 0xffff)

    def emit_byte(self, value):
        self.output.append(value & 0xff)
        self.pc += 1

    def parse_number(self, text, pass_two):
        text = text.strip()
        if not text:
            raise AsmError('empty expression')
        # Sums and differences of a label and a constant.
        m = re.match(r'^(.+?)\s*([+-])\s*(.+)$', text)
        if m and not text.startswith('$') and not text.startswith('-'):
            left = self.parse_number(m.group(1), pass_two)
            right = self.parse_number(m.group(3), pass_two)
            return left + right if m.group(2) == '+' else left - right
        if text.startswith('$'):
            return int(text[1:], 16)
        if text.startswith('%'):
            return int(text[1:], 2)
        if text.startswith('-'):
            return -self.parse_number(text[1:], pass_two)
        if re.match(r'^\d+$', text):
            return int(text, 10)
        if text in self.labels:
            return self.labels[text]
        if pass_two:
            raise AsmError(f'unknown label: {text}')
        return 0

    def parse_operand(self, text, size, pass_two):
        text = text.strip()

        m = DATA_REG.match(text)
        if m:
            return Operand(0, int(m.group(1)), kind='d')

        m = ADDR_REG.match(text)
        if m:
            reg = 7 if m.group(1) is None else int(m.group(1))
            return Operand(1, reg, kind='a')

        m = INDIRECT.match(text)
        if m:
            reg = 7 if m.group(1) is None else int(m.group(1))
            return Operand(2, reg)

        m = POSTINC.match(text)
        if m:
            reg = 7 if m.group(1) is None else int(m.group(1))
            return Operand(3, reg)

        m = PREDEC.match(text)
        if m:
            reg = 7 if m.group(1) is None else int(m.group(1))
            return Operand(4, reg)

        # PC-relative modes. The displacement is measured from the extension
        # word, which this assembler assumes immediately follows the opcode.
        # That holds for every form it can emit.
        m = PC_INDEX.match(text)
        if m:
            target = self.parse_number(m.group(1), pass_two) if m.group(1).strip() else 0
            idx_reg = int(m.group(3))
            idx_is_addr = m.group(2).lower() == 'a'
            idx_long = (m.group(4) or 'w').lower() == 'l'
            disp = target - (self.pc + 2) if m.group(1).strip() else 0
            if not -128 <= disp <= 127:
                raise AsmError('pc-relative index displacement does not fit in a byte')
            ext = ((idx_reg << 12) | (0x8000 if idx_is_addr else 0) |
                   (0x800 if idx_long else 0) | (disp & 0xff))
            return Operand(7, 3, [ext], kind='pcidx')

        m = PC_DISP.match(text)
        if m:
            target = self.parse_number(m.group(1), pass_two)
            disp = target - (self.pc + 2)
            if not -32768 <= disp <= 32767:
                raise AsmError('pc-relative displacement does not fit in a word')
            return Operand(7, 2, [disp & 0xffff], kind='pcdisp')

        m = INDEX.match(text)
        if m:
            disp = self.parse_number(m.group(1), pass_two) if m.group(1).strip() else 0
            reg = 7 if m.group(2) is None else int(m.group(2))
            idx_reg = int(m.group(4))
            idx_is_addr = m.group(3).lower() == 'a'
            idx_long = (m.group(5) or 'w').lower() == 'l'
            ext = ((idx_reg << 12) | (0x8000 if idx_is_addr else 0) |
                   (0x800 if idx_long else 0) | (disp & 0xff))
            return Operand(6, reg, [ext])

        m = DISP.match(text)
        if m and not m.group(1).strip().startswith('#'):
            disp = self.parse_number(m.group(1), pass_two)
            reg = 7 if m.group(2) is None else int(m.group(2))
            return Operand(5, reg, [disp & 0xffff])

        m = IMMEDIATE.match(text)
        if m:
            value = self.parse_number(m.group(1), pass_two)
            if size == 'l':
                ext = [(value >> 16) & 0xffff, value & 0xffff]
            else:
                ext = [value & 0xffff]
            return Operand(7, 4, ext, kind='imm', value=value)

        m = ABS_W.match(text)
        if m:
            value = self.parse_number(m.group(1), pass_two)
            return Operand(7, 0, [value & 0xffff], kind='abs', value=value)

        value = self.parse_number(text, pass_two)
        return Operand(7, 1, [(value >> 16) & 0xffff, value & 0xffff],
                       kind='abs', value=value)

    def emit_operand_ext(self, op):
        for word in op.ext:
            self.emit_word(word)

    # -- assembly --------------------------------------------------------
    def assemble(self, lines, pass_two):
        self.pc = self.org
        self.output = bytearray()
        for lineno, raw in enumerate(lines, 1):
            # Comments: ';' anywhere, or '*' in column 0. '#' cannot start a
            # comment -- it introduces an immediate operand.
            line = raw.strip()
            if line.startswith('*'):
                line = ''
            line = re.sub(r';.*$', '', line).strip()
            if not line:
                continue
            try:
                self.assemble_line(line, pass_two)
            except AsmError as exc:
                raise AsmError(f'line {lineno}: {exc}\n  {raw.strip()}') from None

    def assemble_line(self, line, pass_two):
        m = re.match(r'^([A-Za-z_]\w*)\s+equ\s+(.+)$', line, re.I)
        if m:
            self.labels[m.group(1)] = self.parse_number(m.group(2), pass_two)
            return

        # Labels.
        # A label must carry its colon. Without one, a bare "rts" on its own
        # line would silently become a label instead of an instruction.
        m = re.match(r'^([A-Za-z_][\w]*):\s*(.*)$', line)
        if m:
            label = m.group(1)
            if not pass_two:
                self.labels[label] = self.pc
            line = m.group(2).strip()
            if not line:
                return

        parts = line.split(None, 1)
        mnemonic = parts[0].lower()
        rest = parts[1] if len(parts) > 1 else ''
        operands = [o.strip() for o in split_operands(rest)] if rest else []

        if '.' in mnemonic:
            mnemonic, suffix = mnemonic.split('.', 1)
        else:
            suffix = ''

        handler = DIRECTIVES.get(mnemonic)
        if handler:
            handler(self, suffix, operands, pass_two)
            return

        self.encode(mnemonic, suffix, operands, pass_two)

    def encode(self, mnemonic, suffix, operands, pass_two):
        size = suffix or default_size(mnemonic)
        if size not in ('', 'b', 'w', 'l', 's'):
            raise AsmError(f'bad size suffix: .{suffix}')

        # -- branches ----------------------------------------------------
        if mnemonic in ('bra', 'bsr') or (mnemonic.startswith('b') and
                                          mnemonic[1:] in CONDITIONS and
                                          mnemonic not in ('btst', 'bset',
                                                           'bclr', 'bchg')):
            if mnemonic == 'bra':
                cond = 0
            elif mnemonic == 'bsr':
                cond = 1
            else:
                cond = CONDITIONS[mnemonic[1:]]
            target = self.parse_number(operands[0], pass_two)
            disp = target - (self.pc + 2)
            # The word form is the default: a short form chosen from a
            # not-yet-known forward label would change instruction lengths
            # between the two passes and silently corrupt every later label.
            if suffix in ('s', 'b') and -128 <= disp <= 127 and disp != 0:
                self.emit_word(0x6000 | (cond << 8) | (disp & 0xff))
            else:
                self.emit_word(0x6000 | (cond << 8))
                self.emit_word((target - self.pc) & 0xffff)
            return

        if mnemonic.startswith('db') and mnemonic[2:] in CONDITIONS:
            cond = CONDITIONS[mnemonic[2:]]
            reg = self.parse_operand(operands[0], 'w', pass_two)
            target = self.parse_number(operands[1], pass_two)
            self.emit_word(0x50c8 | (cond << 8) | reg.reg)
            self.emit_word((target - self.pc) & 0xffff)
            return
        if mnemonic == 'dbra':
            reg = self.parse_operand(operands[0], 'w', pass_two)
            target = self.parse_number(operands[1], pass_two)
            self.emit_word(0x51c8 | reg.reg)
            self.emit_word((target - self.pc) & 0xffff)
            return

        if mnemonic.startswith('s') and mnemonic[1:] in CONDITIONS and \
                mnemonic not in ('sub', 'suba', 'subi', 'subq', 'subx', 'swap', 'stop'):
            cond = CONDITIONS[mnemonic[1:]]
            dst = self.parse_operand(operands[0], 'b', pass_two)
            self.emit_word(0x50c0 | (cond << 8) | (dst.mode << 3) | dst.reg)
            self.emit_operand_ext(dst)
            return

        # -- no-operand forms --------------------------------------------
        fixed = {
            'nop': 0x4e71, 'rts': 0x4e75, 'rte': 0x4e73, 'rtr': 0x4e77,
            'trapv': 0x4e76, 'reset': 0x4e70, 'illegal': 0x4afc,
        }
        if mnemonic in fixed:
            self.emit_word(fixed[mnemonic])
            return

        if mnemonic == 'stop':
            value = self.parse_number(operands[0].lstrip('#'), pass_two)
            self.emit_word(0x4e72)
            self.emit_word(value & 0xffff)
            return

        if mnemonic == 'trap':
            value = self.parse_number(operands[0].lstrip('#'), pass_two)
            self.emit_word(0x4e40 | (value & 0xf))
            return

        # -- moves -------------------------------------------------------
        if mnemonic == 'moveq':
            src = self.parse_operand(operands[0], 'b', pass_two)
            dst = self.parse_operand(operands[1], 'l', pass_two)
            self.emit_word(0x7000 | (dst.reg << 9) | (src.value & 0xff))
            return

        if mnemonic in ('move', 'movea'):
            # The status register forms look like ordinary moves but are
            # separate instructions with their own encodings.
            left = operands[0].strip().lower()
            right = operands[1].strip().lower()
            if left == 'sr':
                dst = self.parse_operand(operands[1], 'w', pass_two)
                self.emit_word(0x40c0 | (dst.mode << 3) | dst.reg)
                self.emit_operand_ext(dst)
                return
            if right == 'ccr':
                src = self.parse_operand(operands[0], 'w', pass_two)
                self.emit_word(0x44c0 | (src.mode << 3) | src.reg)
                self.emit_operand_ext(src)
                return
            if right == 'sr':
                src = self.parse_operand(operands[0], 'w', pass_two)
                self.emit_word(0x46c0 | (src.mode << 3) | src.reg)
                self.emit_operand_ext(src)
                return
            if left == 'usp':
                dst = self.parse_operand(operands[1], 'l', pass_two)
                self.emit_word(0x4e68 | dst.reg)
                return
            if right == 'usp':
                src = self.parse_operand(operands[0], 'l', pass_two)
                self.emit_word(0x4e60 | src.reg)
                return

            src = self.parse_operand(operands[0], size, pass_two)
            dst = self.parse_operand(operands[1], size, pass_two)
            self.emit_word((MOVE_SIZE_BITS[size] << 12) | (dst.reg << 9) |
                           (dst.mode << 6) | (src.mode << 3) | src.reg)
            self.emit_operand_ext(src)
            self.emit_operand_ext(dst)
            return

        if mnemonic == 'lea':
            src = self.parse_operand(operands[0], 'l', pass_two)
            dst = self.parse_operand(operands[1], 'l', pass_two)
            self.emit_word(0x41c0 | (dst.reg << 9) | (src.mode << 3) | src.reg)
            self.emit_operand_ext(src)
            return

        if mnemonic == 'pea':
            src = self.parse_operand(operands[0], 'l', pass_two)
            self.emit_word(0x4840 | (src.mode << 3) | src.reg)
            self.emit_operand_ext(src)
            return

        if mnemonic in ('jmp', 'jsr'):
            src = self.parse_operand(operands[0], 'l', pass_two)
            base = 0x4ec0 if mnemonic == 'jmp' else 0x4e80
            self.emit_word(base | (src.mode << 3) | src.reg)
            self.emit_operand_ext(src)
            return

        if mnemonic == 'link':
            reg = self.parse_operand(operands[0], 'l', pass_two)
            disp = self.parse_number(operands[1].lstrip('#'), pass_two)
            self.emit_word(0x4e50 | reg.reg)
            self.emit_word(disp & 0xffff)
            return

        if mnemonic == 'unlk':
            reg = self.parse_operand(operands[0], 'l', pass_two)
            self.emit_word(0x4e58 | reg.reg)
            return

        if mnemonic == 'swap':
            reg = self.parse_operand(operands[0], 'l', pass_two)
            self.emit_word(0x4840 | reg.reg)
            return

        if mnemonic == 'ext':
            reg = self.parse_operand(operands[0], size, pass_two)
            self.emit_word((0x48c0 if size == 'l' else 0x4880) | reg.reg)
            return

        if mnemonic in ('movem',):
            self.encode_movem(size, operands, pass_two)
            return

        # -- single-operand ALU ------------------------------------------
        single = {'clr': 0x4200, 'neg': 0x4400, 'negx': 0x4000, 'not': 0x4600,
                  'tst': 0x4a00}
        if mnemonic in single:
            dst = self.parse_operand(operands[0], size, pass_two)
            self.emit_word(single[mnemonic] | (SIZE_BITS[size] << 6) |
                           (dst.mode << 3) | dst.reg)
            self.emit_operand_ext(dst)
            return

        if mnemonic == 'tas':
            dst = self.parse_operand(operands[0], 'b', pass_two)
            self.emit_word(0x4ac0 | (dst.mode << 3) | dst.reg)
            self.emit_operand_ext(dst)
            return

        # -- quick forms --------------------------------------------------
        if mnemonic in ('addq', 'subq'):
            src = self.parse_operand(operands[0], 'b', pass_two)
            dst = self.parse_operand(operands[1], size, pass_two)
            count = src.value & 7
            base = 0x5000 if mnemonic == 'addq' else 0x5100
            self.emit_word(base | (count << 9) | (SIZE_BITS[size] << 6) |
                           (dst.mode << 3) | dst.reg)
            self.emit_operand_ext(dst)
            return

        # -- shifts -------------------------------------------------------
        shifts = {'as': 0, 'ls': 1, 'rox': 2, 'ro': 3}
        for prefix, kind in (('asl', ('as', 1)), ('asr', ('as', 0)),
                             ('lsl', ('ls', 1)), ('lsr', ('ls', 0)),
                             ('roxl', ('rox', 1)), ('roxr', ('rox', 0)),
                             ('rol', ('ro', 1)), ('ror', ('ro', 0))):
            if mnemonic != prefix:
                continue
            family, left = kind
            type_bits = shifts[family]
            if len(operands) == 1:
                dst = self.parse_operand(operands[0], 'w', pass_two)
                self.emit_word(0xe0c0 | (type_bits << 9) | (left << 8) |
                               (dst.mode << 3) | dst.reg)
                self.emit_operand_ext(dst)
                return
            src = self.parse_operand(operands[0], 'b', pass_two)
            dst = self.parse_operand(operands[1], size, pass_two)
            if src.kind == 'imm':
                count = src.value & 7
                ir = 0
            else:
                count = src.reg
                ir = 1
            self.emit_word(0xe000 | (count << 9) | (left << 8) |
                           (SIZE_BITS[size] << 6) | (ir << 5) |
                           (type_bits << 3) | dst.reg)
            return

        # -- bit operations ------------------------------------------------
        bitops = {'btst': 0, 'bchg': 1, 'bclr': 2, 'bset': 3}
        if mnemonic in bitops:
            src = self.parse_operand(operands[0], 'b', pass_two)
            dst = self.parse_operand(operands[1], 'b', pass_two)
            op = bitops[mnemonic]
            if src.kind == 'imm':
                self.emit_word(0x0800 | (op << 6) | (dst.mode << 3) | dst.reg)
                self.emit_word(src.value & 0xff)
            else:
                self.emit_word(0x0100 | (src.reg << 9) | (op << 6) |
                               (dst.mode << 3) | dst.reg)
            self.emit_operand_ext(dst)
            return

        # -- register-or-memory pairs (addx/subx/abcd/sbcd/cmpm) -------------
        pair = {'addx': 0xd100, 'subx': 0x9100, 'abcd': 0xc100, 'sbcd': 0x8100}
        if mnemonic in pair:
            src = self.parse_operand(operands[0], size, pass_two)
            dst = self.parse_operand(operands[1], size, pass_two)
            if src.mode != dst.mode:
                raise AsmError(f'{mnemonic} operands must both be registers '
                               f'or both be -(an)')
            rm = 1 if src.mode == 4 else 0
            size_bits = 0 if mnemonic in ('abcd', 'sbcd') else SIZE_BITS[size] << 6
            self.emit_word(pair[mnemonic] | (dst.reg << 9) | size_bits |
                           (rm << 3) | src.reg)
            return

        if mnemonic == 'cmpm':
            src = self.parse_operand(operands[0], size, pass_two)
            dst = self.parse_operand(operands[1], size, pass_two)
            if src.mode != 3 or dst.mode != 3:
                raise AsmError('cmpm operands must both be (an)+')
            self.emit_word(0xb108 | (dst.reg << 9) | (SIZE_BITS[size] << 6) | src.reg)
            return

        if mnemonic == 'nbcd':
            dst = self.parse_operand(operands[0], 'b', pass_two)
            self.emit_word(0x4800 | (dst.mode << 3) | dst.reg)
            self.emit_operand_ext(dst)
            return

        if mnemonic == 'exg':
            src = self.parse_operand(operands[0], 'l', pass_two)
            dst = self.parse_operand(operands[1], 'l', pass_two)
            if src.is_dreg and dst.is_dreg:
                opmode = 0x08
            elif src.is_areg and dst.is_areg:
                opmode = 0x09
            elif src.is_dreg and dst.is_areg:
                opmode = 0x11
            else:
                # exg an,dn is encoded as exg dn,an with the registers swapped.
                self.emit_word(0xc188 | (dst.reg << 9) | src.reg)
                return
            self.emit_word(0xc100 | (src.reg << 9) | (opmode << 3) | dst.reg)
            return

        if mnemonic == 'movep':
            src = self.parse_operand(operands[0], size, pass_two)
            dst = self.parse_operand(operands[1], size, pass_two)
            long_bit = 0x40 if size == 'l' else 0
            if src.is_dreg:      # register to memory
                if dst.mode != 5:
                    raise AsmError('movep memory operand must be d16(an)')
                self.emit_word(0x0188 | (src.reg << 9) | long_bit | dst.reg)
                self.emit_word(dst.ext[0])
            else:
                if src.mode != 5:
                    raise AsmError('movep memory operand must be d16(an)')
                self.emit_word(0x0108 | (dst.reg << 9) | long_bit | src.reg)
                self.emit_word(src.ext[0])
            return

        if mnemonic == 'chk':
            src = self.parse_operand(operands[0], 'w', pass_two)
            dst = self.parse_operand(operands[1], 'w', pass_two)
            self.emit_word(0x4180 | (dst.reg << 9) | (src.mode << 3) | src.reg)
            self.emit_operand_ext(src)
            return

        # -- two-operand ALU ------------------------------------------------
        alu = {'or': 0x8000, 'sub': 0x9000, 'cmp': 0xb000, 'eor': 0xb000,
               'and': 0xc000, 'add': 0xd000}
        alu_imm = {'ori': (0x0000, 'or'), 'andi': (0x0200, 'and'),
                   'subi': (0x0400, 'sub'), 'addi': (0x0600, 'add'),
                   'eori': (0x0a00, 'eor'), 'cmpi': (0x0c00, 'cmp')}

        if mnemonic in alu_imm:
            base, _ = alu_imm[mnemonic]
            src = self.parse_operand(operands[0], size, pass_two)
            dst = self.parse_operand(operands[1], size, pass_two)
            self.emit_word(base | (SIZE_BITS[size] << 6) | (dst.mode << 3) | dst.reg)
            self.emit_operand_ext(src)
            self.emit_operand_ext(dst)
            return

        if mnemonic in ('adda', 'suba', 'cmpa'):
            src = self.parse_operand(operands[0], size, pass_two)
            dst = self.parse_operand(operands[1], size, pass_two)
            base = {'adda': 0xd000, 'suba': 0x9000, 'cmpa': 0xb000}[mnemonic]
            opmode = 3 if size == 'w' else 7
            self.emit_word(base | (dst.reg << 9) | (opmode << 6) |
                           (src.mode << 3) | src.reg)
            self.emit_operand_ext(src)
            return

        if mnemonic in ('mulu', 'muls', 'divu', 'divs'):
            src = self.parse_operand(operands[0], 'w', pass_two)
            dst = self.parse_operand(operands[1], 'w', pass_two)
            base = 0xc000 if mnemonic.startswith('mul') else 0x8000
            opmode = 3 if mnemonic in ('mulu', 'divu') else 7
            self.emit_word(base | (dst.reg << 9) | (opmode << 6) |
                           (src.mode << 3) | src.reg)
            self.emit_operand_ext(src)
            return

        if mnemonic in alu:
            src = self.parse_operand(operands[0], size, pass_two)
            dst = self.parse_operand(operands[1], size, pass_two)
            base = alu[mnemonic]

            # An immediate source belongs in the ORI/ANDI/SUBI/ADDI/EORI/CMPI
            # encoding. EOR in particular has no <ea>,Dn direction, so the
            # immediate would otherwise be dropped and a register read instead.
            immediate_form = {'or': 'ori', 'and': 'andi', 'sub': 'subi',
                              'add': 'addi', 'eor': 'eori', 'cmp': 'cmpi'}
            if src.kind == 'imm' and not dst.is_areg:
                self.encode(immediate_form[mnemonic], suffix, operands, pass_two)
                return

            # An address-register destination means the ADDA/SUBA/CMPA form.
            if dst.is_areg and mnemonic in ('add', 'sub', 'cmp'):
                opmode = 3 if size == 'w' else 7
                self.emit_word(base | (dst.reg << 9) | (opmode << 6) |
                               (src.mode << 3) | src.reg)
                self.emit_operand_ext(src)
                return

            if mnemonic == 'eor' or (src.is_dreg and not dst.is_dreg):
                # Dn, <ea>
                opmode = 4 + SIZE_BITS[size]
                self.emit_word(base | (src.reg << 9) | (opmode << 6) |
                               (dst.mode << 3) | dst.reg)
                self.emit_operand_ext(dst)
                return

            # <ea>, Dn
            opmode = SIZE_BITS[size]
            self.emit_word(base | (dst.reg << 9) | (opmode << 6) |
                           (src.mode << 3) | src.reg)
            self.emit_operand_ext(src)
            return

        raise AsmError(f'unsupported mnemonic: {mnemonic}')

    def encode_movem(self, size, operands, pass_two):
        def parse_reglist(text):
            mask = 0
            for part in text.split('/'):
                part = part.strip()
                m = re.match(r'^([da])([0-7])-([da])([0-7])$', part, re.I)
                if m:
                    start = int(m.group(2)) + (8 if m.group(1).lower() == 'a' else 0)
                    end = int(m.group(4)) + (8 if m.group(3).lower() == 'a' else 0)
                    for bit in range(start, end + 1):
                        mask |= 1 << bit
                    continue
                m = re.match(r'^([da])([0-7])$', part, re.I)
                if not m:
                    raise AsmError(f'bad register list: {text}')
                mask |= 1 << (int(m.group(2)) + (8 if m.group(1).lower() == 'a' else 0))
            return mask

        size_bit = 0x40 if size == 'l' else 0
        if re.match(r'^[da][0-7]', operands[0], re.I) and '/' in operands[0] or \
                re.match(r'^[da][0-7]-', operands[0], re.I):
            mask = parse_reglist(operands[0])
            dst = self.parse_operand(operands[1], size, pass_two)
            if dst.mode == 4:   # predecrement reverses the mask
                mask = int(f'{mask:016b}'[::-1], 2)
            self.emit_word(0x4880 | size_bit | (dst.mode << 3) | dst.reg)
            self.emit_word(mask)
            self.emit_operand_ext(dst)
        else:
            src = self.parse_operand(operands[0], size, pass_two)
            mask = parse_reglist(operands[1])
            self.emit_word(0x4c80 | size_bit | (src.mode << 3) | src.reg)
            self.emit_word(mask)
            self.emit_operand_ext(src)


def split_operands(text):
    """Split on commas that are not inside parentheses."""
    parts, depth, current = [], 0, ''
    for ch in text:
        if ch == '(':
            depth += 1
        elif ch == ')':
            depth -= 1
        if ch == ',' and depth == 0:
            parts.append(current)
            current = ''
        else:
            current += ch
    if current.strip():
        parts.append(current)
    return parts


def default_size(mnemonic):
    if mnemonic in ('moveq', 'lea', 'pea', 'jmp', 'jsr', 'swap', 'link', 'unlk',
                    'nop', 'rts', 'rte', 'rtr', 'trap', 'trapv', 'reset',
                    'stop', 'illegal', 'tas'):
        return 'w'
    return 'w'


# -- directives --------------------------------------------------------------

def dir_org(asm, suffix, operands, pass_two):
    asm.org = asm.parse_number(operands[0], pass_two)
    asm.pc = asm.org


def dir_dc(asm, suffix, operands, pass_two):
    size = suffix or 'w'
    for operand in operands:
        operand = operand.strip()
        if operand.startswith('"') or operand.startswith("'"):
            for ch in operand[1:-1]:
                asm.emit_byte(ord(ch))
            continue
        value = asm.parse_number(operand, pass_two)
        if size == 'b':
            asm.emit_byte(value)
        elif size == 'l':
            asm.emit_long(value)
        else:
            asm.emit_word(value)


def dir_ds(asm, suffix, operands, pass_two):
    size = suffix or 'w'
    count = asm.parse_number(operands[0], pass_two)
    width = {'b': 1, 'w': 2, 'l': 4}[size]
    for _ in range(count * width):
        asm.emit_byte(0)


def dir_even(asm, suffix, operands, pass_two):
    if asm.pc & 1:
        asm.emit_byte(0)


DIRECTIVES = {
    'org': dir_org,
    'dc': dir_dc,
    'ds': dir_ds,
    'even': dir_even,
}

MNEMONIC_NAMES = set()


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('source')
    parser.add_argument('-o', '--output', required=True)
    parser.add_argument('--org', default=None,
                        help='origin address, overriding any org directive')
    parser.add_argument('--listing', action='store_true',
                        help='print the label table to stderr')
    args = parser.parse_args()

    with open(args.source, 'r', encoding='utf-8') as handle:
        lines = handle.readlines()

    org = int(args.org, 0) if args.org else 0x1000
    asm = Assembler(org)
    try:
        asm.assemble(lines, pass_two=False)   # collect labels
        start_org = asm.org
        asm.org = start_org
        asm.assemble(lines, pass_two=True)    # emit
    except AsmError as exc:
        print(f'{args.source}: {exc}', file=sys.stderr)
        return 1

    with open(args.output, 'wb') as handle:
        handle.write(asm.output)

    if args.listing:
        for name, value in sorted(asm.labels.items(), key=lambda kv: kv[1]):
            print(f'  {value:08x}  {name}', file=sys.stderr)
    print(f'{args.output}: {len(asm.output)} bytes at ${asm.org:08x}',
          file=sys.stderr)
    return 0


if __name__ == '__main__':
    sys.exit(main())
