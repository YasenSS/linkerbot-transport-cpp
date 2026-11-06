"""Check that analyzer target selectors follow compiler output."""

import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location(
    "prepare_report_db", Path(__file__).with_name("prepare-report-db.py")
)
prepare_report_db = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prepare_report_db)


class TargetDefinesTests(unittest.TestCase):
    def read_defines(self, text):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "macros.txt"
            path.write_text(text)
            return prepare_report_db.target_defines(path)

    def test_arm_target_preserves_byte_order_without_inventing_x86(self):
        self.assertEqual(self.read_defines(
            "#define __linux__ 1\n#define __aarch64__ 1\n"
            "#define __AARCH64EL__ 1\n#define __LP64__ 1\n"
            "#define __GNUC__ 4\n#define unrelated(x) x\n"
        ), ["-D__linux__=1", "-D__aarch64__=1", "-D__AARCH64EL__=1", "-D__LP64__=1"])

    def test_x86_target_has_no_arm_selectors(self):
        self.assertEqual(self.read_defines(
            "#define __linux__ 1\n#define __x86_64__ 1\n#define __LP64__ 1\n"
        ), ["-D__linux__=1", "-D__x86_64__=1", "-D__LP64__=1"])

    def test_missing_target_selectors_fail(self):
        with self.assertRaises(ValueError):
            self.read_defines("#define unrelated 1\n")


if __name__ == "__main__":
    unittest.main()
