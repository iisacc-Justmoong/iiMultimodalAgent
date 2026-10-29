#pragma once
#include <iiMultimodalAgent/agent/Engine.h>
namespace iiMultimodalAgent::agent::detail {
AgentHookExecutor hookAgentExecutor(EngineOptions,std::shared_ptr<TaskStore>);
}
