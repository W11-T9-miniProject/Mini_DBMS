# Phase 3 — C 엔진을 공유하는 Python HTTP API

기존 SQL 파서·실행기·파일 저장소·B+ 트리를 다시 구현하지 않고 **하나의 C 엔진 인스턴스**를
여러 Python worker가 호출한다. HTTP와 JSON, 요청 대기는 Python의 책임이다.

## 1. 구현 방향을 선택한 근거

과제의 핵심은 내부 DB 엔진 재사용과 Thread Pool 병렬 처리다. HTTP 파서·JSON 문자열 처리까지
C로 직접 작성하면 엔진과 별개인 작업이 늘어난다. Python 표준 라이브러리의 HTTP 파서를
사용하고 연결 수명·작업 배분·오류 정책을 직접 제어하도록 구성했다.

| 결정 | 이유 | 대가 |
|---|---|---|
| C 엔진을 DLL/so/dylib로 빌드 | CLI와 동일한 엔진 코드를 재사용 | 언어 간 ABI·수명 관리 필요 |
| ctypes.CDLL | 별도 확장 모듈 빌드 없이 C 호출 | 타입·콜백 선언을 정확하게 맞춰야 함 |
| 엔진 한 번 열기 | 메모리 인덱스와 잠금을 모든 요청이 공유 | 닫기 전에 모든 호출 종료 필요 |
| 고정 worker | 요청마다 새 스레드를 생성하지 않음 | worker가 모두 사용 중이면 대기 |
| 크기가 제한된 큐 | 메모리·대기 연결 수 증가 제한 | 포화 시 요청 거절 필요 |
| worker가 연결 하나를 처리 | HTTP 읽기부터 응답까지 소유권이 명확 | 느린 클라이언트가 worker를 점유 |
| 한 연결에 요청 하나 | idle keep-alive가 worker를 독점하지 않음 | 매 요청의 TCP 연결 비용 발생 |
| 결과를 제한된 메모리에 수집 | C 실행과 네트워크 전송 분리, 정확한 Content-Length | 대규모 응답에는 LIMIT 필요 |

`ThreadingHTTPServer`는 연결마다 스레드를 만드는 방식이므로 그대로 사용하지 않았다.
`ThreadPoolExecutor`도 worker 수만 제한하면 제출 대기량은 별도 제어가 필요하다.
그래서 `Queue(maxsize)`와 고정 Thread 목록으로 배분 구조를 명시했다.

이 서버는 학습용 API다. 표준 `http.server` 자체도 운영용 서버를 대체하는 모듈은 아니다.
TLS, HTTP/2, 접근 권한 구분, 분산 처리, 고가용성은 구현 범위가 아니다.

## 2. 전체 요청 흐름

```text
별도 클라이언트
  → TCP 연결 및 HTTP 요청
  → ApiServer.process_request()               [수신 스레드]
      → BoundedWorkerPool.try_submit()
          ├─ 큐 포화: 503, SQL 실행 안 함
          └─ 큐 등록
              → _worker_loop()                [미리 생성한 worker]
                  → ApiServer._serve_connection()
                      → ApiHandler.setup()/handle()
                          → HTTP 파싱·검증
                          → _dispatch()
                              → _read_sql()
                              → NativeEngine.execute()
                                  → ctypes.CDLL.db_execute()
                                      → 기존 C sql_parse()
                                      → 기존 C execute_insert/select()
                                      → 기존 C B+ 트리·파일 저장소
                                  → collect() 콜백에서 결과 복사
                              → JSON 직렬화
                              → send_json()
                      → 소켓 닫기
                  → 다음 작업 대기
```

수신 스레드는 SQL을 실행하지 않는다. 큐 포화 응답에서는 네트워크 오류를 피하기 위해
최대 50ms 전송 대기와 최대 200ms·64KiB의 입력 정리를 수행한다. 이 거절 경로도 무제한 대기하지 않는다.
거절 응답을 받지 않는 클라이언트에는 최선의 전송만 시도한 뒤 연결을 닫는다.

