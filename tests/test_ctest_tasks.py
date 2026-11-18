from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
CASES = ('nativeLockSharingWaitsWithoutLosingTaskState', 'nativeLockSharingCanBeCancelled', 'atomicTaskPublicationWaitsForAReader', 'persistentTaskSharingFailurePreservesTheOriginalBoard', 'taskPublicationCanBeCancelledWithoutChangingTheBoard', 'retiredNamespaceCannotBeRecreatedByStaleOwners', 'verificationCanReadTheBoardBeforePublication', 'verificationSideEffectsCannotBeOverwrittenByAStaleCommit', 'persistentLifecycleAndDependencies', 'validationTransactionsAndHooks', 'concurrentCreationAndAtomicClaim', 'unicodeOwnerSurvivesPersistentReload', 'todosIsolationAndCorruptStorage', 'processesCoordinateAndWaitingCanBeCancelled', 'modelToolsResumeContextPolicyAndFork')

class TaskCTestContract(unittest.TestCase):
    def test_cases_worker_mode_and_callback_are_preserved(self):
        source = (ROOT / "tests/task_tests.cpp").read_text()
        self.assertEqual(re.findall(r'runner.run\("([^"]+)"', source), list(CASES))
        self.assertIn('args[1] == "--task-worker"', source)
        self.assertIn('statusMatched = change.after["status"] == "completed"', source)
        self.assertIn('CTEST_CHECK(statusMatched)', source)
        self.assertNotRegex(source, r"\b(?:QTest|QSignalSpy)\b|\bQTEST_|<QtTest|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")

    def test_target_and_offline_case(self):
        cmake = (ROOT / "tests/CMakeTests.cmake").read_text()
        link = re.search(r"target_link_libraries\(iiMultimodalAgentTaskTests\s+([^)]*)\)", cmake)
        self.assertIsNotNone(link)
        self.assertNotIn("Qt6::Test", link.group(1))
        self.assertIn("tasks_offline", cmake)
        self.assertIn("tasks_ctest_contract", cmake)

if __name__ == "__main__":
    unittest.main()
