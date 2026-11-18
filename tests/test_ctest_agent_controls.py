from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
CASES = {'procedure': ['hostChangesModelReturnBeforeDispatch', 'remoteReturnControlWorksWhileApiQueueIsFull', 'replacedToolDataMustSatisfyOutputSchema', 'toolReplacementChangesNextModelObservationWithoutRepeatingExecution', 'duplicateChildRequestReusesOneExecutionAndSurvivesRestart', 'pendingCancellationTimeoutAndCompletionReplacementAreVisible', 'parentControlsPendingChildAndDuplicateDoesNotConsumeAnotherSlot', 'initialReceiptRemainsStableAfterIntentionalResume', 'completedRecordsAreEvictedWithinPagingAndMemoryLimits'], 'permission_requests': ['mcpModelAndChildRequestsReachTheConnectionChannel', 'boundedPendingAndHistoryRetainOnlyValidPagesAndRecentResponses', 'apiControlsBypassFullQueueAndChildrenUseTheAuthenticatedChannel', 'mcpControlPoolCannotBeStarvedByWaitingToolsAndRepliesStayConnectionBound', 'clientWinsAgainstACooperativeHookAndOnlyItsUpdatesAreApplied', 'hookWinnerDismissesTheRemotePromptAndHostDenyNeverPrompts', 'repliesAreBoundToOneChannelAndCannotBeReplayedAsNewWork', 'cancellationTimeoutAndCloseLeaveNoPendingApproval', 'invalidAndOversizedResponsesCannotConsumeTheRequest', 'simultaneousConflictingResponsesHaveExactlyOneWinner']}
TARGETS = {'procedure': 'Procedure', 'permission_requests': 'PermissionRequests'}

class ControlCTestContracts(unittest.TestCase):
    def test_all_cases_are_registered_without_qttest(self):
        for suite, cases in CASES.items():
            with self.subTest(suite=suite):
                source = (ROOT / f"tests/{suite}_tests.cpp").read_text()
                for case in cases:
                    self.assertIn(f'runner.run("{case}"', source)
                self.assertNotRegex(source, r"\b(?:QTest|QSignalSpy|QAbstractItemModelTester)\b|\bQTEST_|<QtTest|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")

    def test_targets_do_not_link_qttest(self):
        cmake = (ROOT / "tests/CMakeTests.cmake").read_text()
        for suite, target in TARGETS.items():
            with self.subTest(suite=suite):
                link = re.search(r"target_link_libraries\(iiMultimodalAgent" + target + r"Tests\s+([^)]*)\)", cmake)
                self.assertIsNotNone(link)
                self.assertNotIn("Qt6::Test", link.group(1))

if __name__ == "__main__":
    unittest.main()
