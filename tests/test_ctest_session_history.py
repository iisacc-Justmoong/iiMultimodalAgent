from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]


class SessionHistoryCTestContract(unittest.TestCase):
    def test_cases_and_resource_workspace(self):
        source = (ROOT / "tests/session_history_tests.cpp").read_text()
        for case in (
            "previewPublicationKeepsCursorButContentEditsInvalidateIt",
            "literalUnicodeSearchAndOwnedWorkspace",
            "repeatedFreshSearchesShareThePreviewReadBoundary",
            "paginatesLargeHistoryWithoutWriterLeaseOrDuplicateResults",
            "cursorCannotCrossOwnerQueryOrSnapshot",
            "selectedSessionBoundsAndSymlinks",
            "partialTailRemainsUntouchedAndMalformedRecordsFail",
            "cancellationLimitsAndNativeToolValidation",
            "structuredFieldsAndUnicodeExcerpts",
            "cursorsAreSingleUseBoundedAndExpiring",
        ):
            self.assertIn(f'runner.run("{case}"', source)
        self.assertIn('QDir::temp().filePath("history-XXXXXX")', source)
        self.assertNotRegex(source, r"\b(?:QTest|QSignalSpy|QAbstractItemModelTester)\b|\bQTEST_|<QtTest|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")

    def test_build_and_consumer_targets_do_not_link_qttest(self):
        cmake = (ROOT / "tests/CMakeTests.cmake").read_text()
        link = re.search(r"target_link_libraries\(iiMultimodalAgentSessionHistoryTests\s+([^)]*)\)", cmake)
        self.assertIsNotNone(link)
        self.assertNotIn("Qt6::Test", link.group(1))
        consumer = (ROOT / "tests/consumer/CMakeLists.txt").read_text()
        self.assertRegex(consumer, r"foreach\(_kind[^)]*\bsession_history\b")
        self.assertNotIn("Qt6::Test", consumer)
        self.assertNotRegex(consumer, r"find_package\(Qt6[^)]*\bTest\b")


if __name__ == "__main__":
    unittest.main()
