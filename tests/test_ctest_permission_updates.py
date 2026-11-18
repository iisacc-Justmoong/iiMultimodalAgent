from pathlib import Path
import re
import unittest
ROOT=Path(__file__).resolve().parents[1]
CASES=['approvalAppliesDirectoryGrantBeforePreparingChangedInput', 'approvalRemovalDropsCachedRootsAndRejectsSuppliedSnapshots', 'persistentUpdatesPreserveUnrelatedSettingsAndSurviveRestart', 'runtimeRulesCliOverridesAndModesAreIsolated', 'directoriesRetainBindingsAcrossSessionsAndCanBeRemovedPerSession', 'invalidBatchAndManagedPolicyCannotSilentlyGrant', 'ruleRemovalNormalizesWholeToolAliasesAndEscapesContent', 'concurrentWritersDoNotLoseRulesAndLinksAreRejected', 'clearAndForkInheritTrustedRuntimeStateAndCapacityIsBounded', 'batchValidationKeepsFilesAndRuntimeUnchanged', 'changedInputIsDeniedAfterTheApprovedSettingsArePublished', 'lockedFileTimesOutOrCancelsWithoutPublishing']
class PermissionUpdatesCTestContract(unittest.TestCase):
    def test_cases_and_skip_remain(self):
        text=(ROOT/"tests/permission_updates_tests.cpp").read_text()
        for case in CASES:self.assertIn(f'runner.run("{case}"',text)
        self.assertIn('agent_ctest::skip(',text)
        self.assertNotRegex(text,r"\bQTest\b|\bQTEST_|<QtTest|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")
    def test_qttest_link_and_output_are_removed(self):
        text=(ROOT/"tests/CMakeTests.cmake").read_text()
        link=re.search(r"target_link_libraries\(iiMultimodalAgentPermissionUpdatesTests\s+([^)]*)\)",text)
        self.assertIsNotNone(link);self.assertNotIn("Qt6::Test",link.group(1))
        self.assertNotIn("permission-updates-qt.txt",text)
        self.assertIn("permission_updates_ctest_contract",text)
if __name__=="__main__":unittest.main()
