#!/usr/bin/env python3
"""Builds tests/data/test48.rom, the synthetic ROM the tests boot the machine on.

The repository never ships a Spectrum ROM (see roms/README.md), so every test
that needed the machine to actually boot had to skip itself. This ROM is
written from scratch for that job. It provides the handful of services the
conformance suite in tests/data/z80test relies on:

  * RST 0x10   - print the character in A, with a scrolling 32x24 text screen
  * 0x1601     - CHAN-OPEN, a no-op because the screen is the only channel
  * 0x0038     - an IM 1 handler that counts frames in 0x5B04
  * 0x3D00     - an original 8x8 font, which is where the test's screen reader
                 looks for glyph shapes

On top of that it prints a banner and keeps scanning keyboard row 6, so tests
can check that interrupts, the ULA text plane and the keyboard matrix work.

The generated image is committed, so building and running the tests needs no
Python. Re-run this script after editing it:

    python3 tools/make_test_rom.py
"""

import os
import sys

ROM_SIZE = 0x4000

# Public variables the tests read. Keep in sync with tests/test_rom_selftest.cpp.
CURSOR_ROW = 0x5B00
CURSOR_COL = 0x5B01
KEY_ROW6 = 0x5B02
FRAME_COUNT = 0x5B04

R8 = {"b": 0, "c": 1, "d": 2, "e": 3, "h": 4, "l": 5, "(hl)": 6, "a": 7}
RP = {"bc": 0, "de": 1, "hl": 2, "sp": 3}
RP2 = {"bc": 0, "de": 1, "hl": 2, "af": 3}
CC = {"nz": 0, "z": 1, "nc": 2, "c": 3, "po": 4, "pe": 5, "p": 6, "m": 7}

