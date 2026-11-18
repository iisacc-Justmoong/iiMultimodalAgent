from pathlib import Path
import re
import unittest
ROOT=Path(__file__).resolve().parents[1]
CASES=['deferredQuestionsAreDiscoveredAndResumeWithTheAnswer', 'missingResponseAndDontAskNeverInventAnAnswer', 'questionsOptionsFreeTextAndAnnotationsRoundTrip', 'malformedQuestionsFailBeforeReview', 'hostResponseCannotReplaceQuestionsOrInjectUnknownAnswers', 'htmlValidationIsAnIntentCheckNotASanitizer', 'beforeToolCannotForgeAnswersButPermissionRequestHookCanRespond', 'channelAnswerIsOwnedAndReplayCannotChangeIt', 'expiredAndCancelledQuestionsCannotComplete', 'disabledEngineAndReservedIdentity', 'endingSessionCancelsAQuestionAndPlanCanContinueAfterAnswer', 'modelConsumesTrustedHostAnswer']
ROWS=['model-answers', 'model-annotations', 'wire-owner', 'empty', 'duplicate', 'too-many', 'one-option', 'duplicate-label', 'blank', 'nul', 'type', 'header-limit', 'utf8-payload-limit', 'question', 'options', 'metadata', 'missing-questions', 'unknown-answer', 'unknown-annotation', 'answer-type']
class UserQuestionsCTestContract(unittest.TestCase):
    def test_cases_and_data_rows_remain(self):
        text=(ROOT/"tests/user_questions_tests.cpp").read_text()
        for case in CASES:
            if case in ("malformedQuestionsFailBeforeReview", "hostResponseCannotReplaceQuestionsOrInjectUnknownAnswers"):
                self.assertIn('"'+case+':"',text)
            else:
                self.assertIn(f'runner.run("{case}"',text)
        for row in ROWS:self.assertIn('"'+row+'"',text)
        self.assertNotRegex(text,r"\bQTest\b|\bQTEST_|<QtTest|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")
        self.assertIn('foreignRejected',text);self.assertIn('ScopedDeferredDeletes',text)
    def test_target_does_not_link_qttest(self):
        text=(ROOT/"tests/CMakeTests.cmake").read_text();link=re.search(r"target_link_libraries\(iiMultimodalAgentUserQuestionsTests\s+([^)]*)\)",text)
        self.assertIsNotNone(link);self.assertNotIn("Qt6::Test",link.group(1));self.assertIn('user_questions_ctest_contract',text)
if __name__=="__main__":unittest.main()
