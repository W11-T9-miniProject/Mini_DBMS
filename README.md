# Mini DBMS — C11

직접 구현한 SQL 파서와 파일 저장소, 메모리 기반 B+ 트리를 사용하는 **C DB 엔진 + CLI + Python HTTP API**입니다.
Phase 1·2 엔진을 공유 라이브러리로 재사용하고, Phase 3은 고정 크기 Thread Pool로 병렬 처리합니다.
외부 DB·SQL 파서·서드파티 라이브러리를 사용하지 않습니다.

## 빠른 실행

Windows에서는 Visual Studio의 **C++를 사용한 데스크톱 개발** 워크로드가 필요합니다.
컴파일되는 소스는 모두 C이며 C++ 코드를 사용하지 않습니다.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1 -Test
.\build\mini_db.exe --db data --file examples/demo.sql
.\build\mini_db.exe --db data --sql "SELECT * FROM users WHERE id = 2;"
```

Linux/macOS에서는 C11 컴파일러와 make를 사용합니다.

```sh
make
make test
./build/mini_db --db data --file examples/demo.sql
./build/mini_db --db data --sql "SELECT * FROM users WHERE id = 2;"
make sanitize                 # GCC/Clang의 ASan + UBSan으로 검사
```

CMake 빌드도 지원합니다. MSVC에서는 빌드 구성에 따라 실행 파일이 `build/Release/`에 생깁니다.

```sh
cmake -S . -B build/cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build/cmake --config Release
ctest --test-dir build/cmake -C Release --output-on-failure
```

DB 디렉터리는 없으면 생성합니다. 부모 디렉터리는 존재해야 합니다. 같은 DB를 두 프로세스가
동시에 여는 것은 거부하며, 여러 스레드는 하나의 Db 핸들을 공유할 수 있습니다.
예제 파일을 다시 실행하면 행이 추가됩니다. 초기 상태를 보려면 새 DB 디렉터리를 지정하세요.

## Phase 3: HTTP API

Python 3.10 이상과 기본 빌드 결과가 필요합니다. pip 패키지 설치는 없습니다.
Windows 기본 빌드는 `build/minidb.dll`, Linux/macOS make는 `build/libminidb.so` 또는
`build/libminidb.dylib`도 생성합니다.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/serve-api.ps1 -Db data -Workers 4 -QueueSize 64 -Port 8080
```

```sh
make
python3 -m api.server --db data --workers 4 --queue-size 64 --port 8080
```

별도 터미널에서 예제 클라이언트를 실행합니다.

서버 실행 로그의 `url` 주소를 열면 **MiniDB Studio** 웹페이지를 사용할 수 있습니다.
기본 주소는 http://127.0.0.1:8080/ 이며, PowerShell 실행 스크립트는 포트가 사용 중이면 빈 포트를 선택합니다.
SQL 편집기, 예제 쿼리, 레코드 추가 폼, 결과 표·페이지 이동·CSV, 실행 계획·JSON,
최근 쿼리와 실시간 엔진 통계를 제공합니다. 별도 프런트엔드 빌드나 npm 설치는 필요 없습니다.
모든 SQL은 기존 `/query`를 통해 실제 C 엔진에서 실행됩니다.
페이지를 열기만 해서는 데이터를 추가하지 않습니다. 샘플 추가 버튼은 실제 DB에 5건을 저장합니다.
자세한 사용법과 구성은 [STUDIO.md](docs/STUDIO.md)를 참고하세요.

```sh
python examples/api_client.py
curl http://127.0.0.1:8080/health
curl -X POST http://127.0.0.1:8080/query -H 'Content-Type: application/json' -d '{"sql":"SELECT * FROM users WHERE id=1"}'
```

Windows PowerShell의 curl은 `curl.exe`를 사용하거나 다음처럼 요청할 수 있습니다.

```powershell
$body = @{ sql = 'SELECT * FROM users WHERE id=1' } | ConvertTo-Json
Invoke-RestMethod http://127.0.0.1:8080/query -Method Post -ContentType 'application/json' -Body $body
```

