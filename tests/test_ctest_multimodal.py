from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]


class MultimodalCTestContract(unittest.TestCase):
    def test_cases_are_registered_without_qttest(self):
        source = (ROOT / "tests/multimodal_tests.cpp").read_text()
        for case in (
            "validatesMediaAndTemporalOrder",
            "filtersCapabilitiesBeforeExpectedValueSelection",
            "hostConfigurationExposesRoutesButRejectsPathOverrides",
            "contentOnlyRunSurvivesRecoveryAndFork",
            "queuedAudioAndHistoryRequireBothModalities",
            "toolMediaChangesTheNextModelRoute",
            "missingEconomicsAndUnsupportedModalitiesDoNotGenerate",
            "videoUsesVisionPathAndPreservesTimestamps",
            "currentGoalAndMediaDigestsReachTrustedDecisionInputs",
            "semanticCompactionReceivesTypedMediaAndRetainsOriginalRecords",
            "authenticatedApiAcceptsContentOnlyRun",
        ):
            self.assertIn(f'runner.run("{case}"', source)
        self.assertNotRegex(source, r"\b(?:QTest|QSignalSpy|QAbstractItemModelTester)\b|\bQTEST_|<QtTest|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")

    def test_build_and_consumer_targets_do_not_link_qttest(self):
        cmake = (ROOT / "tests/CMakeTests.cmake").read_text()
        link = re.search(r"target_link_libraries\(iiMultimodalAgentMultimodalTests\s+([^)]*)\)", cmake)
        self.assertIsNotNone(link)
        self.assertNotIn("Qt6::Test", link.group(1))
        consumer = (ROOT / "tests/consumer/CMakeLists.txt").read_text()
        self.assertRegex(consumer, r"foreach\(_kind[^)]*\bmultimodal\b")
        self.assertNotIn("Qt6::Test", consumer)
        self.assertNotRegex(consumer, r"find_package\(Qt6[^)]*\bTest\b")


if __name__ == "__main__":
    unittest.main()
