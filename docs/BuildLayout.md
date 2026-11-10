# 빌드와 테스트 자원 배치

컴파일 결과는 `build/`에 둔다. jsoncons, md4c, libyaml, Lexbor, tree-sitter 소스는 `resources/dependencies/`에 둔다. 이미 있는 소스는 그대로 재사용하고 불완전한 디렉터리는 덮어쓰지 않고 구성을 중단한다. 아직 없는 의존성은 기존의 고정 URL과 해시를 사용하는 CMake FetchContent가 준비한다.

CTest 출력과 임시 파일은 `resources/.state/`에 구성마다 고유한 경로로 두고 CTest fixture가 실행 전 준비하며 완료 후 정리한다. 한 구성에서 여러 CTest 프로세스를 동시에 실행하지 않는다. 하나의 CTest 실행 내 병렬 테스트는 지원한다. 강제 종료로 남은 작업 공간은 소유 여부를 확인한 뒤 정리한다.

네이티브 모델과 실제 에이전트·앱 실행은 단위 테스트 및 컴파일 검증과 별도로 기록한다.

공식 MCP 검증을 위한 외부 Python 환경의 바이트코드 준비는 기본 빌드에 포함되지 않는다. 필요한 경우 `iiMultimodalAgentMcpDependencyArtifacts` 타깃을 명시적으로 선택한다.
