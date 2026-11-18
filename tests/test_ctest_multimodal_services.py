from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]


class MultimodalServicesCTestContract(unittest.TestCase):
    def test_cases_selection_and_resource_workspace(self):
        source = (ROOT / "tests/multimodal_services_tests.cpp").read_text()
        for case in (
            "disabledServicesDoNotAdvertiseGeneration",
            "imageIsAValidPngAndProviderFileWithInlineMedia",
            "validationCancellationAndPermissionPreventSideEffects",
            "imageInputsAreNotRewrittenByHooksOrPermissionResponses",
            "imageSeedAndExplicitValuesReachTheExecutorWithoutNarrowing",
            "nativeAdapterRejectsMissingModelAndOversizeDefaultsBeforeInference",
            "imageBackendResolvesOnlyUnspecifiedValuesAndReportsActualSettings",
            "imageModelSelectionKeepsHostAndExplicitOptions",
            "embeddedMetadataDoesNotReplaceHostOptions",
            "packagedModelsAreForwardedWithoutMetadataOverrides",
            "ggufMetadataCannotReplaceHostOptions",
            "pcmUsesTheBoundAudioBackendAndRejectsInvalidOutput",
            "cancellationAfterAnAudioCallDoesNotPublishPartialOutput",
            "requestedPcmBudgetIsEnforcedBeforeBackendExecution",
            "realNativeDiffusionAdapterReturnsItsModelError",
            "engineCarriesGeneratedMediaAndHostDecisionAcrossTurns",
            "authenticatedClientsGetSeparateProviderDirectories",
        ):
            self.assertIn(f'runner.run("{case}"', source)
        self.assertIn('QDir::temp().filePath("image-profile-XXXXXX")', source)
        self.assertNotIn('IIMULTIMODAL_TEST_RESOURCES', source)
        self.assertNotRegex(source, r"\b(?:QTest|QSignalSpy|QAbstractItemModelTester)\b|\bQTEST_|<QtTest|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")

    def test_build_and_installed_consumer_do_not_link_qttest(self):
        cmake = (ROOT / "tests/CMakeTests.cmake").read_text()
        link = re.search(r"target_link_libraries\(iiMultimodalAgentMultimodalServicesTests\s+([^)]*)\)", cmake)
        self.assertIsNotNone(link)
        self.assertNotIn("Qt6::Test", link.group(1))
        self.assertIn('iiMultimodalAgentMultimodalServicesTests pcmUsesTheBoundAudioBackendAndRejectsInvalidOutput', cmake)
        consumer = (ROOT / "tests/consumer/CMakeLists.txt").read_text()
        self.assertNotIn("Qt6::Test", consumer)
        self.assertNotRegex(consumer, r"find_package\(Qt6[^)]*\bTest\b")


if __name__ == "__main__":
    unittest.main()
