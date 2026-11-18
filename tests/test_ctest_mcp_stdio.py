from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
CASES = ('lifecycleAndDiscovery', 'negotiationAndCapabilities', 'concurrentOutOfOrderRequests', 'legacyBatchesGroupAsyncReverseResponses', 'cancellationAndTimeout', 'initializationTimeoutDetails', 'rootsAndReverseRequests', 'reverseRequestCancellationAndReentry', 'progressConsumerFailureAndInputValidation', 'badListsAndExit', 'aggregateListLimit', 'boundedQueueAndClose', 'agentBridgePreservesResultsAndPolicy', 'nativeAdapterDoesNotDropMedia')
ROWS = ("json", "batch", "utf8", "oversize", "invalid-id")

class McpStdioCTestContract(unittest.TestCase):
    def test_cases_rows_and_progress_remain(self):
        source = (ROOT / "tests/mcp_tests.cpp").read_text()
        for case in CASES:
            self.assertIn(f'runner.run("{case}"', source)
        for row in ROWS:
            self.assertIn('"' + row + '"', source)
        self.assertIn('"malformedPeer:"', source)
        self.assertIn('tests.malformedPeer(QString::fromUtf8(kind))', source)
        self.assertIn('CTEST_CHECK(progressMessageValid.load())', source)
        self.assertNotRegex(source, r"\b(?:QTest|QSignalSpy)\b|\bQTEST_|<QtTest|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")

    def test_target_and_offline_case(self):
        cmake = (ROOT / "tests/CMakeTests.cmake").read_text()
        link = re.search(r"target_link_libraries\(iiMultimodalAgentMcpTests\s+([^)]*)\)", cmake)
        self.assertIsNotNone(link)
        self.assertNotIn("Qt6::Test", link.group(1))
        self.assertIn("mcp_stdio_offline", cmake)
        self.assertIn("mcp_stdio_ctest_contract", cmake)

if __name__ == "__main__":
    unittest.main()
