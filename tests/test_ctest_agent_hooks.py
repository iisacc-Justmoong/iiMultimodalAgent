from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
CASES = ('freshAgentReadsExactTranscriptAndCompletesWithoutLaterTools', 'failedConditionRequestsAnotherParentTurn', 'taskVerifierCanInspectTheSameBoard', 'mcpVerifierUsesEngineOwnerForTasksAndRuntimeGrants', 'transcriptGrantDoesNotExposeAdjacentStateOrWrites', 'explicitTranscriptDenyOverridesTheExactReadGrant', 'invalidResultAndForbiddenControlsCannotComplete', 'ownDeadlineDoesNotCancelParent', 'parentCancellationJoinsAndRemovesVerifier', 'inlineSkillsAndDiscoveryWorkWithoutRecursiveSkillForks', 'scopedSubagentHooksRetainResultToolAndPlanRestrictions', 'backgroundShellsAreStoppedWhenVerificationEnds', 'configurationDeduplicatesAgentAndPromptSeparately')
ROWS = (("assistantMessageLimit", "49th accepted"), ("assistantMessageLimit", "50th not executed"),
        ("verifierUsesDontAskWithExplicitRules", "bypass"), ("verifierUsesDontAskWithExplicitRules", "accept edits"),
        ("verifierUsesDontAskWithExplicitRules", "default"), ("verifierByteLimitsAreDiagnosedAndCleaned", "input"),
        ("verifierByteLimitsAreDiagnosedAndCleaned", "output"))

class AgentHooksCTestContract(unittest.TestCase):
    def test_cases_and_parameter_rows_remain(self):
        source = (ROOT / "tests/agent_hooks_tests.cpp").read_text()
        for case in CASES:
            self.assertIn(f'runner.run("{case}"', source)
        for group, row in ROWS:
            self.assertIn(f'runner.run("{group}:{row}"', source)
        for group in ("assistantMessageLimit:", "verifierUsesDontAskWithExplicitRules:", "verifierByteLimitsAreDiagnosedAndCleaned:"):
            self.assertIn(group, source)
        self.assertNotRegex(source, r"\b(?:QTest|QSignalSpy)\b|\bQTEST_|<QtTest|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")

    def test_target_and_offline_case(self):
        cmake = (ROOT / "tests/CMakeTests.cmake").read_text()
        link = re.search(r"target_link_libraries\(iiMultimodalAgentAgentHooksTests\s+([^)]*)\)", cmake)
        self.assertIsNotNone(link)
        self.assertNotIn("Qt6::Test", link.group(1))
        self.assertIn("agent_hooks_offline", cmake)
        self.assertIn("agent_hooks_ctest_contract", cmake)

if __name__ == "__main__":
    unittest.main()
