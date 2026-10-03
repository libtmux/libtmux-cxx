"""Guard complete API program boundaries before native consumer execution."""

import json
import shutil
import tempfile
import unittest
from pathlib import Path

from tools.docs.check_api_examples import MANIFEST, ROOT, read_manifest


class ManifestContract(unittest.TestCase):
    """Refuse partial programs and ambiguous source ownership."""

    def setUp(self):
        """Copy source inputs without altering the checkout."""
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        shutil.copytree(ROOT / "examples/api", self.root / "examples/api")
        self.path = self.root / MANIFEST.relative_to(ROOT)
        self.manifest = json.loads(self.path.read_text())

    def check(self):
        """Validate the modified manifest against its copied files."""
        self.path.write_text(json.dumps(self.manifest))
        return read_manifest(self.path, self.root)

    def test_source_files_are_complete(self):
        """Accept the complete source-owned programs."""
        self.check()

    def test_a_duplicate_program_is_rejected(self):
        """Refuse two programs with the same selector."""
        self.manifest["programs"].append(self.manifest["programs"][0])
        with self.assertRaisesRegex(ValueError, "duplicate program"):
            self.check()

    def test_a_missing_program_is_rejected(self):
        """Refuse a target whose displayed source was deleted."""
        (self.root / self.manifest["programs"][0]["file"]).unlink()
        with self.assertRaisesRegex(ValueError, "missing or invalid source"):
            self.check()

    def test_a_snippet_without_an_entry_point_is_rejected(self):
        """Refuse code that cannot run as its own program."""
        path = self.root / self.manifest["programs"][0]["file"]
        path.write_text(path.read_text().replace("int main()", "int example()"))
        with self.assertRaisesRegex(ValueError, "no entry point"):
            self.check()

    def test_a_duplicate_target_is_rejected(self):
        """Refuse repeated attachments on one declaration."""
        targets = self.manifest["programs"][0]["symbols"]
        targets.append(targets[0])
        with self.assertRaisesRegex(ValueError, "duplicate or empty targets"):
            self.check()

    def test_an_unbound_setup_file_is_rejected(self):
        """Keep setup inside the source checkout."""
        self.manifest["projectFile"] = "../CMakeLists.txt"
        with self.assertRaisesRegex(ValueError, "missing or invalid source"):
            self.check()

    def test_incomplete_expected_output_is_rejected(self):
        """Preserve exact output including its final newline."""
        program = self.manifest["programs"][0]
        program["expectedOutput"] = program["expectedOutput"].rstrip()
        with self.assertRaisesRegex(ValueError, "output must end with a newline"):
            self.check()

    def test_long_comments_are_rejected(self):
        """Keep displayed explanations within the column limit."""
        path = self.root / self.manifest["programs"][0]["file"]
        path.write_text("// " + "x" * 100 + "\n" + path.read_text())
        with self.assertRaisesRegex(ValueError, "comment exceeds 100 columns"):
            self.check()


if __name__ == "__main__":
    unittest.main()
