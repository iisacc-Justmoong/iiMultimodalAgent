from pathlib import Path
import re
import unittest
ROOT=Path(__file__).resolve().parents[1]
CASES=['additiveNamespacedDocumentsAndPrivateStatus', 'revisionsAreStableAndDataSurvivesDisableUpdateAndRemoval', 'dependencyAvailabilityAndCyclesBlockConfiguration', 'invalidUpdatesAreAtomicAndPackagesCannotEscape', 'tamperedCacheCannotBecomeASelectedPrompt', 'actualSkillAndSubagentUseThePluginSnapshot', 'unsupportedServersAndDisabledDiscoveryAreExplicit', 'mcpProcessAndLspProtocolUsePackageResources', 'commandHooksReceivePluginEnvironmentWithoutPathReevaluation', 'authenticatedApiAndMcpExposeTheSameRedactedSnapshot', 'cliLifecycleReturnsReviewableResults']
class PluginsCTestContract(unittest.TestCase):
    def test_cases_and_offline_selection_remain(self):
        text=(ROOT/"tests/plugin_tests.cpp").read_text()
        for case in CASES:self.assertIn(f'runner.run("{case}"',text)
        self.assertIn('"--offline"',text);self.assertIn('ScopedDeferredDeletes',text)
        self.assertNotRegex(text,r"\bQTest\b|\bQTEST_|<QtTest|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")
    def test_no_qttest_and_resource_working_directory(self):
        text=(ROOT/"tests/CMakeTests.cmake").read_text();link=re.search(r"target_link_libraries\(iiMultimodalAgentPluginTests\s+([^)]*)\)",text)
        self.assertIsNotNone(link);self.assertNotIn("Qt6::Test",link.group(1));self.assertIn('plugins_offline',text)
        self.assertIn('WORKING_DIRECTORY "${_test_output}/tmp"',text)
if __name__=="__main__":unittest.main()
