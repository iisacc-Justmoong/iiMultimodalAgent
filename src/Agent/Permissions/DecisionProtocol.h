#pragma once
#include <iiMultimodalAgent/agent/DecisionGate.h>
#include <iiMultimodalAgent/agent/Types.h>

namespace iiMultimodalAgent::agent {
// Trusted host configuration/return data. Never decoded from model arguments.
IIMULTIMODALAGENT_EXPORT DecisionOptions decisionOptionsFromJson(const QJsonObject&);
IIMULTIMODALAGENT_EXPORT QJsonObject toJson(const DecisionReport&);
namespace detail {
DecisionCandidate decisionCandidate(const ToolCall&,QString description = {});
bool decisionControl(const ToolDefinition&);
DecisionReport decide(const DecisionRequest&,const ToolContext&,const EventCallback&);
struct DeferredDecision {QJsonObject report;};
}
}
