# LVUI 사용자 질문 화면 계약

LVUI 화면은 [System의 iiMultimodalAgent 시안](https://www.figma.com/design/PFMgQb3BVbrTbzPPsoWsRp/System?node-id=34-285)을
기준으로 한다. 뷰에는 질문 표시와 바인딩만 두고, 대기 요청·선택·직접 입력·메모와
전송 명령은 C++ 뷰모델이 소유한다. `NativeQuestionBridge`가 `QuestionInbox`의 변경
이벤트를 받아 해당 요청을 전달하고 답변 또는 거절 명령을 기존 검증 경로에 연결한다. 모델은 Qt,
LVUI, iiGUI와 다른 SDK 기능 모듈에 의존하지 않는다. 반응형 값과 명시적 쿼리는
iiSDKCore를 사용한다.

첫 요청이 동일하고 본문도 같으면 뒤에 요청이 추가되어도 현재 질문 객체와 초안을
유지한다. 현재 요청의 교체·취소·만료 또는 질문 본문 변경 시 이전 행은 퇴역하고
초안을 지운다. 퇴역 행의 늦은 명령은 새 요청에 영향을 주지 않는다. 단일 선택은
직접 입력과 상호 배타적이고 다중 선택은 함께 보낸다. 선택한 답은 원래 옵션 순서로
조합하며, 단일 선택이고 직접 입력이 없을 때만 그 선택지의 미리보기를 주석에 넣는다.
질문 문자열은 `__proto__`를 포함해 항상 데이터로 취급한다.
선택값을 관찰하는 콜백이 질문을 교체하면 퇴역한 행의 나머지 입력 갱신을 중단한다.
문자열 관찰자가 모델을 해제하거나 새 번역으로 재진입하면 이전 갱신은 중단한다.
쿼리 처리 도중 해당 쿼리 포트를 소유한 모델을 파괴하는 것은 허용되지 않으므로,
호스트는 쿼리가 반환된 뒤 모델을 해제해야 한다.

공유 라이브러리의 C 경계와 Python `QuestionModel`은 같은 뷰모델과 쿼리를 사용한다.
요청 스냅샷과 개별 변경을 명시적으로 전달하고, 질문·선택 상태를 조회해 답변을
편집하거나 호스트의 제출·거절 콜백을 실행한다. 행 세대는 현재 질문이 교체될 때
변경되며, 이전 세대의 입력은 새 요청에 적용하지 않고 거부한다. 뒤에 대기 요청만
추가되는 경우에는 현재 행과 입력을 보존한다. 관측 알림은 변경된 상태에만 반응하고
폴링이나 작업 스레드를 만들지 않는다. 모든 호출·콜백·모델 임대 해제는 소유
스레드에서 수행하고 임대는 생성 뷰와 진행 중인 쿼리보다 오래 유지한다.

설치 패키지의 `IIMULTIMODALAGENT_PYTHON_DIR`에서
`iiquestion_presentation.QuestionModel`을 가져올 수 있다. 공유 에이전트 라이브러리와
그 기존 런타임 의존성이 필요하며, 별도의 Qt 없는 에이전트 패키지를 설치한다는 뜻은
아니다. Core가 다른 검색 경로에 있으면 같은 ABI의 디렉터리를 `core_library_dir` 또는
`IIQUESTION_CORE_LIBRARY_DIR`로 지정한다. 생성 뷰와 같은 모델을 사용하는 네이티브
호스트는 `lease_model()`의 `pointer`와 임대를 함께 전달하고 뷰를 분리한 뒤 임대를
닫는다. Python 콜백 안에서는 임대 해제를 거부한다.

```python
from iiquestion_presentation import QuestionModel

with QuestionModel(core_library_dir="/path/to/matching/iiSDKCore/lib") as model:
    model.set_actions(lambda kind, request_id, answers, annotations:
                      submit_to_host(kind, request_id, answers, annotations))
    model.update([{"id": "request-1", "questions": [{
        "text": "Choose a renderer", "header": "Renderer", "multiple": False,
        "options": [{"label": "Native"}, {"label": "Other"}]}]}])
    view = model.snapshot()
    model.select(view["generation"], 0, 0, True)
    model.command("submit")
```

`update`는 대기열 전체의 명시적 스냅샷이고 `change`는 한 요청의 추가·수정·제거다.
`read_requests()`는 호출 시에만 대기열을 읽는다. `observe` 콜백은 속성, 행·선택지
인덱스와 현재 세대를 받는다. `snapshot()`에서 받은 세대를 `select`, `preview`,
`edit_other`, `edit_notes`에 전달해야 하며, 바뀐 세대는 거부된다. `submit`은
질문 문자열을 키로 하는 답변과 주석을 전달하고 `skip`은 같은 제출 콜백에 빈 답변을,
`decline`은 별도 거절 종류를 전달한다. 호스트가 수락하면 콜백은 `True`를 반환한다.
Python 콜백 오류는 Python이 시작한 바깥 호출에서 복귀할 때 다시 전달하며, 외부 C++
호스트가 시작한 호출의 오류는 쿼리가 반환된 뒤 `raise_callback_errors()`로 확인한다.
한 스냅샷은 최대 64개 요청, 각 요청은 최대 4개 질문, 각 질문은 최대 4개 선택지를
받으며, 모델의 기존 검증 조건도 그대로 적용된다.
질문과 답변은 유효한 UTF-8만 받는다. 선택지는 레이블을 접근성 이름으로 노출하고,
직접 답변·메모 입력에는 질문을 포함하는 개별 현지화 접근성 이름을 제공한다.
여러 질문이나 긴 선택지로 본문이 화면을 넘으면 시트 본문을 사용자 스크롤로 이동해
마지막 질문과 전송 버튼까지 접근할 수 있어야 한다. 스크롤은 명시적인 입력에서만
처리하며 이동 전 입력과 선택은 그대로 유지한다.
시트는 플랫폼에 따라 데스크톱 중앙형 또는 모바일 하단형을 사용하도록 명시한다.
Figma의 컴포넌트 기본 변형인 모바일 전용 표시를 모든 플랫폼에 강제하지 않는다.

값이 변경될 때만 관련 바인딩을 갱신한다. 큐 전체 재조회나 시간 기반 폴링은 하지
않으며, 개별 요청 변경은 해당 요청만 전달한다. 호스트는 최초 스냅샷과 이벤트 누락
복구를 명시적으로 전달한다. 질문 검증과
최종 요청 확정은 기존 `QuestionInbox`와 `userQuestionTool`이 계속 담당한다.

## 호스트 구성

앱은 `IIMULTIMODALAGENT_VIEWS_DIR/UserQuestionsSheet.lvui`를 LVUI로 AOT 생성하고
같은 모델을 브리지와 생성된 뷰의 `NativeQuestionBackend` 팩터리에 전달한다. 모델과
`QuestionInbox`, 브리지는 각각의 소유 스레드에서 사용하고, 팩터리와 뷰 수명 동안
모델을 유지한다. 브리지는 수신함을 소유하지 않는다. 수신함이 파괴되면 화면의 요청이
비워지며, 브리지가 파괴된 뒤의 늦은 제출은 실패한다. 언어 문자열은 호스트가
`setStrings()`로 제공할 수 있다.

```cpp
namespace view = iiMultimodalAgent::agent::presentation;
auto model = std::make_shared<view::NativeQuestionViewModel>();
view::NativeQuestionBridge bridge(inbox, model);
auto factories = lvui::native::BackendFactories{}
    .with<view::NativeQuestionBackend>([model] { return view::NativeQuestionBackend(model); });
// Pass factories to the generated UserQuestionsSheet::build at the app's mount.
```

사용자 Escape는 현재 요청만 거절한다. 요청 취소·만료 등으로 모델이 시트를 닫는
경우에는 새 요청을 거절하지 않는다. 레거시 QML은 `src/Legacy/qml`과 설치 패키지의
`IIMULTIMODALAGENT_LEGACY_QML_DIR`에 분리했다.

## 검증 범위와 남은 통합

2026-11-14 macOS에서 모델, `QuestionInbox` 브리지 및 AOT 생성된 뷰의 헤드리스
선택·입력·대기열·취소 검사를 수행했다. 별도 스테이징 패키지의 LVUI 파일과
설치된 C++ 모델을 함께 사용하는 소비자도 검사했다. 실제 앱이나 GUI 창은 실행하지 않았다.
Society와 Dreamscapes macOS 소스에는 수신함에 연결된 iiGUI 표면이 추가되어 있으며,
호스트 실패 시 레거시 화면으로 전환한다. Dreamscapes는 라이선스 상태도 확인한다.
다른 플랫폼과 실제 앱 창 표시의 검증은 남아 있다. 선택지의 포커스와
호버에 따른 미리보기 전환, 설명 영역 클릭과 긴 질문·설명의 폭 기반 줄바꿈은 macOS
헤드리스 검사에서 확인했다. 선택 컨트롤은 키보드와 접근성 활성화 경로를 제공한다.
2026-11-15 추가 검사에서는 데스크톱 중앙 배치와 360×260 창의 질문 4개를 검사했다.
가짜 수신함이 거절된 요청을 제거한 뒤 다음 요청을 열고, 휠 스크롤·이전 선택 보존·
전송을 확인했다. 소스 뷰와 설치 스테이징의 생성 뷰 모두 검사했으며 실제 창은 열지 않았다.
iiGUI Web 텍스트의 폭 기반 줄바꿈은 이후 소스에 추가되었다. WebAssembly와 모의
Canvas 검사에서 측정과 그리기의 줄이 일치했지만 실제 브라우저의 글꼴·레이아웃과
이 질문 화면의 Web 앱 마운트는 검증하지 않았다. 따라서 이 화면의 앱 전환이 끝났다고
보지 않는다.

구형 QML 질문 시트의 테스트와 그 안에 선언된 호스트는
`tests/Legacy/question_sheet_tests.cpp`에 격리했다. 기존 레거시 테스트 타깃과
하네스 검사 목록도 이 경로를 참조한다.
2026-11-15 변경된 경로로 다시 빌드한 질문 시트와 준비·정리 검사가 통과했다.

설치 스테이징의 에이전트 모델과 iiGUI 소비자는 `TaskToken` 레이아웃이 일치하는
Core 빌드로 재구성했으며, 검사 중 로드된 Core 라이브러리 경로를 확인했다. 현재
환경에는 같은 Core 버전 번호로 다른 ABI를 가진 설치본도 있으므로 혼용하면 안 된다.
이 확인은 다른 모델·네트워크 의존성 전체의 ABI 검증이나 실제 서비스 검증이 아니다.
실제로 다른 Core 헤더를 선택한 별도 모델 소비자는 실행 중 중단되었고, 모델과 같은
Core 패키지로 다시 구성해 해당 소비자 검사를 통과했다. 버전 번호가 같다는 이유만으로
서로 다른 Core 빌드를 결합할 수는 없다.

2026-11-15 C·Python 경계의 상태·부분 변경·대기열·선택·답변·거절·오류·스레드·
행 세대와 임대 수명을 검사했다. 실제 설치 스테이징의 C·Python 소비자와 같은 C 모델을
연결한 AOT 생성 LVUI의 헤드리스 소비자도 통과했다. 처음에는 현재 LVUI 컴파일러와
이전 설치 헤더를 섞어 컴파일이 실패했으며, 같은 스테이징의 컴파일러·헤더로 다시
구성해 통과했다. 모델과 C 경계만 Core에 연결한 별도 테스트에 AddressSanitizer와
UndefinedBehaviorSanitizer를 적용했지만 Core 라이브러리는 계측하지 않았다. C 경계의
Windows x86-64 객체 교차 컴파일은 통과했으며 Windows 링크·실행과 실제 앱 창은
검증하지 않았다. 시스템 설치본은 갱신하지 않았다.

소스 트리의 LVUI 소비자도 위 설치 소비자와 같은 C 경계의 헤더와 구현을 함께
빌드하도록 고쳤다. macOS 헤드리스 검사가 통과했고, 같은 소스 뷰와 현재 LVUI·iiGUI·
Core를 iPhoneOS 18 arm64 소비자로 AOT 컴파일하고 링크했다. 이 iOS 검사는 전체
에이전트 라이브러리나 Society 앱이 아니라 질문 모델과 C 경계를 따로 조합한 것이다.
기기 실행, 네트워크 서비스 및 실제 입력·화면 표시는 검증하지 않았다.
동일한 소스 소비자는 Android NDK 30 arm64/API 24에서도 AOT 컴파일하고 링크했다.
Android 앱 패키지나 기기는 실행하지 않았고 전체 에이전트 패키지의 Android 링크를
검증한 것은 아니다.
iOS와 Android 소비자 모두 생성된 질문 뷰와 iiGUI 호스트 마운트 코드가 같이
링크되는 것도 확인했다. 실제 화면 마운트 및 입력 검사는 별도로 남아 있다.

생성된 질문 화면을 외부 소유 `iiGUI::PaintView`와 테스트용 프레임 장치로
구성해 질문 상태와 건너뛰기 입력이 모델에 전달되고, 호스트 요청에 따라
그려지며 종료 시 뷰 수명이 해제되는지 검사했다. 테스트 장치는 실제 GPU나
운영체제 표시 표면이 아니다.
