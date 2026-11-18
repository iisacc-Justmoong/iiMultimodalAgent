from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class ConsumerCTestResources(unittest.TestCase):
    def test_consumers_use_owned_resource_fixture(self):
        cmake = (ROOT / "tests/consumer/CMakeLists.txt").read_text()
        self.assertNotIn('${CMAKE_CURRENT_BINARY_DIR}/tmp', cmake)
        self.assertIn('resources/.state', cmake)
        self.assertIn('FIXTURES_SETUP AgentConsumerTestOutput', cmake)
        self.assertIn('FIXTURES_CLEANUP AgentConsumerTestOutput', cmake)
        self.assertIn('FIXTURES_REQUIRED AgentConsumerTestOutput', cmake)
        self.assertIn('TMPDIR=set:${_consumer_output}/tmp', cmake)


if __name__ == "__main__":
    unittest.main()
