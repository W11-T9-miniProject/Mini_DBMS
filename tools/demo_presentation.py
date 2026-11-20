"""A two-minute live demo of the real DBMS API using an isolated dataset copy."""
import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager
import http.client
import json
import os
from pathlib import Path
import re
import select
import shutil
import subprocess
import sys
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[1]


def program(name, variable):
    path = Path(os.environ.get(variable, str(ROOT / "build" / name))).resolve()
    if not path.is_file():
        raise RuntimeError("먼저 make server benchmark를 실행해 주세요.")
    return str(path)


def dataset(directory):
    try:
        info = json.loads((directory / "dataset.json").read_text())
        count = info["records"]
        if (info.get("format") != "presentation-v1" or type(count) is not int or
                not 1 <= count <= 1_000_000 or (directory / "source.db").stat().st_size != 32 + count*92):
            raise ValueError("invalid metadata")
        return info
    except (OSError, ValueError, KeyError) as error:
        raise RuntimeError("발표용 원본을 확인할 수 없습니다. prepare를 실행하거나 새 --directory를 지정하세요.") from error


def prepare(directory, records):
    directory = Path(directory)
    if not 1 <= records <= 1_000_000:
        raise RuntimeError("records 범위는 1..1000000입니다.")
    source = directory / "source.db"
    metadata = directory / "dataset.json"
    if source.exists() or metadata.exists():
        info = dataset(directory)
        if info["records"] != records:
            raise RuntimeError("다른 크기의 데이터가 있습니다. 새 --directory를 지정하세요.")
        print(f"준비된 원본 {records:,}건을 그대로 사용합니다.")
        return info
    benchmark = program("benchmark", "MINI_DBMS_BENCHMARK")
    directory.mkdir(parents=True, exist_ok=True)
    print(f"실제 SQL INSERT로 {records:,}건 준비 중입니다. 발표 전에 한 번 실행하세요.", flush=True)
    with tempfile.TemporaryDirectory(prefix="mini-dbms-prepare-") as temporary:
        sql = Path(temporary) / "load.sql"
        built = Path(temporary) / "built.db"
        subprocess.run([sys.executable, str(ROOT / "tools/generate_history.py"),
                        "--count", str(records), "--output", str(sql)], check=True)
        completed = subprocess.run([benchmark, str(sql), str(built), "1"],
                                   capture_output=True, text=True, check=True)
        result = json.loads(completed.stdout)
        if result["records"] != records or built.stat().st_size != 32+records*92:
            raise RuntimeError("준비 데이터 검증 실패")
        # Build on Linux /tmp, then copy once: avoid a million writes to a Windows bind mount.
        with built.open("rb") as input_file, source.open("xb") as output_file:
            shutil.copyfileobj(input_file, output_file, length=1024*1024)
            output_file.flush()
            os.fsync(output_file.fileno())
    info = {"format": "presentation-v1", "records": records, "file_bytes": source.stat().st_size}
    with metadata.open("x") as output:
        json.dump(info, output, indent=2)
    print(f"준비 완료: {records:,}건 / {source.stat().st_size:,}바이트")
    print("run은 이 원본을 바꾸지 않고 임시 복사본으로 시연합니다.")
    return info


class Api:
    def __init__(self, port):
        self.port = port

    def query(self, sql):
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=60)
        start = time.perf_counter()
        try:
            connection.request("POST", "/sql", sql, {"Content-Type": "text/plain"})
            response = connection.getresponse()
            result = json.loads(response.read())
            elapsed = (time.perf_counter()-start)*1000
            if response.status != 200:
                raise RuntimeError(f"HTTP {response.status}: {result}")
            return result, elapsed
        finally:
            connection.close()


