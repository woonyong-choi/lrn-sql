CC      = gcc
WARNFLAGS = -Wall -Wextra -Werror
INCLUDES = -Iinclude
# ── sanitizer 기본값 ──
#
# macOS 26(Darwin 25) 이상에서는 ASan 런타임이 초기화 도중 자기 자신을
# 재진입해(AsanInitInternal → ... → malloc → AsanInitFromRtl) spin lock 에
# 걸린다. 프로세스가 main() 에 닿지 못하므로 ASan 을 켠 빌드는 무엇을 하든
# 멈춘다. lrn-sql 의 문제가 아니라 플랫폼 문제이고, 빈 main() 으로도 재현된다.
# 그 환경에서는 UBSan 만 켠다. Linux CI 는 아래 기본값대로 둘 다 켠다.
#
#   make SANITIZE=address,undefined   강제로 둘 다
#   make SANITIZE=                    sanitizer 없이
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
ifeq ($(shell test $$(uname -r | cut -d. -f1) -ge 25 && echo yes),yes)
SANITIZE ?= undefined
endif
endif
SANITIZE ?= address,undefined

ifneq ($(strip $(SANITIZE)),)
SANITIZE_FLAGS = -fsanitize=$(SANITIZE)
endif

CFLAGS  = $(WARNFLAGS) -g $(INCLUDES) $(SANITIZE_FLAGS)
LDFLAGS = $(SANITIZE_FLAGS)
LDLIBS  = -lpthread -lm
DEPFLAGS = -MMD -MP
.DEFAULT_GOAL := all

SRC_DIR   = src
BUILD_DIR = build

SRCS = $(SRC_DIR)/storage/pager.c \
       $(SRC_DIR)/storage/schema.c \
       $(SRC_DIR)/storage/table.c \
       $(SRC_DIR)/storage/bptree.c \
       $(SRC_DIR)/sql/parser.c \
       $(SRC_DIR)/sql/planner.c \
       $(SRC_DIR)/sql/executor.c \
       $(SRC_DIR)/server/http.c \
       $(SRC_DIR)/server/server.c \
       $(SRC_DIR)/server/lock_table.c \
       $(SRC_DIR)/db.c

OBJS = $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SRCS))

# ── main binary (REPL) ──
all: $(BUILD_DIR)/minidb

$(BUILD_DIR)/minidb: $(OBJS) $(BUILD_DIR)/main.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD_DIR)/main.o: $(SRC_DIR)/main.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c -o $@ $<

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c -o $@ $<

# 각 테스트 번역 단위도 헤더 의존성을 추적한다.
$(BUILD_DIR)/tests/%.o: tests/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c -o $@ $<

STORAGE_OBJS = $(BUILD_DIR)/storage/record.o $(BUILD_DIR)/storage/record_page.o \
               $(BUILD_DIR)/storage/overflow.o $(BUILD_DIR)/storage/value.o
BASIC_TESTS = test_all test_record test_record_fault test_overflow \
              test_overflow_fault test_value test_value_fault test_parser_boundary \
              test_http_input
TEST_BINS = $(addprefix $(BUILD_DIR)/,$(BASIC_TESTS))

$(BUILD_DIR)/test_all: $(BUILD_DIR)/tests/test_all.o $(OBJS)
$(BUILD_DIR)/test_parser_boundary: $(BUILD_DIR)/tests/test_parser_boundary.o $(OBJS)
$(BUILD_DIR)/test_http_input: $(BUILD_DIR)/tests/test_http_input.o $(OBJS)
$(BUILD_DIR)/test_record: $(BUILD_DIR)/tests/test_record.o $(BUILD_DIR)/storage/record.o $(BUILD_DIR)/storage/record_page.o
$(BUILD_DIR)/test_overflow: $(BUILD_DIR)/tests/test_overflow.o $(BUILD_DIR)/storage/overflow.o $(BUILD_DIR)/storage/record_page.o
$(BUILD_DIR)/test_overflow_file: $(BUILD_DIR)/tests/test_overflow_file.o $(BUILD_DIR)/storage/overflow.o
$(BUILD_DIR)/test_value: $(BUILD_DIR)/tests/test_value.o $(BUILD_DIR)/storage/value.o
$(BUILD_DIR)/test_value_sqlite: $(BUILD_DIR)/tests/test_value_sqlite.o $(BUILD_DIR)/storage/value.o
$(BUILD_DIR)/test_value_sqlite: LDLIBS += -lsqlite3
$(BUILD_DIR)/test_record_fault: $(BUILD_DIR)/tests/test_record_fault.o $(BUILD_DIR)/storage/record_fault.o $(BUILD_DIR)/storage/record_page_fault.o
$(BUILD_DIR)/test_overflow_fault: $(BUILD_DIR)/tests/test_overflow_fault.o $(BUILD_DIR)/storage/overflow_fault.o
$(BUILD_DIR)/test_value_fault: $(BUILD_DIR)/tests/test_value_fault.o $(BUILD_DIR)/storage/value_fault.o

$(TEST_BINS) $(BUILD_DIR)/test_overflow_file $(BUILD_DIR)/test_value_sqlite:
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD_DIR)/storage/record_fault.o: src/storage/record.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEPFLAGS) -Dmalloc=record_test_malloc -c -o $@ $<

$(BUILD_DIR)/storage/record_page_fault.o: src/storage/record_page.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEPFLAGS) -Dcalloc=record_test_calloc -c -o $@ $<

$(BUILD_DIR)/storage/overflow_fault.o: src/storage/overflow.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEPFLAGS) -Dmalloc=overflow_test_malloc -c -o $@ $<

