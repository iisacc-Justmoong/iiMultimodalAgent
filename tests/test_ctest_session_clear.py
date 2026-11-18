from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
CASES = ['failuresReturnTheNewIdentityAndCloseWaitsForAnAdmittedClear', 'clearStartsANewConversationImmediatelyAndRetainsOldHistory', 'clearCancelsForegroundButKeepsBackgroundChildrenAndShells', 'apiClearIsScopedAndChecksSessionCapacityBeforeEnding']

class SessionClearCTestContract(unittest.TestCase):
    def test_cases_and_desktop_skip_are_preserved(self):
        source = (ROOT / "tests/session_clear_tests.cpp").read_text()
        for name in CASES:
            self.assertIn(f'runner.run("{name}"', source)
        self.assertIn('agent_ctest::skip("Requires a desktop POSIX shell")', source)
        self.assertIn('"--offline"', source)
        self.assertNotRegex(source, r"\b(?:QTest|QSignalSpy|QAbstractItemModelTester)\b|\bQTEST_|<QtTest|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")

    def test_target_does_not_link_qttest(self):
        cmake = (ROOT / "tests/CMakeTests.cmake").read_text()
        link = re.search(r"target_link_libraries\(iiMultimodalAgentSessionClearTests\s+([^)]*)\)", cmake)
        self.assertIsNotNone(link)
        self.assertNotIn("Qt6::Test", link.group(1))
        self.assertIn("iiMultimodalAgent.session_clear_offline", cmake)

if __name__ == "__main__":
    unittest.main()
