/*
 * page_format.h — 디스크 페이지 형식 및 핵심 구조체 정의
 *
 * 역할:
 *   minidb의 모든 온디스크 구조체(DB 헤더, 힙 페이지, B+ tree 노드, 빈 페이지)를
 *   정의한다. __attribute__((packed))로 컴파일러 패딩을 제거하여 구조체 크기가
 *   디스크에 기록되는 바이트와 정확히 일치하도록 보장한다.
 *
 * 페이지 크기:
 *   sysconf(_SC_PAGESIZE)로 결정 — x86_64: 4096바이트, Apple Silicon: 16384바이트
 *
 * 파일 레이아웃 (page_size = 4096 기준):
 *   오프셋      | 페이지 ID | 용도
 *   ────────────┼───────────┼────────────────────
 *   0x0000      | page 0    | DB 헤더 (db_header_t)
 *   0x1000      | page 1    | 첫 번째 힙 페이지
 *   0x2000      | page 2    | B+ tree 루트 (초기에는 리프)
 *   0x3000      | page 3    | 추가 힙/리프/내부 노드...
 *   ...         | ...       | ...
 */

#ifndef PAGE_FORMAT_H
#define PAGE_FORMAT_H

#include <stdint.h>
#include <stdbool.h>

/* ── 페이지 유형 코드 ──
 * 모든 페이지의 첫 4바이트(page_type)로 페이지 종류를 식별한다.
 *
 * 페이지를 읽을 때 첫 4바이트만 memcpy하면 유형을 알 수 있다:
 *   uint32_t ptype;
 *   memcpy(&ptype, page, sizeof(uint32_t));
 *   switch (ptype) { ... }
 */
typedef enum {
    PAGE_TYPE_HEADER   = 0x01,  /* DB 헤더 페이지 (page 0 전용) */
    PAGE_TYPE_HEAP     = 0x02,  /* 힙 데이터 페이지 (행 저장) */
    PAGE_TYPE_LEAF     = 0x03,  /* B+ tree 리프 노드 */
    PAGE_TYPE_INTERNAL = 0x04,  /* B+ tree 내부 노드 */
    PAGE_TYPE_FREE     = 0x05   /* 빈 페이지 (재활용 대기) */
} page_type_t;

/* ── 상수 정의 ── */
#define DB_MAGIC    "MINIDB\0"  /* 매직 넘버: 파일의 첫 8바이트로 minidb 파일인지 식별 */
#define DB_VERSION  1           /* DB 파일 형식 버전 */
#define MAX_COLUMNS 16          /* 테이블당 최대 컬럼 수 */

/* ── 컬럼 데이터 타입 ──
 * 각 타입의 직렬화 크기:
 *   COL_TYPE_INT     → 4바이트  (int32_t)
 *   COL_TYPE_BIGINT  → 8바이트  (int64_t)
 *   COL_TYPE_VARCHAR → N바이트  (CREATE TABLE에서 지정한 크기)
 *
 * 예시: CREATE TABLE users (name VARCHAR(32), age INT)
 *   id   → BIGINT, size=8,  offset=0   (시스템 컬럼, 자동 추가)
 *   name → VARCHAR, size=32, offset=8
 *   age  → INT,     size=4,  offset=40
 *   → row_size = 8 + 32 + 4 = 44바이트
 */
typedef enum {
    COL_TYPE_INT     = 1,   /* 4바이트 정수 (int32_t) */
    COL_TYPE_BIGINT  = 2,   /* 8바이트 정수 (int64_t) */
    COL_TYPE_VARCHAR = 3    /* N바이트 가변 문자열 */
} column_type_t;

/*
 * column_meta_t — 컬럼 메타데이터 (39바이트, packed)
 *
 * DB 헤더의 columns[] 배열에 저장되어 테이블 스키마를 구성한다.
 * schema_compute_layout()이 offset 필드를 자동으로 계산한다.
 *
 * 바이트 레이아웃:
 *   [name: 32바이트][type: 1바이트][size: 2바이트][offset: 2바이트][is_system: 1바이트]
 *   |______________|_______________|______________|________________|___________________|
 *   0              32              33             35               37                  38
 *
 * 예시 (users 테이블의 id 컬럼):
 *   name="id", type=COL_TYPE_BIGINT(2), size=8, offset=0, is_system=1
 */