큐에 들어가는 객체는 `Connection(socket, address, accepted_at)`다.
SQL 문자열이 아니라 아직 읽지 않은 연결을 보관하므로, HTTP 읽기 자체도 worker 수에 의해 제한된다.

## 3. Thread Pool과 두 종류의 큐

Thread Pool은 일을 처리할 스레드를 미리 정한 수만큼 만들고 재사용하는 구조다.
현재 worker 이름은 `db-worker-1` 같은 형태이며 응답 헤더·본문에서 확인할 수 있다.

```text
workers=4, queue_size=64

실행 중 연결: 최대 4
worker 대기 연결: 최대 64
그 이상: 큐 등록 실패 → 503
```

이 68개는 **애플리케이션에 등록된 연결** 상한이다. OS의 listen backlog(128), 아직 accept되지 않은
연결, 클라이언트 측 대기는 별도다. worker 수를 늘린다고 DB의 디스크 동기화 처리량이 선형 증가하지 않는다.

`api/pool.py`의 잠금은 큐 등록·종료 상태·통계에만 사용하며 SQL 실행 중에는 잡고 있지 않는다.
`Queue`의 내부 동기화가 여러 worker의 꺼내기를 안전하게 만든다.
worker는 처리 중 예외가 발생해도 해당 연결의 정리를 마치고 다음 작업을 처리한다.

`/stats`의 active는 HTTP 읽기·C 실행·응답 전송을 포함한 연결 처리 수다.
`peak_active > 1`은 여러 worker가 동시에 연결을 처리한 증거이며,
그 값만으로 CPU 계산이 worker 수만큼 병렬 실행됐다고 주장하지는 않는다.

## 4. C/Python 연결과 GIL

`NativeEngine`은 라이브러리를 `ctypes.CDLL`로 로드한다. `db_open()`으로 얻는 포인터는 하나이며
모든 요청이 공유한다. worker마다 DB를 새로 열지 않는다.

`ctypes.CDLL`은 C 함수를 호출하는 동안 GIL을 해제한다. 따라서 Python worker 여러 개가
기존 C 엔진에 동시에 진입할 수 있다. 엔진의 공유 상태는 앞서 구현한 C 잠금이 보호한다.
`PyDLL`로 바꾸거나 C 호출 전체에 Python 전역 잠금을 걸면 이 동시성을 잃을 수 있다.

다만 C가 Python 결과 콜백을 호출하면 Python 실행을 위해 GIL을 다시 획득한다.
행 변환·JSON 직렬화 같은 Python 계산은 일반적인 CPython GIL의 영향을 받는다.
따라서 엔진 직접 호출 벤치마크와 HTTP 처리량은 서로 다른 지표다.

FFI는 컴파일러가 두 언어의 타입을 함께 검사해주지 않으므로 다음을 명시했다.

- C 포인터는 `c_void_p`, size_t는 `c_size_t`, uint64_t는 `c_uint64`로 선언한다.
- 모든 C 함수의 argtypes/restype를 지정해 64-bit 포인터가 잘리는 기본 반환형 문제를 피한다.
- 콜백은 C의 bool 반환에 맞는 CFUNCTYPE(c_bool, ...)이다.
- DLL의 ABI 버전과 네 구조체 sizeof를 Python 정의와 대조한 뒤 엔진을 연다.
- ABI 버전 1의 필드 순서·타입을 변경하면 C와 Python을 함께 고치고 버전을 올려야 한다.
- C의 행 포인터는 콜백 동안만 유효하므로 그 자리에서 Python 값으로 복사한다.
- 콜백 예외는 C 경계를 넘어가게 두지 않고 기록한 뒤 false를 반환한다.
- 최대 행 수를 넘겨도 false로 조회를 중단하고 RESULT_TOO_LARGE로 응답한다.

경로와 SQL은 UTF-8이다. Windows 저장소는 UTF-8 경로를 wide 문자열로 변환해 파일 API를 사용한다.
CLI 인자도 wmain에서 UTF-8로 변환하므로 한국어 경로와 SQL을 같은 방식으로 처리한다.

## 5. 데이터 동시성은 기존 엔진이 담당

API가 추가된다고 SQL 실행 규칙이나 파일 형식이 바뀌지는 않는다.

