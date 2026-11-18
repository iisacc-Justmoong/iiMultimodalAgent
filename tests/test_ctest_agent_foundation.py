from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
CASES = {'context': ['utf8BomAndWindowsNewlinesKeepRuleScope', 'markdownBoundariesAndEscapedPaths', 'excludesCyclesAndResourceLimits', 'internalSymlinkDedupAndInvalidRunDoesNotPersist', 'engineLoadsAndRefreshesRealToolContext', 'deleteRequiresAnUnchangedReadAndPermission', 'orderImportsAndMarkdown', 'scopedRulesAndNestedDirectories', 'refreshAndBounds', 'confinementAndMalformedInput'], 'skill': ['delimiterWhitespaceDoesNotHideInvocationFlags', 'compactionDoesNotReplayOldSkillAndOtherToolsCannotInject', 'engineInjectsAfterAllResultsAndPreservesSnapshot', 'directInvocationAndModelPermissionsRemainSeparate', 'committedToolPromptIsRecoveredOnceAfterObserverFailure', 'metadataAndLiteralArguments', 'priorityRefreshAndFlags', 'unsupportedExecutionPropertiesFailExplicitly', 'malformedAndBoundedFiles', 'confinedSymlinksAndNoAncestorDiscovery'], 'rag': ['indexesCreatorsSourcesAndPersistsChanges', 'rejectsBrokenInputAndCorruptStorage', 'chunksRetainExactUtf8Offsets', 'suppliedCaptionsAndTranscriptsRetainTheirSourceKinds', 'answeringUsesOnlyRetrievedEvidenceAndReportsCitationFailures', 'decisionGateRetriesAndRejectsIncompleteEvidence', 'expandedSearchFindsNewEvidenceWithoutCombiningScores', 'decisionRankingPrioritizesTheStrongestAdmittedPage', 'evidenceBudgetAndCancellationStopGeneration', 'interchangeableRetrieversMustReturnValidBoundedEvidence', 'concurrentStoresPreserveBothUpdates', 'toolRequiresExplicitHostAuthorizationAndUsesEnginePermissions'], 'model_hooks': ['configurationAndMissingModelAreExplicit', 'argumentsAreSubstitutedOnceAndAfterToolUsesActualObservation', 'limitsRejectContextAndOutputWithoutApprovingAnything', 'deduplicationOnceAndSharedConcurrencyLimits', 'engineLifecycleGetsSnapshotAndStopFalseCancelsWithoutRecursion', 'taskTransactionsAndSubagentHooksUseTheirHostModel', 'toolAndPermissionHooksRespectHostPolicy', 'promptUsesHistoryAndSchemaWithoutExecutingPendingTools', 'falseBlocksContinuationAndInvalidOutputIsNotAValidDecision', 'timeoutCancelsOnlyEvaluatorAndExternalCancellationPropagates']}
TARGETS = {'context': 'Context', 'skill': 'Skill', 'rag': 'Rag', 'model_hooks': 'ModelHooks'}

class AgentCTestContracts(unittest.TestCase):
    def test_all_cases_are_registered_without_qttest(self):
        for suite, cases in CASES.items():
            with self.subTest(suite=suite):
                source = (ROOT / f"tests/{suite}_tests.cpp").read_text()
                for case in cases:
                    self.assertIn(f'runner.run("{case}"', source)
                self.assertNotRegex(source, r"\b(?:QTest|QSignalSpy|QAbstractItemModelTester)\b|\bQTEST_|<QtTest|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")

    def test_targets_do_not_link_qttest(self):
        cmake = (ROOT / "tests/CMakeTests.cmake").read_text()
        for suite, target in TARGETS.items():
            with self.subTest(suite=suite):
                link = re.search(r"target_link_libraries\(iiMultimodalAgent" + target + r"Tests\s+([^)]*)\)", cmake)
                self.assertIsNotNone(link)
                self.assertNotIn("Qt6::Test", link.group(1))

if __name__ == "__main__":
    unittest.main()
