# MiniDB 동시성 제어 이슈 및 해결 기록

## 1. Head-of-Line Blocking (해결됨)

### 현상
고정 크기 스레드 풀(4 workers) + keep-alive 조합에서 발생.
keep-alive 연결 16개가 들어오면 worker 4개가 fd 4개에 묶여 나머지 12개는 큐에서 대기.

### 원인
스레드 풀에서 worker가 keep-alive 연결을 처리하는 동안 다른 연결을 받지 못함.
한 worker가 하나의 fd에 물려있으면 해당 worker는 keep-alive가 끊길 때까지 다른 일을 못 함.

### 해결
thread-per-connection 모델로 전환 (MySQL 방식).
- accept() 시 연결마다 스레드 생성 + pthread_detach
- MAX_CONNECTIONS=128로 동시 연결 수 제한 (128 × 8MB stack ≈ 1GB)
- 스레드 풀 코드(thread_pool.c/h) 제거

### 관련 파일
- `src/server/server.c` — 완전 재작성
- `Makefile` — thread_pool.c 제거

---

## 2. Heap 페이지 래치 누락으로 인한 데드락 (해결됨)

### 현상
heap 연산(table.c)이 `pager_get_page()`로 래치 없이 페이지에 접근.
B+ tree는 래치 커플링이 적용되어 있었으나, heap은 보호 없이 동시 접근 가능한 상태.

### 원인
- `heap_fetch()`: 읽기 래치 없이 페이지 데이터 반환 → torn read 가능
- `heap_insert()`: 쓰기 래치 없이 슬롯/데이터 수정 → 동시 삽입 시 슬롯 충돌
- `heap_delete()`: 쓰기 래치 없이 슬롯 상태 변경

### 해결
모든 heap 연산에 적절한 래치 적용:
- 읽기: `pager_get_page()` → `pager_get_page_rlatch()`, `pager_unpin()` → `pager_unlatch_r()`
- 쓰기: `pager_get_page()` → `pager_get_page_wlatch()`, `pager_unpin()` → `pager_unlatch_w()`
- DDL(DROP TABLE): wrlock 아래이므로 래치 불필요 (의도적 유지)

### 파생 이슈: test_all.c 데드락
heap_fetch가 rlatch를 반환하도록 변경 후, test_all.c가 여전히 `pager_unpin()`을 호출.
rlatch가 해제되지 않아 다음 접근에서 wlatch 대기 → 데드락.
`pager_unpin` → `pager_unlatch_r` 로 교체하여 해결.

### 관련 파일
- `src/storage/table.c` — 전체 heap 연산에 래치 추가
- `src/sql/executor.c` — heap_fetch 후 unlatch_r 사용
- `tests/test_all.c` — pager_unpin → pager_unlatch_r 교체

---

## 3. Writer Starvation (알려진 이슈, 미해결)

### 현상
S lock이 보유된 상태에서 X lock 요청이 대기 중일 때, 새로운 S lock 요청이 대기 중인 X를 무시하고 바로 진입한다.

### 상세 시나리오
```
1. 스레드 A: S lock 획득 (id=1)
2. 스레드 B: X lock 요청 (id=1) → S와 충돌, 대기
3. 스레드 C: S lock 요청 (id=1) → 보유 중인 lock은 S뿐이므로 바로 획득
4. 스레드 D: S lock 요청 (id=1) → 마찬가지로 바로 획득
5. 스레드 B: S가 계속 추가되므로 무한 대기 가능
```

### 원인 — conflict_exists() 구현 방식
```c
static int conflict_exists(lock_table_t *lt, uint64_t row_id,
                           lock_mode_t mode, pthread_t self) {
    // lock table에 이미 올라가 있는 엔트리만 검사
    // → "대기 중인" lock은 보이지 않음
    // → S + S는 항상 호환으로 판정
}
```
lock_acquire()에서 lock table 엔트리는 **획득 성공 후에만** 추가된다. 대기 중인 X lock은 엔트리가 없으므로 conflict_exists()에 보이지 않는다.

### 호환성 정리
| 보유 \ 요청 | S (새 요청) | X (새 요청) |
|---|---|---|
| S 보유 | 바로 허용 | 대기 |
| X 보유 | 대기 | 대기 |
| X 대기 중 (미보유) | **바로 허용 (문제)** | 대기 불가 (X 미보유) |

### 해결 방안 (향후)

**방안 1: 대기 큐 도입 (Fair Locking)**
대기 중인 lock 요청을 큐에 등록하고, 새 S 요청은 대기 큐에 X가 있으면 함께 대기.
```
// 의사 코드
if (대기큐에 X 요청이 있고 && 새 요청이 S) {
    새 S도 대기큐 뒤에 넣는다
}
```
장점: 완전한 공정성. 단점: 구현 복잡도 증가, 처리량 감소.

**방안 2: X 우선순위 (Writer-Preferring)**
X lock 요청이 대기 중이면 새 S lock을 막는다. `pthread_rwlock`의 PTHREAD_RWLOCK_PREFER_WRITER와 유사.
```c
// lock_table에 waiting_writers 카운터 추가
if (waiting_writers > 0 && mode == LOCK_S) {
    // S도 대기
}
```
장점: 간단한 구현. 단점: reader starvation 가능성 (역전).

**방안 3: Timeout으로 완화 (현재)**
현재 3초 timeout이 있으므로 무한 대기는 아님. Writer가 3초 안에 X를 획득하지 못하면 timeout 에러를 반환. 실용적 수준에서 동작하지만 이론적으로 불완전.

