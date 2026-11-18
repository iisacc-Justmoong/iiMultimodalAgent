from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
CASES = (
    "keywordAndDirectSearch", "selectionIsScopedAndPersisted",
    "sameTurnAndChangedSchemaCannotBypassDiscovery", "atomicReplacementPreservesSnapshots",
    "configuredHttpToolsAndNotificationRefresh", "configErrorsAndCredentialsStayIsolated",
    "configuredStdioReloadAndRecovery", "failedConnectionAndDiscoveryDiagnostics",
    "perServerDeadlinesAndReload", "validationBeforeEffects",
    "selectionBoundsPolicyAndCompaction", "closeDoesNotCancelTheHostsToken",
)


class DiscoveryCTestContract(unittest.TestCase):
    def test_cases_remain_without_qttest(self):
        source = (ROOT / "tests/discovery_tests.cpp").read_text()
        for case in CASES:
            self.assertIn(f'runner.run("{case}"', source)
        self.assertNotRegex(source, r"\b(?:QTest|QSignalSpy)\b|\bQTEST_|<QtTest|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")

    def test_target_and_offline_case(self):
        cmake = (ROOT / "tests/CMakeTests.cmake").read_text()
        link = re.search(r"target_link_libraries\(iiMultimodalAgentDiscoveryTests\s+([^)]*)\)", cmake)
        self.assertIsNotNone(link)
        self.assertNotIn("Qt6::Test", link.group(1))
        self.assertIn("discovery_ctest_contract", cmake)
        self.assertIn("discovery_offline", cmake)


if __name__ == "__main__":
    unittest.main()
