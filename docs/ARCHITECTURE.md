# 함수로 따라가는 미니 DBMS

이 프로젝트의 범위는 **C DB 엔진 + CLI(Phase 1·2)** 이다. API 서버는 포함하지 않는다.
각 함수를 디버거에서 따라가면서 읽을 수 있도록 입력, 파싱, 실행, 저장, 인덱스를 나눴다.

## 1. 먼저 읽을 파일

| 순서 | 파일 | 확인할 질문 |
|---|---|---|
| 1 | `include/db.h` | 외부 프로그램은 엔진을 어떻게 사용하는가? |
| 2 | `src/main.c` | SQL 입력과 결과 출력은 어디에서 하는가? |
| 3 | `src/engine.c` | 실행 계획을 선택하고 잠그는 곳은 어디인가? |
| 4 | `src/parser.c` | 텍스트가 어떻게 Statement로 바뀌는가? |
| 5 | `src/storage.c` | 메모리의 Row가 어떤 바이트로 저장되는가? |
| 6 | `src/bptree.c` | 키 검색과 노드 분할은 어떻게 하는가? |
| 7 | `src/platform.c` | 운영체제에 의존하는 부분은 무엇인가? |

`internal.h`의 Statement는 파싱 결과다. `DbRow`는 레코드의 메모리 표현이고,
파일에는 C 구조체를 그대로 저장하지 않는다. `db_execute()`는 출력하지 않고
행 콜백과 DbResult로 결과를 전달한다. CLI와 향후 API가 동일한 함수를 사용할 수 있다.

## 2. INSERT 한 문장 추적

```sql
INSERT INTO users(name, age) VALUES('Alice', 25);
```

```text
main
 └─ execute_cli_statement
     └─ db_execute
         └─ execute_sql
             ├─ sql_parse
             │   ├─ next_token: 단어, 숫자, 문자열, 연산자 구분
             │   └─ parse_insert
             │       └─ parse_insert_tuple → parse_insert_value
             ├─ platform_mutex_lock(writer_lock): 쓰기 요청끼리 직렬화
             ├─ execute_insert
             │   ├─ storage_append
             │   │   ├─ ID 부여: 기존 행 수 + 1
             │   │   ├─ encode_record: little-endian 직렬화 + 체크섬
             │   │   ├─ fwrite → fflush → platform_sync
             │   │   └─ 실패 시 rollback_failed_append
             │   ├─ platform_write_lock(table_lock): 짧은 공개 구간
             │   ├─ bptree_insert(ID, 파일 오프셋)
             │   ├─ published_rows 갱신
             │   └─ platform_write_unlock(table_lock)
             ├─ platform_mutex_unlock(writer_lock)
             └─ sql_statement_destroy
```

한 INSERT의 모든 VALUES 튜플을 파싱·검증한 뒤 파일을 변경한다. 따라서 마지막 튜플에
타입 오류가 있으면 앞의 튜플도 저장되지 않는다. 여러 INSERT 문장으로 된 파일은 문장별로
실행하며, 뒤 문장이 실패해도 앞에서 성공한 문장은 유지한다.

디스크 기록 중에는 기존의 공개된 행을 계속 읽을 수 있다. SELECT가 보는 행 수는
`published_rows`이며, 파일 기록·동기화와 인덱스 등록이 끝난 뒤 테이블 잠금 안에서 갱신한다.
따라서 아직 인덱스에 등록되지 않은 새 행을 스캔으로 먼저 관찰하지 않는다.
디스크 기록은 성공했지만 인덱스 메모리 할당에 실패하면 엔진을 사용 불가로
표시하고 오류를 반환한다. 재시작하면 이미 저장된 행으로 인덱스를 재구축한다. 이 오류에서
INSERT를 무조건 재시도하면 중복 데이터가 생길 수 있으므로 오류 메시지를 확인해야 한다.

## 3. ID SELECT 추적

