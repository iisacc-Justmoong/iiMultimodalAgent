from pathlib import Path
import re
import unittest
ROOT=Path(__file__).resolve().parents[1]
CASES=['endIsOncePerActivationAndAllowsReadOnlyReentryAndResume', 'runningAndQueuedCancellationIsScopedAndConcurrentEndDoesNotDuplicate', 'activeRunEndsBeforeTheHookAndOtherSessionsRemainUsable', 'exceptionsCannotVetoEndAndChildEnginesDoNotEmitMainEnd', 'shortBudgetStopsLaterHooksWithoutRequiringCallbackAdmission', 'commandReasonMatchersAndCommonBudgetStopQueuedProcesses', 'invalidOrPreCancelledRequestsLeaveTheSessionOpen', 'destructorRunsARealCommandAfterTheTranscriptIsStored', 'apiExposesAnAuthenticatedSessionEnd', 'apiEndCancelsRunningAndQueuedRequestsBeforeResumption', 'nativeOperationsAreCancelledWithoutCancellingTheirCaller']
class SessionEndCTestContract(unittest.TestCase):
    def test_cases_and_offline_selection_remain(self):
        text=(ROOT/"tests/session_end_tests.cpp").read_text()
        for case in CASES:self.assertIn(f'runner.run("{case}"',text)
        self.assertIn('"--offline"',text);self.assertNotRegex(text,r"\bQTest\b|\bQTEST_|<QtTest|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")
    def test_target_does_not_link_qttest(self):
        text=(ROOT/"tests/CMakeTests.cmake").read_text();link=re.search(r"target_link_libraries\(iiMultimodalAgentSessionEndTests\s+([^)]*)\)",text)
        self.assertIsNotNone(link);self.assertNotIn("Qt6::Test",link.group(1));self.assertIn('session_end_offline',text)
if __name__=="__main__":unittest.main()
