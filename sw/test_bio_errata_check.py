
import tempfile
import unittest
from pathlib import Path

from bio_errata_check import (
    scan_linked_code,
    check_linked_binary,
)


def w(value):
    return value.to_bytes(4, "little")


class BioErrataTests(unittest.TestCase):

    def test_real_jal_bug2(self):
        self.assertEqual(
            scan_linked_code(w(0xD54FF0EF), 4),
            [(0, 0xD54FF0EF, "BUG2 phantom-rs2")],
        )

    def test_safe_jals(self):
        image = w(0xD3EFF0EF) + w(0xD2AFF0EF)
        self.assertEqual(scan_linked_code(image, 8), [])

    def test_mixed_instruction_lengths(self):
        image = b"\x01\x00" + w(0xD54FF0EF)
        self.assertEqual(
            scan_linked_code(image, 6),
            [(2, 0xD54FF0EF, "BUG2 phantom-rs2")],
        )

    def test_bug1(self):
        self.assertEqual(
            scan_linked_code(w(0x00080037), 4),
            [(0, 0x00080037, "BUG1 phantom-rs1")],
        )

    def test_i_type_bug2(self):
        self.assertEqual(
            scan_linked_code(w(0x01400513), 4),
            [(0, 0x01400513, "BUG2 phantom-rs2")],
        )

    def test_rsb_are_not_bug2(self):
        image = b"".join(map(
            w, [0x01400033, 0x01400023, 0x01400063]
        ))
        self.assertEqual(scan_linked_code(image, 12), [])

    def test_appended_data_excluded(self):
        image = b"\x01\x00\x00\x00" + w(0xD54FF0EF)
        self.assertEqual(scan_linked_code(image, 2), [])

    def test_invalid_extents(self):
        for end in (0, 3, 6):
            with self.subTest(end=end):
                with self.assertRaises(ValueError):
                    scan_linked_code(w(0xD54FF0EF), end)

    def test_truncated_instruction(self):
        with self.assertRaisesRegex(ValueError, "Truncated"):
            scan_linked_code(b"\xef\xf0", 2)

    def test_long_instruction(self):
        with self.assertRaisesRegex(ValueError, "Unsupported"):
            scan_linked_code(b"\x1f\x00", 2)

    def test_invalid_footer(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / "image.bin"

            binary.write_bytes(b"\x01\x00")
            with self.assertRaisesRegex(ValueError, "footer"):
                check_linked_binary(binary)

            binary.write_bytes(b"\x01\x00" + w(3))
            with self.assertRaisesRegex(
                ValueError, "Invalid executable extent"
            ):
                check_linked_binary(binary)

    def test_integration_of_binary_footer(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / "image.bin"
            source_image = b"\x01\x00" + w(0xD54FF0EF)

            # The dangerous word is data, outside the executable extent.
            binary.write_bytes(source_image + w(2))
            self.assertEqual(check_linked_binary(binary), 2)
            self.assertEqual(binary.read_bytes(), source_image)

            # The same word is executable and must be rejected.
            binary.write_bytes(source_image + w(6))
            with self.assertRaisesRegex(ValueError, "BUG2"):
                check_linked_binary(binary)

    def kinds(self, image):
        return [kind for _, _, kind in scan_linked_code(image, len(image))]

    def h(self, value):
        return value.to_bytes(2, "little")

    def test_u_type_rs1_quantum_and_event(self):
        # lui a1, 0xa0 ([19:15] = 20) and lui a1, 0xff0 ([19:15] = 30)
        self.assertEqual(self.kinds(w(0x000A05B7)), ["phantom-rs1-quantum"])
        self.assertEqual(self.kinds(w(0x00FF05B7)), ["phantom-rs1-event"])

    def test_jal_rs1_quantum(self):
        # jal with [19:15] = 20: the quantum field acts without a read
        self.assertEqual(self.kinds(w(0x000A00EF)), ["phantom-rs1-quantum"])

    def test_fence_fields(self):
        # fence with [19:15] = 16 and with [24:20] = 16
        self.assertEqual(self.kinds(w(0x0008000F)), ["phantom-rs1-fifo"])
        self.assertEqual(self.kinds(w(0x0100000F)), ["phantom-rs2-fifo"])

    def test_compressed_shift_quantum(self):
        # c.slli s0, 20 and c.srli s0, 20 (16-bit words were skipped)
        self.assertEqual(self.kinds(self.h(0x0452)), ["phantom-rs2-quantum"])
        self.assertEqual(self.kinds(self.h(0x8051)), ["phantom-rs2-quantum"])

    def test_read_without_pop(self):
        # c.slli x16, 16 and addi x5, x16, 16: x16 is read, not popped
        self.assertEqual(self.kinds(self.h(0x0842)), ["fifo-read-without-pop"])
        self.assertEqual(self.kinds(w(0x01080293)), ["fifo-read-without-pop"])

    def test_pop_during_halt(self):
        # add x5, x16, x20 and c.add x16, x20
        self.assertEqual(self.kinds(w(0x014802B3)), ["fifo-pop-during-halt"])
        self.assertEqual(self.kinds(self.h(0x9852)), ["fifo-pop-during-halt"])

    def test_fence_rd(self):
        # fence with rd = x16 and rd = x20; rd = x5 has no BIO effect
        self.assertEqual(self.kinds(w(0x0000080F)), ["fence-writes-rd"])
        self.assertEqual(self.kinds(w(0x00000A0F)), ["fence-writes-rd"])
        self.assertEqual(self.kinds(w(0x0000028F)), [])

    def test_pop_multicycle(self):
        # mul x5, x16, x6
        self.assertEqual(self.kinds(w(0x026802B3)), ["fifo-pop-multicycle"])

    def test_illegal_word_fields(self):
        # funct3 3 load (illegal on RV32) with [19:15] = 16
        self.assertEqual(self.kinds(w(0x00083283)), ["phantom-rs1-fifo"])

    def test_plain_bio_register_use_is_safe(self):
        image = b"".join([
            w(0x01400033),   # add x0, x0, x20 (quantum wait)
            w(0x000802B3),   # add x5, x16, x0 (FIFO read)
            w(0x01E002B3),   # add x5, x0, x30 (event wait)
            w(0x00080293),   # addi x5, x16, 0
            w(0x00582023),   # sw x5, 0(x16)
            self.h(0x8816),  # c.mv x16, x5 (FIFO write)
            self.h(0x82C2),  # c.mv x5, x16
            self.h(0x0846),  # c.slli x16, 17
        ])
        self.assertEqual(self.kinds(image), [])

    def test_documented_names_unchanged(self):
        # BUG 1 and BUG 2 keep their names and are not reported twice
        self.assertEqual(self.kinds(w(0x00080037)), ["BUG1 phantom-rs1"])
        self.assertEqual(self.kinds(w(0x01400513)), ["BUG2 phantom-rs2"])
        self.assertEqual(self.kinds(w(0x014A0537)),
                         ["BUG2 phantom-rs2", "phantom-rs1-quantum"])


if __name__ == "__main__":
    unittest.main()