typedef struct {
    char          name[32];   /* 컬럼 이름 (null 종료 문자열) */
    uint8_t       type;       /* 컬럼 타입 (column_type_t 값) */
    uint16_t      size;       /* 바이트 크기 (INT=4, BIGINT=8, VARCHAR=N) */
    uint16_t      offset;     /* 행 내 바이트 오프셋 (schema_compute_layout이 계산) */
    uint8_t       is_system;  /* 시스템 컬럼 여부 (1 = id 컬럼, 자동 생성) */
} __attribute__((packed)) column_meta_t;

/*
 * db_header_t — DB 헤더 (page 0에 저장)
 *
 * 데이터베이스의 전역 메타데이터를 관리한다.
 * pager_open() 시 파일의 첫 page_size 바이트에서 읽어 메모리에 캐시하고,
 * pager_close() 시 변경사항을 디스크에 기록한다.
 *
 * 바이트 레이아웃 (column_meta_t × 16 = 624바이트):
 *   필드                  | 오프셋 | 크기   | 설명
 *   ──────────────────────┼────────┼────────┼─────────────────
 *   magic                 | 0      | 8      | "MINIDB\0"
 *   version               | 8      | 4      | DB_VERSION (1)
 *   page_size             | 12     | 4      | 4096 또는 16384
 *   root_index_page_id    | 16     | 4      | B+ tree 루트 페이지
 *   first_heap_page_id    | 20     | 4      | 첫 힙 페이지
 *   next_page_id          | 24     | 4      | 다음 할당 ID
 *   free_page_head        | 28     | 4      | 빈 페이지 리스트 헤드
 *   next_id               | 32     | 8      | 자동 증가 ID
 *   row_count             | 40     | 8      | 살아있는 행 수
 *   column_count          | 48     | 2      | 컬럼 수
 *   row_size              | 50     | 2      | 행 직렬화 크기
 *   columns[16]           | 52     | 624    | 컬럼 메타데이터 배열
 *   ──────────────────────┴────────┴────────┴─────────────────
 *   총 크기: 676바이트 (page_size보다 훨씬 작으므로 page 0에 여유 있음)
 *
 * 예시 (users 테이블, 행 100개 삽입 후):
 *   magic="MINIDB\0", version=1, page_size=4096
 *   root_index_page_id=2, first_heap_page_id=1, next_page_id=5
 *   free_page_head=0, next_id=101, row_count=100
 *   column_count=3, row_size=44
 *   columns[0] = {name="id",   type=2, size=8,  offset=0,  is_system=1}
 *   columns[1] = {name="name", type=3, size=32, offset=8,  is_system=0}
 *   columns[2] = {name="age",  type=1, size=4,  offset=40, is_system=0}
 */
typedef struct {
    char     magic[8];              /* 매직 넘버 ("MINIDB\0") — 8바이트 */
    uint32_t version;               /* DB 파일 형식 버전 — 4바이트 */
    uint32_t page_size;             /* 페이지 크기 (OS에서 결정) — 4바이트 */
    uint32_t root_index_page_id;    /* B+ tree 루트 페이지 ID — 4바이트 */
    uint32_t first_heap_page_id;    /* 첫 번째 힙 페이지 ID — 4바이트 */
    uint32_t next_page_id;          /* 다음에 할당할 페이지 ID — 4바이트 */
    uint32_t free_page_head;        /* 빈 페이지 연결 리스트 헤드 (0 = 없음) — 4바이트 */
    uint64_t next_id;               /* 다음 자동 증가 ID — 8바이트 */
    uint64_t row_count;             /* 살아 있는 행 수 — 8바이트 */
    uint16_t column_count;          /* 등록된 컬럼 수 — 2바이트 */
    uint16_t row_size;              /* 한 행의 직렬화 크기 — 2바이트 */
    column_meta_t columns[MAX_COLUMNS]; /* 컬럼 메타데이터 배열 — 39×16 = 624바이트 */
} __attribute__((packed)) db_header_t;

