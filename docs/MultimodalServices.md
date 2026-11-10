# Multimodal providers and generated media

`EngineOptions::multimodal` binds independent image, audio and speech providers.
The same Tool/ToolRegistry contracts serve embedded Engine, subagents and MCP.
The authenticated Api assigns each client a generated-media directory below its
private state root. Model pointers/paths/runtimes remain trusted host configuration.

| Tool | Actual provider call | Published result |
| --- | --- | --- |
| GenerateImage | `iiLocalDiffusion::generateNativeImageWithProgress` or host native executor | RGB PNG, dimensions and metadata |
| GenerateAudio | Loaded `iiAudioGen::Generator::generate` | Validated float32 PCM as PCM16 WAV |
| Speak | `iiVoiceOver::SpeechSynthesizer::synthesize` | PCM16 WAV when a supported native voice backend is available; the previous backend is retired |

The built-in image call crosses a small private native adapter loaded from beside
the agent DLL/shared library. It is built/installed with the package and retained
for the process lifetime so backend caches stay valid. Its first load happens
inside the admitted tool call, avoiding the image/tensor library startup cost for
text-only agents, denied actions and host-supplied image executors. The adapter
cannot be selected by a model, environment path or wire argument. A missing or
incompatible adapter reports RuntimeUnavailable; backend model errors report
RuntimeFailure. The native model-error regression distinguishes those cases.

Only configured providers advertise tools. Model/backend errors fail the call;
there is no fabricated output or implicit model download/runtime fallback.
iiAudioGen's supplied Diffusers backend runs a persistent Python worker. Injecting
a native Backend can replace it without changing the agent interface. This package
does not claim to implement a new native audio model engine.

Text, image, audio and increasing timestamped-frame understanding retain the
existing iiLocalLLM typed media/projector path. Frames do not imply video container
decoding or video generation. Generated media can enter the next model turn when
the chosen model supports it; iiDecision capability filtering and host economics
still select routes. Media/model output cannot invent economic assessments.

Generation uses ordinary tool permissions, plan edit restrictions, hooks,
session commits and Engine decision admission. Missing host economics defer work.
Tool arguments contain prompts, bounded settings and an optional host-registered
image model ID, never a model path, executable or output path. Default image
maximum is 2048 pixels per edge on an 8-pixel grid;
prompts contain 1..4096 UTF-8 bytes without NUL. The host audio ceiling defaults to
30 seconds and artifact budget to 32 MiB. RGB length, PCM format/alignment,
finite samples and actual output budgets are checked before publication.
Set a nonempty host `decision.valueUnit` using the same unit as its trusted
profiles/input estimates. A missing unit is invalid configuration, even when
numeric probability/value profiles were supplied.

## Image model selection and package defaults

The image tool preserves every explicit generation field supplied by the host
or tool call. Tool-call fields take precedence over host fields. A C++ host
marks width, height or steps as unspecified with zero; the CLI and MCP host
configuration use that form by default. The selected backend resolves only
those unspecified fields; the iiLocalDiffusion adapter uses its own model
defaults. The CLI and MCP host take their initial seed from iiLocalDiffusion's
shared default catalog; a C++ host retains the seed in its request. Explicit
width, height and steps must be valid and cannot be zero. The host's
maximum image dimension remains a rejection limit, not a resize instruction.
The tool accepts the backend's step range of 1–1000 and signed 64-bit seeds;
negative seeds request backend-selected randomness. The exact prompt is passed
without trimming. Hooks and permission responses may deny a generation call,
but an attempt to rewrite its arguments is rejected before execution.

`GenerateImage` accepts the optional `model` ID. `default` (also used when the
argument is absent) selects `MultimodalServices::image->modelPath`; other IDs
must be present in the host's `imageModels` map. IDs are case-sensitive ASCII
letters, digits, `.`, `_`, or `-`, at most 64 bytes. `default` is reserved and
the host may register at most 128 additional IDs. Unregistered IDs and arbitrary
paths are rejected. The tool schema advertises only these IDs. Native and
host-injected image executors receive the selected host-owned path.

The agent does not inspect model packages to invent family or package defaults.
An unsupported model still fails in the selected backend. Nonzero fields in a
default-constructed `NativeGenerationRequest` are host values: set a field to
zero explicitly when the host wants backend selection instead. The existing C++
request layout and legacy `imageExecutor` remain compatible.
Build the agent against an iiLocalDiffusion package that provides
`generateNativeImageWithReport` and `resolveNativeImageDefaults`; the selected
CMake package supplies the matching headers and library.

