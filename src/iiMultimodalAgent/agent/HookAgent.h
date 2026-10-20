#pragma once
#include "Engine.h"
namespace iiMultimodalAgent::agent::detail {
AgentHookExecutor hookAgentExecutor(EngineOptions,std::shared_ptr<TaskStore>);
}
