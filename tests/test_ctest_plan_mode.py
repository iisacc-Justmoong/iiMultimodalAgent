from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
CASES = ('activePlanPublishesItsExactWritePathWithoutRestrictingProjectReads', 'verificationAgentCannotReadAnotherSessionsPlanInsideWorkspace', 'mcpEngineWrapperCanEnterPlanningAndKeepTaskControlsUsable', 'transitionInvalidatesAnAlreadyPreparedProjectWrite', 'permissiveCustomPolicyCannotBypassPlanning', 'deferredPlanningToolsCanBeDiscovered', 'modelCanEnterPlanningAndReceivesCurrentState', 'ownedPlanIsTheOnlyWritableFileAndApprovalRestoresPolicy', 'deniedOrForgedExitLeavesPlanningActive', 'hostMayEditExactlyTheReviewedPlan', 'changesAfterPermissionBeforeExecutionAreDetected', 'explicitDenyAndAskStillApplyToPlanWrites', 'actionDescriptionsNeverGrantShellPermission', 'corruptStateAndSymlinkTargetsFailClosed', 'planStateAndForeignFilesAreNotSearchable', 'restartAndForkPreserveContentWithoutTransferringApproval', 'unreviewedExternalChangeReentersRestrictedState', 'planByteAndTextLimitsAreEnforced', 'emptyPlanCanBeReviewedButCannotExitAnInactiveSession', 'configuredPlanModeReturnsToDefaultAfterReview', 'brokerCancellationKeepsPlanAndOneConcurrentApprovalWins', 'engineEndRetainsPlanClearStartsFreshAndDisabledHostsReject')
ROWS = (("subagentKeepsPlanningRestrictionsAndCannotReadForeignPlan", "engine-options"),
        ("subagentKeepsPlanningRestrictionsAndCannotReadForeignPlan", "scoped-context-only"),
        ("changedPlanCannotReceiveAnOlderApproval", "allow"),
        ("changedPlanCannotReceiveAnOlderApproval", "host-edit"))

class PlanModeCTestContract(unittest.TestCase):
    def test_cases_rows_and_callback_results_remain(self):
        source = (ROOT / "tests/plan_mode_tests.cpp").read_text()
        for case in CASES:
            self.assertIn(f'runner.run("{case}"', source)
        for group, row in ROWS:
            self.assertIn(f'runner.run("{group}:{row}"', source)
        for result in ("changedDuringReview", "changedAtExecution"):
            self.assertIn(f'CTEST_CHECK({result})', source)
        self.assertNotRegex(source, r"\b(?:QTest|QSignalSpy)\b|\bQTEST_|<QtTest|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")

    def test_target_and_offline_case(self):
        cmake = (ROOT / "tests/CMakeTests.cmake").read_text()
        link = re.search(r"target_link_libraries\(iiMultimodalAgentPlanModeTests\s+([^)]*)\)", cmake)
        self.assertIsNotNone(link)
        self.assertNotIn("Qt6::Test", link.group(1))
        self.assertIn("plan_mode_offline", cmake)
        self.assertIn("plan_mode_ctest_contract", cmake)

if __name__ == "__main__":
    unittest.main()