### 현재 상태
방안 3(timeout)으로 운용 중. W08 과제 범위에서는 충분하나, 프로덕션 수준에서는 방안 1 또는 2가 필요.

---

## 4. INSERT Gap Check Hang (해결됨)

### 현상
Range Lock 구현 후, INSERT의 phantom 방지를 위해 exec_insert() 내부에서 `lock_acquire(lt, my_id, LOCK_X)`를 무조건 호출하도록 추가. 동시 INSERT 테스트(4스레드 × 25건)에서 hang 발생.

### 환경
- 4 스레드가 각각 고유 id로 INSERT
- 각 INSERT마다 새 id에 대해 X point lock 획득
- 모든 id가 다르므로 이론적 충돌 없음

### 원인 분석
lock_acquire 내부에서 conflict_exists()가 point_vs_point + point_vs_range를 검사.
range_locks는 NULL이므로 point_vs_range는 즉시 반환. point_vs_point도 다른 id이므로 충돌 없음.

추정 원인: ASAN 환경에서 lock_table mutex 경합이 과도하게 발생. 256개 버킷을 매번 순회하는 lock_release_all + 각 INSERT의 lock_acquire가 같은 mutex를 경합하면서, 4스레드 × 25회의 lock/unlock 싸이클이 ASAN의 메모리 접근 지연과 맞물려 실질적 livelock에 빠진 것으로 추정.

### 해결
range_locks가 존재할 때만 gap check를 수행하도록 최적화:
```c
// 수정 전: 무조건 lock_acquire
lock_acquire(lt, my_id, LOCK_X);

// 수정 후: range lock이 있을 때만 gap check
if (lt->range_locks != NULL) {
    lock_acquire(lt, my_id, LOCK_X);
}
```
range lock이 없으면 phantom insert가 발생할 범위 조건 자체가 없으므로, gap check를 건너뛰는 것이 의미상 정확하다.

### 관련 파일
- `src/sql/executor.c` — exec_insert() 내 gap check 조건부 실행

---

## 5. Heap 체인 레이스 + 페이지 오버플로 (해결됨)

### 현상
test_step2의 `test_concurrent_insert` (4스레드 × 25건)에서 COUNT(*)가 간헐적으로 100 미만을 반환.
g_success_count == 100은 통과 (INSERT 자체는 전부 성공)하지만, heap_scan으로 세면 누락 발생.

### 원인 1: 힙 체인 링크 레이스

`heap_insert`에서 새 힙 페이지를 할당할 때 `pager->last_heap_page_id`의 읽기→쓰기가 lock 없이 수행.
```
Thread A: find_heap_page → 0 (가득 참)
Thread B: find_heap_page → 0 (가득 참)
Thread A: alloc page 5, page1→next=5, last_heap=5
Thread B: alloc page 6, page1→next=6 (덮어씀!), last_heap=6
→ page 5 고아 → heap_scan에서 누락
```

### 원인 2: TOCTOU 페이지 오버플로

`find_heap_page()`가 rlatch 하에서 공간을 확인하고 반환한 뒤, `heap_insert()`가 wlatch를 잡기 전에 다른 스레드가 페이지를 채울 수 있다.
```
Thread A: find_heap_page → page 1 (slot 77/78 사용, 1칸 남음)
Thread B: find_heap_page → page 1 (같은 상태)
Thread A: wlatch(page 1), slot 78 삽입, unlatch
Thread B: wlatch(page 1), slot_count=78, 빈 슬롯 없음
  → else(새 슬롯) 분기 진입, 공간 검사 없이 row_offset 계산
  → slots_end(648) > row_offset(620): 슬롯 디렉터리와 행 데이터 겹침
  → 기존 슬롯 메타데이터 훼손 → heap_scan에서 일부 행 누락
```

### 해결

**원인 1**: `header_lock`으로 힙 체인 수정 구간(alloc + link + last_heap_page_id 갱신) 직렬화.
```c
pthread_mutex_lock(&pager->header_lock);
pid = pager_alloc_page(pager);
/* ... 새 페이지 초기화 + 체인 연결 ... */
pager->last_heap_page_id = pid;
pthread_mutex_unlock(&pager->header_lock);
```

**원인 2**: wlatch 획득 후 공간 재확인. 부족하면 새 페이지 할당 후 재시도.
```c
retry_insert:
    page = pager_get_page_wlatch(pager, pid);
    if (free_slot_head == NONE && available_space < need) {
        pager_unlatch_w(pager, pid);
        /* header_lock 아래에서 새 페이지 할당 + 체인 연결 */
        goto retry_insert;
    }
    /* 삽입 진행 */
```

Lock 순서: `header_lock → page wlatch` (단방향). 일반 삽입 경로는 wlatch 해제 후 header_lock을 획득하므로 역방향 없음.

### 관련 파일
- `src/storage/table.c` — heap_insert() 전면 수정

---

## 6. 현재 동시성 제어 아키텍처 요약

```
Level 4 — Row Lock + Range Lock (Strict 2PL, 3초 timeout)
  Point Lock: WHERE id=X → S/X lock
  Range Lock: WHERE id>X, id<X 등 → [low, high] 범위 lock
  Gap Check: INSERT 시 range_locks 존재하면 새 id에 X lock

Level 3 — Engine RWLock
  DML → rdlock (동시 실행)
  DDL → wrlock (독점)

Level 2 — Page Latch (프레임별 rwlock)
  B+ tree: 래치 커플링 (crab protocol)
  Heap: 읽기=rlatch, 쓰기=wlatch

Level 1 — Pager Mutex
  프레임 메타데이터 보호 (pin, tick, dirty)

Header Lock — pager->header_lock
  next_id, row_count 원자적 갱신
```
