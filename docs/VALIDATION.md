# 검증 기록

검증일: 2026-11-16. 이 문서는 C DB 엔진·CLI, 과제 Phase 1·2의 검증 기록이다.
이후 추가한 Phase 3 API와 HTTP 검증은 [API_SERVER.md](API_SERVER.md)에 기록했다.

## 환경

- Windows x64, OS build `10.0.26200`
- Intel Core i5-8265U @ 1.60 GHz, 논리 프로세서 8개
- 물리 메모리 약 15.55 GiB
- Visual Studio 2022 MSVC 14.41, C11, `/O2 /W4 /WX`
- 외부 패키지·외부 DB 라이브러리 없음

## 실행 결과

| 검증 | 명령 | 결과 |
|---|---|---|
| 빌드·C 테스트 | `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1 -Test` | 경고를 오류로 처리한 빌드 성공, 750,278 assertions 통과 |
| CLI 기능 | `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test-cli.ps1` | SQL 파일·재시작·인덱스·강제 스캔·오류 종료·표준 입력 통과 |
| 메모리 오류 | `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1 -Sanitize` | MSVC AddressSanitizer에서 같은 테스트 통과, 메모리 오류 보고 없음 |
| 대량 데이터 | `build/db_bench.exe build/benchmark-1000000 1000000 30` | 100만 건 삽입·재시작·조회 정확성 검증 통과 |

ASan 검증은 Windows MSVC에서 수행했다. Linux/macOS용 Makefile·CMake는 제공하지만
이 환경에서 Linux/macOS 실행이나 UBSan 검증을 수행했다고 주장하지 않는다.
ASan 실행 성공이 모든 메모리 누수·경쟁 상태·장애 상황의 부재를 증명하는 것은 아니다.

## 초기 구현의 100만 건 벤치마크

원본 출력: [benchmark-windows.txt](benchmark-windows.txt).
아래 수치는 동시성 최적화 이전의 기록이다. 현재 엔진의 동시 요청 결과는
[LOAD_TEST.md](LOAD_TEST.md)에 따로 기록했다.

| 항목 | 측정값 |
|---|---:|
| 레코드 수 | 1,000,000 |
| INSERT 배치 | 한 SQL 문장당 1,000 튜플 |
| INSERT 시간 | 5.202646 s |
| 재시작·인덱스 재구축 시간 | 0.983637 s |
| 파일 크기 | 208,000,016 bytes |
| B+ 트리 높이 | 5 |
| B+ 트리 노드 수 | 66,405 |
| B+ 트리 메모리 추정 | 53,128,048 bytes |

| 조회 | 중앙값 (ms) | p95 (ms) |
|---|---:|---:|
| ID B+ 트리 | 0.233950 | 0.456000 |
| 같은 ID의 강제 전체 스캔 | 389.609700 | 649.987000 |
| 고유 name의 전체 스캔 | 405.715950 | 561.420600 |

이 실행에서 동일 ID 조건의 중앙값 기준 인덱스 경로는 약 1,665배 빨랐다.
이는 특정 환경·워크로드의 측정값이며, 일반적인 성능 보장 수치가 아니다.

측정 조건:

- 기본 동기 쓰기 사용: INSERT 문장마다 `fflush` + `_commit`.
- 실제 SQL 파서·실행기를 사용해 데이터를 생성했다.
- 재시작한 엔진에서 전체 행 수가 100만 건인지 확인했다.
- 난수 시드 `20261116`, 세 경로 모두 3회 워밍업 후 30회 측정했다.
- 조회 순서를 번갈아 실행했다. 출력은 시간 측정에서 제외했다.
- 각 쿼리가 기대한 ID 1건을 반환하는지 콜백으로 확인했다.
- 인덱스 경로의 examined_rows는 1, 스캔 경로는 100만인지 확인했다.
- 파싱·잠금·파일 읽기·체크섬·결과 콜백은 측정에 포함했다.
- OS 페이지 캐시를 비우지 않은 warm-cache 비교다.
- 인덱스 메모리는 sizeof로 계산한 노드·예비 노드·트리 합이며 프로세스 RSS가 아니다.

재현 시 `build/benchmark-다른이름`처럼 새 경로를 지정하면 된다.
100만 건 파일은 `build/benchmark-1000000/users.db`에 남아 있다.
벤치마크는 기존 데이터가 있는 디렉터리에 덮어쓰거나 행을 추가하지 않는다.

## 알려진 범위와 한계

- 스키마는 미리 정의한 users 테이블 하나다. 여러 테이블용 카탈로그는 구현하지 않았다.
- B+ 트리는 메모리 기반이다. 실행마다 원본 파일을 순회해 재구축한다.
- 초기 구현의 동시 부하 병목을 수정했다. 짧은 인덱스 공개 잠금·writer mutex·위치 지정 읽기를 사용한다.
- 추가 검증에서는 751,426 assertions와 MSVC ASan 검사를 통과했다.
- 동시 부하의 변경 내용·실제 결과는 [LOAD_TEST.md](LOAD_TEST.md)를 참고한다.
- 여기의 엔진 동시성 테스트는 4 writers + 2 readers다. 별도로 Python API·Thread Pool과 HTTP 테스트를 추가했다.
- 체크섬 불일치, 손상 헤더, 불완전한 마지막 레코드는 열기를 거부한다.
- WAL, 트랜잭션, 크래시 시 배치 원자성, 백업 자동 복구는 구현하지 않았다.
- 디스크 부족·I/O 장애 주입 테스트, 강제 전원 차단 테스트는 수행하지 않았다.
- 정렬·JOIN·집계·NULL·UPDATE·DELETE는 지원하지 않는다.

파일 형식과 장애 처리·잠금 범위는 [ARCHITECTURE.md](ARCHITECTURE.md)를 참고한다.
