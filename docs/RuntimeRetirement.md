# 폐기한 모델 런타임

2026-10-27부터 ONNX·ONNX Runtime을 빌드·모델 실행·설치·배포 경로에서 제거한다.
선택적 설치나 자동 다운로드 경로도 제공하지 않는다. 과거 검증 기록은 현재 지원
선언이 아니며, 해당 모델을 지원하는 것으로 간주하지 않는다.

Windows의 구형 ONNX 런타임 배포 분기를 제거한다. 음성 합성 공급자가 없으면
`Speak` 도구를 등록하지 않는다. `iiMultimodalAgent.speech_backend_retired`가 폐기한
음성 모델의 명시적 실패와 도구 미노출을 확인한다.

`runtime_bindings`와 `runtime_bootstrap` 검사는 선택한 SDK 라이브러리가 실제로
로드되는지 확인한다. 과거 설치본과 같은 이름의 라이브러리가 다른 백엔드 디렉터리에
있더라도 선택한 패키지를 우선해야 한다.

iiDecision·iiLocalDiffusion·iiVoiceOver의 설치 갱신은 해당 SDK 접두사에 남은 구형
ONNX 공유 라이브러리를 제거한다. 제품 공통 배포 정책은 ONNX 런타임, 기존 Piper
바이너리와 OpenCV DNN의 재포함을 거부한다. 폐기한 모델 기능이 필요하면 지원되는
네이티브 API와 교체 가능한 iiSDK 인터페이스에 별도 구현을 연결해야 한다.