```text
INSERT
  writer mutex: ID 부여·파일 append·동기화
  짧은 공개 잠금: B+ 트리 등록·published_rows 갱신

SELECT
  짧은 읽기 잠금: 최초 공개된 행 수 또는 파일 오프셋 확보
  잠금 밖: 위치 지정 읽기·조건 평가·Python 콜백
```

공개된 레코드가 수정·삭제되지 않는 append-only 특성을 사용한다.
SELECT 시작 후 추가된 행은 그 SELECT의 대상이 되지 않는다. 범위 조회는 오프셋을 배치로 복사하며
노드 포인터를 잠금 밖에 보관하지 않는다. 일반적인 MVCC·트랜잭션 구현은 아니다.

`NativeEngine._borrow_handle()`의 Condition은 엔진 포인터의 수명만 보호한다.
조건 잠금은 C 호출 전에 풀리므로 여러 쿼리를 직렬화하지 않는다.
close는 새 호출을 차단하고 기존 호출 수가 0이 된 뒤 db_close를 호출한다.

## 6. HTTP 인터페이스

| 요청 | 동작 |
|---|---|
| GET /health | 생존 확인. DB 준비 상태·손상 여부를 보장하는 readiness 검사는 아님 |
| GET /stats | 엔진 행 수·인덱스 통계와 worker·대기·거절 통계 |
| POST /query | JSON의 sql 한 문장을 기존 C 엔진에 전달 |

```json
{"sql":"SELECT name,id FROM users WHERE id=1"}
```

응답 예:

```json
{
  "columns": ["name", "id"],
  "rows": [["Alice", 1]],
  "row_count": 1,
  "affected_rows": 1,
  "first_insert_id": null,
  "plan": "INDEX LOOKUP",
  "explain": false,
  "examined_rows": 1,
  "lock_wait_ms": 0.001,
  "lock_hold_ms": 0.002,
  "request": {
    "worker": "db-worker-1",
    "queue_wait_ms": 0.1,
    "execution_ms": 0.3
  }
}
```

row_count는 반환된 행 수, affected_rows는 INSERT의 삽입 수 또는 SELECT의 일치 행 수다.
INSERT는 rows가 빈 배열이고 first_insert_id를 반환한다.
EXPLAIN은 조회 계획만 반환한다. API에서 강제 스캔 옵션은 노출하지 않는다.

| 상태 | 의미 |
|---:|---|
| 200 | 실행·조회 성공 |
| 400 | JSON·UTF-8·NUL·요청 필드·길이 오류 |
| 401 | 설정된 Bearer 토큰 불일치 |
| 404 / 405 | 없는 경로 / 허용되지 않은 메서드 |
| 408 | 큐 또는 입력 수신 기한 초과. SQL 호출 전 거절 |
| 411 / 415 / 417 | Content-Length 없음 / JSON 아님 / Expect 미지원 |
| 413 / 414 / 431 | 본문·결과 / 요청 줄 / 헤더 크기 초과 |
| 422 | C SQL 파서·스키마·타입 오류 |
| 503 | 큐 포화 또는 엔진 사용 불가 |
| 500 | 내부 오류·메모리·결과 인코딩 문제 |

모든 응답은 Content-Length를 가진 JSON이며 Connection: close로 끝난다.
JSON 중복 키와 NaN/Infinity를 거부한다. HTTP chunked 입력, pipelining, HTTP/2는 제공하지 않는다.
한 POST는 한 SQL 문장이다. SQL 파일의 여러 문장을 한 POST에 보내면 C 파서가 오류로 반환한다.

## 7. 자원 제한과 시간 제한

기본값은 workers=4, queue=64, 헤더=16KiB, 요청 줄=4KiB,
JSON 본문=64KiB, 결과=1,000행·1MiB다. 결과를 무제한 Python 리스트에 담지 않는다.

입력 제한 시간은 accept 시점부터 5초다. 큐에서 보낸 시간도 포함한다.
`DeadlineSocketReader`는 실제 recv마다 남은 절대 기한을 계산하므로 조금씩 데이터를 보내도
타임아웃이 매번 새로 5초로 늘어나지 않는다. 전송은 별도 소켓 timeout(기본 2초/작업)을 사용한다.

