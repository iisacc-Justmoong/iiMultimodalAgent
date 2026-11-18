from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
CASES = (
    "singleSelectionAndPlainPreview",
    "multilineFreeAnswerAndNotesPreservePrototypeKeys",
    "newRequestDoesNotResetFocusOrDraftAndSkipAdvances",
    "cancellationClosesAndEscapeDeclines",
    "fourQuestionsAllowPartialFreeAnswers",
    "multipleSelectionsDoNotUseAnUnselectedPreview",
)


class QuestionSheetCTestContract(unittest.TestCase):
    def test_all_cases_and_window_input_are_preserved(self):
        source = (ROOT / "tests/Legacy/question_sheet_tests.cpp").read_text()
        for case in CASES:
            self.assertIn(f'runner.run("{case}"', source)
        self.assertIn("agent_ctest::clickWindow(window,point)", source)
        self.assertIn("agent_ctest::pressEscape(f.window)", source)
        self.assertEqual(source.count("3000)"), 2)
        self.assertNotRegex(source, r"\b(?:QTest|QSignalSpy)\b|\bQTEST_|<QtTest|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")
        helper = (ROOT / "tests/Legacy/QtGuiInput.h").read_text()
        for event in ("MouseButtonPress", "MouseButtonRelease", "KeyPress", "KeyRelease"):
            self.assertIn(event, helper)
        self.assertIn("mapToGlobal", helper)
        self.assertIn("QPointer<QWindow>", helper)

    def test_target_has_no_qttest_dependency(self):
        cmake = (ROOT / "tests/CMakeTests.cmake").read_text()
        link = re.search(r"target_link_libraries\(iiMultimodalAgentQuestionSheetTests\s+([^)]*)\)", cmake)
        self.assertIsNotNone(link)
        self.assertNotIn("Qt6::Test", link.group(1))
        self.assertNotRegex(cmake, r"find_package\(Qt6[^\n]*\bTest\b")
        self.assertIn("iiMultimodalAgent.question_sheet_ctest_contract", cmake)
        self.assertIn('WORKING_DIRECTORY "${_test_output}/tmp"', cmake[link.end():])


if __name__ == "__main__":
    unittest.main()
