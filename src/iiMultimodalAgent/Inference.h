#pragma once
#include "Export.h"
#include "Modalities.h"
#include <iiLocalLLM.h>
#include <MediaContent.h>
#include <Rpc.h>

// The adapter shares the inference module's value/cancellation contracts.
// Runtime/model ownership remains in iiLocalLLM; the agent owns no model loader.
namespace iiMultimodalAgent {
using iiLocalLLM::Error;
using iiLocalLLM::ErrorCode;
using iiLocalLLM::CancellationToken;
using iiLocalLLM::GenerationOptions;
using iiLocalLLM::GenerationResult;
using iiLocalLLM::GenerationHandle;
using iiLocalLLM::FinishReason;
using iiLocalLLM::StreamEvent;
using iiLocalLLM::StreamEventKind;
using iiLocalLLM::StreamCallback;
using iiLocalLLM::TextCallback;
using iiLocalLLM::ContextBudget;
using iiLocalLLM::Usage;
using iiLocalLLM::Service;
using iiLocalLLM::ConversationRequest;
using iiLocalLLM::ChatMessage;
using iiLocalLLM::Role;
using iiLocalLLM::RpcHandler;
using iiLocalLLM::RpcHandle;
using iiLocalLLM::RpcEventCallback;
using iiLocalLLM::enumName;
using iiLocalLLM::nativeContent;
using iiLocalLLM::contentModalities;
using iiLocalLLM::mediaBytes;
using iiLocalLLM::mediaEvidence;
using iiLocalLLM::validateGenerationOptions;
using iiLocalLLM::generationOptionsFromJson;
using iiLocalLLM::generationOptionsToJson;
using iiLocalLLM::modelId;
namespace agent {}
namespace mcp {}
}