/* ── row_ref_t — 행 참조 (6바이트, packed) ──
 *
 * 힙 페이지 내 특정 행의 위치를 가리킨다.
 * B+ tree 리프 엔트리의 값(value)으로 사용된다: key(id) → row_ref_t
 *
 * 바이트 레이아웃:
 *   [page_id: 4바이트][slot_id: 2바이트]
 *   |_________________|__________________|
 *   0                 4                  6
 *
 * 예시: row_ref_t { page_id=1, slot_id=3 }
 *   → page 1의 slot[3]에 저장된 행을 가리킨다
 *   → slot[3].offset = 4096 - 44×4 = 3920 (row_size=44 기준)
 */
typedef struct {
    uint32_t page_id;   /* 행이 저장된 힙 페이지 ID */
    uint16_t slot_id;   /* 페이지 내 슬롯 번호 */
} __attribute__((packed)) row_ref_t;

/* ── free_page_header_t — 빈 페이지 헤더 (8바이트) ──
 *
 * 해제된 페이지의 첫 8바이트에 기록되며, 빈 페이지 연결 리스트를 형성한다.
 * db_header.free_page_head가 리스트의 시작이다.
 *
 * 빈 페이지 연결 리스트 예시:
 *   db_header.free_page_head = 7
 *   page 7: {PAGE_TYPE_FREE, next=5}  →  page 5: {PAGE_TYPE_FREE, next=3}  →  page 3: {PAGE_TYPE_FREE, next=0}
 *                                                                                                     ↑ 리스트 끝
 *
 * 새 페이지 할당 시 free_page_head에서 꺼내고, 해제 시 head에 삽입한다 (LIFO).
 */
typedef struct {
    uint32_t page_type;       /* PAGE_TYPE_FREE (0x05) — 4바이트 */
    uint32_t next_free_page;  /* 다음 빈 페이지 ID (0 = 리스트 끝) — 4바이트 */
} free_page_header_t;

/* ── 힙 페이지 구조 ──
 *
 * 슬롯 기반 힙 페이지: 행 데이터를 저장하는 페이지이다.
 * 슬롯 디렉터리는 앞에서 뒤로, 행 데이터는 뒤에서 앞으로 성장한다.
 *
 * 페이지 레이아웃 (page_size=4096, row_size=44, 슬롯 3개 사용 중):
 *
 *   오프셋 0                                              오프셋 4096
 *   ┌──────────┬────────┬────────┬────────┬─────────┬──────┬──────┬──────┐
 *   │  헤더    │ slot 0 │ slot 1 │ slot 2 │  빈공간 │ row2 │ row1 │ row0 │
 *   │ 16바이트 │ 8바이트│ 8바이트│ 8바이트│         │ 44B  │ 44B  │ 44B  │
 *   └──────────┴────────┴────────┴────────┴─────────┴──────┴──────┴──────┘
 *   |← 앞에서 뒤로 성장 →|                            |← 뒤에서 앞으로 성장 →|
 *
 *   slots_end = 16 + 3×8 = 40바이트 (슬롯 영역 끝)
 *   data_start = 4096 - 44×3 = 3964바이트 (행 데이터 시작)
 *   사용 가능 공간 = 3964 - 40 = 3924바이트
 *   페이지당 최대 행 수 = (4096 - 16) / (44 + 8) = 4080 / 52 ≈ 78개
 */

/* 슬롯 상태 코드 */
#define SLOT_ALIVE 0x01   /* 유효한 행이 존재 */
#define SLOT_FREE  0x03   /* 빈 슬롯 (재활용 가능, free_slot_head 체인) */
#define SLOT_NONE  0xFFFF /* 빈 슬롯 없음 표시 (free_slot_head 초기값) */

/*
 * heap_page_header_t — 힙 페이지 헤더 (16바이트, packed)
 *
 * 바이트 레이아웃:
 *   [page_type: 4][next_heap_page_id: 4][slot_count: 2][free_slot_head: 2][free_space_offset: 2][reserved: 2]
 *   |______________|_____________________|_______________|___________________|______________________|____________|
 *   0              4                     8               10                  12                     14           16
 *
 * 예시 (슬롯 80개, 빈 슬롯 없음, 행 데이터 3520바이트 사용):
 *   page_type=0x02, next_heap_page_id=3, slot_count=80,
 *   free_slot_head=0xFFFF, free_space_offset=3520, reserved=0
 */