외부 장비에서 접속하려면 `--host 0.0.0.0`으로 바인딩하고 해당 컴퓨터의 IP를 사용합니다.
`MINIDB_API_TOKEN` 환경 변수를 지정하면 `/query`와 `/stats`에 Bearer 인증을 적용합니다.
기본 바인딩은 `127.0.0.1`입니다. HTTPS·다중 사용자 권한은 제공하지 않습니다.

- `POST /query`: `{"sql":"..."}` 한 문장 실행, JSON 응답.
- `GET /health`: 서버 생존 확인.
- `GET /stats`: 엔진 행 수·인덱스 통계와 worker·queue 상태.
- 큐 포화 `503`, 입력 기한 초과 `408`, SQL 오류 `422`, 결과 제한 초과 `413`.
- 기본 입력 최대 64 KiB, 결과 최대 1,000행·1 MiB. 큰 SELECT에는 LIMIT를 사용하세요.
- Ctrl+C 시 수신을 중단하고 이미 받은 작업의 종료를 기다린 뒤 엔진을 닫습니다.
- CMake 빌드 등 라이브러리 경로가 다르면 `--library 경로`를 지정합니다.

```sh
python -m unittest discover -s tests -p 'test_api*.py' -v
python tools/http_benchmark.py build/my-http-load --rows 10000 --requests 200 --repeats 3 --output build/http-results.json
```

요청 한 건의 함수 호출 순서, Thread Pool·큐·ctypes·GIL 설명, 오류 정책과 실제 검증 결과는
**[API 설계 문서](docs/API_SERVER.md)**를 참고하세요.

## 지원 SQL

이미 존재하는 스키마는 `users` 하나입니다. CREATE TABLE은 필요하지 않으므로 구현하지 않았습니다.

| 컬럼 | 타입 | 규칙 |
|---|---|---|
| id | 양의 64-bit 정수 | 1부터 자동 증가, 직접 INSERT 불가 |
| name | 문자열 | 최대 63바이트, 필수 |
| age | signed 32-bit 정수 | 필수, INT32_MIN~INT32_MAX |
| email | 문자열 | 최대 127바이트, 생략 시 빈 문자열 |

문자열은 UTF-8 바이트를 그대로 보존하며 길이는 문자 수가 아닌 **바이트 수**입니다.
입력 인코딩의 유효성을 별도로 검사하거나 문자열을 정규화하지는 않습니다.
age는 정수 타입 학습용 컬럼이므로 실제 사람 나이의 도메인 제약은 적용하지 않습니다.

```sql
INSERT INTO users (name, age) VALUES ('Alice', 25);
INSERT INTO users VALUES ('Bob', 30, 'bob@example.com');
INSERT INTO users (age, name, email) VALUES
    (28, 'O''Brien; Kim', 'kim@example.com'),
    (20, 'Jung', 'jung@example.com');

SELECT * FROM users;
SELECT name, age FROM users WHERE id = 2;
SELECT * FROM users WHERE name = 'Bob';
SELECT * FROM users WHERE age >= 25 LIMIT 10;
SELECT id, name FROM users WHERE id BETWEEN 2 AND 100 LIMIT 5;

EXPLAIN SELECT * FROM users WHERE id = 2;
EXPLAIN SELECT * FROM users WHERE age = 25;
```

- 키워드·테이블·컬럼 이름은 대소문자를 구분하지 않습니다. 문자열 비교는 바이트 기준이며 대소문자를 구분합니다.
- 문자열은 작은따옴표를 사용하고 `''`로 따옴표를 표현합니다. 역슬래시 이스케이프는 없습니다.
- 단일 WHERE 조건의 `=`, `!=`, `<>`, `<`, `<=`, `>`, `>=`를 지원합니다.
- 숫자 컬럼은 `BETWEEN low AND high`도 지원합니다. 양 끝값을 포함합니다.
- ID의 동등·범위 조건은 B+ 트리를 사용합니다. 그 외에는 선형 탐색합니다.
- LIMIT 0은 아무 행도 읽지 않습니다. LIMIT 생략 시 모든 결과를 스트리밍합니다.
- `--` 한 줄 주석과 `/* ... */` 블록 주석을 지원합니다. 중첩 블록 주석은 지원하지 않습니다.
- SQL 파일은 세미콜론으로 문장을 구분하며 마지막 세미콜론은 생략할 수 있습니다.
- INSERT 한 문장에 최대 4,096개 VALUES 튜플, 문장당 최대 4 MiB입니다.
- 지원하지 않는 SQL은 오류로 반환합니다. NULL, UPDATE, DELETE, JOIN, ORDER BY, 집계, 복합 조건, 트랜잭션은 구현 범위 밖입니다.

