from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
CASES = (
    "authenticatedRegistrationAndRemoval", "privateDirectoryAndRecords",
    "rejectRemoteCommandsMalformedAndStaleRecords", "automaticRefreshAndCredentialRedaction",
    "appRefreshDoesNotRereadCommandConfiguration", "objectThreadAndOwnerLifetime",
    "timedOutAndCancelledQueuedActionsAreDiscarded", "timeoutAfterDispatchReportsPossibleEffects",
    "shutdownDoesNotWaitForTheUiThread",
)


class LocalApplicationsCTestContract(unittest.TestCase):
    def test_cases_and_platform_skip_remain(self):
        source = (ROOT / "tests/local_applications_tests.cpp").read_text()
        for case in CASES:
            self.assertIn(f'runner.run("{case}"', source)
        self.assertIn("tests.initTestCase()", source)
        self.assertIn("agent_ctest::skip(", source)
        self.assertNotRegex(source, r"\b(?:QTest|QSignalSpy)\b|\bQTEST_|<QtTest|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")

    def test_target_and_offline_case(self):
        cmake = (ROOT / "tests/CMakeTests.cmake").read_text()
        link = re.search(r"target_link_libraries\(iiMultimodalAgentLocalApplicationTests\s+([^)]*)\)", cmake)
        self.assertIsNotNone(link)
        self.assertNotIn("Qt6::Test", link.group(1))
        self.assertIn("local_applications_ctest_contract", cmake)
        self.assertIn("local_applications_offline", cmake)
        self.assertIn("SKIP_RETURN_CODE 77", cmake)


if __name__ == "__main__":
    unittest.main()
