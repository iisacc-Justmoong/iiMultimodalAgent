#pragma once
#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace iiMultimodalAgent::diagnostics {
// Bounded, native evidence projection. Rendering never opens files, loads a
// model, executes a tool or decides that an unsuccessful operation succeeded.
struct FailureContext {
    std::string jobId, operation, stage, backend, error, workerLog, tracePath, traceTail;
    std::map<std::string, std::string> parameters;
    std::optional<long long> step, total, exitCode;
    bool timedOut = false;
};
struct FailureReport {
    std::string category, summary, details;
    std::vector<std::string> checks;
};
inline std::string tail(std::string value, std::size_t limit) {
    return value.size() <= limit ? value : "[earlier content omitted]\n" + value.substr(value.size() - limit);
}
inline FailureReport explain(const FailureContext& input) {
    FailureReport report{"unclassified", "The operation failed; the underlying cause is not confirmed.", {}, {}};
    auto observation = input.error + '\n' + tail(input.workerLog, 16384) + '\n' + tail(input.traceTail, 8192);
    std::transform(observation.begin(), observation.end(), observation.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const auto has = [&](const char* value) { return observation.find(value) != std::string::npos; };
    std::string interpretation;
    if (input.timedOut) {
        report.category = "no-progress-timeout";
        report.summary = "The progress deadline expired before the operation completed.";
        interpretation = "A deadline confirms a lack of reported progress, not a proven deadlock. Slow storage, model preparation, missing progress events or a stalled worker remain possible. Heartbeats and memory usage alone do not prove forward progress.";
        report.checks = {"Inspect the final progress event and trace timestamps; distinguish preparing/loading from denoising.",
            "Check whether model/cache bytes or compute batches were still advancing before the deadline.",
            "Check the selected drive's read speed, free space and runtime cache; preserve the original model and diagnostic trace."};
    } else if (has("unauthorized") || has("invalid access token") || has("token expired") || has("http 401") || has("http 403")) {
        report.category = "authorization"; report.summary = "The service rejected authentication or authorization.";
        interpretation = "The recorded response indicates an access failure. The exact response and requested operation determine whether the account session, token, scope or destination permission needs correction.";
        report.checks = {"Check the response status and the account/destination used for this operation.",
            "Verify the required account permission and token validity; reconnect the affected account if the session is expired.",
            "Before retrying a publish operation, check whether any earlier steps already created remote posts."};
    } else if (has("rate limit") || has("http 429") || has("too many requests")) {
        report.category = "rate-limit"; report.summary = "The service reported a request or publishing rate limit.";
        interpretation = "A rate-limit response is evidence of a service quota or throttling boundary. It does not establish a reset time unless the response supplies one.";
        report.checks = {"Inspect the returned rate-limit and Retry-After fields; do not invent a retry time.",
            "Check the affected account's quota and pause duplicate submissions.",
            "Reconcile already completed publishing steps before any retry to avoid duplicate posts."};
    } else if (has("out of memory") || has("cannot allocate memory") || has("bad_alloc") || has("resource limit") || has("allocation failed")) {
        report.category = "resource-allocation"; report.summary = "The worker reported a resource or memory allocation failure.";
        interpretation = "The recorded allocation error is evidence of resource pressure. It does not establish which allocation, device or process consumed the budget without a memory trace.";
        report.checks = {"Compare system/GPU memory and the worker's memory telemetry at the failure time.",
            "Retry one output with a smaller resolution and fewer video frames; close unused model sessions.",
            "Check whether the selected model and precision fit the device before increasing the allocation budget."};
    } else if (has("no space left") || has("disk full") || has("permission denied") || has("read-only file")) {
        report.category = "storage-access"; report.summary = "The worker reported a storage access or capacity error.";
        interpretation = "The error concerns the path named in the original error/log. A write failure can affect temporary data, a model cache or the final result; it does not mean the source document is corrupt.";
        report.checks = {"Check the exact failing path, mounted drive, available space and write access.",
            "Verify the Society container is still available and the output/cache destination is writable.",
            "Preserve any successfully saved outputs before retrying; do not delete original models or canvases."};
    } else if (has("no such file") || has("file not found") || has("model became unavailable") || has("missing model")) {
        report.category = "missing-resource"; report.summary = "A required local resource was unavailable.";
        interpretation = "The missing-path message is an observation. A disconnected drive, an undownloaded file, an absent model component or a stale reference may explain it; the exact path identifies which resource to check.";
        report.checks = {"Check the failing path and confirm the file is downloaded and its drive mounted.",
            "Verify checkpoint, text encoder, VAE, tokenizer and reference inputs required by this model.",
            "Refresh the model inventory after restoring the resource, then retry the original request."};
    } else if (input.operation != "Image" && input.operation != "Video" && input.operation != "Generation"
        && (has("unsupported") || has("not implemented"))) {
        report.category = "operation-capability"; report.summary = "The requested operation or input is unsupported by this backend.";
        interpretation = "An unsupported-operation error identifies a capability boundary. It is not evidence of a damaged model or a memory problem. The original error determines whether the input format, destination or provider operation is unavailable.";
        report.checks = {"Check the exact operation, input format and destination identified by the error.",
            "Verify the selected backend/provider advertises the required capability and required parameters.",
            "Use a supported format or documented completion route; preserve source files and already completed work."};
    } else if (has("unsupported") || has("not implemented") || has("ggml_assert") || has("assertion failed") || has("bf16")) {
        report.category = "runtime-compatibility"; report.summary = "The log contains a runtime compatibility or assertion diagnostic.";
        interpretation = "The diagnostic may indicate an unsupported operation, tensor type, backend or model architecture. An assertion identifies the failing code boundary; it does not by itself prove that the model file is damaged.";
        report.checks = {"Identify the operation, tensor type, backend and assertion location in the final log/trace.",
            "Verify this model architecture and precision are supported by the installed runtime.",
            "If supported, reproduce with another available device/precision using a small request and retain both logs."};
    } else if (has("invalid output") || has("cannot decode") || has("could not be saved") || has("unreadable") || has("ffmpeg")) {
        report.category = "output-validation"; report.summary = "The output or publication path reported an error.";
        interpretation = "Generation, media encoding, output validation and publication are separate stages. The current evidence does not establish that an invalid final file was a model inference failure.";
        report.checks = {"Check whether inference completed before the save/encode failure.",
            "Inspect encoder exit status, expected frame count, media format and output destination.",
            "Retain intermediate files if available and validate the final media before retrying publication."};
    } else {
        interpretation = "The available evidence is insufficient to name a root cause. No missing model, memory exhaustion or hardware fault is assumed. The original error and last diagnostic events below are the starting point for investigation.";
        report.checks = {"Inspect the first causal error before the final exit message in the worker log.",
            "Check the failed operation's inputs, target path or service, access and submitted settings.",
            "Compare the error and trace before changing unrelated settings; preserve any already completed outputs."};
    }
    if (input.operation == "Publish")
        report.checks.insert(report.checks.begin(), "Check completedSteps, nextStep and requiresReconciliation. Confirm existing remote posts before retrying; partially completed work must not be blindly submitted again.");
    std::ostringstream out;
    const auto value = [](const std::string& text) { return text.empty() ? std::string("Not reported") : text; };
    out << "FAILURE SUMMARY\n" << report.summary << "\n\nOBSERVED FACTS\n"
        << "Job: " << value(input.jobId) << "\nOperation: " << value(input.operation)
        << "\nStage: " << value(input.stage) << "\nBackend: " << value(input.backend) << '\n';
    out << "Progress: ";
    if (input.step) out << *input.step; else out << "not reported";
    if (input.total && *input.total > 0) out << " / " << *input.total;
    else out << " / total unknown";
    out << "\nWorker exit: "; if (input.exitCode) out << *input.exitCode; else out << "not reported";
    out << "\nDeadline expired: " << (input.timedOut ? "yes" : "no")
        << "\n\nORIGINAL ERROR\n" << value(tail(input.error, 8192))
        << "\n\nINTERPRETATION AND LIMITS\n" << interpretation << "\n\nSUBMITTED SETTINGS\n";
    if (input.parameters.empty()) out << "Not reported\n";
    std::size_t fields = 0;
    for (const auto& [key, setting] : input.parameters) {
        if (++fields > 64) { out << "[remaining fields omitted]\n"; break; }
        out << tail(key, 128) << ": " << tail(setting, 2048) << '\n';
    }
    out << "\nRECOMMENDED CHECKS\n";
    for (std::size_t i = 0; i < report.checks.size(); ++i) out << i + 1 << ". " << report.checks[i] << '\n';
    out << "\nWORKER LOG (tail, up to 16 KiB)\n" << value(tail(input.workerLog, 16384))
        << "\n\nDIAGNOSTIC TRACE\nPath: " << value(input.tracePath) << '\n' << value(tail(input.traceTail, 8192))
        << "\n\nThis report describes a failed operation. It did not retry, change settings, repair files or publish content.\n";
    report.details = out.str();
    return report;
}
}