typedef struct {
    uint32_t page_type;          /* PAGE_TYPE_HEAP (0x02) — 4바이트 */
    uint32_t next_heap_page_id;  /* 다음 힙 페이지 ID (연결 리스트) — 4바이트 */
    uint16_t slot_count;         /* 할당된 슬롯 총 수 (빈 슬롯 포함) — 2바이트 */
    uint16_t free_slot_head;     /* 빈 슬롯 체인의 헤드 (SLOT_NONE=없음) — 2바이트 */
    uint16_t free_space_offset;  /* 페이지 끝에서 사용된 행 데이터의 총 크기 — 2바이트 */
    uint16_t reserved;           /* 예약 (정렬용) — 2바이트 */
} __attribute__((packed)) heap_page_header_t;

/*
 * slot_t — 슬롯 디렉터리 엔트리 (8바이트, packed)
 *
 * 힙 페이지 헤더 바로 뒤에 slot_count개가 연속으로 배치된다.
 *
 * 바이트 레이아웃:
 *   [offset: 2바이트][status: 2바이트][next_free: 2바이트][reserved: 2바이트]
 *   |________________|________________|___________________|__________________|
 *   0                2                4                   6                  8
 *
 * 슬롯 상태별 동작:
 *   SLOT_ALIVE (0x01): offset이 유효, 해당 위치에 행 데이터가 있음
 *   SLOT_FREE  (0x03): offset은 그대로 유지, next_free로 빈 슬롯 체인 형성
 *                       재사용 시 status만 ALIVE로 변경 (기존 offset 재활용)
 *
 * 빈 슬롯 체인 예시 (slot 2 → slot 5 → slot 0):
 *   free_slot_head = 2
 *   slot[2]: {offset=3920, status=FREE, next_free=5}
 *   slot[5]: {offset=3788, status=FREE, next_free=0}
 *   slot[0]: {offset=4052, status=FREE, next_free=SLOT_NONE}  ← 체인 끝
 */
typedef struct {
    uint16_t offset;    /* 페이지 내 행 데이터의 시작 오프셋 — 2바이트 */
    uint16_t status;    /* 슬롯 상태 (SLOT_ALIVE/FREE) — 2바이트 */
    uint16_t next_free; /* 빈 슬롯 체인의 다음 슬롯 ID (FREE일 때만 유효) — 2바이트 */
    uint16_t reserved;  /* 예약 (정렬용) — 2바이트 */
} __attribute__((packed)) slot_t;

/* ── B+ tree 리프 노드 ──
 *
 * 리프 노드는 (key, row_ref_t) 쌍을 정렬된 순서로 저장한다.
 * 이중 연결 리스트(prev/next)로 범위 스캔을 지원한다.
 *
 * 페이지 레이아웃 (page_size=4096 기준):
 *   [leaf_page_header_t: 20바이트][entry_0: 14바이트][entry_1: 14바이트]...[entry_N]
 *   |_________ 헤더 _____________||_________________ 엔트리 배열 ___________________|
 *   0                            20                                               4096
 *
 * 최대 엔트리 수 = (4096 - 20) / 14 = 4076 / 14 = 291개
 * 최소 엔트리 수 = 291 / 2 = 145개 (분할 기준)
 */

/*
 * leaf_page_header_t — 리프 노드 헤더 (20바이트, packed)
 *
 * 바이트 레이아웃:
 *   [page_type: 4][parent_page_id: 4][key_count: 4][next_leaf: 4][prev_leaf: 4]
 *   |______________|_________________|_______________|______________|______________|
 *   0              4                 8               12             16             20
 *
 * 예시 (루트 리프, 키 3개):
 *   page_type=0x03, parent_page_id=0, key_count=3,
 *   next_leaf_page_id=0, prev_leaf_page_id=0
 */
typedef struct {
    uint32_t page_type;           /* PAGE_TYPE_LEAF (0x03) — 4바이트 */
    uint32_t parent_page_id;      /* 부모 내부 노드의 페이지 ID (루트면 0) — 4바이트 */
    uint32_t key_count;           /* 저장된 키 수 — 4바이트 */
    uint32_t next_leaf_page_id;   /* 오른쪽 형제 리프 (0 = 없음) — 4바이트 */
    uint32_t prev_leaf_page_id;   /* 왼쪽 형제 리프 (0 = 없음) — 4바이트 */
} __attribute__((packed)) leaf_page_header_t;

