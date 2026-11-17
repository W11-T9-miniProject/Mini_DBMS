# Mini DBMS · Redis Market

직접 구현한 C Mini Redis와 상품 조회 웹서비스입니다. 현재 웹서비스는 별도로 실행하는 Mini DBMS HTTP API와 연동합니다. DBMS 엔진 소스는 이 커밋에 포함하지 않습니다.

## 구성

- `c-mini-redis/`: C11 RESP2 서버, TTL, 자료구조, RDB, Lua와 테스트
- `product-demo/`: Node.js 웹 서버와 상품 조회 화면, DBMS SQL 콘솔, 캐시 전후 비교, CSV 다운로드
- `product-demo/products.schema`: Mini DBMS의 상품 테이블 스키마

## 실행

```sh
cd c-mini-redis
make
cd ../product-demo
npm start
```

웹페이지: http://127.0.0.1:3000. Redis 기본 포트: 16380. DBMS API 기본 주소: http://127.0.0.1:8080/query.

상품 등록과 캐시 MISS 처리를 위해 Mini DBMS API를 별도로 실행하고 상품 스키마를 준비하세요. 상세 설정과 시연 순서는 [웹서비스 실행 안내](product-demo/README.md), Redis 빌드와 명령은 [C Redis 안내](c-mini-redis/README.md)를 참고하세요.

## 검증

```sh
cd product-demo
npm test
```

웹 테스트는 실제 C Redis와 테스트용 DBMS HTTP 응답 서버를 사용합니다. 실제 DBMS 엔진의 동작을 검증하는 테스트는 아닙니다. 실행 데이터, 컴파일 결과와 개인 환경 파일은 버전 관리에서 제외합니다.
