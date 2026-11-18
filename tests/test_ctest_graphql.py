from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
CASES = (
    "nativeTransportAndPermission",
    "workflowUsesNativeGraphqlWithCrudShellAndSkill",
    "configAndMcpExecutable",
    "daemonAndCliCallGraphQLDirectly",
)


class GraphQLCTestContract(unittest.TestCase):
    def test_cases_remain_without_qttest(self):
        source = (ROOT / "tests/graphql_tests.cpp").read_text()
        for case in CASES:
            self.assertIn(f'runner.run("{case}"', source)
        self.assertNotRegex(source, r"\bQTest\b|\bQTEST_|<QtTest|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")

    def test_native_http_uses_iinetwork(self):
        for filename in ("GraphQL.cpp", "GraphQLTransport.cpp", "GraphQLTransport.h"):
            source = (ROOT / "src/Agent/Integrations" / filename).read_text()
            self.assertNotRegex(source, r"<QtNetwork/|\bQNetwork(?:AccessManager|Request|Reply)\b")
        self.assertIn("ii::network::HttpTransport", (ROOT / "src/Agent/Integrations/GraphQLTransport.cpp").read_text())
        self.assertIn("iiNetwork::iiNetwork", (ROOT / "CMakeLists.txt").read_text())
        self.assertIn("find_dependency(iiNetwork 0.6 CONFIG", (ROOT / "cmake/iiMultimodalAgentConfig.cmake.in").read_text())

    def test_target_and_resource_workspace(self):
        cmake = (ROOT / "tests/CMakeTests.cmake").read_text()
        link = re.search(r"target_link_libraries\(iiMultimodalAgentGraphQLTests\s+([^)]*)\)", cmake)
        self.assertIsNotNone(link)
        self.assertNotIn("Qt6::Test", link.group(1))
        self.assertIn("graphql_ctest_contract", cmake)
        self.assertRegex(cmake, r'set_tests_properties\(iiMultimodalAgent\.graphql PROPERTIES TIMEOUT 240\s+WORKING_DIRECTORY "\$\{_test_output\}/tmp"\)')


if __name__ == "__main__":
    unittest.main()
