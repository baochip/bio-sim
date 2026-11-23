
"""Post-link verification of BIO executable instructions (RV32IMC)."""

from pathlib import Path

# Documented BIO phantom-register errata.
BUG1_OPCODES = {0x17, 0x37, 0x6F}  # AUIPC, LUI, JAL
BUG2_OPCODES = {
    0x03, 0x0F, 0x13, 0x17, 0x37, 0x67, 0x6F, 0x73
}  # Non-R/S/B opcodes in RV32IMC

# Registers with a side effect in the BIO core: a read of x16-x19 pops a
# FIFO, x20 halts until the next quantum, x30 waits for an event. The core
# decodes two register fields for every instruction ([19:15] and [24:20],
# or their place in a 16-bit word) and acts on them even when the
# instruction does not read them:
#   - field 20 halts on either field, read or not;
#   - fields 16-19 and 30 act on [19:15] for every instruction but JAL, and
#     on [24:20] unless the instruction is an immediate ALU or shift, load,
#     JALR, LUI, AUIPC or JAL;
#   - when both fields name the same FIFO and only [19:15] is read, the
#     value is read but not popped;
#   - a FIFO read during a halt or event wait pops on every cycle until
#     the FIFO is empty, and so does a FIFO read by MUL, MULH, MULHSU or
#     MULHU, which hold the read for several cycles.
FIFO_REGS = range(16, 20)
QUANTUM_REG = 20
EVENT_REG = 30

BRANCH_FUNCT3 = {0, 1, 4, 5, 6, 7}
LOAD_FUNCT3 = {0, 1, 2, 4, 5}
STORE_FUNCT3 = {0, 1, 2}
ALU_IMM_FUNCT3 = {0, 2, 3, 4, 6, 7}
SHIFT_IMM = {(1, 0x00), (5, 0x00), (5, 0x20)}
ALU_REG_SUB_FUNCT3 = {0, 5}         # sub, sra (funct7 0x20)
MUL_FUNCT3 = {0, 1, 2, 3}           # mul, mulh, mulhsu, mulhu (funct7 0x01)
SYSTEM_WORDS = {0x00000073, 0x00100073}  # ecall, ebreak


def _operands32(word):
    """(rs1, rs2, reads1, reads2, live1, live2, multi) of a 32-bit word.

    rs1/rs2 are the fields on the read ports, reads* whether the
    instruction reads them, live* whether the port is enabled; illegal
    words read nothing and keep both ports enabled.
    """
    op = word & 0x7F
    f3 = (word >> 12) & 7
    f7 = word >> 25
    rs1 = (word >> 15) & 31
    rs2 = (word >> 20) & 31
    if op in (0x37, 0x17):                       # lui, auipc
        return rs1, rs2, False, False, True, False, False
    if op == 0x6F:                               # jal
        return rs1, rs2, False, False, False, False, False
    if op == 0x67 and f3 == 0:                   # jalr
        return rs1, rs2, True, False, True, False, False
    if op == 0x03 and f3 in LOAD_FUNCT3:
        return rs1, rs2, True, False, True, False, False
    if op == 0x13 and (f3 in ALU_IMM_FUNCT3 or (f3, f7) in SHIFT_IMM):
        return rs1, rs2, True, False, True, False, False
    if ((op == 0x63 and f3 in BRANCH_FUNCT3)
            or (op == 0x23 and f3 in STORE_FUNCT3)
            or (op == 0x33 and (f7 == 0
                                or (f7 == 0x20 and f3 in ALU_REG_SUB_FUNCT3)))):
        return rs1, rs2, True, True, True, True, False
    if op == 0x33 and f7 == 1 and f3 in MUL_FUNCT3:
        return rs1, rs2, True, True, True, True, True
    if (op == 0x0F and f3 == 0) or word in SYSTEM_WORDS:  # fence, ecall, ebreak
        return rs1, rs2, False, False, True, True, False
    return rs1, rs2, False, False, True, True, False   # illegal


def _operands16(half):
    """The same tuple for a 16-bit word, as the core expands it."""
    quad = half & 3
    f3 = half >> 13
    b12 = (half >> 12) & 1
    hi = (half >> 7) & 31                  # [11:7]
    lo = (half >> 2) & 31                  # [6:2]
    rp_hi = 8 + ((half >> 7) & 7)          # rs1'
    rp_lo = 8 + ((half >> 2) & 7)          # rs2'
    none = (0, 0, False, False, True, False, False)
    if quad == 0:
        if f3 == 0:                                        # c.addi4spn
            return (2, 0, True, False, True, False, False)
        if f3 == 2:                                        # c.lw
            return (rp_hi, 0, True, False, True, False, False)
        if f3 == 6:                                        # c.sw
            return (rp_hi, rp_lo, True, True, True, True, False)
        return none
    if quad == 1:
        if f3 == 0 or (f3 == 3 and hi == 2 and (b12 or lo)):  # c.addi, c.addi16sp
            return (hi, 0, True, False, True, False, False)
        if f3 == 4:
            if not (half >> 11) & 1 and not b12:          # c.srli, c.srai
                return (rp_hi, lo, True, False, True, False, False)
            if (half >> 10) & 3 == 2:                      # c.andi
                return (rp_hi, 0, True, False, True, False, False)
            if (half >> 10) & 7 == 3:                      # c.sub, c.xor, c.or, c.and
                return (rp_hi, rp_lo, True, True, True, True, False)
            return none
        if f3 in (6, 7):                                   # c.beqz, c.bnez
            return (rp_hi, 0, True, True, True, True, False)
        return none                                        # c.jal, c.li, c.lui, c.j
    if quad == 2:
        if f3 == 0 and not b12:                            # c.slli
            return (hi, lo, True, False, True, False, False)
        if f3 == 2 and hi:                                 # c.lwsp
            return (2, 0, True, False, True, False, False)
        if f3 == 4:
            if not lo and hi:                              # c.jr, c.jalr
                return (hi, 0, True, False, True, False, False)
            if lo and not b12:                             # c.mv
                return (0, lo, True, True, True, True, False)
            if lo:                                         # c.add
                return (hi, lo, True, True, True, True, False)
            return none
        if f3 == 6:                                        # c.swsp
            return (2, lo, True, True, True, True, False)
    return none


