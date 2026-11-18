from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
CASES = (
    "registeredControlsHaveCapacityWhenNormalStreamsAndRetentionAreFull",
    "authenticatedSessionsAndNotifications",
    "concurrentReverseRequestsStayOnTheirOriginalStreams",
    "streamCapacityKeepsCancellationAvailable",
    "shutdownAndRelistenJoinCallbacksWithoutHoldingTheSessionMap",
)


class McpHttpServerCTestContract(unittest.TestCase):
    def test_cases_remain_without_qttest(self):
        source = (ROOT / "tests/mcp_http_server_tests.cpp").read_text()
        for case in CASES:
            self.assertIn(f'runner.run("{case}"', source)
        self.assertNotRegex(source, r"\b(?:QTest|QSignalSpy)\b|\bQTEST_|<QtTest|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")

    def test_target_and_timeout(self):
        cmake = (ROOT / "tests/CMakeTests.cmake").read_text()
        link = re.search(r"target_link_libraries\(iiMultimodalAgentMcpHttpServerTests\s+([^)]*)\)", cmake)
        self.assertIsNotNone(link)
        self.assertNotIn("Qt6::Test", link.group(1))
        self.assertIn("mcp_http_server_ctest_contract", cmake)
        self.assertIn("iiMultimodalAgent.mcp_http_server PROPERTIES TIMEOUT 60", cmake)


if __name__ == "__main__":
    unittest.main()