이 제한은 **C 쿼리 실행 취소 기능이 아니다.** C 호출에 이미 진입한 뒤에는 완료를 기다린다.
Python Future.cancel()만으로 실행 중인 C INSERT를 중단할 수는 없다.
비선택적 대용량 스캔은 결과가 적어도 오래 걸릴 수 있고 종료 대기에도 영향을 준다.

클라이언트가 응답을 받기 전에 끊기면 INSERT가 이미 저장됐을 수도 있다.
응답 전송 timeout을 입력 timeout으로 바꿔 "SQL 미실행"이라고 보고하지 않는다.
저장 후 인덱스 메모리 실패도 가능하므로 오류나 연결 끊김에 INSERT를 자동 재시도하면 안 된다.
이 버전은 idempotency key나 exactly-once 전달을 구현하지 않았다.

## 8. 시작과 정상 종료

```text
공유 라이브러리 로드·ABI 확인
→ db_open·인덱스 복구
→ HTTP listen·worker 시작
→ ready 로그 출력
→ 요청 수신

Ctrl+C / SIGTERM
→ stop_event 설정
→ 수신 루프 종료
→ listen 소켓 닫기
→ 큐에 등록된 요청 처리·소켓 정리
→ 모든 worker join
→ C 호출 종료 확인
→ db_close
```

프로세스 강제 종료는 정상 종료가 아니다. Windows 테스트의 terminate()도 강제 종료이며,
별도 생명주기 테스트에서 수신 중단·큐 drain·worker join을 검증한다.

기본 호스트는 loopback이다. 다른 장비에서 접속하려면 --host 0.0.0.0으로 열고 서버 IP를 사용한다.
MINIDB_API_TOKEN이 설정되면 /query와 /stats에서 인증한다. /health는 공개 생존 확인이다.
토큰은 접근 허용만 제어하며 사용자별 SQL 권한이나 TLS 암호화를 제공하지 않는다.

## 9. 검증과 코드 읽기

```sh
python -m unittest discover -s tests -p 'test_api*.py' -v
python tools/http_benchmark.py build/http-validation --rows 10000 --requests 200 --repeats 3 --output build/http-validation.json
```

테스트는 C 공유 라이브러리를 실제 로드하고, 별도 Python 서버 프로세스에 HTTP로 요청한다.
일반 기능·병렬 삽입·ID/범위/스캔 경로·재시작·입력/출력 한계·인증·입력 timeout을 검사한다.
큐 포화와 종료는 Event로 실행을 제어해 시간 추측 없이 등록 상한과 미실행 거절을 확인한다.

부하 도구는 같은 seed 파일을 복사해 각 케이스를 시작한다. C 엔진에는 기본 동기 쓰기를 적용한다.
HTTP 200의 삽입 ID를 모으고, 서버 종료 후 C 엔진으로 다시 열어 행 수·고유 ID·name·age를 확인한다.
포화의 503은 예상된 거절로 따로 집계하고 네트워크 오류나 잘못된 결과와 구분한다.
클라이언트는 closed-loop이며 요청마다 새 TCP 연결을 만든다.
지연은 연결·요청 전송·서버 대기·C 실행·JSON·응답 파싱을 포함한다.
execution_ms는 C 호출과 결과 변환, queue_wait_ms는 accept 이후 worker 배정까지다.

함수를 읽는 순서:

1. api/server.py의 main: 엔진과 서버의 소유 관계.
2. ApiServer.process_request: accept와 큐 등록의 경계.
3. api/pool.py의 _worker_loop: 고정 스레드와 반복 사용.
4. ApiHandler.handle → _dispatch → _read_sql: HTTP와 JSON 검증.
5. api/native.py의 load_library → execute → collect: 타입과 C 콜백.
6. 기존 src/engine.c의 db_execute: Phase 1·2가 그대로 호출되는 지점.
7. send_json → shutdown_request: 엔진 작업과 네트워크 전송의 분리.
8. server_close → pool.close → NativeEngine.close: 안전한 종료 순서.

## 10. 실제 실행 결과

