from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
CASES = (
    "jsonLifecycleAndToolAdapter", "sseReverseRequestsProgressAndConcurrency",
    "resumeWithoutReposting", "initializationCanResumeThroughSse",
    "largeRetryDoesNotReconnectEarly", "postHasItsOwnDeadline",
    "cancellationTimeoutAndNoReplay", "expiredSessionReinitializesWithoutReplayingTools",
    "authenticationAndRedirectIsolation", "separateClientsAndNoEventChannel",
    "tlsRejectsAnUntrustedCertificate", "invalidInputAndBounds",
)


class McpHttpClientCTestContract(unittest.TestCase):
    def test_cases_setup_skip_and_assertions_remain(self):
        source = (ROOT / "tests/mcp_http_tests.cpp").read_text()
        for case in CASES:
            self.assertIn(f'runner.run("{case}"', source)
        self.assertIn("tests.initTestCase()", source)
        self.assertIn("agent_ctest::skip(", source)
        self.assertNotRegex(source, r"\b(?:QTest|QSignalSpy)\b|\bQTEST_|<QtTest|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")

    def test_target_and_skip_status(self):
        cmake = (ROOT / "tests/CMakeTests.cmake").read_text()
        link = re.search(r"target_link_libraries\(iiMultimodalAgentMcpHttpTests\s+([^)]*)\)", cmake)
        self.assertIsNotNone(link)
        self.assertNotIn("Qt6::Test", link.group(1))
        self.assertIn("mcp_http_client_ctest_contract", cmake)
        self.assertRegex(cmake, r"iiMultimodalAgent\.mcp_http_client PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77")


if __name__ == "__main__":
    unittest.main()
