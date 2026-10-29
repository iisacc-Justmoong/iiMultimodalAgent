#pragma once
#include <iiMultimodalAgent/agent/CommandHooks.h>
namespace iiMultimodalAgent::agent::detail {
struct PromptHook {QString prompt,model;bool agent=false;};
struct PromptHookResult {HookResult result;Usage usage;QString model;QJsonObject details;bool cancelled=false;};
QJsonObject hookDecisionSchema();
PromptHookResult evaluatePromptHook(const PromptHook&,const HookInput&,const QByteArray&,
    const CommandHookOptions&,int timeoutMs,const CancellationToken&,const std::function<void()>& started);
}
