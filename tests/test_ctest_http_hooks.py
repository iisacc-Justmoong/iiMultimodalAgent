from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
CASES = (
    "httpPostsOriginalJsonAndOnlyInterpolatesAllowedHeaders",
    "successfulHttpDecisionsReachTheRealToolAndRecheckHostDeny",
    "httpStatusAndMalformedBodiesCannotGrantOrInjectPlainText",
    "urlAndEnvironmentPoliciesAreAppliedBeforeAnyRequest",
    "privateAndMappedAddressesAreRejectedAndLoopbackDnsWorks",
    "timeoutAndCancellationReleaseCapacityAndOnceIsShared",
    "httpPermissionRequestKeepsItsDecisionWhenACommandFinishesLater",
    "unsupportedAndInvalidConfigurationIsRejected",
    "environmentProxyAndNoProxyUseHostOwnedRouting",
    "duplicateHttpUrlsUseLastMatchedConfigurationButKeepDistinctConditions",
    "invalidHeaderNamesAreRejectedBeforeNetworkIo",
)


class HttpHooksCTestContract(unittest.TestCase):
    def test_cases_remain_without_qttest(self):
        source = (ROOT / "tests/http_hooks_tests.cpp").read_text()
        for case in CASES:
            self.assertIn(f'runner.run("{case}"', source)
        self.assertNotRegex(source, r"\b(?:QTest|QSignalSpy)\b|\bQTEST_|<QtTest|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")

    def test_target_and_timeout(self):
        cmake = (ROOT / "tests/CMakeTests.cmake").read_text()
        link = re.search(r"target_link_libraries\(iiMultimodalAgentHttpHooksTests\s+([^)]*)\)", cmake)
        self.assertIsNotNone(link)
        self.assertNotIn("Qt6::Test", link.group(1))
        self.assertIn("http_hooks_ctest_contract", cmake)
        self.assertIn("iiMultimodalAgent.http_hooks PROPERTIES TIMEOUT 90", cmake)


if __name__ == "__main__":
    unittest.main()
