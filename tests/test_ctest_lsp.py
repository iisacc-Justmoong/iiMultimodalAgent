from pathlib import Path
import re
import unittest
ROOT=Path(__file__).resolve().parents[1]
CASES=['nativeOperationsUsePersistentServerAndPreserveProtocolData', 'changedFilesSendVersionedUtf16FullRangeReplacement', 'scopePermissionsAndOwnerIsolation', 'cancellationTimeoutAndProtocolErrorsReleaseProcesses', 'schemaAndBoundsRejectBeforeStartingServer', 'transientContentModifiedRetriesWithinTheSameBudget', 'hostPolicyReceivesTheSameNativeDefinitionDuringRevalidation', 'preparedPathCannotBeRedirectedAfterPermissionPreview', 'initializationErrorsArePublicErrors', 'ignoredLocationsAndHostConfigAreValidated', 'queuedCancellationDoesNotWaitForActiveAnalysis', 'engineApiAndMcpShareOwnedLanguageServices']
class LspCTestContract(unittest.TestCase):
    def test_cases_remain_without_qttest(self):
        text=(ROOT/"tests/lsp_tests.cpp").read_text()
        for case in CASES:self.assertIn(f'runner.run("{case}"',text)
        self.assertNotRegex(text,r"\bQTest\b|\bQTEST_|<QtTest|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")
    def test_target_and_resource_workspace(self):
        text=(ROOT/"tests/CMakeTests.cmake").read_text();link=re.search(r"target_link_libraries\(iiMultimodalAgentLspTests\s+([^)]*)\)",text)
        self.assertIsNotNone(link);self.assertNotIn("Qt6::Test",link.group(1));self.assertIn('lsp_ctest_contract',text)
        self.assertRegex(text,r'set_tests_properties\(iiMultimodalAgent\.lsp PROPERTIES TIMEOUT 90\s+WORKING_DIRECTORY "\$\{_test_output\}/tmp"\)')
if __name__=="__main__":unittest.main()
