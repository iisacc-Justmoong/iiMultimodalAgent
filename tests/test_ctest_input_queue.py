from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
CASES = ('pendingLockDeletionWaitsForTheOwningHandle', 'atomicPublicationWaitsForAReaderToReleaseDeleteSharing', 'persistentSharingFailureKeepsTheOriginalQueue', 'cancellationDuringPublicationPreservesTheOriginalQueue', 'identifiedReplayAfterAcknowledgementDoesNotDuplicateTranscriptInput', 'trustedProducerRetriesKeepOnePendingIdentity', 'notificationTransferPreservesIdentityAndNeverMovesPrompts', 'fullDestinationLeavesNotificationsAtTheirSource', 'transferredNotificationReplayIgnoresOnlyQueueLocalOrdering', 'preparationCanInspectPublishAndWithdrawWithoutHoldingQueueLock', 'preparationFailureAndCancellationLeaveTheInputPending', 'preparedRejectionAcknowledgesOnlyTheCurrentItem', 'preparedAndLegacyConsumersShareTheDeliveryLock', 'orderedPersistentAndBounded', 'concurrentPublicationAndLinkedCancellation', 'messageKindsAreNotMixedInOneDelivery', 'interruptedAcknowledgementDoesNotDuplicateTheTranscript', 'conflictingTranscriptIdentityIsNotAcknowledged', 'nextWaitsForToolAndLaterWaitsForAnswer', 'urgentCancelsToolAndPreservesPairedHistory', 'turnLimitAndObserverReentryPreservePendingInput', 'explicitCancellationLeavesUnconsumedInputForResume', 'rejectsCorruptAndRedirectedStorageWithoutReplacingIt')

class InputQueueCTestContract(unittest.TestCase):
    def test_cases_rows_and_worker_mode_remain(self):
        source = (ROOT / "tests/input_queue_tests.cpp").read_text()
        for case in CASES:
            self.assertIn(f'runner.run("{case}"', source)
        for row in ("default-batch", "one-assignment"):
            self.assertIn(f'runner.run("perRunInputLimitKeepsOtherAssignmentsForLaterRuns:{row}"', source)
        self.assertIn('"--queue-publish"', source)
        self.assertNotRegex(source, r"\b(?:QTest|QSignalSpy)\b|\bQTEST_|<QtTest|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")

    def test_target_and_offline_case(self):
        cmake = (ROOT / "tests/CMakeTests.cmake").read_text()
        link = re.search(r"target_link_libraries\(iiMultimodalAgentInputQueueTests\s+([^)]*)\)", cmake)
        self.assertIsNotNone(link)
        self.assertNotIn("Qt6::Test", link.group(1))
        self.assertIn("input_queue_offline", cmake)
        self.assertIn("input_queue_ctest_contract", cmake)

if __name__ == "__main__":
    unittest.main()
