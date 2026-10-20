# Module extraction and dependency ownership

The extraction moves the real implementation, including Windows storage,
background ownership and session-history fingerprint fixes. Agent execution is
not delegated back into the old library. Lower inference/decision modules never
reference the higher agent package.

| Before | After |
| --- | --- |
| Agent `find_package(iiLocalLLM 0.55 ...)` | `find_package(iiMultimodalAgent 0.1 CONFIG REQUIRED)` |
| Agent `iiLocalLLM::iiLocalLLM` target | `iiMultimodalAgent::iiMultimodalAgent` |
| `<agent/Engine.h>` | `<iiMultimodalAgent/agent/Engine.h>` |
| `<mcp/Client.h>` | `<iiMultimodalAgent/mcp/Client.h>` |
| `iiLocalLLM::agent` / `iiLocalLLM::mcp` | `iiMultimodalAgent::agent` / `iiMultimodalAgent::mcp` |
| Agent-enabled `iiLocalLLMD` | `iiMultimodalAgentD` / `iimagent-service` |
| `iillm agent ...`, `iillm-mcp`, `iillm-plugins` | `iimagent agent ...`, `iimagent-mcp`, `iimagent-plugins` |
| Harness `IILOCALLLM_TEST_*` | `IIMULTIMODALAGENT_TEST_*` |
| `IILOCALLLM_QML_DIR` | `IIMULTIMODALAGENT_QML_DIR` |

iiLocalLLM advances to 0.56.0 because removing agent/MCP exports changes the
combined shared-library ABI. Rebuild agent consumers. Windows uses the new
`IIMULTIMODALAGENT_EXPORT` definition, preserving imported iiLocalLLM DLL symbols.

`iiMultimodalAgent::Inference.h` explicitly shares the inference module's value
and cancellation types. Native media/projector handling stays in iiLocalLLM.
The Qt-free DecisionGate header retains lightweight native modality types;
decision JSON/procedure adapters belong to the agent. iiDecision stays independent.

On macOS the agent and its consumers mark the selected shared iiFileProviderNative
as a needed library before indirect provider dependencies. CMake normally retains
the last repeated dylib entry on this platform; nominal target order alone can
otherwise load an older copy bundled in another SDK prefix. Runtime binding tests
compare the actual symbol-owner paths with the imported CMake targets. This does
not depend on a global dynamic-library environment override.

An exported `DYLD_LIBRARY_PATH` or `DYLD_FRAMEWORK_PATH` can still override
macOS package resolution. On Apple hosts, CTest and the installer remove those
overrides, including fallback/versioned variants, only from their own processes.
They do not edit the user's shell configuration. Keep the same isolation when
qualifying an installed application. The runtime-binding test verifies actual
loaded paths, not just CMake's selected prefixes.

Permissions, recovery, memory, quotas, native child-process behavior and trusted
economic assessment semantics are preserved. `agent.run`, `iisacc.agent/1`, stored
`iisacc.*` schemas and `IILOCALLLM_APP_ENDPOINTS` remain compatible. Assign daemon
endpoints deliberately; two daemons must not own one socket concurrently.

The inherited Qt Core/Network implementation is a documented migration boundary.
No Qt dependency is added to iiDecision, iiSDKCore or the generation modules.
iiAudioGen's existing Python worker is used only when the host binds that backend.
Historical feature documents are marked separately from the current verification.
