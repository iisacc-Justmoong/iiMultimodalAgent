#include <iiMultimodalAgent/diagnostics/FailureReport.h>
#include <iostream>

int main() {
    iiMultimodalAgent::diagnostics::FailureContext failure;
    failure.jobId = "example-video";
    failure.operation = "Video";
    failure.stage = "loading";
    failure.backend = "native";
    failure.error = "The worker stopped reporting progress.";
    failure.step = 0; failure.total = 0; failure.timedOut = true;
    failure.parameters = {{"model", "example model"}, {"width", "576"}, {"height", "1024"}, {"frames", "72"}};
    std::cout << iiMultimodalAgent::diagnostics::explain(failure).details;
}
