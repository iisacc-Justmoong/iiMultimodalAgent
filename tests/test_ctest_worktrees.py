from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
CASES = (
    "realGitEnterKeepAndResumePreserveTheOriginalCheckout",
    "removeRequiresDiscardForFilesAndNewCommits",
    "ownershipCollisionsAndInvalidNamesCannotDeleteOtherWork",
    "failedCreationAndMissingWorktreeCanReturnWithoutDeletingData",
    "preparedOperationsCannotReuseChangedStateOrDiscardNewContents",
    "removalPreviewRequiresACompleteGitSnapshot",
    "sparseCheckoutAndWhitespacePathsRetainTheOriginalChanges",
    "nonGitAdaptersAreOwnerBoundAndRemovalMustActuallySucceed",
    "cachedRemoteBaseAndChangedFileContentAreBound",
    "boundScopesReloadSettingsAndExpireReadsWithoutGrantingTheOriginalRoot",
    "engineSwitchesFilesShellPolicyAndPromptInsideOneToolBatch",
    "sessionEndHooksObserveTheCurrentWorktree",
    "engineResumeAndBackgroundShellUseTheOwnedWorkspace",
    "ignoredFilesAndChangedBranchAreProtected",
)


class WorktreeCTestContract(unittest.TestCase):
    def test_cases_remain_without_qttest(self):
        source = (ROOT / "tests/worktree_tests.cpp").read_text()
        for case in CASES:
            self.assertIn(f'runner.run("{case}"', source)
        self.assertNotRegex(source, r"\b(?:QTest|QSignalSpy)\b|\bQTEST_|<QtTest|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")

    def test_target_and_resource_workspace(self):
        cmake = (ROOT / "tests/CMakeTests.cmake").read_text()
        link = re.search(r"target_link_libraries\(iiMultimodalAgentWorktreeTests\s+([^)]*)\)", cmake)
        self.assertIsNotNone(link)
        self.assertNotIn("Qt6::Test", link.group(1))
        self.assertIn("worktrees_ctest_contract", cmake)
        self.assertRegex(cmake, r'set_tests_properties\(iiMultimodalAgent\.worktrees PROPERTIES TIMEOUT 180\s+WORKING_DIRECTORY "\$\{_test_output\}/tmp"\)')


if __name__ == "__main__":
    unittest.main()
