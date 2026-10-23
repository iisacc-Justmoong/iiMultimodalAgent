# Package extraction verification (2026-10-20)

This record qualifies iiMultimodalAgent 0.1.0 and iiLocalLLM 0.56.0 on macOS
arm64 with AppleClang 21, C++23, Qt 6.8.3 Core/Network and the existing native
SDK dependency packages. Every build directory is named `build/`.

The detailed evidence root is
`/Volumes/Storage/Workspace/SDK/build/agent-extraction-20261020/`.
Installed-package consumers use fresh prefixes under that root's `staging/`;
the user's global SDK installations and shell profiles are unchanged.

## Completed evidence

| Check | Result | Evidence in the report root |
| --- | --- | --- |
| Module ownership, install discovery and acyclic dependency tests | 5/5 passed | `module-boundary-clean-final.log` |
| iiLocalLLM final Release build | Passed | `inference-build-clean-final.log` |
| iiMultimodalAgent final Release build | Passed | `agent-build-clean-final.log` |
| Independent inference source suite, including native GGUF and MLX GPU/CPU | 21/21 passed | `inference-source-final.xml` |
| Installed inference consumer, with Decision/Agent packages disabled | 2/2 passed | `inference-consumer-final.xml` |
| Installed agent consumers | 10/10 passed | `agent-consumer-final.xml` |
| Installed public header closure | 53 headers, no missing owned includes | `package-ownership-audit.json` |
| Focused generated-media integration after host-fixture correction | 1/1 passed | `multimodal-regression.xml` |

The harness qualification covers **100 unique CTest entries out of 131 registered**:
96 base checks (including real vision/audio acceptance) and four additional native
checks for speech, local tools, decision admission and MCP. All 100 have passing
final results. The initial base run was 94/96; the two affected entries were
resolved through the documented host fixture correction and MCP test timing/state
corrections. The final complete MCP server test then passed three consecutive
runs. The remaining 31 optional model-quality checks were not run here.

`verification-report.json` records every latest result and retains the initial
failed runs rather than presenting a synthetic clean first run. See
`agent-source-contracts-final.xml`, `agent-native-closure.xml`,
`agent-mcp-native-final.xml` and `mcp-server-state-final.xml` for their boundaries.

Actual native input acceptance used llama.cpp/Metal: the red image returned
`Red`, ordered frames reached the video-capable route, and speech recognition
returned `The sky is blue.` Media token counts, unsupported modalities and the
aggregate decoded-image budget were checked. Generated image/audio outputs use
the separate provider-boundary tests described below.

## What the installed consumers execute

They compare symbol-owner paths for the selected iiFileProviderNative,
iiLocalLLM, iiLocalDiffusion, iiAudioGen, iiVoiceOver and iiMultimodalAgent packages;
verify that an ordinary agent bootstrap does not load the image backend; exercise
host decision admission, typed modality routing, generated media in the next model
turn, authenticated client output isolation, session-history/fingerprint recovery,
memory and an application-owned tool/session ABI.

The native Speak tool synthesizes `Hello World` with the installed bundled
Piper/ONNX voice and returns 17,920 PCM frames as a provider-created WAV with inline
media. This is actual model inference; the test does not play sound on the user's
audio device.

Image publication is tested with a deterministic host native executor and a
valid RGB PNG that is decompressed and compared. The default native adapter also
calls the actual iiLocalDiffusion entrypoint with an unavailable model: the test
requires its RuntimeFailure, rejecting adapter-loader RuntimeUnavailable. This
proves the installed native bridge, not successful trained-model image generation.
Audio publication is tested through an injected iiAudioGen Backend with validated
float32 PCM, duration/size limits, WAV delivery and cancellation. A full trained
Diffusers image/audio generation sweep is not part of this qualification.

## Reproducibility and retained diagnostics

CTest and both installers clear DYLD library/framework/fallback/versioned
overrides only inside their processes. A login-shell SDK override previously
selected an older bundled iiFileProviderNative despite correct imported CMake
paths. Actual runtime symbol-path checks now verify the selected prefixes.
The login shell also exposes image SDK headers through CPATH. Their four native
headers were byte-identical to the selected package; a separate installed app
consumer was rebuilt without compiler/library environment overrides and confirmed
that its dependency file used the selected installed headers. See
`image-header-identity.json` and `consumer-isolated-header-tests.xml`.
The macOS link additionally marks the chosen shared Native provider as needed
before indirect SDK libraries.

Initial generated-media Engine/API tests omitted the required host value unit.
The decision policy correctly rejected them; tests now supply USD consistently
with their trusted estimates. No admission rule was weakened. Initial failures
and the focused rerun are retained. MCP test helpers now allow ten seconds for
provider-backed state requests and poll the child's actual idle state instead of
assuming completion inside a single 1.5-second wait. Exact response cardinality,
connection ownership and permission assertions remain. The external official MCP
1.26.0 oracle cold-imported in 24.94 seconds on this storage; its model acceptance
fixture explicitly allows a 60-second initialize, including the configured-peer
path. Production's ten-second initialize default is unchanged. The final native
model/MCP acceptance passed, and the complete MCP server test passed three times.
Early MLX startup timeouts are likewise
retained; the final complete inference suite passed both MLX GPU and CPU tests.

## Boundaries

This extraction preserves the inherited Qt Core/Network harness. It adds no
QtGui dependency and does not claim a Qt-free rewrite. iiDecision remains an
independent SDK and its integration/routing belongs to the agent.
The image adapter and Piper/ONNX speech run in native code. iiAudioGen's supplied
Diffusers implementation uses an explicit host-selected Python worker; its
Backend interface permits native implementations without an agent API change.
The current video input contract is supplied ordered timestamped frames; it does
not decode video containers or generate video.

Windows and Linux metadata/branches are carried into the new module, but their
binaries were not executed in this macOS qualification. Existing optional model
quality sweeps are not implied by the focused results. Source changes are local;
no remote repository or publication is fabricated.

## 설치본의 외장 스토리지 검사

MemoryDream 고정 모델 검사는 자식 프로세스 시작과 파일 게시·비동기 완료를 최대 30초 기다린다. 전체 검사 상한은 240초이다. 모델 실행 예산과 의도한 시간 초과 입력은 변경하지 않으며, 실제 잠금·죽은 보유자 복구·저장 내용·상태 검증을 그대로 수행한다. 설치본 초기 로드와 외장 스토리지 I/O가 3초·5초의 기존 하네스 대기를 넘긴 사례를 재현한 뒤 적용했다.
