# Redis Market — 상품 조회 실험실

직접 구현한 C Mini Redis를 상품 캐시로, Mini DBMS의 `products` 테이블을 원본 저장소로 사용하는 로컬 웹 서비스입니다. 상품 조회는 Redis를 먼저 확인하고, 캐시 미스면 DBMS에서 읽어 Redis에 TTL과 함께 채웁니다. 비교 화면은 캐시를 비운 조회와 이어지는 캐시 적중 조회의 시간을 기록하고 CSV로 내려받습니다.

## 실행

Node.js 18 이상과 C 컴파일러, Lua가 필요합니다. `/path/to/...`는 자신의 체크아웃 경로로 바꾸세요.

먼저 이 저장소의 C Redis를 빌드합니다.

```sh
cd /path/to/Mini_DBMS/c-mini-redis
make
```

macOS에서는 `brew install lua`로 Lua를 설치할 수 있습니다. 다른 환경은 `make LUA_PREFIX=/path/to/lua`로 설치 위치를 지정하세요. DBMS 서버 소스는 이 업로드에 포함하지 않으므로 기존 Mini DBMS 소스를 별도로 준비해야 합니다.

터미널 1 — Mini DBMS API와 원본 테이블:

```sh
cd /path/to/mini_dbms
make mini_dbms_server
mkdir -p /tmp/mini-db-api
cp examples/db/people.schema /tmp/mini-db-api/people.schema
cp /path/to/Mini_DBMS/product-demo/products.schema /tmp/mini-db-api/products.schema
./mini_dbms_server /tmp/mini-db-api 8080 4 64
```

이미 DBMS가 실행 중이면 데이터 디렉터리에 `products.schema`를 추가한 뒤 사용할 수 있습니다. 기존 people 테이블과 파일은 그대로 유지됩니다. 상품 테이블은 `id, name, category, price, kind, color, cache_ttl` 순서의 컬럼을 사용합니다.

터미널 2 — Redis Market:

```sh
cd /path/to/Mini_DBMS/product-demo
npm start
```

http://127.0.0.1:3000 에 접속하세요. C Redis는 기본 16380 포트로 실행되고 `.redis-data/`에 데이터를 저장합니다. `Ctrl+C`는 웹 서버와 이 명령이 시작한 Redis를 함께 종료합니다. Mini DBMS는 별도 터미널에서 실행하므로 따로 종료합니다.

포트 변경: `WEB_PORT=3001 REDIS_PORT=6381 npm start`. DBMS 포트 변경: `DBMS_PORT=8081 npm start` 또는 `DBMS_URL=http://127.0.0.1:8081/query`.

## 시연 순서

1. **DBMS 샘플 등록**을 눌러 예제 상품을 원본 `products` 테이블에 넣습니다. 이미 있는 ID는 중복 삽입하지 않습니다.
2. ID `1001`을 조회합니다. Redis MISS → DBMS SELECT → Redis SET → 상품 반환 흐름을 봅니다.
3. 같은 ID를 다시 조회합니다. Redis HIT로 반환되고 DBMS 조회는 생략됩니다.
4. **전후 비교**를 누르면 해당 캐시 키를 먼저 지우고 MISS 경로와 바로 뒤 HIT 경로의 시간을 표에 기록합니다. **CSV 다운로드**로 기록을 저장할 수 있습니다.
5. 상품 원본 등록은 DBMS INSERT 후 Redis 키를 지웁니다. 다음 상품 조회가 DBMS에서 다시 읽어 캐시를 채웁니다.
6. 상품 카드의 × 버튼은 Redis 키만 지웁니다. DBMS 원본은 남아 있으므로 다음 조회에서 자동으로 다시 채워집니다. DBMS 원본에서 상품을 찾을 수 없으면 HTTP 404를 돌려줍니다.
7. 상품별 TTL은 DBMS `cache_ttl` 컬럼에 저장하고 Redis 캐시 만료에 사용합니다. 샘플은 등록할 때 입력한 TTL(기본 120초)을 씁니다.
8. **Mini DBMS SQL 콘솔**에서 직접 SQL을 실행할 수도 있습니다. 상품 원본 조회 예: `SELECT * FROM products WHERE id = 1001;`.

## 요청과 지표

브라우저 요청은 같은 출처의 Node 웹 서버를 거쳐 Redis RESP2 TCP와 Mini DBMS HTTP API에 전달됩니다. 상품 HTTP API는 `/api/products/:id`이며 비교 API는 `/api/products/:id?compare=1`입니다. `POST /api/seed`는 DBMS에 예제 원본을 준비하고 해당 Redis 키들을 무효화합니다. `POST /api/products/:id`는 DBMS 원본을 추가한 뒤 캐시를 무효화합니다. `DELETE /api/products/:id`는 캐시만 삭제합니다.

상품 조회 지표는 사용자 요청 기준이며 비교 한 번은 MISS와 HIT 각 1회로 기록합니다. 응답 시간은 웹 서버 안에서 측정한 Redis/DBMS 요청 시간입니다. 비교 기록과 명령 로그는 웹 프로세스 메모리에 보관되어 서버 재시작 시 초기화됩니다. 사이트는 로컬 개발과 시연을 위한 작은 데이터셋을 대상으로 합니다.

## 검증

```sh
npm test
```
