from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
CASES = ['matchesIndependentNodeIgnoreCorpus', 'mergesSourcesAndRefreshesRemoval', 'managedDropInsAndSourceSelection', 'anchoredBasenamesNegationAndSourceRoots', 'disabledBypassAndInvalidSettingsDoNotGrant', 'unicodeDirectoryParentsAndDuplicateNegations', 'filesCannotSilentlyDropRulesOrRewriteAuthority', 'finalComponentDirectoryMetadataIsRefreshedBeforeMatching', 'hostilePatternsAreBoundedAndCancellationPropagates']

class PermissionSettingsCTestContract(unittest.TestCase):
    def test_cases_are_registered_without_qttest(self):
        source = (ROOT / "tests/permission_settings_tests.cpp").read_text()
        for case in CASES:
            self.assertIn(f'runner.run("{case}"', source)
        self.assertNotRegex(source, r"\b(?:QTest|QSignalSpy|QAbstractItemModelTester|QFINDTESTDATA)\b|\bQTEST_|<QtTest|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")
        self.assertIn("IIMULTIMODAL_PERMISSION_TEST_DATA_DIR", source)

    def test_target_does_not_link_qttest_and_has_fixture_path(self):
        cmake = (ROOT / "tests/CMakeTests.cmake").read_text()
        link = re.search(r"target_link_libraries\(iiMultimodalAgentPermissionSettingsTests\s+([^)]*)\)", cmake)
        self.assertIsNotNone(link)
        self.assertNotIn("Qt6::Test", link.group(1))
        self.assertIn("IIMULTIMODAL_PERMISSION_TEST_DATA_DIR", cmake)

if __name__ == "__main__":
    unittest.main()