def bio_field_issues(word, length):
    """Kinds of BIO register-field hazards in one instruction word.

    Neutral names; the documented BUG 1 and BUG 2 cases are reported by
    scan_linked_code under their own names and not repeated here.
    """
    if length == 2:
        ops = _operands16(word & 0xFFFF)
    else:
        ops = _operands32(word)
    rs1, rs2, reads1, reads2, live1, live2, multi = ops
    kinds = []
    for name, reg, reads, live in (("rs1", rs1, reads1, live1),
                                   ("rs2", rs2, reads2, live2)):
        if reads:
            continue
        if reg == QUANTUM_REG:
            kinds.append(f"phantom-{name}-quantum")
        elif live and reg in FIFO_REGS:
            kinds.append(f"phantom-{name}-fifo")
        elif live and reg == EVENT_REG:
            kinds.append(f"phantom-{name}-event")
    if reads1 and not reads2 and not live2 and rs1 == rs2 and rs1 in FIFO_REGS:
        kinds.append("fifo-read-without-pop")
    pops = ((reads1 and rs1 in FIFO_REGS) or (reads2 and rs2 in FIFO_REGS))
    if pops:
        halts = (QUANTUM_REG in (rs1, rs2)
                 or (reads1 and rs1 == EVENT_REG) or (reads2 and rs2 == EVENT_REG))
        if halts:
            kinds.append("fifo-pop-during-halt")
        if multi:
            kinds.append("fifo-pop-multicycle")
    if length == 4 and word & 0x707F == 0x000F:   # fence
        # The core has no FENCE case of its own: it runs FENCE as an ALU
        # operation and writes rd. rd 16-19 pushes a FIFO, rd 20 halts.
        rd = (word >> 7) & 31
        if rd in FIFO_REGS or rd == QUANTUM_REG:
            kinds.append("fence-writes-rd")
    return kinds


def scan_linked_code(image, code_end):
    """Scan executable bytes, excluding appended data and alignment."""
    if not (0 < code_end <= len(image)) or code_end % 2:
        raise ValueError(
            f"Invalid executable extent 0x{code_end:x} "
            f"for {len(image)}-byte image"
        )

    issues = []
    pc = 0

    while pc < code_end:
        halfword = int.from_bytes(image[pc:pc + 2], "little")

        if (halfword & 0x3) != 0x3:
            for kind in bio_field_issues(halfword, 2):
                issues.append((pc, halfword, kind))
            pc += 2
            continue

        if (halfword & 0x1F) == 0x1F:
            raise ValueError(
                f"Unsupported >=48-bit instruction at 0x{pc:04x}"
            )

        if pc + 4 > code_end:
            raise ValueError(
                f"Truncated 32-bit instruction at 0x{pc:04x}"
            )

        word = int.from_bytes(image[pc:pc + 4], "little")
        opcode = word & 0x7F

        bug1 = opcode in BUG1_OPCODES and ((word >> 15) & 31) in range(16, 20)
        bug2 = opcode in BUG2_OPCODES and ((word >> 20) & 31) == 20
        if bug1:
            issues.append((pc, word, "BUG1 phantom-rs1"))

        if bug2:
            issues.append((pc, word, "BUG2 phantom-rs2"))

        for kind in bio_field_issues(word, 4):
            if (bug1 and kind == "phantom-rs1-fifo") or (
                    bug2 and kind == "phantom-rs2-quantum"):
                continue
            issues.append((pc, word, kind))

        pc += 4

    return issues


def check_linked_binary(bin_path):
    """Validate and strip a 4-byte linker-resolved code-end footer."""
    path = Path(bin_path)
    linked = path.read_bytes()

    if len(linked) < 6:
        raise ValueError("Missing or truncated BIO code-end footer")

    code_end = int.from_bytes(linked[-4:], "little")
    image = linked[:-4]

    if len(image) > 0xF00:
        raise ValueError(
            f"BIO image exceeds 0xF00 bytes: {len(image)} bytes"
        )

    issues = scan_linked_code(image, code_end)

    if issues:
        report = ", ".join(
            f"0x{pc:04x}=0x{word:08x} ({kind})"
            for pc, word, kind in issues
        )
        raise ValueError(f"Unsafe linked BIO instructions: {report}")

    # Write only the actual executable image and its data.
    path.write_bytes(image)
    return code_end
