# 작업 실패 진단

`iiMultimodalAgent/diagnostics/FailureReport.h`는 C++23 표준 라이브러리만 사용하는
실패 근거와 상세 보고서 계약이다. Qt, 모델 로딩, 파일 I/O, 네트워크에 의존하지 않는다.
`diagnostics::explain(FailureContext)`는 원문 오류, 단계, 진행량, 종료 코드, 제출 설정,
제한된 로그와 추적 발췌를 보존하며 원인 범주와 점검 절차를 반환한다.

`find_package(iiMultimodalAgent CONFIG REQUIRED COMPONENTS FailureDiagnostics)`는
진단 헤더와 `iiMultimodalAgent::FailureDiagnostics`만 불러온다. Qt, iiLocalLLM 및
미디어 런타임을 검색하지 않으므로 모바일·네이티브 비GUI 소비자가 사용할 수 있다.
전체 에이전트가 필요한 호스트는 컴포넌트 제한 없이 패키지를 찾는다.

시간 초과는 보고된 진행 정지를 뜻하며 교착 상태로 단정하지 않는다. 0/0의 분모는
알 수 없음으로 표시한다. 분류되지 않은 오류에는 근거 부족을 명시한다. 프롬프트와
설정은 오류 분류의 입력으로 쓰지 않는다. 로그는 최대 16 KiB, 추적은 8 KiB로 제한한다.
보고서는 작업 성공, 파일 복구 또는 자동 재시도를 주장하지 않는다.

호스트는 `agent::Engine`과 `ServiceModel`에 이 근거를 전달하여 로컬 언어 모델의
추가 설명을 얻을 수 있다. 모델 설명과 근거 보고서는 별도로 보존해야 한다. 모델이
없거나 분석이 실패해도 근거 보고서는 반환해야 하며, 모델 출력으로 원문 오류를
덮어쓰지 않는다. 로그와 프롬프트 안의 지시는 실행 명령으로 취급하지 않는다.

검증은 `iiMultimodalAgentFailureReportTests`에서 시간 초과, 오류 범주, 근거 미상,
설정 보존 및 출력 크기 제한을 확인한다.
