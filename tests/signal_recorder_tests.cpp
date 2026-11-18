#include "CTestSupport.h"
#include "SignalRecorder.h"
#include <QCoreApplication>
#include <QPointer>
#include <QTimer>

struct CustomPayload { int number; QString text; };
Q_DECLARE_METATYPE(CustomPayload)

class SignalFixture final : public QObject {
    Q_OBJECT
signals:
    void event(QString name, bool accepted);
    void custom(CustomPayload payload);
};

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    agent_ctest::CTestRunner runner;
    runner.run("capturesArgumentsAndWaitsForTheNextSignal", [] {
        SignalFixture sender;
        agent_ctest::SignalRecorder records(&sender, &SignalFixture::event);
        emit sender.event("first", true);
        CTEST_COMPARE(records.size(), 1);
        CTEST_COMPARE(records[0][0].toString(), QString("first"));
        CTEST_CHECK(records[0][1].toBool());
        QTimer::singleShot(0, [&] { emit sender.event("second", false); });
        CTEST_CHECK(records.wait(1000));
        CTEST_COMPARE(records.size(), 2);
        CTEST_COMPARE(records[1][0].toString(), QString("second"));
        CTEST_CHECK(!records[1][1].toBool());
        CTEST_COMPARE(records.last()[0].toString(), QString("second"));
        CTEST_CHECK(!records.wait(0));
    });
    runner.run("preservesCustomPayload", [] {
        SignalFixture sender;
        agent_ctest::SignalRecorder records(&sender, &SignalFixture::custom);
        emit sender.custom({42, QStringLiteral("payload")});
        CTEST_COMPARE(records.size(), 1);
        const auto payload = qvariant_cast<CustomPayload>(records[0][0]);
        CTEST_COMPARE(payload.number, 42);
        CTEST_COMPARE(payload.text, QStringLiteral("payload"));
    });
    runner.run("disconnectsWhenRecorderIsDestroyed", [] {
        SignalFixture sender;
        {
            agent_ctest::SignalRecorder records(&sender, &SignalFixture::event);
            emit sender.event("first", true);
            CTEST_COMPARE(records.count(), 1);
        }
        emit sender.event("after", true);
    });
    runner.run("flushesDeferredDeletesEvenDuringFailure", [] {
        QPointer<QObject> deleted = new QObject;
        try {
            agent_ctest::ScopedDeferredDeletes cleanup;
            deleted->deleteLater();
            CTEST_CHECK(deleted);
            throw std::runtime_error("test exception");
        } catch (const std::runtime_error&) {}
        CTEST_CHECK(!deleted);
    });
    runner.run("rejectsMissingSender", [] {
        SignalFixture* sender = nullptr;
        CTEST_THROWS(std::runtime_error, agent_ctest::SignalRecorder(sender, &SignalFixture::event));
    });
    return runner.result();
}
#include "signal_recorder_tests.moc"
