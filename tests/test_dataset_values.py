import unittest

from scripts.disaster_validation.dataset_values import hex_literal


class DatasetValuesTest(unittest.TestCase):
    def test_every_id_yields_an_even_length_literal_that_round_trips(self):
        # The regression this pins: X'{i:04x}' produced 5 digits from id 65536
        # onward, so MySQL rejected the batch with ERROR 1064 and every
        # 100k/1m profile aborted at row 65537.
        for value in (0, 1, 4095, 65535, 65536, 65537, 99999, 1000000, 2**31):
            literal = hex_literal(value)
            self.assertTrue(literal.startswith("X'") and literal.endswith("'"))
            digits = literal[2:-1]
            self.assertGreater(len(digits), 0)
            self.assertEqual(len(digits) % 2, 0, f"id {value} -> {literal}")
            self.assertRegex(digits, r"^[0-9a-f]+$")
            self.assertEqual(int(digits, 16), value, f"id {value} -> {literal}")

    def test_stored_bytes_match_the_old_valid_format(self):
        # Where the old four-digit format was still legal, the stored value is
        # unchanged, so earlier smoke evidence stays comparable.
        for value in (1, 255, 4096, 65535):
            self.assertEqual(int(hex_literal(value)[2:-1], 16),
                             int(f"{value:04x}", 16))

    def test_old_zero_padded_format_crosses_into_an_odd_length(self):
        # Documents why the previous format was wrong rather than just wrong.
        self.assertEqual(len(f"{65535:04x}"), 4)
        self.assertEqual(len(f"{65536:04x}"), 5)
        self.assertEqual(len(hex_literal(65536)[2:-1]) % 2, 0)

    def test_negative_id_is_rejected(self):
        with self.assertRaises(ValueError):
            hex_literal(-1)


if __name__ == "__main__":
    unittest.main()