## CLI

```text
mini_db [--db DIRECTORY] [--file FILE | --sql SQL] [--scan] [--quiet] [--no-sync]
```

`--file`과 `--sql`을 생략하면 표준 입력에서 SQL을 읽습니다. `--sql`은 한 문장만 허용합니다.
조회 행은 CSV 형식으로, 실행 요약은 `#`으로 시작하는 줄에 출력합니다. `--quiet`는 결과 출력을 생략합니다.
`--scan`은 동일 SQL을 인덱스 없이 실행해 비교하는 옵션입니다.

```powershell
.\build\mini_db.exe --db data --sql "SELECT * FROM users WHERE id=2"
# INDEX LOOKUP: rows=1 examined=1

.\build\mini_db.exe --db data --scan --sql "SELECT * FROM users WHERE id=2"
# TABLE SCAN: rows=1 examined=<테이블 전체 행 수>
```

종료 코드: 성공 `0`, 실행·SQL·파일 오류 `1`, 잘못된 CLI 인자 `2`.
SQL 파일은 첫 오류에서 중단합니다. 앞에서 성공한 문장은 유지됩니다.

기본 INSERT는 `fflush` 후 `fsync`/`_commit`으로 동기화합니다.
`--no-sync`는 OS 캐시까지만 flush하므로 전원 장애 내구성이 약해집니다.
WAL과 트랜잭션을 구현한 DB는 아닙니다. 중단된 쓰기·손상 파일 처리와 정확한 보장 범위는
[설계 문서](docs/ARCHITECTURE.md)의 저장 형식 항목에 설명했습니다.

