import hashlib
import tempfile
import unittest
from pathlib import Path

from scripts.disaster_validation.corruption_matrix import (
    CorruptionError,
    _corruption_verdict,
    apply_ibd_mutation,
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


def _write_compressed_tablespace(path, page_size=4096, zip_ssize=1):
    data = bytearray(page_size * 2)
    data[24:26] = b"\x00\x08"          # FIL_PAGE_TYPE FSP_HDR
    flags = (zip_ssize << 1) | 0x21      # POST_ANTELOPE | ATOMIC_BLOBS
    data[54:58] = flags.to_bytes(4, "big")
    path.write_bytes(data)
    return data


class CompressionMetadataMutationTest(unittest.TestCase):
    def test_invalid_zip_ssize_is_written_on_a_real_layout_copy(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "compressed.ibd"
            _write_compressed_tablespace(source, zip_ssize=1)
            target = root / "mutated.ibd"
            result = apply_ibd_mutation(source, target, 4096, "compression_flags_invalid")
            flags = int.from_bytes(target.read_bytes()[54:58], "big")
            self.assertEqual((flags >> 1) & 0xF, 5)
            self.assertEqual(result.byte_offset, 54)
            self.assertNotEqual(result.target_sha256, result.source_sha256)
            # source is untouched evidence
            self.assertEqual((int.from_bytes(source.read_bytes()[54:58], "big") >> 1) & 0xF, 1)

    def test_stride_redeclaration_changes_zip_ssize_only(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "compressed.ibd"
            original = _write_compressed_tablespace(source, zip_ssize=1)
            target = root / "mutated.ibd"
            apply_ibd_mutation(source, target, 4096, "compression_stride_mismatch")
            flags = int.from_bytes(target.read_bytes()[54:58], "big")
            self.assertEqual((flags >> 1) & 0xF, 2)
            # every other flag bit is preserved
            self.assertEqual(flags & ~0x1E, int.from_bytes(original[54:58], "big") & ~0x1E)

    def test_uncompressed_tablespace_is_refused(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "plain.ibd"
            _write_compressed_tablespace(source, zip_ssize=0)
            with self.assertRaises(CorruptionError):
                apply_ibd_mutation(source, root / "mutated.ibd", 4096,
                                   "compression_flags_invalid")

    def test_compression_verdict_requires_a_rejected_status(self):
        good = {"runs": {"compression_flags_invalid": {
            "returncode": 0, "contract_complete": True,
            "classification": "forensic_evidence", "export_success": False,
            "importable": False, "compression_status": "CORRUPTED_METADATA",
            "compression_rejection_reason": "compressed FSP metadata is corrupted"}}}
        self.assertEqual(
            _corruption_verdict(good, "compression_flags_invalid")[0],
            "PASS_EVIDENCE_ONLY")

        not_rejected = {"runs": {"compression_flags_invalid": {
            "returncode": 0, "contract_complete": True,
            "classification": "forensic_evidence", "export_success": False,
            "importable": False, "compression_status": "NOT_COMPRESSED",
            "compression_rejection_reason": ""}}}
        verdict, reasons = _corruption_verdict(not_rejected, "compression_flags_invalid")
        self.assertEqual(verdict, "PRODUCT_FAIL")
        self.assertIn("not rejected", reasons[0])

        exportable = {"runs": {"compression_flags_invalid": {
            "returncode": 0, "contract_complete": True,
            "classification": "structured_recovery", "export_success": True,
            "importable": True, "compression_status": "RECOVERED"}}}
        self.assertEqual(
            _corruption_verdict(exportable, "compression_flags_invalid")[0],
            "PRODUCT_FAIL")


if __name__ == "__main__":
    unittest.main()
