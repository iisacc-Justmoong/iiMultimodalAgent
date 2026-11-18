from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]


class PermissionRequestCTestContract(unittest.TestCase):
    def test_cases_are_registered_without_qttest(self):
        source = (ROOT / "tests/permission_request_tests.cpp").read_text()
        for case in (
            "askRunsHooksBeforeHostAndRepreparesUpdatedInput",
            "allowAndDenyDoNotAskAndChangedInputCannotBypassDenyOrSchema",
            "structuredHostFallbackAndLegacyCallbackRemainAvailable",
            "updatesUseOnlyTrustedHostHandlerAndAffectLaterDecisions",
            "denialIsDistinctFromInterruptAndInvalidResponseNeverExecutes",
            "authenticatedApiUsesSameStructuredApprovalAndDoesNotAcceptWireGrants",
        ):
            self.assertIn(f'runner.run("{case}"', source)
        self.assertNotRegex(source, r"\b(?:QTest|QSignalSpy|QAbstractItemModelTester)\b|\bQTEST_|<QtTest|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")

    def test_target_does_not_link_qttest(self):
        cmake = (ROOT / "tests/CMakeTests.cmake").read_text()
        link = re.search(r"target_link_libraries\(iiMultimodalAgentPermissionRequestTests\s+([^)]*)\)", cmake)
        self.assertIsNotNone(link)
        self.assertNotIn("Qt6::Test", link.group(1))


if __name__ == "__main__":
    unittest.main()
