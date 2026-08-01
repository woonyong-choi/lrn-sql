# MiniDB vs PostgreSQL 16 벤치마크 결과

측정일: 2026-08-01. 모든 수치는 실제 실행 출력(`minidb_run.log`, `minidb_point10x.log`, `pg_fsync_on.log`, `pg_fsync_off.log`)에서 그대로 옮김.

## 조건

| 항목 | MiniDB | PostgreSQL |
|---|---|---|
| 버전 | /tmp/minidb (교육용 C 엔진) | PostgreSQL 16.13 (Ubuntu 16.13-0ubuntu0.24.04.1) |
| 빌드 | `gcc -O2 -Wall -Iinclude` **(sanitizer 제거, 원본 Makefile은 -fsanitize=address,undefined + -g)** | 배포판 바이너리 |
| 인터페이스 | REPL(`build-o2/minidb`) stdin 파이프, stdout → /dev/null | psycopg2, TCP 127.0.0.1, 단일 커넥션 |
| 트랜잭션 | 문장 단위 실행(트랜잭션/WAL 없음, 페이지는 캐시 축출·종료 시에만 flush) | autocommit=true (문장당 1 커밋) |
| fsync | 해당 없음 (fsync 자체가 없음) | fsync=on(기본) / fsync=off 두 가지, synchronous_commit=on |
| 동시성 | 단일 프로세스 순차 — 잠금 경합 없음 | 단일 커넥션 순차 |
| 데이터 | 단일 테이블, 10,000행, value BIGINT(+자동 id BIGINT), 고정 시드 동일 워크로드(workload.py) | id BIGINT PRIMARY KEY, value BIGINT, 동일 값 |
| 반복 | 시나리오별 3회, 중앙값 | 동일 |

MiniDB 측정 방식: 시나리오별로 REPL 프로세스를 새로 띄워 전체 벽시계 시간을 재고, no-op 실행(열기+`.exit`) 기준선 중앙값 3.6 ms를 뺐다. Point SELECT는 순수 시간(1.9 ms)이 기준선 노이즈에 가까워, 동일 쿼리 10,000회(10배) 보조 측정으로 2.50 µs/query(≈400k ops/s)를 확인했다.

파서 제약: MiniDB에 BETWEEN/AND가 없어 Range는 `SELECT * FROM bench WHERE id >= a LIMIT 1000`으로 대체(id가 1..10000 연속 + 힙이 id 순서라 반환 행은 BETWEEN a AND a+999와 동일). 단, 실행 경로는 인덱스 범위 스캔이 아니라 **힙 스캔 + LIMIT 조기 종료**다(B+tree는 `id = X`에만 사용됨, EXPLAIN으로 확인). 행 수 축소는 불필요했다(10,000행 그대로).

## 결과 (중앙값, ops/sec)

| 시나리오 | MiniDB (-O2) | PG fsync=on | PG fsync=off |
|---|---:|---:|---:|
| INSERT 10,000행 (개별 문장) | **379,783** (26.3 ms) | 739 (13,534 ms) | 10,592 (944 ms) |
| Point SELECT `id = ?` ×1,000 | **≈400,000** (2.50 µs/q, 보조측정; 본측정 1.9 ms/1000회) | 10,724 (93.3 ms) | 10,981 (91.1 ms) |
| Range 1,000행 ×100 | 2,026 (49.4 ms) | 1,724 (58.0 ms) | **2,077** (48.1 ms) |
| Full scan 10,000행 ×10 | **342** (29.2 ms) | 235 (42.5 ms) | 257 (38.9 ms) |

## 해석 (3줄)

1. INSERT/Point에서 MiniDB가 36~514배 빠른 것은 엔진이 우수해서가 아니라 **WAL·내구성·MVCC·네트워크 프로토콜이 아예 없기 때문**이다 — PG fsync=on의 INSERT 13.5초는 커밋마다 WAL fsync를 하는 비용이고, fsync=off로도 남는 ~36배 차이는 TCP 왕복 + 문장별 parse/plan 비용이다.
2. Range에서는 차이가 사라진다(2,026 vs 2,077 ops/s, fsync=off 기준 PG가 근소 우위) — MiniDB의 범위 질의는 O(테이블 크기) 힙 스캔이라 데이터가 커지면 급격히 밀리고, PG는 인덱스 범위 스캔으로 O(log N + 반환 행)이다.
3. 10,000행(≈160 KB)은 전부 캐시에 들어가는 규모라, 이 벤치마크는 "커널 캐시 위 함수 호출 vs 완전한 DBMS 프로토콜 스택"의 비교이지 스토리지 엔진 성능 비교가 아니다.

## 불공정 요소

- **WAL/내구성 부재**: MiniDB 커밋은 메모리 쓰기일 뿐, dirty 페이지는 종료·축출 시에만 디스크에 감. PG 기본(fsync=on) 비교는 근본적으로 불공정하며, fsync=off조차 PG는 WAL 레코드를 여전히 기록한다.
- **네트워크/프로토콜 스택 차이**: MiniDB는 같은 프로세스 stdin 파이프 + /dev/null 출력, PG는 TCP loopback + psycopg2 파싱, prepared statement 미사용(매 쿼리 parse/plan).
- **Range 실행 경로가 다름**: PG는 진짜 `BETWEEN`(인덱스 범위 스캔), MiniDB는 `id >= a LIMIT 1000`(힙 스캔 조기 종료). 반환 행만 동일.
- **자료형/행 포맷 차이**: MiniDB 고정 16바이트 행·단일 테이블·id 자동 할당, PG는 24바이트 튜플 헤더 + MVCC 버전 관리 + 시스템 카탈로그.
- **PG INSERT는 배치/COPY 미사용**: autocommit 개별 INSERT는 PG에 최악의 패턴(단일 트랜잭션 배치나 COPY면 수십 배 빨라짐). MiniDB의 문장 단위 실행과 맞추기 위한 선택.
- **규모가 작음**: 전 데이터가 캐시 상주. PG의 강점(대용량, 동시성, 복구, 옵티마이저)이 전혀 발휘되지 않는 조건.
- **MiniDB 측정에 프로세스 기동/종료 포함** → 기준선(3.6 ms) 차감으로 보정했으나 Point 시나리오는 노이즈 비중이 커서 10배 보조 측정으로 확인.
- **REPL 경로는 SELECT 시 row/range lock을 잡지 않음**(잠금은 서버 모드의 `db_execute` 래퍼에 있음; INSERT의 gap-check lock만 실행됨). 단일 커넥션이라 결과에는 영향 없음.

## 원시 로그 요약

```
MiniDB (bench_minidb.py):
rep 0: baseline=3.0ms insert=29.9ms point=5.5ms range=51.6ms full=32.0ms
rep 1: baseline=3.9ms insert=26.0ms point=5.6ms range=52.9ms full=32.8ms
rep 2: baseline=3.6ms insert=31.9ms point=5.3ms range=55.2ms full=34.6ms
point 10x 보조측정: net 25.01 ms / 10,000회 = 2.50 us/query

PostgreSQL fsync=on:
rep 0: insert=13533.9ms point=93.3ms range=59.5ms full=42.5ms
rep 1: insert=13688.5ms point=110.4ms range=58.0ms full=47.9ms
rep 2: insert=13159.9ms point=89.1ms range=52.9ms full=36.7ms

PostgreSQL fsync=off:
rep 0: insert=944.1ms point=91.1ms range=48.1ms full=40.7ms
rep 1: insert=858.5ms point=88.6ms range=53.4ms full=38.9ms
rep 2: insert=966.1ms point=91.1ms range=46.3ms full=37.9ms
```
