"""Show concurrent API requests and verify their effects in a temporary database."""
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor
import http.client
import json
from pathlib import Path
import re
import select
import subprocess
import tempfile
import threading


def main():
    root = Path(__file__).resolve().parents[1]
    binary = root / "build/mini-dbms-server"
    if not binary.is_file():
        raise RuntimeError("먼저 make server를 실행해 주세요.")

    with tempfile.TemporaryDirectory(prefix="mini-dbms-live-demo-") as directory:
        process = subprocess.Popen(
            [str(binary), "--db", str(Path(directory) / "demo.db"), "--port", "0",
             "--workers", "4", "--queue", "64"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            if not select.select([process.stdout], [], [], 10)[0]:
                raise RuntimeError("실습 서버 시작 시간 초과")
            line = process.stdout.readline()
            match = re.search(r"LISTENING 127\.0\.0\.1:(\d+)", line)
            if not match:
                raise RuntimeError("실습 서버를 시작하지 못했습니다: " + line)
            port = int(match[1])

            def request(sql):
                connection = http.client.HTTPConnection("127.0.0.1", port, timeout=15)
                try:
                    connection.request("POST", "/sql", sql, {"Content-Type": "text/plain"})
                    response = connection.getresponse()
                    result = json.loads(response.read())
                    if response.status != 200:
                        raise RuntimeError(f"HTTP {response.status}: {result}")
                    return result
                finally:
                    connection.close()

            people = 24
            print("[1] 실습용 서버: 작업 스레드 4개, 새 임시 DB", flush=True)
            print("[2] 24명이 각각 다른 재생 기록을 동시에 저장 요청", flush=True)
            start = threading.Barrier(people)

            def save(person):
                start.wait(timeout=15)
                sql = ("INSERT INTO play_history (event_code,user_id,song_id,genre) "
                       f"VALUES ('DEMO{person:03d}',{person},108,'pop');")
                result = request(sql)
                if result["record"]["user_id"] != person:
                    raise RuntimeError("저장 응답의 사용자 번호가 요청과 다릅니다.")
                return person, result["worker_id"], result["record"]["id"]

            with ThreadPoolExecutor(max_workers=people) as clients:
                saved = list(clients.map(save, range(1, people + 1)))

            groups = defaultdict(list)
            for person, worker_id, record_id in saved:
                groups[worker_id].append(person)
            print("\n[3] 서버가 응답한 작업 담당 내역", flush=True)
            for worker_id in range(1, 5):
                persons = ", ".join(f"{person}번" for person in sorted(groups[worker_id])) or "없음"
                print(f"  작업 스레드 {worker_id}: {len(groups[worker_id]):2d}건 — 요청자 {persons}")
            print("  ※ 번호순으로 정리한 목록이며 실제 완료 순서는 다를 수 있습니다.")

            ids = [record_id for _, _, record_id in saved]
            rows = request("SELECT * FROM play_history;")["rows"]
            expected_codes = {f"DEMO{person:03d}" for person in range(1, people + 1)}
            if (sorted(ids) != list(range(1, people + 1)) or len(rows) != people or
                    {row["event_code"] for row in rows} != expected_codes):
                raise RuntimeError("저장 결과에 누락·중복·불일치가 있습니다.")

            def verify(item):
                person, _, record_id = item
                result = request(f"SELECT * FROM play_history WHERE id={record_id};")
                return (result["access_path"] == "bplus_tree" and result["matched"] == 1 and
                        result["rows"][0]["event_code"] == f"DEMO{person:03d}")

            with ThreadPoolExecutor(max_workers=people) as clients:
                if not all(clients.map(verify, saved)):
                    raise RuntimeError("B+ 트리 ID 조회 결과가 저장 요청과 다릅니다.")

            print("\n[4] 실제 DB와 인덱스 재확인")
            print("  저장 요청 24건 → 저장된 기록 24건")
            print("  ID 중복 0건 · ID 누락 0건")
            print("  B+ 트리로 다시 조회: 24건 모두 원래 요청과 일치")
            print("\n요청자 24명과 작업 스레드 4개는 서로 다른 개념입니다.")
            print("작업 스레드는 한 요청을 마치면 다음 요청을 맡습니다.")
        finally:
            process.terminate()
            try:
                _, error = process.communicate(timeout=12)
            except subprocess.TimeoutExpired:
                process.kill()
                process.communicate()
                raise RuntimeError("실습 서버 종료 시간 초과")
            if process.returncode != 0:
                raise RuntimeError(f"실습 서버 오류: {error}")
    print("\n실습용 서버와 임시 DB 정리 완료. 기존 data/ 파일은 변경하지 않았습니다.")


if __name__ == "__main__":
    main()
