#pragma once
#include <iiMultimodalAgent/agent/Plugins.h>
#include <iiMultimodalAgent/agent/CommandHooks.h>
#include <iiMultimodalAgent/agent/McpConnections.h>

namespace iiMultimodalAgent::agent {
struct EngineOptions;
// Host composition layer above the execution engines. Construct and attach once
// before creating Engine/Subagents/Teams/McpConnections. Children inherit the
// composed options; they never discover or re-add tools after profile filtering.
class IIMULTIMODALAGENT_EXPORT PluginRuntime {
public:
    PluginRuntime(PluginSnapshot, CommandHookOptions);
    static void attach(EngineOptions&, AgentProfileOptions&, McpConnectionOptions&,
        std::shared_ptr<PluginRuntime>);
    QJsonObject status() const;
private:
    PluginSnapshot snapshot;
    QString workspace;
    QList<Hook> callbacks;
};
}