The result's `generation_settings` object records requested values and their
sources (`request`, `host`, or `backend_default`), the resolved request and one
record per executed model stage. The iiLocalDiffusion engine reports its
resolved seed, aligned base and canvas sizes, sampler, scheduler, CFG, Hires,
the effective sigma schedules for base and Hires, image-to-image strength,
negative prompt, selected LoRAs and Q8 cache use; final output dimensions are recorded
separately. Thus a `-1` seed is retained as the request while the random seed
chosen by the engine is recorded in its stage. The same object is embedded in
the PNG's uncompressed `iTXt` entry named `iiSDK.Generation`. It contains the
prompt and negative prompt and travels with the image. Model and LoRA paths in
the embedded report are restricted to their file names.

The legacy `imageExecutor` does not return applied settings and is labeled
`unreported`; the agent never claims its inputs are the settings actually
used. Hosts that can report them may use the `multimodalTools` overload taking
`ReportingImageExecutor`. Its returned `NativeImageExecution` must include a
resolved request and at least one resolved stage. The agent rejects a report
that changes explicit request dimensions, steps, seed, prompt or model. Engine
sampling effects and output quality still depend on the selected model and
backend; a report is not a proof of model support or output quality.

Files are published exclusively via iiFileProvider and participate in its metadata
and preview pipeline. PNG encoding uses the existing QtCore zlib utility plus a
small PNG container writer, without QtGui. WAV encoding uses iiVoiceOver's
in-memory `waveBytes()` and the agent publishes those bytes exclusively through
iiFileProvider. Existing output files are never replaced.

Inline MCP-compatible content defaults to at most 512 KiB of binary media.
Larger files return structured metadata/path and `inline_media:false`. Raising
that ceiling also requires suitable host/model context limits. `inlineMedia=false`
always returns a file and metadata.

Cancellation is checked before dispatch and publication. Diffusion bridges to its
native atomic flag and forwards progress; speech bridges to `std::stop_token`,
allowing an in-flight ONNX chunk to return. The current audio Backend interface
has no cancellation input: an active call must finish, after which cancellation
discards the result without publishing. This is not immediate backend interruption.
Calls within one generated tool batch are serialized. Hosts sharing backends across
independent batches must supply the backend's required thread-safety policy.

Tests cover PNG decompression, PCM/WAV delivery, permissions, path injection,
cancellation without publication, malformed backend output, actual native
diffusion model-load failure, media reaching the next model turn under iiDecision,
and a historical bundled-voice Hello World smoke. The retired model is not
available in the current voice SDK.

## Independent voice package integration (2026-10-28)

On macOS arm64, the agent and its test targets were built against the separately
staged iiVoiceOver package. The `runtime_bindings` and `voice_pcm_boundary`
tests passed: the former checked the loaded paths of the selected SDK binaries,
including that staged voice library; the latter used a synthetic audio backend,
checked that the provider-published WAV matched the inline bytes, and rejected
non-finite output. The test-owned directory was removed. No model inference,
audio playback, GPU work, or GUI application was run.

The build reported an existing runtime search-path ordering cycle, duplicate
RPATH entries, and an MLX library built for macOS 27.0.1 while the link target
was 27.0. The runtime binding check passed on this macOS 27.0.1 host; it does
not establish compatibility on macOS 27.0 or on other platforms. The agent
still directly depends on several SDK modules and has not yet been migrated
to the workspace-wide independence rule.

## Explicit daemon/MCP configuration

The daemon accepts `--agent-multimodal /private/providers.json`; MCP accepts
`--multimodal-config /private/providers.json` together with its agent `--model`.
The file follows the existing private-file rules and must be outside the tool
workspace. The daemon also requires its ordinary workspace/state/credential and
decision configuration. MCP stores generated media under its owned session root;
the authenticated Api uses separate client directories.

```json
{
  "image": {
    "model": "/models/image-model.iildmodel",
    "models": { "moss": "/models/moss-image" }
  },
  "speech": { "bundled": true }
}
```

Custom speech uses `model`, optional `configuration`, and `phonemizer_data` in
place of `bundled`. To bind the existing iiAudioGen Python implementation, the
host must explicitly provide:

```json
{
  "audio": {
    "backend": "diffusers",
    "model": "/models/local-audioldm",
    "python": "/existing/audio-runtime/bin/python",
    "device": "cpu"
  }
}
```

Paths must exist. No Python backend is silently selected. Native custom audio
backends are supplied through the C++ `iiAudioGen::Backend` interface. Unknown
configuration fields, arbitrary output destinations and wire model-path arguments
are rejected. For example, a tool call can use `{"prompt":"a poster","model":"moss"}`.
`agent.info` exposes configured generation capabilities; actual
input-model capability filtering still belongs to iiDecision routing.
