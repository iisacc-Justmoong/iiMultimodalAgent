# Multimodal providers and generated media

`EngineOptions::multimodal` binds independent image, audio and speech providers.
The same Tool/ToolRegistry contracts serve embedded Engine, subagents and MCP.
The authenticated Api assigns each client a generated-media directory below its
private state root. Model pointers/paths/runtimes remain trusted host configuration.

| Tool | Actual provider call | Published result |
| --- | --- | --- |
| GenerateImage | `iiLocalDiffusion::generateNativeImageWithProgress` or host native executor | RGB PNG, dimensions and metadata |
| GenerateAudio | Loaded `iiAudioGen::Generator::generate` | Validated float32 PCM as PCM16 WAV |
| Speak | `iiVoiceOver::SpeechSynthesizer::synthesize` | Native Piper/ONNX speech as PCM16 WAV |

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
Tool arguments contain prompts and bounded settings, never a model, executable or
output path. Default image maximum is 2048 pixels per edge on an 8-pixel grid;
prompts contain 1..4096 UTF-8 bytes without NUL. The host audio ceiling defaults to
30 seconds and artifact budget to 32 MiB. RGB length, PCM format/alignment,
finite samples and actual output budgets are checked before publication.
Set a nonempty host `decision.valueUnit` using the same unit as its trusted
profiles/input estimates. A missing unit is invalid configuration, even when
numeric probability/value profiles were supplied.

Files are published exclusively via iiFileProvider and participate in its metadata
and preview pipeline. PNG encoding uses the existing QtCore zlib utility plus a
small PNG container writer, without QtGui. WAV publication reuses iiVoiceOver's
iiFileProvider writer. Existing output files are never replaced.

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
and a separate real bundled-voice Hello World smoke.

## Explicit daemon/MCP configuration

The daemon accepts `--agent-multimodal /private/providers.json`; MCP accepts
`--multimodal-config /private/providers.json` together with its agent `--model`.
The file follows the existing private-file rules and must be outside the tool
workspace. The daemon also requires its ordinary workspace/state/credential and
decision configuration. MCP stores generated media under its owned session root;
the authenticated Api uses separate client directories.

```json
{
  "image": { "model": "/models/image-model.iildmodel" },
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
are rejected. `agent.info` exposes configured generation capabilities; actual
input-model capability filtering still belongs to iiDecision routing.
