#include "CTestSupport.h"

#include <sstream>
#include <stdexcept>
#include <string>

int main()
{
    std::ostringstream errors;
    std::ostringstream output;
    auto* previous = std::cerr.rdbuf(errors.rdbuf());
    auto* previousOutput = std::cout.rdbuf(output.rdbuf());
    agent_ctest::CTestRunner empty;
    const int emptyResult = empty.result();
    agent_ctest::CTestRunner missing("absent");
    missing.run("other", [] { CTEST_CHECK(false); });
    const int missingResult = missing.result();
    agent_ctest::CTestRunner failure;
    bool continued = false;
    failure.run("bad-row", [] { CTEST_CHECK(false); });
    failure.run("next-row", [&] { continued = true; });
    const int failureResult = failure.result();
    agent_ctest::CTestRunner skipped;
    skipped.run("unsupported", [] { agent_ctest::skip("no desktop shell"); });
    const int skippedResult = skipped.result();
    agent_ctest::CTestRunner mixed;
    mixed.run("unsupported", [] { agent_ctest::skip("no desktop shell"); });
    mixed.run("supported", [] {});
    const int mixedResult = mixed.result();
    std::cerr.rdbuf(previous);
    std::cout.rdbuf(previousOutput);
    if (emptyResult == 0 || missingResult == 0 || failureResult == 0 || !continued ||
        skippedResult != 77 || mixedResult != 0 ||
        errors.str().find("no desktop shell") == std::string::npos ||
        errors.str().find("No test cases") == std::string::npos ||
        errors.str().find("bad-row") == std::string::npos ||
        errors.str().find("ctest_support.cpp:") == std::string::npos)
        return 1;

    agent_ctest::CTestRunner success;
    bool selected = false;
    bool unexpected = false;
    agent_ctest::CTestRunner filtered("selected");
    filtered.run("other", [&] { unexpected = true; });
    filtered.run("selected", [&] { selected = true; });
    if (!selected || unexpected || filtered.result() != 0)
        return 1;
    agent_ctest::CTestRunner group("parameterized");
    int groupRows = 0;
    group.run("parameterized:first", [&] { ++groupRows; });
    group.run("parameterized:second", [&] { ++groupRows; });
    group.run("parameterizedExtra:other", [&] { unexpected = true; });
    if (groupRows != 2 || unexpected || group.result() != 0) return 1;
    agent_ctest::CTestRunner row("parameterized:second");
    int selectedRows = 0;
    row.run("parameterized:first", [&] { unexpected = true; });
    row.run("parameterized:second", [&] { ++selectedRows; });
    if (selectedRows != 1 || unexpected || row.result() != 0) return 1;
    success.run("checks", [] {
        int evaluations = 0;
        CTEST_CHECK(true);
        CTEST_COMPARE(++evaluations, 1);
        CTEST_COMPARE(evaluations, 1);
        CTEST_THROWS(std::runtime_error, throw std::runtime_error("expected"));
    });
    return success.result();
}
