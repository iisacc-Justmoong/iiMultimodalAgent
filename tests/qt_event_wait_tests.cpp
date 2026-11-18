#include "QtEventWait.h"
#include <QCoreApplication>
#include <QElapsedTimer>

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    agent_ctest::CTestRunner runner;
    runner.run("waitsForTimer", [] {
        bool ready = false;
        QTimer::singleShot(0, [&] { ready = true; });
        CTEST_WAIT_VERIFY(ready, 1000);
    });
    runner.run("reportsTimeout", [] {
        CTEST_CHECK(!agent_ctest::waitFor([] { return false; }, 0));
    });
    runner.run("processesEventsForTheRequestedDuration", [] {
        bool delivered = false;
        QTimer::singleShot(0, [&] { delivered = true; });
        QElapsedTimer elapsed;
        elapsed.start();
        agent_ctest::processFor(15);
        CTEST_CHECK(delivered);
        CTEST_CHECK(elapsed.elapsed() >= 15);
    });
    return runner.result();
}
