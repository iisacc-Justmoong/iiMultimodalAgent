from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
CASES = (
    "hostQuantitiesCrossHttpAndDecisionResultCrossesIpc",
    "procedureReturnsCrossIpcAndHttpWithoutBlockingControls",
    "authSessionsForkAndEventOrder", "crossTransportCancellationAndDisconnect",
    "timeoutAndShutdown", "streamOverflowHasIndependentDeadline",
)


class AgentTransportCTestContract(unittest.TestCase):
    def test_cases_and_callback_assertion_remain(self):
        source = (ROOT / "tests/agent_transport_tests.cpp").read_text()
        for case in CASES:
            self.assertIn(f'runner.run("{case}"', source)
        self.assertIn("CTEST_CHECK(resumeAccepted)", source)
        self.assertNotRegex(source, r"\b(?:QTest|QSignalSpy)\b|\bQTEST_|<QtTest|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")

    def test_target_and_timeout(self):
        cmake = (ROOT / "tests/CMakeTests.cmake").read_text()
        link = re.search(r"target_link_libraries\(iiMultimodalAgentAgentTransportTests\s+([^)]*)\)", cmake)
        self.assertIsNotNone(link)
        self.assertNotIn("Qt6::Test", link.group(1))
        self.assertIn("agent_transport_ctest_contract", cmake)
        self.assertIn("iiMultimodalAgent.agent_transport PROPERTIES TIMEOUT 60", cmake)

    def test_ipc_socket_has_short_isolated_resource_workspace(self):
        source = (ROOT / "tests/agent_transport_tests.cpp").read_text()
        cmake = (ROOT / "tests/CMakeTests.cmake").read_text()
        self.assertIn('IIMULTIMODALAGENT_TEST_RESOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/resources"', cmake)
        self.assertIn('QTemporaryDir ipcDirectory{QStringLiteral(IIMULTIMODALAGENT_TEST_RESOURCE_ROOT "/ipc-XXXXXX")}', source)
        self.assertLess(source.index("QTemporaryDir ipcDirectory"), source.index("std::unique_ptr<LocalIpcServer> ipc"))
        self.assertIn('ipc->listen(ipcDirectory.filePath("s"))', source)
        self.assertIn('ipcDirectory.isValid()', source)
        self.assertIn('agent transport IPC workspace:', source)
        self.assertIn('agent transport IPC listening:', source)

    def test_each_transport_case_is_available_separately(self):
        cmake = (ROOT / "tests/CMakeTests.cmake").read_text()
        self.assertTrue('iiMultimodalAgent.agent_transport.${case}' in cmake)
        for case in CASES:
            self.assertTrue(f'        {case}' in cmake, f'missing CTest case {case}')


if __name__ == "__main__":
    unittest.main()