# A 5x7 font drawn from scratch, padded to the 8x8 cell the ULA uses. The test
# screen reader matches whole cells against these shapes, so the generator
# asserts that every glyph is unique.
FONT = {
    " ": (".....", ".....", ".....", ".....", ".....", ".....", "....."),
    "!": ("..#..", "..#..", "..#..", "..#..", "..#..", ".....", "..#.."),
    '"': (".#.#.", ".#.#.", ".....", ".....", ".....", ".....", "....."),
    "#": (".#.#.", ".#.#.", "#####", ".#.#.", "#####", ".#.#.", ".#.#."),
    "$": ("..#..", ".####", "#.#..", ".###.", "..#.#", "####.", "..#.."),
    "%": ("##...", "##..#", "...#.", "..#..", ".#...", "#..##", "...##"),
    "&": (".##..", "#..#.", "#.#..", ".#...", "#.#.#", "#..#.", ".##.#"),
    "'": ("..#..", "..#..", ".....", ".....", ".....", ".....", "....."),
    "(": ("...#.", "..#..", ".#...", ".#...", ".#...", "..#..", "...#."),
    ")": (".#...", "..#..", "...#.", "...#.", "...#.", "..#..", ".#..."),
    "*": (".....", "#.#.#", ".###.", "#####", ".###.", "#.#.#", "....."),
    "+": (".....", "..#..", "..#..", "#####", "..#..", "..#..", "....."),
    ",": (".....", ".....", ".....", ".....", "..##.", "..#..", ".#..."),
    "-": (".....", ".....", ".....", "#####", ".....", ".....", "....."),
    ".": (".....", ".....", ".....", ".....", ".....", ".##..", ".##.."),
    "/": ("....#", "...#.", "..#..", "..#..", ".#...", "#....", "....."),
    "0": (".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###."),
    "1": ("..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###."),
    "2": (".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####"),
    "3": ("#####", "...#.", "..#..", "...#.", "....#", "#...#", ".###."),
    "4": ("...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#."),
    "5": ("#####", "#....", "####.", "....#", "....#", "#...#", ".###."),
    "6": ("..##.", ".#...", "#....", "####.", "#...#", "#...#", ".###."),
    "7": ("#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#..."),
    "8": (".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###."),
    "9": (".###.", "#...#", "#...#", ".####", "....#", "...#.", ".##.."),
    ":": (".....", ".##..", ".##..", ".....", ".##..", ".##..", "....."),
    ";": (".....", ".##..", ".##..", ".....", ".##..", "..#..", ".#..."),
    "<": ("...#.", "..#..", ".#...", "#....", ".#...", "..#..", "...#."),
    "=": (".....", ".....", "#####", ".....", "#####", ".....", "....."),
    ">": (".#...", "..#..", "...#.", "....#", "...#.", "..#..", ".#..."),
    "?": (".###.", "#...#", "....#", "...#.", "..#..", ".....", "..#.."),
    "@": (".###.", "#...#", "#.###", "#.#.#", "#.###", "#....", ".###."),
    "A": (".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"),
    "B": ("####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."),
    "C": (".###.", "#...#", "#....", "#....", "#....", "#...#", ".###."),
    "D": ("###..", "#..#.", "#...#", "#...#", "#...#", "#..#.", "###.."),
    "E": ("#####", "#....", "#....", "####.", "#....", "#....", "#####"),
    "F": ("#####", "#....", "#....", "####.", "#....", "#....", "#...."),
    "G": (".###.", "#...#", "#....", "#.###", "#...#", "#...#", ".####"),
    "H": ("#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"),
    "I": (".###.", "..#..", "..#..", "..#..", "..#..", "..#..", ".###."),
    "J": ("..###", "...#.", "...#.", "...#.", "...#.", "#..#.", ".##.."),
    "K": ("#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#"),
    "L": ("#....", "#....", "#....", "#....", "#....", "#....", "#####"),
    "M": ("#...#", "##.##", "#.#.#", "#.#.#", "#...#", "#...#", "#...#"),
    "N": ("#...#", "#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#"),
    "O": (".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."),
    "P": ("####.", "#...#", "#...#", "####.", "#....", "#....", "#...."),
    "Q": (".###.", "#...#", "#...#", "#...#", "#.#.#", "#..#.", ".##.#"),
    "R": ("####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#"),
    "S": (".####", "#....", "#....", ".###.", "....#", "....#", "####."),
    "T": ("#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."),
    "U": ("#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."),
    "V": ("#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#.."),
    "W": ("#...#", "#...#", "#...#", "#.#.#", "#.#.#", "##.##", "#...#"),
    "X": ("#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#"),
    "Y": ("#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#.."),
    "Z": ("#####", "....#", "...#.", "..#..", ".#...", "#....", "#####"),
    "[": (".###.", ".#...", ".#...", ".#...", ".#...", ".#...", ".###."),
    "\\": ("#....", ".#...", "..#..", "..#..", "...#.", "....#", "....."),
    "]": (".###.", "...#.", "...#.", "...#.", "...#.", "...#.", ".###."),
    "^": ("..#..", ".#.#.", "#...#", ".....", ".....", ".....", "....."),
    "_": (".....", ".....", ".....", ".....", ".....", ".....", "#####"),
    "`": (".#...", "..#..", ".....", ".....", ".....", ".....", "....."),
    "a": (".....", ".....", ".###.", "....#", ".####", "#...#", ".####"),
    "b": ("#....", "#....", "####.", "#...#", "#...#", "#...#", "####."),
    "c": (".....", ".....", ".###.", "#....", "#....", "#...#", ".###."),
    "d": ("....#", "....#", ".####", "#...#", "#...#", "#...#", ".####"),
    "e": (".....", ".....", ".###.", "#...#", "#####", "#....", ".###."),
    "f": ("..##.", ".#..#", ".#...", "###..", ".#...", ".#...", ".#..."),
    "g": (".....", ".####", "#...#", "#...#", ".####", "....#", ".###."),
    "h": ("#....", "#....", "####.", "#...#", "#...#", "#...#", "#...#"),
    "i": ("..#..", ".....", ".##..", "..#..", "..#..", "..#..", ".###."),
    "j": ("...#.", ".....", "..##.", "...#.", "...#.", "#..#.", ".##.."),
    "k": ("#....", "#....", "#..#.", "#.#..", "##...", "#.#..", "#..#."),
    "l": (".##..", "..#..", "..#..", "..#..", "..#..", "..#..", ".###."),
    "m": (".....", ".....", "##.#.", "#.#.#", "#.#.#", "#...#", "#...#"),
    "n": (".....", ".....", "####.", "#...#", "#...#", "#...#", "#...#"),
    "o": (".....", ".....", ".###.", "#...#", "#...#", "#...#", ".###."),
    "p": (".....", "####.", "#...#", "#...#", "####.", "#....", "#...."),
    "q": (".....", ".####", "#...#", "#...#", ".####", "....#", "....#"),
    "r": (".....", ".....", "#.##.", "##..#", "#....", "#....", "#...."),
    "s": (".....", ".....", ".####", "#....", ".###.", "....#", "####."),
    "t": (".#...", ".#...", "###..", ".#...", ".#...", ".#..#", "..##."),
    "u": (".....", ".....", "#...#", "#...#", "#...#", "#...#", ".####"),
    "v": (".....", ".....", "#...#", "#...#", "#...#", ".#.#.", "..#.."),
    "w": (".....", ".....", "#...#", "#...#", "#.#.#", "#.#.#", ".#.#."),
    "x": (".....", ".....", "#...#", ".#.#.", "..#..", ".#.#.", "#...#"),
    "y": (".....", "#...#", "#...#", "#...#", ".####", "....#", ".###."),
    "z": (".....", ".....", "#####", "...#.", "..#..", ".#...", "#####"),
    "{": ("...##", "..#..", "..#..", ".##..", "..#..", "..#..", "...##"),
    "|": ("..#..", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."),
    "}": ("##...", "..#..", "..#..", "..##.", "..#..", "..#..", "##..."),
    "~": (".....", ".....", ".#..#", "#.#.#", "#..#.", ".....", "....."),
    "\x7f": ("#####", "#...#", "#...#", "#...#", "#...#", "#...#", "#####"),
}

# The routines the conformance suite needs, plus a boot banner and a keyboard
# scan. Labels are resolved by the two-pass assembler below.
PROGRAM = """
        ORG 0x0000
        JP boot

        ORG 0x0008
        RET                     ; RST 8: the error restart is unused here

        ORG 0x0010
        JP print_char           ; RST 10: print the character in A

        ORG 0x0038
        PUSH AF                 ; IM 1: count frames, nothing else
        PUSH HL
        LD HL,(0x5B04)
        INC HL
        LD (0x5B04),HL
        POP HL
        POP AF
        EI
        RETI

        ORG 0x0066
        RETN                    ; NMI

        ORG 0x0100
boot:
        DI
        LD SP,0x7FF0
        IM 1
        LD HL,0x5B00            ; clear the public variables
        LD B,8
        XOR A
boot_clear:
        LD (HL),A
        INC HL
        DJNZ boot_clear
        CALL cls
        LD HL,msg_banner
        CALL print_str
        LD A,3
        LD (0x5B00),A
        XOR A
        LD (0x5B01),A
        EI
idle:
        LD A,0xBF               ; select keyboard row 6 (ENTER, L, K, J, H)
        IN A,(0xFE)
        AND 0x1F
        LD (0x5B02),A
        XOR A                   ; leave the border black again
        OUT (0xFE),A
        HALT
        JR idle

; ---------------------------------------------------------------- text output

; A = character. Keeps every register, so it is safe to call from anywhere.
print_char:
        PUSH AF
        PUSH BC
        PUSH DE
        PUSH HL
        CP 0x0D
        JR Z,pc_newline
        CP 0x0C
        JR Z,pc_cls
        CP 0x20
        JR C,pc_done            ; other control codes are ignored
        CALL draw_glyph
pc_advance:
        LD A,(0x5B01)
        INC A
        CP 32
        JR C,pc_set_col
        XOR A
        LD (0x5B01),A
        JR pc_newline2
pc_set_col:
        LD (0x5B01),A
        JR pc_done
pc_newline:
        XOR A
        LD (0x5B01),A
pc_newline2:
        LD A,(0x5B00)
        INC A
        CP 24
        JR C,pc_set_row
        CALL scroll
        LD A,23
pc_set_row:
        LD (0x5B00),A
pc_done:
        POP HL
        POP DE
        POP BC
        POP AF
        RET
pc_cls:
        CALL cls
        XOR A
        LD (0x5B00),A
        LD (0x5B01),A
        JR pc_done

; A = character >= 0x20, drawn at the cursor. Clobbers AF, BC, DE, HL.
draw_glyph:
        SUB 0x20
        LD L,A
        LD H,0
        ADD HL,HL
        ADD HL,HL
        ADD HL,HL               ; eight bytes per glyph
        LD DE,0x3D00
        ADD HL,DE
        PUSH HL
        LD A,(0x5B00)
        LD B,A
        AND 0x18
        ADD A,0x40
        LD D,A
        LD A,B
        AND 0x07
        RRCA
        RRCA
        RRCA                    ; (row & 7) << 5
        LD E,A
        LD A,(0x5B01)
        OR E
        LD E,A
        POP HL
        LD B,8
dg_loop:
        LD A,(HL)
        LD (DE),A
        INC HL
        INC D                   ; the next pixel line is 0x100 further down
        DJNZ dg_loop
        RET

; A = row -> HL = address of that text row's first pixel byte. Keeps A.
row_addr:
        PUSH AF
        AND 0x18
        ADD A,0x40
        LD H,A
        POP AF
        PUSH AF
        AND 0x07
        RRCA
        RRCA
        RRCA
        LD L,A
        POP AF
        RET

scroll:
        PUSH BC
        PUSH DE
        PUSH HL
        LD B,23
        LD C,0
sc_loop:
        PUSH BC
        LD A,C
        CALL row_addr           ; HL = row C, the destination
        EX DE,HL
        INC A
        CALL row_addr           ; HL = row C + 1, the source
        LD B,8
sc_lines:
        PUSH BC
        LD BC,32
        LDIR                    ; one pixel line, copied upwards so the source
        POP BC                  ; is still intact when it is read
        PUSH BC
        LD BC,0x00E0            ; LDIR left both pointers 32 bytes into the
        EX DE,HL                ; line; 32 + 0xE0 lands on the next one, which
        ADD HL,BC               ; sits exactly 0x100 lower in the pixel area
        EX DE,HL
        ADD HL,BC
        POP BC
        DJNZ sc_lines
        POP BC
        INC C
        DJNZ sc_loop

        LD A,23                 ; blank the row that just scrolled in
        CALL row_addr
        LD B,8
        XOR A
sc_blank:
        PUSH BC
        LD B,32
sc_blank_line:
        LD (HL),A
        INC HL
        DJNZ sc_blank_line
        POP BC
        PUSH BC
        LD BC,0x00E0
        ADD HL,BC
        POP BC
        DJNZ sc_blank

        LD HL,0x5820            ; shift the attributes up one row as well
        LD DE,0x5800
        LD BC,736
        LDIR
        LD HL,0x5AE0
        LD B,32
        LD A,0x38
sc_attr:
        LD (HL),A
        INC HL
        DJNZ sc_attr
        POP HL
        POP DE
        POP BC
        RET

cls:
        PUSH AF
        PUSH BC
        PUSH DE
        PUSH HL
        LD HL,0x4000
        LD DE,0x4001
        LD BC,0x17FF
        XOR A
        LD (HL),A
        LDIR
        LD HL,0x5800
        LD DE,0x5801
        LD BC,0x02FF
        LD A,0x38               ; black ink on white paper
        LD (HL),A
        LDIR
        XOR A
        LD (0x5B00),A
        LD (0x5B01),A
        POP HL
        POP DE
        POP BC
        POP AF
        RET

; HL = zero-terminated string.
print_str:
        LD A,(HL)
        OR A
        RET Z
        INC HL
        PUSH HL
        CALL print_char
        POP HL
        JR print_str

msg_banner:
        DB 'RainbowNext test ROM',0x0D
        DB 'no copyrighted code',0x0D
        DB 'RST 10 / 1601 / IM 1',0x0D
        DB 0

        ORG 0x1601
        RET                     ; CHAN-OPEN: the screen is the only channel

        ORG 0x3D00
        DB FONT
"""


def le16(value):
    value &= 0xFFFF
    return [value & 0xFF, value >> 8]


class Assembler:
    """Just enough Z80 to assemble PROGRAM: fixed-size encodings, two passes."""

    def __init__(self):
        self.image = bytearray(ROM_SIZE)
        self.owner = [None] * ROM_SIZE
        self.labels = {}
        self.pc = 0
        self.emit = False

    def value(self, token):
        token = token.strip()
        if len(token) >= 3 and token[0] == "'" and token[-1] == "'":
            return ord(token[1])
        if token.lower() in self.labels:
            return self.labels[token.lower()]
        try:
            return int(token, 0)
        except ValueError:
            if self.emit:
                raise SystemExit(f"unknown label {token!r}")
            return 0

    def relative(self, target):
        delta = self.value(target) - (self.pc + 2)
        if self.emit and not -128 <= delta <= 127:
            raise SystemExit(f"relative jump out of range at {self.pc:#06x}")
        return delta & 0xFF

    def encode(self, mnemonic, ops):
        m = mnemonic.lower()
        v = self.value

        simple = {
            "nop": [0x00], "halt": [0x76], "di": [0xF3], "ei": [0xFB],
            "exx": [0xD9], "daa": [0x27], "rlca": [0x07], "rrca": [0x0F],
            "rla": [0x17], "rra": [0x1F], "scf": [0x37], "ccf": [0x3F],
            "cpl": [0x2F], "ldir": [0xED, 0xB0], "lddr": [0xED, 0xB8],
            "ldi": [0xED, 0xA0], "neg": [0xED, 0x44], "reti": [0xED, 0x4D],
            "retn": [0xED, 0x45],
        }
        if m in simple:
            return simple[m]

        if m == "ex":
            if ops == ["de", "hl"]:
                return [0xEB]
            if ops == ["(sp)", "hl"]:
                return [0xE3]
            if ops == ["af", "af'"]:
                return [0x08]
            raise SystemExit(f"cannot encode EX {','.join(ops)}")
        if m == "ret":
            return [0xC9] if not ops else [0xC0 + 8 * CC[ops[0]]]
        if m == "im":
            return [0xED, {0: 0x46, 1: 0x56, 2: 0x5E}[v(ops[0])]]
        if m == "rst":
            return [0xC7 + v(ops[0])]
        if m == "push":
            return [0xC5 + 16 * RP2[ops[0]]]
        if m == "pop":
            return [0xC1 + 16 * RP2[ops[0]]]
        if m == "jp":
            if len(ops) == 1:
                return [0xC3] + le16(v(ops[0]))
            return [0xC2 + 8 * CC[ops[0]]] + le16(v(ops[1]))
        if m == "jr":
            if len(ops) == 1:
                return [0x18, self.relative(ops[0])]
            return [0x20 + 8 * CC[ops[0]], self.relative(ops[1])]
        if m == "djnz":
            return [0x10, self.relative(ops[0])]
        if m == "call":
            if len(ops) == 1:
                return [0xCD] + le16(v(ops[0]))
            return [0xC4 + 8 * CC[ops[0]]] + le16(v(ops[1]))
        if m == "out":
            return [0xD3, v(ops[0].strip("()"))]
        if m == "in":
            return [0xDB, v(ops[1].strip("()"))]
        if m in ("inc", "dec"):
            base = 0x03 if m == "inc" else 0x0B
            step = 0x04 if m == "inc" else 0x05
            if ops[0] in RP:
                return [base + 16 * RP[ops[0]]]
            return [step + 8 * R8[ops[0]]]
        if m == "add":
            if ops[0] == "hl":
                return [0x09 + 16 * RP[ops[1]]]
            source = ops[-1]
            return [0x80 + R8[source]] if source in R8 else [0xC6, v(source)]

        alu = {
            "adc": (0x88, 0xCE), "sub": (0x90, 0xD6), "sbc": (0x98, 0xDE),
            "and": (0xA0, 0xE6), "xor": (0xA8, 0xEE), "or": (0xB0, 0xF6),
            "cp": (0xB8, 0xFE),
        }
        if m in alu:
            base, immediate = alu[m]
            source = ops[-1]
            return [base + R8[source]] if source in R8 else [immediate, v(source)]

        if m == "ld":
            dst, src = ops[0], ops[1]
            special = {("a", "i"): 0x57, ("a", "r"): 0x5F, ("i", "a"): 0x47, ("r", "a"): 0x4F}
            if (dst, src) in special:
                return [0xED, special[(dst, src)]]
            if dst == "sp" and src == "hl":
                return [0xF9]
            if dst in ("(bc)", "(de)") and src == "a":
                return [0x02 + 16 * RP[dst[1:-1]]]
            if dst == "a" and src in ("(bc)", "(de)"):
                return [0x0A + 16 * RP[src[1:-1]]]

            def memory(operand):
                """The address inside (..), or None when it is a register."""
                if operand.startswith("(") and operand.endswith(")"):
                    inner = operand[1:-1]
                    return None if inner in R8 or inner in RP else inner
                return None

            target = memory(src)
            if dst == "a" and target is not None:
                if target in ("bc", "de"):
                    return [0x0A + 16 * RP[target]]
                return [0x3A] + le16(v(target))
            source = memory(dst)
            if src == "a" and source is not None:
                if source in ("bc", "de"):
                    return [0x02 + 16 * RP[source]]
                return [0x32] + le16(v(source))
            if dst == "hl" and target is not None:
                return [0x2A] + le16(v(target))
            if src == "hl" and source is not None:
                return [0x22] + le16(v(source))
            if dst in RP:
                if target is not None:
                    return [0xED, 0x4B + 16 * RP[dst]] + le16(v(target))
                return [0x01 + 16 * RP[dst]] + le16(v(src))
            if src in RP and source is not None:
                return [0xED, 0x43 + 16 * RP[src]] + le16(v(source))
            if dst in R8:
                if src in R8:
                    return [0x40 + 8 * R8[dst] + R8[src]]
                return [0x06 + 8 * R8[dst], v(src)]
            raise SystemExit(f"cannot encode LD {dst},{src}")

        raise SystemExit(f"cannot encode {mnemonic} {','.join(ops)}")

    def directive(self, mnemonic, ops, line):
        m = mnemonic.upper()
        if m == "ORG":
            self.pc = self.value(ops[0])
            return
        if m == "DB":
            for op in ops:
                if op.lower() == "font":
                    for code in range(32, 128):
                        for row in FONT[chr(code)]:
                            bits = row.replace("#", "1").replace(".", "0")
                            self.byte(int(bits, 2) << 2, line)
                        self.byte(0, line)
                    continue
                if len(op) >= 3 and op[0] == "'" and op[-1] == "'":
                    for ch in op[1:-1]:
                        self.byte(ord(ch), line)
                else:
                    self.byte(self.value(op) & 0xFF, line)
            return
        if m == "DW":
            for op in ops:
                for b in le16(self.value(op)):
                    self.byte(b, line)
            return
        if m == "DS":
            for _ in range(self.value(ops[0])):
                self.byte(0, line)
            return
        raise SystemExit(f"unknown directive {mnemonic}")

    def byte(self, value, line):
        if self.emit:
            if self.pc >= ROM_SIZE:
                raise SystemExit(f"{line}: address {self.pc:#06x} is outside the ROM")
            if self.owner[self.pc] is not None:
                raise SystemExit(f"{line}: {self.pc:#06x} already written by {self.owner[self.pc]}")
            self.image[self.pc] = value
            self.owner[self.pc] = line
        self.pc += 1

    def run(self, emit, source=None):
        self.pc = 0
        self.emit = emit
        if not emit:
            self.labels = {}
        for raw in (PROGRAM if source is None else source).splitlines():
            line = raw.split(";")[0].rstrip()
            if not line.strip():
                continue
            while ":" in line.split()[0]:
                label, _, line = line.partition(":")
                label = label.strip().lower()
                if not emit:
                    if label in self.labels:
                        raise SystemExit(f"duplicate label {label}")
                    self.labels[label] = self.pc
                line = line.strip()
                if not line:
                    break
            if not line:
                continue

            parts = line.split(None, 1)
            mnemonic = parts[0]
            operand_text = parts[1] if len(parts) > 1 else ""
            ops = []
            depth = 0
            current = ""
            for ch in operand_text:
                if ch == "(":
                    depth += 1
                elif ch == ")":
                    depth -= 1
                if ch == "," and depth == 0:
                    ops.append(current.strip())
                    current = ""
                else:
                    current += ch
            if current.strip():
                ops.append(current.strip())

            if mnemonic.upper() in ("ORG", "DB", "DW", "DS"):
                self.directive(mnemonic, ops, line)
            else:
                for b in self.encode(mnemonic, [op.lower() for op in ops]):
                    self.byte(b, line)

LOG_ROUTINE = """
; Appends every printed character to a buffer at 0xC000 so a test can read back
; what the ROM was asked to print. Only assembled with --log.
log_char:
        PUSH HL
        PUSH DE
        LD HL,(0x5B10)
        LD DE,0xC000
        ADD HL,DE
        LD (HL),A
        LD HL,(0x5B10)
        INC HL
        LD (0x5B10),HL
        POP DE
        POP HL
        RET

"""

def main():
    if len(FONT) != 96:
        raise SystemExit(f"font covers {len(FONT)} glyphs, expected 96")
    shapes = {}
    for ch, rows in FONT.items():
        key = tuple(rows)
        if key in shapes:
            raise SystemExit(f"glyphs {shapes[key]!r} and {ch!r} are identical")
        shapes[key] = ch

    args = sys.argv[1:]
    log = "--log" in args
    out = None
    if "--out" in args:
        out = args[args.index("--out") + 1]
    if out is None:
        root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        out = os.path.join(root, "tests", "data", "test48.rom")

    source = PROGRAM
    if log:
        # Hook print_char so every character also lands in the log buffer.
        source = source.replace("print_char:\n        PUSH AF",
                                "print_char:\n        CALL log_char\n        PUSH AF", 1)
        source = source.replace("        ORG 0x3D00", LOG_ROUTINE + "        ORG 0x3D00", 1)

    assembler = Assembler()
    assembler.run(emit=False, source=source)
    assembler.run(emit=True, source=source)

    with open(out, "wb") as handle:
        handle.write(assembler.image)
    print(f"wrote {out} ({len(assembler.image)} bytes)" + (" with logging" if log else ""))


if __name__ == "__main__":
    sys.exit(main())
