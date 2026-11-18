from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
CASES = ('exactObservationRepairUsesTheExistingTurnLimit', 'exactFieldRepairRequiresAnUnambiguousObservation', 'exactValueUsesDecodedTextBeforeProtocolMetadata', 'exactJsonGeneratedByCustomModelsIsDecodedWithoutAnotherToolCall', 'catalogBudgetUsesCurrentInputAndDoesNotReplaceGenerationHistory', 'singleToolLimitIsPassedToModels', 'completionToolStopsBeforeLaterTools', 'verifierPermissionModeDoesNotInheritBypass', 'schemaAndRegistry', 'permissionsAndHookRevalidation', 'transcriptInvariantsAndRecovery', 'toolLoopAndPersistentResume', 'interruptedToolIsNotExecutedAgain', 'schemaFailureBecomesToolResult', 'cancellationAndConsumerFailure', 'workspaceReadEditAndStaleGuard', 'parallelToolsAndExclusiveBarrier', 'stopHookAndTurnLimit', 'shellTimeoutAndExitStatus', 'invalidResultsAndDuplicateCalls', 'registrySnapshotDuringExecution')

class AgentEngineCTestContract(unittest.TestCase):
    def test_all_cases_stay_in_the_original_order_without_qttest(self):
        source = (ROOT / "tests/agent_tests.cpp").read_text()
        self.assertEqual(re.findall(r'runner.run\("([^"]+)"', source), list(CASES))
        self.assertNotRegex(source, r"\b(?:QTest|QSignalSpy)\b|\bQTEST_|<QtTest|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")
        self.assertNotIn("Q_OBJECT", (ROOT / "tests/agent_tests.h").read_text())

    def test_target_and_offline_case(self):
        cmake = (ROOT / "tests/CMakeTests.cmake").read_text()
        link = re.search(r"target_link_libraries\(iiMultimodalAgentAgentTests\s+([^)]*)\)", cmake)
        self.assertIsNotNone(link)
        self.assertNotIn("Qt6::Test", link.group(1))
        self.assertIn("agent_ctest_contract", cmake)
        self.assertIn("agent_offline", cmake)

if __name__ == "__main__":
    unittest.main()