Windows 11 x64 / i5-8265U / Python 3.12.14 / MSVC C11에서 확인했다.
Linux/macOS 빌드·CI 경로를 제공하지만 이 장비에서 해당 OS 실행까지 검증한 것은 아니다.

| 검증 | 결과 |
|---|---|
| C 엔진 회귀 | 751,426 assertions 통과 |
| C 엔진 MSVC AddressSanitizer | 같은 검사 통과, 메모리 접근 오류 보고 없음 |
| Python bridge·실제 HTTP·종료 테스트 | 21 tests 통과 |
| CLI 회귀 | SQL 파일·재시작·강제 스캔·오류 종료·표준 입력 통과 |
| UTF-8 상호 운용 | 한국어 경로·CLI SQL 인자·SQL 파일을 API에서 그대로 조회 |
| 실제 HTTP 부하 | 21케이스, 총 4,200요청, 200 성공 4,069건, 예상된 503 거절 131건 |
| 쓰기 보존 | 성공한 INSERT 649건의 고유 ID·행 수·name·age를 재시작 후 검증 |

부하 검증은 초기 10,000행, 케이스당 200요청, 3회 반복이다.
point/mixed는 workers=4·queue=64이며 각각 1·8·32개 클라이언트를 사용했다.
과부하는 workers=2·queue=2에 클라이언트 64개가 단건 INSERT를 보냈다.
SQL은 모두 실제 공유 C 엔진을 사용하고 기본 동기 쓰기를 유지했다.

| 패턴 | 클라이언트 | 성공 처리량 중앙값(req/s) | 실행별 HTTP p95 중앙값(ms) | C 호출·행 변환 p95 중앙값(ms) |
|---|---:|---:|---:|---:|
| ID 조회 | 1 | 59.1 | 27.34 | 0.356 |
| ID 조회 | 8 | 159.0 | 81.00 | 0.893 |
| ID 조회 | 32 | 215.8 | 158.34 | 0.865 |
| 읽기 90%·쓰기 10% | 1 | 53.4 | 26.46 | 2.789 |
| 읽기 90%·쓰기 10% | 8 | 175.6 | 67.05 | 2.103 |
| 읽기 90%·쓰기 10% | 32 | 192.2 | 183.03 | 2.203 |

HTTP 지연에는 매 요청의 새 연결, 입력, 대기, 전송, 클라이언트 처리가 포함된다.
엔진 직접 호출의 초당 처리량과 이 표를 동일 지표로 비교하면 안 된다.
이 환경에서는 C 실행보다 전송·스케줄링·대기 비용이 크게 관측됐다. 이를 단순히
Python 또는 네트워크의 한 요소만의 비용으로 분해했다고 주장하지 않는다.
queue_wait_ms는 Windows/Python의 monotonic clock 해상도 영향도 받는다.

과부하 케이스의 성공/거절은 각각 156/44, 158/42, 155/45였다.
최대 active는 2를 넘지 않았으며 거절 요청은 SQL 실행 없이 끝났다.
최종 보고서의 네트워크 오류·예상하지 않은 HTTP 오류·조회 오류는 0건이다.

원본: [http-benchmark.json](http-benchmark.json).
제한된 실행 환경의 일부 시도에서는 Winsock 10014로 중단되어,
최종 측정은 승인받아 샌드박스 밖의 로컬 loopback에서 수행했다.
이 기록은 장시간 운영 안정성이나 공개 인터넷 처리 용량을 보장하는 결과는 아니다.

추가 테스트의 ASan 검사는 C 테스트 실행 파일에 적용했으며,
Python 프로세스에 로드한 DLL 전체를 ASan으로 계측한 검사는 아니다.
ThreadSanitizer·강제 전원 차단·TLS·원격 장비 방화벽 연결 테스트는 수행하지 않았다.

## 공식 참고


- [ctypes: C 라이브러리 호출과 CDLL/GIL](https://docs.python.org/3/library/ctypes.html)
- [socketserver: 요청 처리와 서버 종료](https://docs.python.org/3/library/socketserver.html)
- [http.server: HTTP 파서와 적용 범위](https://docs.python.org/3/library/http.server.html)
