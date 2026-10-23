# iiMultimodalAgent 작업 안내

런타임과 도구 오케스트레이션은 `src/iiMultimodalAgent/agent/`에 있다.
추론 런타임과 모델 소유권은 iiLocalLLM에 두며 중복 로더를 만들지 않는다.
공통 실패 진단은 `src/iiMultimodalAgent/diagnostics/FailureReport.h`에 있다.
이 계층은 C++23 표준 라이브러리만 사용하며 Qt와 I/O에 의존하지 않는다.
확인한 사실과 추정을 구분하고 원문 오류를 보존한다.

빌드 산출물은 `build/`에 둔다. API 변경에는 문서와 관련 테스트를 함께 갱신한다.
실패 보고서 검사는 `iiMultimodalAgentFailureReportTests`와 CTest의
`iiMultimodalAgent.failure_report`이다. `example/failure_report.cpp`는
모델이나 외부 서비스 없이 실행할 수 있는 최소 소비 예이다.
전체 에이전트 테스트와 실제 모델 추론 검증은 구분하여 보고한다.
