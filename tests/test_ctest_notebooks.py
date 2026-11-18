from pathlib import Path
import re
import unittest
ROOT=Path(__file__).resolve().parents[1]
CASES=['replacementRequiresReadAndPreservesUnrelatedData', 'actualCellIdPrecedesFallbackIndexAndInsertionIsAfter', 'conversionProducesValidFieldsAndOldFormatsUseIndexSelectors', 'largeAttributionHasImmutableBeforeAndAfterFiles', 'invalidInputsAndDocumentsNeverWrite', 'rawCellsArraysAttachmentsAndFuturePayloadsArePreserved', 'readsAreBoundToContentOwnerAndRevisions', 'permissionAliasesUseTheNotebookPathAndCanonicalTarget', 'boundsCancellationAndFailedArtifactsLeaveOriginalIntact', 'directControlsKeepTranscriptAndExpireObservationsAfterCompaction', 'modelBatchReadsBeforeEditingAndNativeControlsFollowWorktrees', 'apiAndMcpExposeOwnedNativeNotebookOperations']
class NotebookCTestContract(unittest.TestCase):
    def test_cases_and_qttest_removal(self):
        source=(ROOT/"tests/notebook_tests.cpp").read_text()
        for case in CASES:self.assertIn(f'runner.run("{case}"',source)
        self.assertIn('"--offline"',source)
        self.assertNotRegex(source,r"\bQTest\b|\bQTEST_|<QtTest|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")
    def test_target_and_fixture(self):
        cmake=(ROOT/"tests/CMakeTests.cmake").read_text()
        link=re.search(r"target_link_libraries\(iiMultimodalAgentNotebookTests\s+([^)]*)\)",cmake)
        self.assertIsNotNone(link);self.assertNotIn("Qt6::Test",link.group(1))
        self.assertIn('iiMultimodalAgent.notebooks_offline',cmake)
        self.assertIn('WORKING_DIRECTORY "${_test_output}/tmp"',cmake)
if __name__=="__main__":unittest.main()
