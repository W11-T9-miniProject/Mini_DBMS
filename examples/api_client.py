"""Run with the API server listening on 127.0.0.1:8080."""

import json
import os
import urllib.request


def execute(sql):
    headers = {"Content-Type": "application/json"}
    if os.environ.get("MINIDB_API_TOKEN"):
        headers["Authorization"] = "Bearer " + os.environ["MINIDB_API_TOKEN"]
    body = json.dumps({"sql": sql}, ensure_ascii=False).encode("utf-8")
    request = urllib.request.Request("http://127.0.0.1:8080/query", data=body, headers=headers)
    with urllib.request.urlopen(request, timeout=10) as response:
        return json.load(response)


if __name__ == "__main__":
    print(json.dumps(execute("INSERT INTO users(name,age) VALUES('API user',25)"), indent=2))
    print(json.dumps(execute("SELECT * FROM users WHERE id=1"), ensure_ascii=False, indent=2))
