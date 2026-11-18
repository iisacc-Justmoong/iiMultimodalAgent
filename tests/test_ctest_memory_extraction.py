from pathlib import Path
import re
import unittest
ROOT=Path(__file__).resolve().parents[1]
CASES=['identicalParentPrefixNativeWriteAndNoForkTranscript', 'latestPendingCoalescesAndRecalculatesCursor', 'failedAttemptCanRetryAndMissingCursorAfterCompactionUsesVisibleRange', 'attemptedDirectMemoryWriteSkipsEvenWhenItsToolResultFailed', 'capabilitiesDenyMcpBeforeValidationAndWritesOutsideOwnedMemory', 'cancellationIsIndependentFromParentAndCloseJoins', 'copiedReadObservationsAreFrozenAndCannotWriteBackToParentCache', 'explicitDenyAskAndHookRewritesCannotExpandMemoryWrites', 'boundedTurnsTimeoutCadenceAndContextRetention', 'nativeReadOnlyShellDoesNotExecuteStartupFilesOrUnsafeSyntax', 'aFullChildReadCacheCannotEvictParentObservations']
class MemoryExtractionCTestContract(unittest.TestCase):
    def test_cases_and_skip_are_preserved(self):
        text=(ROOT/"tests/memory_extraction_tests.cpp").read_text()
        for case in CASES:self.assertIn(f'runner.run("{case}"',text)
        self.assertIn('"--offline"',text);self.assertIn('agent_ctest::skip(',text)
        self.assertNotRegex(text,r"\bQTest\b|\bQTEST_|<QtTest|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")
    def test_target_and_resource_fixture(self):
        text=(ROOT/"tests/CMakeTests.cmake").read_text();link=re.search(r"target_link_libraries\(iiMultimodalAgentMemoryExtractionTests\s+([^)]*)\)",text)
        self.assertIsNotNone(link);self.assertNotIn("Qt6::Test",link.group(1))
        self.assertIn('memory_extraction_offline',text);self.assertIn('memory_extraction_fake_model',text)
        self.assertIn('WORKING_DIRECTORY "${_test_output}/tmp"',text)
if __name__=="__main__":unittest.main()
