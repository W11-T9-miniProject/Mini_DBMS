# MiniDB Studio — 브라우저에서 C 엔진 사용하기

기존 Python API 서버가 웹페이지와 SQL API를 함께 제공한다. 브라우저 안에서 데이터를 흉내 내는
DB를 따로 만들지 않았다. SQL 파싱·실행·B+ 트리 검색·파일 저장은 기존 C 공유 라이브러리가 수행한다.

## 실행

저장소 폴더에서 실행한다. C 라이브러리를 아직 빌드하지 않았다면 먼저 build.ps1 또는 make를 사용한다.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/serve-api.ps1 -Db data -Port 8080
```

```sh
python3 -m api.server --db data --port 8080
```

브라우저 주소: **http://127.0.0.1:8080/**

web/index.html을 파일로 직접 여는 대신 위 서버 주소를 사용해야 한다.
서버와 웹페이지가 같은 출처이므로 별도 개발 서버·프록시·CORS 설정이 필요 없다.
같은 data 폴더를 CLI에서 동시에 여는 것은 기존 프로세스 잠금 정책에 따라 거부된다.

## 기능

- SQL 입력 후 실행 버튼 또는 Ctrl+Enter(Mac에서는 ⌘+Enter)로 실행한다.
- 편집기에서 선택한 텍스트가 있으면 그 텍스트를, 없으면 전체 입력을 보낸다.
- 한 번에 SQL 한 문장만 지원한다. 여러 INSERT 튜플은 한 문장으로 보낼 수 있다.
- 빠른 시작 버튼은 SQL만 편집기에 불러온다. 실행 버튼을 누르기 전에는 DB가 변경되지 않는다.
- 실행 계획 확인은 SELECT 앞에 EXPLAIN을 붙여 기존 엔진의 계획을 확인한다.
- 레코드 추가 폼은 따옴표를 SQL 규칙으로 이스케이프하고 name/email의 UTF-8 바이트 수·INT32를 검사한다.
- 샘플 데이터 추가는 명시적으로 5건을 INSERT한다. 다시 누르면 5건이 더 추가된다.
- 결과 표는 서버에서 반환받은 행을 50개씩 표시한다. 페이지 이동은 새 쿼리를 실행하지 않는다.
- CSV는 현재 서버가 반환한 결과 전체를 내려받는다. DB 전체를 자동으로 다시 조회하지 않는다.
- 실행 계획과 검사한 행 수는 실제 엔진 응답이다. 데이터 크기에 따라 인덱스와 스캔을 비교할 수 있다.
- 전체 응답 시간은 브라우저 요청·전송·JSON 처리를 포함한다.
- 엔진·변환 시간은 API의 execution_ms이며 C 호출과 Python 행 변환을 포함한다.
- 엔진 통계는 페이지가 보이고 SQL 실행 중이 아닐 때 10초 간격으로 갱신한다.

기본 API 결과 제한은 1,000행·1MiB다. 결과가 크면 LIMIT를 사용한다.
입력·요청·worker 제한과 C 엔진 수명 정책은 [API_SERVER.md](API_SERVER.md)와 같다.

## 인증·최근 쿼리

MINIDB_API_TOKEN이 설정된 서버는 상단 연결 설정에서 토큰을 입력한다.
토큰은 JavaScript 메모리에만 보관하며 localStorage나 서버 HTML에 저장하지 않는다.
새로고침하면 다시 입력해야 한다. 웹페이지 자체는 토큰을 입력할 수 있도록 인증 없이 제공하지만
실제 조회·통계 요청에는 기존 API 인증을 적용한다.

최근 20개 쿼리는 이 브라우저의 localStorage에 보관한다. 결과 행과 인증 토큰은 보관하지 않는다.
최근 쿼리 지우기는 브라우저 내역만 지우며 DB 레코드는 삭제하지 않는다.

네트워크 오류나 브라우저의 응답 대기 만료가 C 쿼리 취소를 의미하지는 않는다.
INSERT였다면 저장됐을 수도 있으므로 자동 재시도하지 않는다. UI는 오류 내용을 표시하고 사용자가
저장 여부를 확인하도록 안내한다. 실행 버튼 중복 클릭은 진행 중 비활성화로 막는다.

## 구조

```text
GET /                       → web/index.html
GET /assets/studio.css       → web/studio.css
GET /assets/studio.js        → web/studio.js
GET /favicon.svg             → web/favicon.svg

실행 버튼
  → runQuery()
  → fetch('/query', JSON)
  → 기존 ApiHandler._execute_query()
  → NativeEngine.execute()
  → C db_execute()
  → 실제 파일/B+ 트리
  → renderSuccess() 또는 renderFailure()
```

서버의 정적 경로는 고정된 목록만 허용한다. URL을 파일 경로에 그대로 붙이지 않는다.
README·Python 소스·users.db 등을 정적 파일로 노출하지 않는다.
HTML/JS/CSS는 같은 서버의 파일만 사용하며 외부 CDN·폰트·스크립트에 의존하지 않는다.
CSP는 외부 스크립트, iframe 삽입, 외부 API 연결 등을 제한한다.
레코드·SQL·오류·내역은 textContent와 DOM 노드로 표시하므로 DB 값이 HTML로 실행되지 않는다.
신뢰된 정적 파일은 API의 결과 바이트 제한과 별개로 전송하며 전체 응답 전송 기한을 유지한다.

## 검증 범위

```sh
node --check web/studio.js
python -m unittest discover -s tests -p 'test_api*.py' -v
```

추가 HTTP 테스트는 정적 자산·MIME·Content-Length·CSP, 임의 파일 접근 거부,
HEAD/메서드 제한, 인증 전 페이지 접근, 실제 SQL 연결, 결과 크기 제한과 정적 파일의 분리를 검사한다.
이 테스트는 HTTP와 엔진 통합을 검증하며 브라우저 렌더링 검사를 대신하지 않는다.

최종 실행에서는 기존 API를 포함한 26개 테스트가 통과했다. JavaScript 문법 검사와
HTML의 67개 고유 ID·DOM 참조 검사도 통과했다. 큰 정적 자산은 8KiB 청크로 전송하며,
청크마다 기한이 새로 늘어나지 않도록 응답 전체에 하나의 전송 기한을 적용했다.

이번 환경에서 브라우저 자동 조작의 앱 승인이 만료되어 시각적·실제 클릭 기반 검증은 완료하지 못했다.
Windows 소켓 계층의 10014 및 간헐적인 전송 지연도 테스트 제약으로 관측됐다.
실제로 통과한 검사와 완료되지 않은 검증을 최종 작업 결과에서 구분한다.
