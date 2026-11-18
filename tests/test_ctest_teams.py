from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
CASES = ('teammateSchemaTracksOnlyItsPendingShutdown', 'directTeamCallsUseTheAdvertisedProvider', 'rejectedShutdownResponseDoesNotInventPendingRequest', 'undeliverableClaimRetainsIntentAndDoesNotRunOrClaimAgain', 'oversizedAssignmentIsRejectedBeforeClaimAndCanBeCorrected', 'peerSummaryIsLimitedToTheCurrentRun', 'automaticTasksAreClaimedAtStartupAndAfterIdle', 'idleMembersClaimEachEligibleTaskOnceAndRespectDependencies', 'idleNotificationsDoNotCopyTheFinalAnswer', 'queuedTeamMessagesUseShutdownLeaderAndPeerPriority', 'teamAttachmentPreservesDisabledTaskToolsForEveryMember', 'failedMemberPublicationLeavesNoUnownedConversation', 'shutdownResponseCannotBeReplayedOrBorrowAnotherMembersRequest', 'boundedMailboxPreservesInputsTheLeaderHasNotConsumed', 'activeMembersPreventDeletionAndStopCooperatesWithCancellation', 'nativeMemberTasksShareTheLeaderBoard', 'readOnlyMemberCanSendTextButCannotWriteOrEscalate', 'modelShutdownDecisionPersistsBeforeTheMemberStops', 'clearTransfersTheTeamBoardAndPendingLeaderNotifications', 'restartRetainsIdentityButDoesNotRepeatMemberWork', 'teamIdentityIsolationAndFreshTaskNamespaces', 'memberActuallyRunsAndRepliesToTheLeader', 'recipientsAndStructuredMessagesCannotForgeAuthority')
SCHEMA_ROWS = ('plaintext-missing-summary', 'plaintext-empty-summary', 'plaintext-complete', 'shutdown-without-summary', 'shutdown-empty-summary', 'shutdown-with-summary', 'wrong-summary-type', 'unknown-control-type', 'leader-cannot-respond', 'request-id-is-host-owned', 'request-cannot-approve', 'request-empty-reason')

class TeamsCTestContract(unittest.TestCase):
    def test_cases_and_parameter_rows_remain(self):
        source = (ROOT / "tests/team_tests.cpp").read_text()
        for case in CASES:
            self.assertIn(f'runner.run("{case}"', source)
        for row in SCHEMA_ROWS:
            self.assertIn(f'runner.run("messageSchemaPreservesConditionalSummary:{row}"', source)
        for row in ("automatic-off", "tasks-off"):
            self.assertIn(f'runner.run("automaticClaimsRespectHostOptOut:{row}"', source)
        self.assertNotRegex(source, r"\b(?:QTest|QSignalSpy)\b|\bQTEST_|<QtTest|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")

    def test_target_and_offline_case(self):
        cmake = (ROOT / "tests/CMakeTests.cmake").read_text()
        link = re.search(r"target_link_libraries\(iiMultimodalAgentTeamTests\s+([^)]*)\)", cmake)
        self.assertIsNotNone(link)
        self.assertNotIn("Qt6::Test", link.group(1))
        self.assertIn("teams_offline", cmake)
        self.assertIn("teams_ctest_contract", cmake)

if __name__ == "__main__":
    unittest.main()
