from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
CASES = ('graphqlDirectApiUsesOwnedSessionsAndPermission', 'teamRoutesUseAuthenticatedSessionIdentity', 'permissionsInspectionIsAuthenticatedAndCannotChangePolicy', 'liveProfilesShareHostConfigurationButRespectClientIdentity', 'childGenerationUsesHostOptionsIndependentlyOfParentRequests', 'subagentControlsAreAuthenticatedAndRemainResponsive', 'skillsAreClientScopedAndRunWithoutPlaceholderPrompt', 'inputsReachAnActiveRunWithoutAnAvailableRunWorker', 'shellOutputHonorsApiDeadlineWithoutStoppingCommand', 'backgroundShellsAreAuthenticatedAndControlledDuringRuns', 'taskStateIsAuthenticatedAndAccessibleDuringRuns', 'manualCompactionUsesAuthenticatedRunLifecycle', 'projectContextIsAuthenticatedAndWorkspaceBound', 'authenticationIsolationAndRestart', 'cancellationAndAdmission', 'forkPreservesCompleteBoundary', 'runAdmissionStillValidatesTranscriptBody', 'limitsQueueAndPrivateState', 'forkCopiesArtifactsAndOversizedHeaderIsAtomic')

class MigratedCTestContract(unittest.TestCase):
    def test_cases_and_platform_skips_remain(self):
        source = (ROOT / "tests/agent_api_tests.cpp").read_text()
        self.assertEqual(re.findall(r'runner.run\("([^"]+)"', source), list(CASES))
        self.assertIn("agent_ctest::skip(", source)
        self.assertNotRegex(source, r"\b(?:QTest|QSignalSpy)\b|\bQTEST_|<QtTest|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")

    def test_target_is_qttest_free(self):
        cmake = (ROOT / "tests/CMakeTests.cmake").read_text()
        link = re.search(r"target_link_libraries\(iiMultimodalAgentAgentApiTests\s+([^)]*)\)", cmake)
        self.assertIsNotNone(link)
        self.assertNotIn("Qt6::Test", link.group(1))
        self.assertIn("agent_api_ctest_contract", cmake)
        self.assertIn("agent_api_offline", cmake)

if __name__ == "__main__":
    unittest.main()