@contextmanager
def running_server(database):
    binary = program("mini-dbms-server", "MINI_DBMS_SERVER")
    process = subprocess.Popen([binary, "--db", str(database), "--port", "0", "--workers", "4", "--queue", "64"],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        if not select.select([process.stdout], [], [], 30)[0]:
            raise RuntimeError("실습 서버 시작 시간 초과")
        line = process.stdout.readline()
        match = re.search(r"LISTENING 127\.0\.0\.1:(\d+)", line)
        if not match:
            raise RuntimeError("실습 서버 시작 실패: " + line)
        yield Api(int(match[1]))
    finally:
        process.terminate()
        try:
            _, error = process.communicate(timeout=12)
        except subprocess.TimeoutExpired:
            process.kill(); process.communicate()
            raise RuntimeError("실습 서버 종료 시간 초과")
        if process.returncode != 0:
            raise RuntimeError("실습 서버 오류: " + error)


def pause(title, auto):
    if not auto:
        input(f"\nEnter → {title} ")
        if sys.stdout.isatty():
            print("\033[2J\033[H", end="", flush=True)
    print(f"\n{title}", flush=True)


def run_demo(directory, auto=False):
    directory = Path(directory)
    count = dataset(directory)["records"]
    print(f"발표용 DBMS 데모 — 원본 {count:,}건, 작업 스레드 4개", flush=True)
    with tempfile.TemporaryDirectory(prefix="mini-dbms-stage-") as temporary:
        clone = Path(temporary) / "demo.db"
        shutil.copyfile(directory / "source.db", clone)
        with running_server(clone) as api:
            pause("① SQL을 HTTP로 보내 저장하고 다시 조회", auto)
            sql = "INSERT INTO play_history (event_code,user_id,song_id,genre) VALUES ('LIVE_DEMO',71,108,'jazz');"
            print("요청: 사용자 71 / 노래 108 / 장르 jazz 저장")
            saved, _ = api.query(sql)
            record = saved["record"]
            if record["id"] != count+1:
                raise RuntimeError("예상하지 못한 새 ID")
            found, _ = api.query(f"SELECT * FROM play_history WHERE id={record['id']};")
            if found["rows"] != [record]:
                raise RuntimeError("저장과 재조회 불일치")
            print(f"DBMS가 부여한 기록 ID: {record['id']:,}")
            print(f"같은 ID로 다시 조회: 사용자 {record['user_id']} / 노래 {record['song_id']} / {record['genre']}")
            print("연결: 외부 HTTP 요청 → SQL 해석 → 파일 저장 → 인덱스 등록")

            pause("② 같은 기록을 B+ 트리와 전체 탐색으로 검색", auto)
            target = max(1, count//2)
            indexed, index_ms = api.query(f"SELECT * FROM play_history WHERE id={target};")
            linear, linear_ms = api.query(f"SELECT * FROM play_history WHERE event_code='PLAY{target:09d}';")
            if (indexed["rows"] != linear["rows"] or indexed["matched"] != 1 or linear["matched"] != 1 or
                    indexed["scanned"] != 1 or linear["scanned"] != count+1 or
                    indexed["access_path"] != "bplus_tree" or linear["access_path"] != "full_scan"):
                raise RuntimeError("검색 경로·결과 검증 실패")
            print(f"검색 대상: ID {target:,} / PLAY{target:09d}")
            print("두 방식이 반환한 기록: 동일한 1개")
            print(f"ID 인덱스  : 파일 기록 {indexed['scanned']:,}건 읽음 / 이번 요청 {index_ms:.3f}ms")
            print(f"전체 탐색  : 파일 기록 {linear['scanned']:,}건 읽음 / 이번 요청 {linear_ms:.3f}ms")
            print("연결: RAM의 인덱스에서 위치를 찾으면 모든 기록을 읽을 필요가 없습니다.")
            print("시간은 이번 1회 HTTP 왕복·클라이언트 해석 포함 값입니다. 내부 검색 벤치마크와 다릅니다.")

            pause("③ 요청 24개를 작업 스레드 4개가 처리", auto)
            start = threading.Barrier(24)

            def write(person):
                start.wait(timeout=20)
                answer, _ = api.query("INSERT INTO play_history (event_code,user_id,song_id,genre) "
                                      f"VALUES ('CLIENT{person:02d}',{person},108,'pop');")
                if answer["record"]["user_id"] != person:
                    raise RuntimeError("동시 저장 응답 불일치")
                return person, answer["worker_id"], answer["record"]["id"]

            with ThreadPoolExecutor(max_workers=24) as clients:
                completed = list(clients.map(write, range(1, 25)))
            ids = sorted(item[2] for item in completed)
            if ids != list(range(count+2, count+26)):
                raise RuntimeError("동시 저장 ID 누락 또는 중복")
            workers = Counter(item[1] for item in completed)
            for worker in range(1, 5):
                print(f"작업 스레드 {worker}: {workers[worker]}건 처리")
            for person, _, record_id in completed:
                answer, _ = api.query(f"SELECT * FROM play_history WHERE id={record_id};")
                if answer["matched"] != 1 or answer["rows"][0]["event_code"] != f"CLIENT{person:02d}":
                    raise RuntimeError("동시 저장 후 인덱스 조회 불일치")
            print("저장 24건 / ID 중복·누락 0건 / ID 재조회 24건 모두 일치")
            print("연결: 스레드 풀은 요청 분배, 쓰기 락은 파일·인덱스 변경을 단독으로 보호합니다.")
            print("여러 INSERT가 동시에 파일을 바꿨다는 뜻은 아닙니다.")
            result = {"base_records": count, "indexed_scanned": indexed["scanned"],
                      "linear_scanned": linear["scanned"], "indexed_http_ms": index_ms,
                      "linear_http_ms": linear_ms, "concurrent_inserted": 24, "verified": 24}
    print("\n시연 종료. 임시 서버·복사본 정리 완료, 발표용 원본과 기존 연습 DB는 그대로입니다.")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("prepare", "run"))
    parser.add_argument("--directory", type=Path, default=ROOT / "data/presentation_dbms")
    parser.add_argument("--records", type=int, default=1_000_000)
    parser.add_argument("--auto", action="store_true", help="pause 없이 전체 실행: 검증·리허설용")
    args = parser.parse_args()
    try:
        if args.mode == "prepare":
            prepare(args.directory, args.records)
        else:
            run_demo(args.directory, args.auto)
    except (RuntimeError, OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"데모 중단: {error}\n")


if __name__ == "__main__":
    main()
