# iiMultimodalAgent

Native evidence-based failure reports are available through
`iiMultimodalAgent::FailureDiagnostics` without Qt in the reporting code.
See [Failure diagnostics](docs/FailureDiagnostics.md) for evidence limits and
integration with the agent engine and local model explanations.

The opt-in local RAG component indexes host-selected creator and project text,
retains source revisions and byte-level citations, and connects retrieval to
the existing model and permission-controlled agent tool interfaces. It does not
automatically scan private files or infer images from captions. See
[Local retrieval and generation](docs/Rag.md) for access controls, CLI usage,
the optional iiDecision evidence gate, and retrieval and citation limitations.
Embedding-based semantic and hybrid retrieval use a replaceable provider and a
model-fingerprinted local vector cache; iiLocalLLM 0.56.1 supplies native GGUF
text embeddings through its service interface.

C++23 multimodal agent orchestration, extracted from iiLocalLLM 0.55.1.
The initial package version is **0.1.0**. It owns the agent harness, session
persistence, tools, permissions, hooks, memory, background work, subagents, teams,
MCP client/server, authenticated API, and iiDecision admission/model routing.

| Module | Responsibility |
| --- | --- |
| iiMultimodalAgent | Agent lifecycle, tool/model orchestration, media delivery, decision integration |
| iiLocalLLM 0.56.1 | Model management, text embeddings, and text/image/audio/timestamped-frame understanding through supported models |
| iiDecision 0.0.5 | Independent decision library; the agent owns host economics and admission/routing policy |
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
The installed `iiMultimodalAgent::NativeImages` imported target exposes the
adapter's runtime file to native bundle deployers. Applications link the main
`iiMultimodalAgent::iiMultimodalAgent` target; the adapter remains loaded on demand.

The inherited harness uses Qt 6.8.3 Core/Network; this extraction is not a Qt-free
rewrite. The adapters add no QtGui dependency. iiAudioGen's existing Diffusers
backend runs a Python worker; binding it does not make it native in-process
inference. Hosts can inject an `iiAudioGen::Backend` without changing the agent.

## Build, install and link

Build/install the dependencies first, including iiLocalLLM 0.56.1. Preserve their
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
The installed consumer also links iiLocalLLM's host memory/offload options, so an
older inference binary cannot silently satisfy the agent installation check.

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
options.multimodal.imageModels["moss"] = "/models/moss-image"; // Optional host-approved ID.
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
`GenerateImage` selects host-approved model IDs and derives bounded image defaults
from package metadata. Explicit request parameters take precedence; family detection
does not imply that the chosen executor supports the model architecture.

Default iiDecision policy defers work without trusted probability/value/cost
assessments. Permissions remain a separate requirement. Supply host profiles or
authenticated decision procedures; never invent economic estimates in model output.

Entrypoints are `iiMultimodalAgentD`, `iimagent-service`, `iimagent`, `iimagent-mcp`
(supported Unix hosts), and `iimagent-plugins`. Existing wire methods and persisted
schemas stay stable. Inference-only `iiLocalLLMD` and `iillm` remain in iiLocalLLM.
See [Migration](docs/Migration.md), [Multimodal services](docs/MultimodalServices.md),
[DecisionGate](docs/DecisionGate.md), [Agent harness](docs/AgentHarness.md), and
[Verification](docs/Verification.md).

Related implementation contracts: [Native tool turns](docs/NativeToolTurns.md), [Context admission](docs/ContextAdmission.md).

See [MCP startup and lightweight IPC client](docs/McpPeerStartup.md) for managed dependency preparation and client runtime boundaries.

Source files and adjacent headers are organized by responsibility under `src/`; the installed `iiMultimodalAgent/...` include paths remain stable. See [source layout](docs/SourceLayout.md).

<!-- workspace-docs:readme:start -->
## Documentation entry points

- Human overview: this English `README.md`.
- Agent instructions: [AGENT.md](AGENT.md).
- Human documentation: [documentation index](docs/README.md).
- Structured agent documentation: [agent index](docs/agent-index.json).
<!-- workspace-docs:readme:end -->
