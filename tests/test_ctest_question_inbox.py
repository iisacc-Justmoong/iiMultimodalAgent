from pathlib import Path
import re
import unittest
ROOT=Path(__file__).resolve().parents[1]
CASES=['nativeViewModelReceivesDeltasAndSendsValidatedReplies', 'nativeViewModelRecoversAMissedRequestEvent', 'retiredNativeBridgeCannotSubmit', 'changedRequestCanBeQueriedWithoutReadingTheWholeQueue', 'idleInboxDoesNotPoll', 'mcpQuestionWaitDoesNotLockAppMutations', 'workerQuestionAppearsAndReceivesGuiAnswer', 'stableSnapshotsDoNotResetTheUi', 'malformedUiInputDoesNotConsumeTheRequest', 'concurrentOwnersAreAnsweredIndependently', 'cancellationExpiryAndDestructionReleaseWorkers', 'borrowedChannelSurvivesViewDestruction', 'sharedChannelPublishesChangesToEachView', 'viewCanRetireDuringChangeNotification']
class QuestionInboxCTestContract(unittest.TestCase):
    def test_cases_and_signal_payloads_remain(self):
        text=(ROOT/"tests/question_inbox_tests.cpp").read_text()
        for case in CASES:self.assertIn(f'runner.run("{case}"',text)
        self.assertIn("deltas.last().at(0).toString()",text)
        self.assertNotRegex(text,r"\b(?:QTest|QSignalSpy)\b|\bQTEST_|<QtTest|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")
    def test_qttest_link_removed_and_resource_workspace(self):
        text=(ROOT/"tests/CMakeTests.cmake").read_text();link=re.search(r"target_link_libraries\(iiMultimodalAgentQuestionInboxTests\s+([^)]*)\)",text)
        self.assertIsNotNone(link);self.assertNotIn("Qt6::Test",link.group(1))
        self.assertIn('question_inbox_ctest_contract',text)
        self.assertRegex(text,r'set_tests_properties\(iiMultimodalAgent\.question_inbox PROPERTIES\s+WORKING_DIRECTORY "\$\{_test_output\}/tmp"\)')
if __name__=="__main__":unittest.main()
