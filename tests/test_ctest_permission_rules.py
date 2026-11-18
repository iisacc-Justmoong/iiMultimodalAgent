from pathlib import Path
import re
import unittest
ROOT=Path(__file__).resolve().parents[1]
CASES=['parsesListsWithoutSplittingArgumentPatterns', 'precedenceAndInvocationLifetime', 'fileAndSkillRulesKeepTheirScope', 'bashChecksEveryCommandAndFindsNestedDenials', 'redirectsSymlinksAndUnanalysedShellDoNotExpandGrants', 'argumentRulesDoNotGrantUnknownToolsAndListsAreBounded', 'whitespaceCommandNameDoesNotBecomeCommandAndArgument', 'readWriteRedirectChecksBothAccessRules', 'trailingArgumentDoesNotReplaceRedirectTarget', 'filePermissionSnapshotRejectsPathReplacementBeforeExecution']
class PermissionRuleCTestContract(unittest.TestCase):
    def test_cases_are_preserved_without_qttest(self):
        source=(ROOT/"tests/permission_rules_tests.cpp").read_text()
        for case in CASES: self.assertIn(f'runner.run("{case}"',source)
        self.assertNotRegex(source,r"\bQTest\b|\bQTEST_|<QtTest|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")
    def test_target_does_not_link_qttest(self):
        cmake=(ROOT/"tests/CMakeTests.cmake").read_text()
        link=re.search(r"target_link_libraries\(iiMultimodalAgentPermissionRuleTests\s+([^)]*)\)",cmake)
        self.assertIsNotNone(link);self.assertNotIn("Qt6::Test",link.group(1))
        self.assertIn("iiMultimodalAgent.permission_rules_ctest_contract",cmake)
if __name__=="__main__":unittest.main()
