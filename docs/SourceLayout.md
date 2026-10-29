# 소스 구조

작성한 공개 헤더와 구현은 `src/` 아래 책임별 디렉터리에 함께 둔다. 공통 API는 `API/`, 에이전트 런타임은 `Agent/Runtime/`, 메모리·세션·권한·도구·연동·확장 등은 `Agent/`의 각 하위 디렉터리에 둔다. MCP 전송은 `MCP/`, 이미지 로딩 내부 계약은 `Media/Detail/`, 실패 진단은 `Diagnostics/`에 둔다.

빌드 중에는 기존 `<iiMultimodalAgent/agent/...>`, `<iiMultimodalAgent/mcp/...>` 등 공개 include 경로를 생성 헤더로 연결하며, 설치 시에는 원본 공개 헤더를 기존 경로에 배치한다. `iiMultimodalAgent.source_layout`은 소스 분류와 중복 디렉터리 재발을 검사한다. 이 구조 변경 자체는 모델 추론이나 플랫폼 지원을 검증하지 않는다.