```text
db_execute → execute_sql
 ├─ sql_parse → parse_select → parse_predicate
 └─ execute_select
     ├─ choose_select_plan → INDEX LOOKUP
     ├─ acquire_select_lock → 공개된 행 수와 ID 오프셋 확보
     ├─ release_select_lock
     └─ execute_index_lookup
         ├─ storage_read: 해당 오프셋의 208바이트 읽기
         │   └─ decode_record: 체크섬과 문자열 경계 검증
         └─ visit_matching_row
             ├─ sql_row_matches
             └─ 사용자가 전달한 DbRowVisitor 호출
```

`id = 42`에서 ID를 파일 행 번호로 직접 계산하지 않는다. 실제 B+ 트리가 반환한
오프셋으로 조회한다. 파일이 고정 길이인 것과 무관하게 인덱스가 조회 경로를 결정한다.

`WHERE name='Alice'`는 `execute_table_scan()`으로 SELECT 시작 시 공개된 행을 확인한다.
256개 레코드씩 위치 지정 읽기를 수행하며 조건을 평가한다.
`WHERE id BETWEEN 10 AND 20`은 `execute_index_range()`에서
`bptree_lower_bound()`로 첫 리프를 찾고 `bptree_cursor_next()`로 연결된 리프를 순회한다.
인덱스 잠금 안에서 최대 256개 오프셋을 복사한 뒤 잠금을 풀고 읽는다.
다음 배치에서는 마지막 키 다음부터 다시 탐색한다. 잠금 밖으로 노드 포인터나 cursor를
보관하면 다른 스레드의 노드 분할과 충돌하므로 오프셋만 복사한다.
ID의 `=`, `<`, `<=`, `>`, `>=`, `BETWEEN`은 인덱스를 사용하고 `!=`는 전체 스캔한다.
일치 결과가 LIMIT에 도달하면 중단한다. `examined_rows`는 조건을 평가한 행 수이며,
배치에서 미리 읽은 행 수와 다를 수 있다. EXPLAIN은 계획만 반환하고 행은 읽지 않는다.

## 4. B+ 트리 삽입 추적

```text
bptree_insert
 ├─ bptree_find: 중복 키 거부
 ├─ reserve_split_nodes: 분할에 필요한 노드 메모리 먼저 확보
 └─ insert_into_node
     ├─ 내부 노드: 구분 키를 이진 탐색하고 자식으로 재귀 이동
     ├─ 리프 노드: 정렬 위치에 키/오프셋 삽입
     ├─ 리프 초과: split_leaf → 오른쪽 첫 키를 부모에 복사
     ├─ 내부 초과: split_internal → 가운데 구분 키를 부모로 이동
     └─ 루트까지 분할이 전파되면 새 루트 생성
```

최대 키 31개, 자식 32개다. 삽입 중에는 키 32개를 잠시 허용한 뒤 나눈다.
리프는 16/16, 내부는 가운데 키를 올리고 16/15로 분할한다.
리프의 `next`는 범위 검색용이다. 내부 노드의 separator는 오른쪽 서브트리의 최소 키다.

`bptree_validate()`는 정렬, 최소·최대 키 개수, 리프 깊이, 구분 키, 리프 연결,
전체 키 수와 노드 수를 재귀적으로 검증한다. 테스트는 정순·역순·고정 시드 무작위 삽입을
각각 5만 건 수행하며 표준적인 정수 키/값 규칙과 모든 검색 결과를 비교한다.

## 5. 저장 형식과 재시작

`users.db`는 16바이트 헤더와 208바이트 레코드의 반복이다. 숫자는 little-endian이다.

| 위치 | 길이 | 내용 |
|---|---:|---|
| 헤더 0 | 8 | `MINIDB01` |
| 헤더 8 | 4 | 버전 1 |
| 헤더 12 | 4 | 레코드 크기 208 |
| 레코드 0 | 8 | ID (양수, signed 64-bit 범위) |
| 레코드 8 | 4 | age (signed 32-bit) |
| 레코드 12 | 64 | name (NUL 포함) |
| 레코드 76 | 128 | email (NUL 포함) |
| 레코드 204 | 4 | 앞의 204바이트에 대한 FNV-1a 체크섬 |

행 번호 n(0부터)의 오프셋은 `16 + n × 208`이다. 체크섬은 우발적인 손상 탐지용이며
충돌 가능성이 있고 변조 방지용은 아니다.

