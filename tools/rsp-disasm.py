#!/usr/bin/env python3
"""RSP disassembler for reading microcode from a ROM (a test-only tool).

    tools/rsp-disasm.py ROM OFFSET SIZE [BASE]

OFFSET and SIZE in hex; BASE is the IMEM address of the first word (default
0x1000, where osSpTaskLoad puts the boot code). Branch targets are IMEM
addresses; delay slots are not marked. A vector op's element shows as [n]
(one lane), [nq] or [nh]; MTC2 and MFC2 show the byte it starts at. The GB
Tower's programs (gb_tower_hle.c): test_rsp_lle prints their offsets, and
each is followed by 0xC0 bytes of tables."""
import struct
import sys

GPR = ["$%d" % i for i in range(32)]
COP0 = ["SP_MEM_ADDR", "SP_DRAM_ADDR", "SP_RD_LEN", "SP_WR_LEN", "SP_STATUS", "SP_DMA_FULL", "SP_DMA_BUSY",
        "SP_SEMAPHORE", "DPC_START", "DPC_END", "DPC_CURRENT", "DPC_STATUS", "DPC_CLOCK", "DPC_BUFBUSY",
        "DPC_PIPEBUSY", "DPC_TMEM"]
VOPS = {0: "vmulf", 1: "vmulu", 2: "vrndp", 3: "vmulq", 4: "vmudl", 5: "vmudm", 6: "vmudn", 7: "vmudh",
        8: "vmacf", 9: "vmacu", 10: "vrndn", 11: "vmacq", 12: "vmadl", 13: "vmadm", 14: "vmadn", 15: "vmadh",
        16: "vadd", 17: "vsub", 19: "vabs", 20: "vaddc", 21: "vsubc", 29: "vsar", 32: "vlt", 33: "veq",
        34: "vne", 35: "vge", 36: "vcl", 37: "vch", 38: "vcr", 39: "vmrg", 40: "vand", 41: "vnand", 42: "vor",
        43: "vnor", 44: "vxor", 45: "vnxor", 48: "vrcp", 49: "vrcpl", 50: "vrcph", 51: "vmov", 52: "vrsq",
        53: "vrsql", 54: "vrsqh", 55: "vnop"}
LS = ["bv", "sv", "lv", "dv", "qv", "rv", "pv", "uv", "hv", "fv", "wv", "tv"]
LS_SHIFT = [0, 1, 2, 3, 4, 4, 3, 3, 4, 4, 4, 4]
ELEM = ["", "", "[0q]", "[1q]", "[0h]", "[1h]", "[2h]", "[3h]", "[0]", "[1]", "[2]", "[3]", "[4]", "[5]", "[6]", "[7]"]


def sx(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def dis(w, pc):
    op = w >> 26
    rs, rt, rd, sa, fn = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31, w & 63
    imm = w & 0xFFFF
    simm = sx(imm, 16)
    br = (pc + 4 + (simm << 2)) & 0xFFF | 0x1000
    if w == 0:
        return "nop"
    if op == 0:
        if fn in (0, 2, 3):
            return "%s %s, %s, %d" % (["sll", "", "srl", "sra"][fn], GPR[rd], GPR[rt], sa)
        if fn in (4, 6, 7):
            return "%s %s, %s, %s" % ({4: "sllv", 6: "srlv", 7: "srav"}[fn], GPR[rd], GPR[rt], GPR[rs])
        if fn == 8:
            return "jr %s" % GPR[rs]
        if fn == 9:
            return "jalr %s, %s" % (GPR[rd], GPR[rs])
        if fn == 13:
            return "break"
        names = {32: "add", 33: "addu", 34: "sub", 35: "subu", 36: "and", 37: "or", 38: "xor", 39: "nor",
                 42: "slt", 43: "sltu"}
        if fn in names:
            return "%s %s, %s, %s" % (names[fn], GPR[rd], GPR[rs], GPR[rt])
    if op == 1:
        names = {0: "bltz", 1: "bgez", 16: "bltzal", 17: "bgezal"}
        return "%s %s, 0x%03X" % (names.get(rt, "regimm?"), GPR[rs], br)
    if op in (2, 3):
        return "%s 0x%03X" % (["j", "jal"][op - 2], (w & 0x3FF) << 2 | 0x1000)
    if op in (4, 5):
        return "%s %s, %s, 0x%03X" % (["beq", "bne"][op - 4], GPR[rs], GPR[rt], br)
    if op in (6, 7):
        return "%s %s, 0x%03X" % (["blez", "bgtz"][op - 6], GPR[rs], br)
    if op in (8, 9, 10, 11):
        return "%s %s, %s, %d" % (["addi", "addiu", "slti", "sltiu"][op - 8], GPR[rt], GPR[rs], simm)
    if op in (12, 13, 14):
        return "%s %s, %s, 0x%X" % (["andi", "ori", "xori"][op - 12], GPR[rt], GPR[rs], imm)
    if op == 15:
        return "lui %s, 0x%X" % (GPR[rt], imm)
    if op == 16:
        return "%s %s, %s" % ("mfc0" if rs == 0 else "mtc0", GPR[rt], COP0[rd & 15])
    if op == 18:
        if rs & 16:
            e = rs & 15
            return "%s $v%d, $v%d, $v%d%s" % (VOPS.get(fn, "v?%d" % fn), sa, rd, rt, ELEM[e])
        e = (w >> 7) & 15
        names = {0: "mfc2", 2: "cfc2", 4: "mtc2", 6: "ctc2"}
        if rs in (0, 4):
            return "%s %s, $v%d[%d]" % (names[rs], GPR[rt], rd, e)
        return "%s %s, $vc%d" % (names.get(rs, "cop2?"), GPR[rt], rd)
    if op in (32, 33, 35, 36, 37, 40, 41, 43):
        names = {32: "lb", 33: "lh", 35: "lw", 36: "lbu", 37: "lhu", 40: "sb", 41: "sh", 43: "sw"}
        return "%s %s, %d(%s)" % (names[op], GPR[rt], simm, GPR[rs])
    if op in (50, 58):
        o = rd
        e = (w >> 7) & 15
        off = sx(w & 0x7F, 7)
        if o < len(LS):
            return "%s%s $v%d[%d], %d(%s)" % ("l" if op == 50 else "s", LS[o], rt, e, off << LS_SHIFT[o], GPR[rs])
    return ".word 0x%08X" % w


def main():
    rom = open(sys.argv[1], "rb").read()
    off, size = int(sys.argv[2], 16), int(sys.argv[3], 16)
    base = int(sys.argv[4], 16) if len(sys.argv) > 4 else 0x1000
    for i in range(0, size, 4):
        w = struct.unpack(">I", rom[off + i:off + i + 4])[0]
        print("%03X: %08X  %s" % (base + i, w, dis(w, base + i)))


if __name__ == "__main__":
    main()
