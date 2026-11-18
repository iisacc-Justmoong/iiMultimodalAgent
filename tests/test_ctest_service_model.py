from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
CASES = ('copiedObservationsUseModelGeneratedJsonAndPreserveThePublicStream', 'singularAndPluralFileContentsUseTheSameMeasuredCompletionContract', 'directFileReadIsExplicitButGeneralOrQuotedProseIsNot', 'loadedSkillMayPutContextBeforeAnExplicitToolSentence', 'explicitReadPathsUseOnlyTheCurrentInstructionAndIntersectHostSchemas', 'namedArgumentsDoNotInviteUnrequestedRevisionOrMetadataFields', 'explicitFieldUpdatesPreserveUnmentionedTaskProperties', 'repeatedNamedCommandsAreCountedAndHistoricalReadsNeedNoNewTool', 'nativeMediaBudgetAndToolObservationsUseThePublicServiceAdapter', 'agentModelHookControlsReachMeasurementAndGeneration', 'explicitUserToolSequenceUsesNativeRequiredChoiceWithoutTrustingObservations', 'agentToolObservationsPreserveDataAndExactText', 'agentStructuredToolDataParticipatesInContextBudget', 'agentRejectsReasoningOnlyWithoutPromotingToolText')

class ServiceModelCTestContract(unittest.TestCase):
    def test_cases_and_fixtures_remain_without_qttest(self):
        source = (ROOT / "tests/service_model_tests.cpp").read_text()
        for case in CASES:
            self.assertIn(f'run("{case}"', source)
        self.assertIn("tests.init()", source)
        self.assertIn("tests.cleanup()", source)
        self.assertNotRegex(source, r"\b(?:QTest|QSignalSpy)\b|\bQTEST_|<QtTest|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")

    def test_target_is_qttest_free(self):
        cmake = (ROOT / "tests/CMakeTests.cmake").read_text()
        link = re.search(r"target_link_libraries\(iiMultimodalAgentServiceModelTests\s+([^)]*)\)", cmake)
        self.assertIsNotNone(link)
        self.assertNotIn("Qt6::Test", link.group(1))
        self.assertIn("service_model_ctest_contract", cmake)

if __name__ == "__main__":
    unittest.main()