## 테스트

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1 -Test
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test-cli.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1 -Sanitize
```

`db_tests`는 파서·B+ 트리·저장소·실행기·SQL 파일 처리·동시성을 검증합니다.

| 검증 | 내용 |
|---|---|
| SQL | 정상/오류 문법, 타입, 숫자 overflow, 문자열 길이, 따옴표·주석 |
| B+ 트리 | 정순·역순·무작위 각 5만 건, 모든 키 검색, 중복, 범위, 구조 불변식 |
| 파일 | 재시작, 자동 ID 연속성, 빈 DB, 체크섬·헤더·잘린 레코드 오류 |
| 실행 | projection, 계획 선택, 인덱스/스캔 결과 일치, LIMIT, 경계값 |
| 동시성 | 4개 writer + 2개 reader, 800건 고유 ID·행 수·재시작 |
| 스냅샷 | SELECT 콜백 중 400건 INSERT, 배치 경계·노드 분할 후에도 최초 조회 범위 유지 |
| CLI | 실제 실행 파일을 이용한 SQL 파일 입력, 재실행, 오류 종료, 스캔 옵션 |

테스트 DB는 `build/test-*`에 생성하고 테스트 성공 시 해당 테스트 파일만 제거합니다.
벤치마크는 별도이며 일반 테스트마다 100만 건을 생성하지 않습니다.

## 100만 건 성능 비교

```powershell
.\build\db_bench.exe build/benchmark-data 1000000 30
```

```sh
make benchmark
# 또는 ./build/db_bench build/benchmark-data 1000000 30
```

벤치마크 경로는 새 DB 또는 비어 있는 DB여야 합니다. 기존 행이 있으면 변경하지 않고 종료합니다.
재실행하려면 다른 디렉터리를 지정하세요. 생성된 약 208 MB 파일은 직접 확인할 수 있도록 남깁니다.

1. 실제 SQL 파서와 실행기를 통해 1,000개 튜플 단위 INSERT로 100만 건을 생성합니다.
2. 기본 내구성 설정을 사용하고, INSERT 문장마다 디스크 동기화합니다.
3. 엔진을 닫고 다시 열어 인덱스 재구축과 전체 행 수를 검증합니다.
4. 고정 시드로 조회 ID를 선정하고 아래 세 경로를 비교합니다.
5. 각 조회가 기대한 ID 하나를 반환하는지와 실제 확인한 행 수를 검사합니다.

| 경로 | 읽는 레코드 | 비교 목적 |
|---|---:|---|
| id_index | 1 | ID 인덱스 조회 |
| same_id_forced_scan | 1,000,000 | 동일 조건에서 인덱스 효과 분리 |
| unique_name_scan | 1,000,000 | 다른 필드 선형 탐색과 비교하는 과제 요구 |

워밍업 후 30회 중앙값·p95를 보고합니다. 출력 시간은 조회 측정에 포함하지 않습니다.
측정에는 파싱, 잠금, 체크섬 검증, 행 콜백이 포함됩니다.
OS 캐시를 비우지 않는 warm-cache 실험이며, 디스크의 cold-read 성능으로 해석하면 안 됩니다.
B+ 트리 높이·노드 수·노드 메모리 추정치, 파일 크기, 삽입 시간·재구축 시간도 출력합니다.
메모리 수치는 트리 객체와 노드의 sizeof 합이며 malloc 오버헤드나 프로세스 전체 RSS는 아닙니다.

실제 실행 결과와 환경은 [검증 기록](docs/VALIDATION.md)에 정리합니다.

## 코드 읽기

```text
include/db.h       엔진 공개 API: open → execute → close
src/abi.c          C/Python ABI 버전·구조체 크기 확인
include/db_script.h SQL 파일 스트리밍 API
src/main.c         CLI 인자, CSV 출력
src/script.c       따옴표·주석을 구분하는 SQL 파일 분리
src/parser.c       토큰화, 문법·타입 검증, 조건 평가
src/engine.c       실행 계획, 테이블 잠금, INSERT/SELECT 실행
src/storage.c      파일 헤더, 직렬화, 체크섬, append/read
src/bptree.c       B+ 트리 검색, 삽입, 분할, 범위 순회
src/platform.c     Windows/POSIX 파일·잠금·시간 처리
tests/test_main.c  C 단위·통합·동시성 테스트
tools/benchmark.c  SQL 기반 대량 생성과 검증 가능한 벤치마크
tools/load_benchmark.c 동시 요청·락 대기·p95/p99 측정
api/native.py      ctypes 공유 라이브러리 연결과 엔진 수명 관리
api/pool.py        고정 worker + 제한 큐
api/server.py      HTTP 검증·라우팅·JSON 응답
api/http_io.py     입력 크기·절대 기한 제한
web/              같은 서버에서 제공하는 MiniDB Studio (HTML/CSS/JS)
tools/http_benchmark.py 실제 HTTP 부하·재시작 검증
```

동시 부하 측정 방법과 수정 전후 비교는 [LOAD_TEST.md](docs/LOAD_TEST.md)에 있습니다.
현재 SELECT는 공개된 행 수를 짧게 잠가 확보한 뒤, 위치 지정 읽기와 결과 콜백을 잠금 밖에서
수행합니다. INSERT의 디스크 동기화 중에도 기존 행을 조회할 수 있습니다.

**함수별 호출 흐름과 디버거 중단점 순서는 [ARCHITECTURE.md](docs/ARCHITECTURE.md)를 읽으세요.**

공개 API를 사용하는 최소 예제:

```c
#include "db.h"
#include <stdio.h>

int main(void) {
    DbError error;
    DbResult result;
    Db *db = db_open("data", true, &error);
    if (!db) { fprintf(stderr, "%s\n", error.message); return 1; }
    DbStatus status = db_execute(db,
        "INSERT INTO users(name,age) VALUES('Alice',25)",
        NULL, NULL, &result, &error);
    if (status != DB_OK) fprintf(stderr, "%s\n", error.message);
    db_close(db);
    return status == DB_OK ? 0 : 1;
}
```
