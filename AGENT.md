# iiMultimodalAgent 작업 안내

런타임과 도구 오케스트레이션은 `src/Agent/`에 있다.
추론 런타임과 모델 소유권은 iiLocalLLM에 두며 중복 로더를 만들지 않는다.
공통 실패 진단은 `src/Diagnostics/FailureReport.h`에 있다.
이 계층은 C++23 표준 라이브러리만 사용하며 Qt와 I/O에 의존하지 않는다.
확인한 사실과 추정을 구분하고 원문 오류를 보존한다.

빌드 산출물은 `build/`에 둔다. API 변경에는 문서와 관련 테스트를 함께 갱신한다.
실패 보고서 검사는 `iiMultimodalAgentFailureReportTests`와 CTest의
`iiMultimodalAgent.failure_report`이다. `example/failure_report.cpp`는
모델이나 외부 서비스 없이 실행할 수 있는 최소 소비 예이다.
전체 에이전트 테스트와 실제 모델 추론 검증은 구분하여 보고한다.

이미지 모델 선택은 호스트가 등록한 ID에 한정한다. 자동 생성 기본값은 선택한
패키지 내부 메타데이터에서 읽으며, 요청에서 명시한 값이 우선한다. 형식과
한계는 [docs/MultimodalServices.md](docs/MultimodalServices.md)를 따른다.

로컬 RAG의 자료 범위, 출처·리비전, 호스트 인가, 선택적 iiDecision 판정과
검증 한계는 [docs/Rag.md](docs/Rag.md)를 따른다. 기본 어휘 검색 외에 명시적으로
구성한 임베딩 의미·결합 검색을 제공한다. 자막·메타데이터를 원본 이미지 추론으로
취급하지 않는다.

<!-- workspace-docs:agent:start -->
## 문서 구조와 에이전트 탐색

- 프로젝트 루트의 `README.md`는 사람용 영문 진입 문서이며 `AGENT.md`는 에이전트 진입점이다.
- 하위 인간용 Markdown과 구조화된 agent JSON을 함께 유지한다. Markdown이 원본이며 JSON은 원문 해시와 줄 범위를 가진 파생 문서이다.
- 먼저 [docs/agent-index.json](docs/agent-index.json)에서 관련 문서와 JSON 투영을 찾는다. 원문 SHA-256이 다르면 현재 Markdown을 읽고 투영을 갱신한다.
- JSON의 규칙 후보는 검색용 어휘 일치 결과이다. 과거 기록·예제·실패 상태를 현재 지시나 검증 완료로 자동 해석하지 않는다.
- 기존 프로젝트별 규칙, 동결된 증거, 라이선스, 외부 원본, 사용자 데이터와 다른 작업의 변경을 보존한다.
- 이 문서 재편 지침은 과거 작업의 기능 구현·배포를 자동 재개하라는 요청이 아니다. 현재 사용자의 작업 범위를 우선한다.
- 문서를 수정하면 JSON 쌍·색인을 함께 갱신하고 검증한다. 기존 소스 변경에 대한 빌드·테스트 규칙은 계속 따른다.

Workspace에서 이 프로젝트의 문서를 갱신하는 명령은 다음과 같다.

```sh
python3 ../../Engineering/WorkspaceDocumentation/App/workspace_docs.py --workspace ../.. sync --project SDK/iiMultimodalAgent
```
<!-- workspace-docs:agent:end -->