/*
 * leaf_entry_t — 리프 엔트리 (14바이트, packed)
 *
 * key(id) → 힙 행 위치(row_ref_t) 매핑을 저장한다.
 *
 * 바이트 레이아웃:
 *   [key: 8바이트][row_ref_t: 6바이트]
 *   |_____________|___________________|
 *   0             8                   14
 *
 * 예시: INSERT INTO users VALUES ('Alice', 25) → id=1 자동 할당
 *   key=1, row_ref={page_id=1, slot_id=0}
 *   → "id 1인 행은 page 1의 slot 0에 있다"
 */
typedef struct {
    uint64_t  key;      /* 인덱스 키 (id 값) — 8바이트 */
    row_ref_t row_ref;  /* 힙 페이지 내 행 위치 — 6바이트 */
} __attribute__((packed)) leaf_entry_t;

/* ── B+ tree 내부 노드 ──
 *
 * 내부 노드는 자식 노드로의 라우팅 정보를 저장한다.
 * 키를 기준으로 어떤 자식으로 내려갈지 결정한다.
 *
 * 자식 탐색 규칙 (예시: keys=[30, 60], children=[p2, p3, p4]):
 *   key < 30           → leftmost_child (page 2)로 이동
 *   30 ≤ key < 60      → entries[0].right_child (page 3)로 이동
 *   key ≥ 60           → entries[1].right_child (page 4)로 이동
 *
 * 페이지 레이아웃 (page_size=4096 기준):
 *   [internal_page_header_t: 16바이트][entry_0: 12바이트][entry_1: 12바이트]...[entry_N]
 *   |_____________ 헤더 _______________||___________________ 엔트리 배열 ___________________|
 *   0                                 16                                                 4096
 *
 * 최대 엔트리 수 = (4096 - 16) / 12 = 4080 / 12 = 340개
 * 최소 엔트리 수 = 340 / 2 = 170개 (분할 기준)
 * 최대 자식 수 = 340 + 1 = 341개 (leftmost_child + 각 엔트리의 right_child)
 */

/*
 * internal_page_header_t — 내부 노드 헤더 (16바이트, packed)
 *
 * 바이트 레이아웃:
 *   [page_type: 4][parent_page_id: 4][key_count: 4][leftmost_child: 4]
 *   |______________|_________________|_______________|___________________|
 *   0              4                 8               12                  16
 *
 * 예시 (루트 내부 노드, 키 1개 = 자식 2개):
 *   page_type=0x04, parent_page_id=0, key_count=1,
 *   leftmost_child_page_id=2  (+ entries[0].right_child=3)
 */
typedef struct {
    uint32_t page_type;               /* PAGE_TYPE_INTERNAL (0x04) — 4바이트 */
    uint32_t parent_page_id;          /* 부모 노드 페이지 ID (루트면 0) — 4바이트 */
    uint32_t key_count;               /* 저장된 키 수 — 4바이트 */
    uint32_t leftmost_child_page_id;  /* 가장 왼쪽 자식 페이지 ID — 4바이트 */
} __attribute__((packed)) internal_page_header_t;

/*
 * internal_entry_t — 내부 노드 엔트리 (12바이트, packed)
 *
 * 구분 키(separator key)와 오른쪽 자식 페이지 ID를 저장한다.
 * "이 key 이상인 값은 right_child 방향에 있다"는 의미이다.
 *
 * 바이트 레이아웃:
 *   [key: 8바이트][right_child_page_id: 4바이트]
 *   |_____________|_____________________________|
 *   0             8                             12
 *
 * 예시: 리프가 key=30에서 분할되었을 때
 *   key=30, right_child_page_id=5
 *   → "key ≥ 30인 엔트리는 page 5 (또는 그 하위)에 있다"
 */
typedef struct {
    uint64_t key;                   /* 구분 키 — 8바이트 */
    uint32_t right_child_page_id;   /* 이 키 이상인 값이 있는 자식 페이지 ID — 4바이트 */
} __attribute__((packed)) internal_entry_t;

#endif /* PAGE_FORMAT_H */
