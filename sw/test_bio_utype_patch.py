import unittest

from clang2rustasm import (
    _i_type_imm_has_bug2,
    _patch_auipc,
    _patch_lui,
    _u_type_phantom_rs1,
    _u_type_phantom_rs2,
)

MASK = 0xFFFFFFFF


def run(lines):
    """Evaluate the lui/auipc/addi/slli/add/sw/lw lines of a patch (pc 0).

    Returns the registers, and fails on an emitted immediate that would
    itself trigger a phantom-register erratum.
    """
    regs = {"zero": 0, "sp": 0x800, "t0": 0x5A5A5A5A}
    stack = {}
    for text, _ in lines:
        if text is None:
            continue
        op, rest = text.split(None, 1)
        args = [a.strip() for a in rest.split(",")]
        if op in ("lui", "auipc"):
            imm = int(args[1], 0)
            assert not _u_type_phantom_rs1(imm), text
            assert not _u_type_phantom_rs2(imm), text
            regs[args[0]] = (imm << 12) & MASK
        elif op in ("addi", "slli"):
            imm = int(args[2], 0)
            assert not _i_type_imm_has_bug2(imm), text
            src = regs[args[1]]
            regs[args[0]] = (src + imm if op == "addi" else src << imm) & MASK
        elif op == "add":
            regs[args[0]] = (regs[args[1]] + regs[args[2]]) & MASK
        elif op in ("sw", "lw"):
            off, base = args[1].rstrip(")").split("(")
            addr = regs[base] + int(off)
            if op == "sw":
                stack[addr] = regs[args[0]]
            else:
                regs[args[0]] = stack[addr]
        else:
            raise AssertionError(f"unexpected instruction {text}")
    return regs


class UTypePatchTests(unittest.TestCase):

    def test_quantum_and_event_fields_are_patched(self):
        # Every 20-bit immediate whose bits [7:3] (instruction [19:15])
        # are 20 or 30, through lui and auipc.
        for field in (20, 30):
            for high in range(1 << 12):
                for low in range(8):
                    imm = (high << 8) | (field << 3) | low
                    for patch in (_patch_lui, _patch_auipc):
                        name = patch.__name__[len("_patch_"):]
                        text = f"{name} a1, 0x{imm:x}"
                        lines = patch(name, ["a1", f"0x{imm:x}"], text)
                        self.assertIsNotNone(lines, text)
                        regs = run(lines)
                        self.assertEqual(regs["a1"], imm << 12, text)
                        self.assertEqual(regs["t0"], 0x5A5A5A5A, text)

    def test_comment_names(self):
        quantum = _patch_lui("lui", ["a1", "0xa0"], "lui a1, 0xa0")
        event = _patch_lui("lui", ["a1", "0xff0"], "lui a1, 0xff0")
        bug1 = _patch_lui("lui", ["a1", "0x90"], "lui a1, 0x90")
        self.assertIn("phantom-rs1-quantum", quantum[0][1])
        self.assertIn("phantom-rs1-event", event[0][1])
        self.assertIn("BUG1", bug1[0][1])

    def test_safe_immediates_unchanged(self):
        for imm in (0x0, 0x1, 0xF00, 0x12345 & ~(0x1F << 3)):
            self.assertIsNone(_patch_lui("lui", ["a1", hex(imm)], "lui"))


if __name__ == "__main__":
    unittest.main()
