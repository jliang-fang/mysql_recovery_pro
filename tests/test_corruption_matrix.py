import hashlib
import tempfile
import unittest
from pathlib import Path

from scripts.disaster_validation.corruption_matrix import (
    CorruptionError,
    _corruption_verdict,
    apply_mutation,
    apply_tde_candidate,
)


class CorruptionMatrixTest(unittest.TestCase):
    page_size = 4096

    def make_image(self, root: Path) -> Path:
        image = root / "baseline.img"
        data = bytearray(self.page_size * 4)
        data[4:8] = (0).to_bytes(4, "big")
        data[24:26] = b"\x00\x08"
        data[34:38] = (11).to_bytes(4, "big")
        page = memoryview(data)[self.page_size:self.page_size * 2]
        page[16:24] = (17).to_bytes(8, "big")
        page[4:8] = (1).to_bytes(4, "big")
        page[24:26] = b"\x45\xbf"
        page[34:38] = (11).to_bytes(4, "big")
        image.write_bytes(data)
        return image

    def test_image_mutations_are_derived_and_bounded(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = self.make_image(root)
            original = hashlib.sha256(source.read_bytes()).hexdigest()
            for mutation in (
                "checksum_body_flip", "fil_header_page_no_flip", "page_type_flip",
                "record_chain_break", "tail_truncate_partial_page", "fsp_flags_corrupt",
                "garbage_append",
            ):
                target = root / f"{mutation}.img"
                result = apply_mutation(source, target, self.page_size, mutation)
                self.assertEqual(result.source_sha256, original)
                self.assertNotEqual(result.target_sha256, original)
                self.assertEqual(hashlib.sha256(source.read_bytes()).hexdigest(), original)
                self.assertTrue(target.is_file())
                self.assertIsNotNone(result.byte_offset)

    def test_tde_candidate_is_supported_only_for_preserved_ibd(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = self.make_image(root)
            target = root / "candidate.ibd"
            result = apply_tde_candidate(source, target, self.page_size)
            self.assertEqual(result.name, "tde_candidate")
            payload = target.read_bytes()
            self.assertEqual(payload[24:26], b"\x45\xbf")
            self.assertEqual(payload[38], 6)
            with self.assertRaises(CorruptionError):
                apply_mutation(source, root / "invalid.img", self.page_size, "tde_candidate")

    def test_tde_verdict_requires_non_success_non_exportable_contract(self):
        good = {
            "runs": {"tde_candidate": {
                "returncode": 1, "contract_complete": True,
                "classification": "expected_reject", "export_success": False,
                "importable": False,
            }},
        }
        self.assertEqual(_corruption_verdict(good, "tde_candidate")[0], "PASS_EVIDENCE_ONLY")

        bad = {
            "runs": {"tde_candidate": {
                "returncode": 0, "contract_complete": True,
                "classification": "expected_reject", "export_success": False,
                "importable": False,
            }},
        }
        self.assertEqual(_corruption_verdict(bad, "tde_candidate")[0], "PRODUCT_FAIL")


if __name__ == "__main__":
    unittest.main()
