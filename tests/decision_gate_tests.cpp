#include "iiMultimodalAgent/agent/DecisionGate.h"
#include "CTestSupport.h"
#include <cmath>
#include <array>
#include <stdexcept>
using namespace iiMultimodalAgent::agent;
class DecisionGateTests final {
public:
    void choosesValueRatherThanTheMostLikelyCandidate() {
        DecisionGate gate;
        const std::array candidates{DecisionCandidate{"likely"},DecisionCandidate{"valuable"}};
        DecisionInput input{"USD",{{"likely",10,1,2,0.99L},{"valuable",100,10,5,0.8L}}};
        const auto report=gate.evaluate(candidates,input);CTEST_COMPARE(report.selectedCandidateId,std::string("valuable"));
        CTEST_CHECK(std::abs(report.assessments[1].expectedValue-73)<1e-15L);
    }
    void lowProbabilityAndLowValueDoNotExecuteEvenWithLargeUpside() {
        DecisionGate gate;
        const std::array candidates{DecisionCandidate{"unlikely"},DecisionCandidate{"small"},DecisionCandidate{"negative"}};
        const auto report=gate.evaluate(candidates,{"USD",{{"unlikely",100000,1,0,0.1L},{"small",1,0,0.5L,0.9L},{"negative",100,1000,1,0.8L}}});
        CTEST_CHECK(!report.execute());CTEST_COMPARE(report.assessments[0].reason,std::string("low_probability"));CTEST_COMPARE(report.assessments[1].reason,std::string("low_value"));CTEST_CHECK(report.assessments[2].expectedValue<0);
    }
    void evidenceChangesTheActualIiDecisionPosterior() {
        DecisionEstimate estimate{"work",100,100,5};
        estimate.states={{"success",{{"verified","yes"}},std::log(0.8L)},{"failure",{{"verified","yes"}},std::log(0.2L)},
            {"success",{{"verified","no"}},std::log(0.1L)},{"failure",{{"verified","no"}},std::log(0.9L)}};
        const std::array candidates{DecisionCandidate{"work"}};DecisionGate gate;
        const auto yes=gate.evaluate(candidates,{"USD",{estimate},{{{"verified","yes"}}}});
        const auto no=gate.evaluate(candidates,{"USD",{estimate},{{{"verified","no"}}}});
        CTEST_CHECK(yes.execute());CTEST_CHECK(!no.execute());CTEST_CHECK(std::abs(yes.assessments[0].successProbability-0.8L)<1e-15L);
        CTEST_CHECK(yes.assessments[0].inference.normalization_error<1e-15L);
    }
    void missingHostEconomicsDefersAndInvalidValuesAreRejected() {
        const std::array candidates{DecisionCandidate{"work"}};DecisionGate gate;
        CTEST_CHECK(!gate.evaluate(candidates,{}).execute());
        CTEST_THROWS(std::invalid_argument,gate.evaluate(candidates,{"USD",{{"work",10,1,1,1.1L}}}));
        CTEST_THROWS(std::invalid_argument,gate.evaluate(candidates,{"USD",{{"work",10,-1,1,0.9L}}}));
        CTEST_THROWS(std::invalid_argument,gate.evaluate(candidates,{"USD",{{"unknown",10,1,1,0.9L}}}));
    }
    void rareFailureLossIsNotRoundedAwayAndTiesAreStable() {
        const std::array candidates{DecisionCandidate{"first"},DecisionCandidate{"second"}};
        DecisionGate gate;DecisionInput input{"USD",{{"first",10,0,0,0.5L},{"second",10,0,0,0.5L}}};
        DecisionOptions half;half.minSuccessProbability=0.5L;CTEST_COMPARE(DecisionGate(half).evaluate(candidates,input).selectedCandidateId,std::string("first"));
        DecisionEstimate rare{"first",100,1e30L,0};rare.states={{"success",{},0},{"failure",{},std::log(1e-30L)}};
        const std::array one{DecisionCandidate{"first"}};auto r=gate.evaluate(one,{"USD",{rare}});CTEST_CHECK(std::abs(r.assessments[0].expectedLoss-1)<1e-14L);
    }
};
int main()
{
    agent_ctest::CTestRunner runner;
    DecisionGateTests tests;
    runner.run("choosesValueRatherThanTheMostLikelyCandidate", [&] { tests.choosesValueRatherThanTheMostLikelyCandidate(); });
    runner.run("lowProbabilityAndLowValueDoNotExecuteEvenWithLargeUpside", [&] { tests.lowProbabilityAndLowValueDoNotExecuteEvenWithLargeUpside(); });
    runner.run("evidenceChangesTheActualIiDecisionPosterior", [&] { tests.evidenceChangesTheActualIiDecisionPosterior(); });
    runner.run("missingHostEconomicsDefersAndInvalidValuesAreRejected", [&] { tests.missingHostEconomicsDefersAndInvalidValuesAreRejected(); });
    runner.run("rareFailureLossIsNotRoundedAwayAndTiesAreStable", [&] { tests.rareFailureLossIsNotRoundedAwayAndTiesAreStable(); });
    return runner.result();
}