$(BUILD_DIR)/storage/value_fault.o: src/storage/value.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEPFLAGS) -Dmalloc=value_test_malloc -c -o $@ $<

test: $(TEST_BINS)
	@set -e; for binary in $(TEST_BINS); do "$$binary"; done

test-overflow-file: $(BUILD_DIR)/test_overflow_file
	@directory=$$(mktemp -d "$(BUILD_DIR)/overflow.XXXXXX") && \
	trap 'rmdir "$$directory" 2>/dev/null || true' EXIT HUP INT TERM && \
	./$(BUILD_DIR)/test_overflow_file "$$directory/roundtrip.db"

test-value-sqlite: $(BUILD_DIR)/test_value_sqlite
	./$(BUILD_DIR)/test_value_sqlite

# ── data generator ──
$(BUILD_DIR)/gen_data: tools/gen_data.c $(OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

run: $(BUILD_DIR)/minidb
	./$(BUILD_DIR)/minidb sql.db

N ?= 1000000
gen: $(BUILD_DIR)/gen_data
	./$(BUILD_DIR)/gen_data sql.db $(N)

# ── benchmark ──
#
# bench      규모별 기울기. 개선 전(993d4d8^)·개선 후·인덱스 비활성 세 빌드를
#            같은 소스에서 뽑아 같은 워크로드로 잰다. 약 1분.
#            결과: bench/scaling.md + bench/scaling.json + Daphnis 차트 2개
# bench-1m   1M 행 단일 규모 측정 (개선 전 INSERT 가 느려 수 분 걸린다)
# bench-http HTTP 서버 경로 부하 생성기 (make run-server 가 먼저 필요)
bench:
	python3 bench/scaling.py
	npm_config_cache=$(BUILD_DIR)/npm-cache npm exec --yes --package=daphnis@0.1.3 -- daphnis check docs/assets/sql-scaling-*.dap --strict --no-deprecated --require-data
	npm_config_cache=$(BUILD_DIR)/npm-cache npm exec --yes --package=daphnis@0.1.3 -- daphnis render docs/assets/sql-scaling-*.dap --strict --require-data

bench-1m:
	python3 bench/bench_minidb_param.py --rows 1000000

$(BUILD_DIR)/bench_http: tools/bench.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(WARNFLAGS) -g $(INCLUDES) -o $@ $< -lpthread

bench-http: $(BUILD_DIR)/bench_http
	@echo "서버를 먼저 실행하세요: make run-server"
	./$(BUILD_DIR)/bench_http 127.0.0.1 8080 4 100

# ── stress test (혼합 부하 + 실시간 모니터링) ──
$(BUILD_DIR)/stress: tools/stress.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(WARNFLAGS) -g $(INCLUDES) -o $@ $< -lpthread -lm

STRESS_THREADS  ?= 8
STRESS_REQUESTS ?= 10000
STRESS_DURATION ?= 0

stress: $(BUILD_DIR)/stress
	@echo "서버를 먼저 실행하세요: make run-server"
	./$(BUILD_DIR)/stress 127.0.0.1 8080 $(STRESS_THREADS) $(STRESS_REQUESTS) $(STRESS_DURATION)

run-server: $(BUILD_DIR)/minidb
	./$(BUILD_DIR)/minidb --server 8080 stress.db

# ── step tests ──
$(BUILD_DIR)/test_step0: $(BUILD_DIR)/tests/test_step0_db_execute.o $(OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

test-step0: $(BUILD_DIR)/test_step0
	./$(BUILD_DIR)/test_step0

$(BUILD_DIR)/test_step1: $(BUILD_DIR)/tests/test_step1_sql_ext.o $(OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

test-step1: $(BUILD_DIR)/test_step1
	./$(BUILD_DIR)/test_step1

$(BUILD_DIR)/test_prop: $(BUILD_DIR)/tests/test_bptree_property.o $(OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

test-prop: $(BUILD_DIR)/test_prop
	./$(BUILD_DIR)/test_prop

$(BUILD_DIR)/test_step2: $(BUILD_DIR)/tests/test_step2_concurrency.o $(OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

test-step2: $(BUILD_DIR)/test_step2
	./$(BUILD_DIR)/test_step2

$(BUILD_DIR)/test_step3: $(BUILD_DIR)/tests/test_step3_regression.o $(OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

test-step3: $(BUILD_DIR)/test_step3
	./$(BUILD_DIR)/test_step3

$(BUILD_DIR)/sql_probe: $(BUILD_DIR)/tests/sql_probe.o $(OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

test-pg-compat: $(BUILD_DIR)/sql_probe
	python3 tests/pg_compat.py --probe ./$(BUILD_DIR)/sql_probe

test-legacy-adopt: $(BUILD_DIR)/minidb
	python3 tests/test_legacy_adopt.py --minidb ./$(BUILD_DIR)/minidb

test-all: test test-prop test-step0 test-step1 test-step2 test-step3 test-legacy-adopt test-overflow-file test-value-sqlite

clean:
	rm -rf $(BUILD_DIR) build-o2 *.db __test__*.db __test_step3_*.db

.PHONY: test-overflow-file test-value-sqlite

-include $(OBJS:.o=.d) $(STORAGE_OBJS:.o=.d) $(BUILD_DIR)/main.d
-include $(wildcard $(BUILD_DIR)/tests/*.d $(BUILD_DIR)/storage/*_fault.d)

.PHONY: all test test-prop test-step0 test-step1 test-step2 test-step3 test-all run run-server gen bench bench-1m bench-http stress clean
