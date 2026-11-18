from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
CASES = ('queuedWakeCancellationDoesNotWaitForAnUnrelatedModelRun', 'asyncTimeoutMetadataDoesNotReplaceTheCommandDeadline', 'engineSessionEndIsForcedSynchronousEvenWhenConfiguredAsync', 'subagentStartCompletionIsDeliveredWithinTheChildLifetime', 'wakeDoesNotInheritOneRunToolGrants', 'apiControlsAreAuthenticatedAndUseReservedCapacity', 'directMcpHooksBelongToTheConnectionAndCloseCancelsThem', 'automaticWakeCanBeCancelledAfterTheOriginalHandleCompleted', 'automaticWakeChainsAreBoundedAndRetainPendingContext', 'engineQueuesCompletionForOnlyItsOwningSessionWithoutWaking', 'engineRewakesAnIdleSessionAfterItsOriginalRunFinished', 'endingASessionCancelsItsBackgroundHookBeforeReturning', 'completionsCarryOnlyContextAndCannotApplyLateControl', 'asyncRewakeOnlyRequestsWakeForExitTwo', 'isolatedScopeOwnsDeliveryAndHardCancellation', 'forceSynchronousConsumesAnAsyncHeaderAndFinalDecision', 'capacityAndOnceStayBoundedWhileTheCommandIsRunning', 'configuredAsyncReturnsBeforeWorkFinishesAndDeliversStdin', 'firstLineAsyncHandshakeReturnsWhileProcessRemainsLive')

class HookCTestContract(unittest.TestCase):
    def test_cases_remain_without_qttest(self):
        source = (ROOT / "tests/async_hooks_tests.cpp").read_text()
        for case in CASES:
            self.assertIn(f'runner.run("{case}"', source)
        self.assertNotRegex(source, r"\b(?:QTest|QSignalSpy)\b|\bQTEST_|<QtTest|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")

    def test_target_is_qttest_free(self):
        cmake = (ROOT / "tests/CMakeTests.cmake").read_text()
        link = re.search(r"target_link_libraries\(iiMultimodalAgentAsyncHooksTests\s+([^)]*)\)", cmake)
        self.assertIsNotNone(link)
        self.assertNotIn("Qt6::Test", link.group(1))
        self.assertIn("async_hooks_ctest_contract", cmake)

if __name__ == "__main__":
    unittest.main()
