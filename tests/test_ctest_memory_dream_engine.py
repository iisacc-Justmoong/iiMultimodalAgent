from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]


class MemoryDreamEngineCTestContract(unittest.TestCase):
    def test_cases_and_resource_workspace(self):
        source = (ROOT / "tests/memory_dream_engine_tests.cpp").read_text()
        for case in (
            "automaticDreamStartsBeforeStopWithoutBlockingOrAppending",
            "clearCancelsAndCloseDrainsIndependentMaintenance",
            "availabilityAutomaticFlagAndDependencies",
            "apiOwnerIsolationAndReservedControls",
            "mcpConnectionControlsAndCapabilities",
        ):
            self.assertIn(f'runner.run("{case}"', source)
        self.assertIn('QDir::temp().filePath("dream-engine-XXXXXX")', source)
        self.assertNotRegex(source, r"\b(?:QTest|QSignalSpy|QAbstractItemModelTester)\b|\bQTEST_|<QtTest|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")

    def test_target_does_not_link_qttest(self):
        cmake = (ROOT / "tests/CMakeTests.cmake").read_text()
        link = re.search(r"target_link_libraries\(iiMultimodalAgentMemoryDreamEngineTests\s+([^)]*)\)", cmake)
        self.assertIsNotNone(link)
        self.assertNotIn("Qt6::Test", link.group(1))


if __name__ == "__main__":
    unittest.main()
