# iiMultimodalAgent

C++23 multimodal agent orchestration, extracted from iiLocalLLM 0.55.1.
The initial package version is **0.1.0**. It owns the agent harness, session
persistence, tools, permissions, hooks, memory, background work, subagents, teams,
MCP client/server, authenticated API, and iiDecision admission/model routing.

| Module | Responsibility |
| --- | --- |
| iiMultimodalAgent | Agent lifecycle, tool/model orchestration, media delivery, decision integration |
| iiLocalLLM 0.56 | Model management and text/image/audio/timestamped-frame understanding through supported models |
| iiDecision | Independent decision library; the agent owns host economics and admission/routing policy |
| iiLocalDiffusion 0.6 | In-process native image generation |
| iiAudioGen 0.3 | Host-selected audio generation backend and float32 PCM |
| iiVoiceOver 0.2 | In-process Piper/ONNX speech synthesis |
| iiFileProvider / Native 0.8.2 | Exclusive file publication, metadata and preview pipeline |

Dependency direction is `iiMultimodalAgent -> inference/generation/decision modules`.
Neither iiLocalLLM nor iiDecision depends on the agent. Models and destinations
are host configuration, never arbitrary model tool arguments. No model download
or runtime installation occurs implicitly.

The native image adapter is an internal shared library beside the agent library.
It loads on the first actual native image call, after ordinary tool admission;
starting a text agent does not load the image/tensor backend. It uses the same
C++23 tool API and adds no interpreter. Install the complete package, including
`iiMultimodalAgentImages`, rather than copying the main library alone. Hosts that
call iiLocalDiffusion functions directly additionally link that SDK's target.

The inherited harness uses Qt 6.8.3 Core/Network; this extraction is not a Qt-free
rewrite. The adapters add no QtGui dependency. iiAudioGen's existing Diffusers
backend runs a Python worker; binding it does not make it native in-process
inference. Hosts can inject an `iiAudioGen::Backend` without changing the agent.

## Build, install and link

Build/install the dependencies first, including iiLocalLLM 0.56. Preserve their
optional accelerators and pass the same dependency prefixes to installed consumers.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="<installed SDK and Qt prefixes>" \
  -DCMAKE_INSTALL_PREFIX="$HOME/.local/SDK/iiMultimodalAgent"
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
cmake --install build
```

`./install.sh` also verifies an installed consumer. SDK-wide installation discovers
the package and its six SDK dependencies automatically. Optional real-model
qualification uses `IIMULTIMODALAGENT_TEST_*` paths. Set
`IIMULTIMODALAGENT_TEST_BUNDLED_VOICE=ON` for native Hello World synthesis through
the new `Speak` tool; an absent fixture is not counted as successful inference.

```cmake
find_package(iiMultimodalAgent 0.1 CONFIG REQUIRED)
target_link_libraries(MyApp PRIVATE iiMultimodalAgent::iiMultimodalAgent)
```

```cpp
#include <iiMultimodalAgent.h>
namespace agent = iiMultimodalAgent::agent;

agent::EngineOptions options;
options.sessionsDirectory = "/private/agent/sessions";
options.multimodal.outputDirectory = "/existing/generated/files";
options.multimodal.image = iiLocalDiffusion::NativeGenerationRequest{};
options.multimodal.image->modelPath = "/models/image-model.iildmodel";
options.multimodal.audio = hostLoadedAudioGenerator;
options.multimodal.speech = hostSpeechSynthesizer;
options.decision.valueUnit = "USD"; // Same unit as the trusted host estimates.
// Set trusted decision profiles/inputs and the host permission policy.
agent::Engine engine(hostModel, hostToolRegistry, hostPermissionPolicy, options);
```

Configured providers register `GenerateImage`, `GenerateAudio` and `Speak`.
`ServiceModel` adapts an existing iiLocalLLM `Service` to the agent `Model` interface;
hosts can also implement `Model` directly. `Api` gives each authenticated client
a private generated-media directory. Direct MCP hosts may add
`multimodalTools(services)` to their registry.

Default iiDecision policy defers work without trusted probability/value/cost
assessments. Permissions remain a separate requirement. Supply host profiles or
authenticated decision procedures; never invent economic estimates in model output.

Entrypoints are `iiMultimodalAgentD`, `iimagent-service`, `iimagent`, `iimagent-mcp`
(supported Unix hosts), and `iimagent-plugins`. Existing wire methods and persisted
schemas stay stable. Inference-only `iiLocalLLMD` and `iillm` remain in iiLocalLLM.
See [Migration](docs/Migration.md), [Multimodal services](docs/MultimodalServices.md),
[DecisionGate](docs/DecisionGate.md), [Agent harness](docs/AgentHarness.md), and
[Verification](docs/Verification.md).