```text
db_open
 ├─ storage_open
 │   ├─ 디렉터리 생성 또는 확인
 │   ├─ users.lock: 다른 프로세스의 동시 열기 차단
 │   ├─ 헤더 생성 또는 검증
 │   └─ 불완전한 마지막 레코드면 DB_CORRUPT (자동 삭제하지 않음)
 └─ rebuild_id_index
     └─ 모든 레코드의 체크섬·ID 연속성을 확인하고 B+ 트리 재구축
```

인덱스는 메모리에만 있고 파일이 원본이다. 재시작은 전체 파일을 읽으므로 데이터가 커질수록
오래 걸린다. 성공한 INSERT는 기본적으로 `fflush`와 `fsync`/`_commit`까지 수행한다.
`--no-sync`는 OS 캐시까지만 전달하는 명시적인 성능 옵션이다.

WAL과 트랜잭션은 없다. 전원 장애 중 튜플 일부만 기록될 수 있으며, 다중 VALUES의
크래시 원자성은 보장하지 않는다. 완전한 레코드 경계까지 기록됐다면 그 행은 재시작 후
보일 수 있고, 마지막 레코드가 끊겼다면 열기를 거부한다. 헤더나 완전한 레코드가 손상돼도
열기를 거부하고 기존 파일을 자동 복구하거나 덮어쓰지 않는다. 운영용 DB의 완전한 ACID,
디렉터리 메타데이터의 전원 장애 내구성까지 보장하는 구현은 아니다.

## 6. 스레드 안전성

- 파싱은 호출별 로컬 상태로 수행한다.
- INSERT는 writer mutex 안에서 ID 부여·append·동기화를 수행한다. INSERT끼리는 직렬화된다.
- 테이블 쓰기 잠금은 B+ 트리 갱신과 published_rows 공개 구간에서만 사용한다.
- SELECT는 짧은 테이블 읽기 잠금으로 published_rows 또는 오프셋을 확보한다.
- 공개된 레코드는 수정·삭제하지 않으므로 잠금을 푼 뒤 파일을 읽어도 기존 값은 바뀌지 않는다.
- 이후 INSERT된 ID는 최초 published_rows보다 크므로 진행 중 SELECT에서 제외된다.
- 공유 FILE 읽기 커서를 사용하지 않는다. POSIX는 pread, Windows는 독립 OVERLAPPED/event를 사용한다.
- 디스크 읽기와 행 콜백은 테이블 잠금 밖에서 수행한다. 콜백에서 같은 Db에 INSERT할 수 있다.
- 범위 조회는 노드 포인터를 잠금 밖에 보관하지 않고, 오프셋만 배치로 복사한다.
- 이 방식은 append-only 조건에 의존한다. UPDATE/DELETE를 추가하려면 별도의 버전·수명 관리가 필요하다.
- 일반적인 MVCC나 다중 문장 트랜잭션을 제공하는 구현은 아니다.
- 행 콜백에서 db_close를 호출하면 안 된다.
- db_close 전에 호출자 스레드를 모두 종료해야 한다.
- OS 잠금의 공정성이나 CPU 코어 수에 비례한 성능 향상은 보장하지 않는다.

## 7. 디버깅 실습

1. `db_execute`에 중단점을 걸고 `examples/demo.sql`을 실행한다.
2. `sql_parse`가 반환한 `Statement`의 kind, columns, rows, predicate를 확인한다.
3. INSERT에서 `storage_append` 전후의 row_count와 `first_insert_id`를 비교한다.
4. ID 조회에서 `bptree_find`가 반환한 offset을 확인한다.
5. 같은 SELECT를 `--scan`으로 실행하고 `examined_rows` 차이를 확인한다.
6. 32건 이상 삽입하며 `split_leaf`에, 더 많은 행을 삽입하며 `split_internal`에 중단점을 건다.
7. 프로세스를 종료하고 `rebuild_id_index`에 중단점을 건 뒤 같은 DB를 다시 연다.

테스트 자체가 사용 예제다. `test_engine()`은 API 사용과 재시작,
`test_script()`는 SQL 파일 분리, `test_concurrency()`는 공유 엔진의 동시 호출을 보여준다.
