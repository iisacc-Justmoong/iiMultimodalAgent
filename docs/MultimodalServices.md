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

`GenerateImage` accepts the optional `model` ID. `default` (also used when the
argument is absent) selects `MultimodalServices::image->modelPath`; other IDs
must be present in the host's `imageModels` map. IDs are case-sensitive ASCII
letters, digits, `.`, `_`, or `-`, at most 64 bytes. `default` is reserved and
the host may register at most 128 additional IDs. Unregistered IDs and arbitrary
paths are rejected. The tool schema advertises only these IDs. Native and
host-injected image executors receive the selected host-owned path.

On each admitted invocation, the agent reads bounded metadata from the selected
local package itself. Supported metadata sources are `model_index.json`,
`config.json`, `transformer/config.json` and `generation_config.json` inside a
model directory, Safetensors metadata and the corresponding embedded assets in
an iiLocalDiffusion Safetensors package, or a stored `.iildmodel` ZIP/ZIP64
manifest and its first-stage Safetensors or GGUF metadata. Standalone GGUF
metadata is also inspected. Directory metadata redirected
outside the model directory is ignored. External sidecars, filenames, and
unrelated description text do not identify a model. Inspection does not extract,
hash or load weights. Compressed archives and other unsupported formats are
not currently inspected; they retain host defaults unless the request overrides
them. An unreadable, unrecognized, malformed or oversized metadata source is
ignored, leaving model validation to the executor.

The precedence for each supplied field is request argument, valid package
generation setting, recognized model-family default, then the host request
default. `generation_config.json` or the embedded `generation_config`,
`generation_parameters`, or `iild.generation_parameters` JSON object can
specify `num_inference_steps` (or `steps`), `width`, and `height`. A package
step must be 1–150; dimensions must fit the host maximum and the 8-pixel grid.
Invalid hints are ignored. Other backend-specific parameters are not changed.
The built-in family defaults are 8 steps for MOSS-Image and 52 for Krea2. For
MOSS-Image, this requested quick setting also applies to recognized non-distilled
models; the [upstream Base guide](https://github.com/OpenMOSS/MOSS-Image/blob/main/USERGUIDE.md)
recommends at least 50 steps for best results. Callers can set `steps: 50` or
choose a package that declares a different default. The result includes the
selected ID, actual `steps`, `steps_source` (`request`, `package`, `model_family`,
or `host`) and, when recognized, `model_family`.

These defaults do not establish that the configured image executor supports the
identified architecture. In particular, detecting MOSS-Image does not itself add
MOSS-Image inference to iiLocalDiffusion. A compatible host executor or backend
must provide that capability. Rebuild C++ consumers after the `MultimodalServices`
layout change.

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
