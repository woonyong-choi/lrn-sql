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
PYTHON ?= python3
PYTEST ?= $(PYTHON) -m pytest
TEST_TIMEOUT ?= 60
FAULT_SEED ?= 1
TEST_RUNNER = $(PYTHON) tests/fault_runner.py --timeout $(TEST_TIMEOUT) --seed $(FAULT_SEED)
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
               $(BUILD_DIR)/storage/overflow.o $(BUILD_DIR)/storage/value.o $(BUILD_DIR)/storage/catalog.o \
               $(BUILD_DIR)/storage/page_file.o $(BUILD_DIR)/storage/catalog_store.o $(BUILD_DIR)/storage/legacy_migrate.o
BASIC_TESTS = test_all test_record test_record_fault test_overflow \
              test_overflow_fault test_value test_value_fault test_value_legacy_varchar test_parser_boundary \
              test_http_input test_io test_io_lock test_catalog test_catalog_fault \
              test_lexer test_lexer_fault test_page_file test_catalog_store test_legacy_migrate
TEST_BINS = $(addprefix $(BUILD_DIR)/,$(BASIC_TESTS))

$(BUILD_DIR)/test_all: $(BUILD_DIR)/tests/test_all.o $(OBJS)
$(BUILD_DIR)/test_parser_boundary: $(BUILD_DIR)/tests/test_parser_boundary.o $(OBJS)
$(BUILD_DIR)/test_http_input: $(BUILD_DIR)/tests/test_http_input.o $(OBJS)
$(BUILD_DIR)/test_io: $(BUILD_DIR)/tests/test_io.o $(OBJS)
$(BUILD_DIR)/test_io_lock: $(BUILD_DIR)/tests/test_io_lock.o $(OBJS)
$(BUILD_DIR)/test_record: $(BUILD_DIR)/tests/test_record.o $(BUILD_DIR)/storage/record.o $(BUILD_DIR)/storage/record_page.o
$(BUILD_DIR)/test_overflow: $(BUILD_DIR)/tests/test_overflow.o $(BUILD_DIR)/storage/overflow.o $(BUILD_DIR)/storage/record_page.o
$(BUILD_DIR)/test_overflow_file: $(BUILD_DIR)/tests/test_overflow_file.o $(BUILD_DIR)/storage/overflow.o
$(BUILD_DIR)/test_value: $(BUILD_DIR)/tests/test_value.o $(BUILD_DIR)/storage/value.o
$(BUILD_DIR)/test_value_legacy_varchar: $(BUILD_DIR)/tests/test_value_legacy_varchar.o $(OBJS)
$(BUILD_DIR)/storage/value_varchar_hook.o: src/storage/value.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEPFLAGS) -Dmalloc=value_test_malloc -c -o $@ $<

$(BUILD_DIR)/test_value_varchar: $(BUILD_DIR)/tests/test_value_varchar.o $(BUILD_DIR)/storage/value_varchar_hook.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD_DIR)/test_value_sqlite: $(BUILD_DIR)/tests/test_value_sqlite.o $(BUILD_DIR)/storage/value.o
$(BUILD_DIR)/test_value_sqlite: LDLIBS += -lsqlite3
$(BUILD_DIR)/test_record_fault: $(BUILD_DIR)/tests/test_record_fault.o $(BUILD_DIR)/storage/record_fault.o $(BUILD_DIR)/storage/record_page_fault.o
$(BUILD_DIR)/test_overflow_fault: $(BUILD_DIR)/tests/test_overflow_fault.o $(BUILD_DIR)/storage/overflow_fault.o
$(BUILD_DIR)/test_value_fault: $(BUILD_DIR)/tests/test_value_fault.o $(BUILD_DIR)/storage/value_fault.o
$(BUILD_DIR)/test_catalog: $(BUILD_DIR)/tests/test_catalog.o $(BUILD_DIR)/storage/catalog.o
$(BUILD_DIR)/test_catalog_fault: $(BUILD_DIR)/tests/test_catalog_fault.o $(BUILD_DIR)/storage/catalog_fault.o
$(BUILD_DIR)/test_lexer: $(BUILD_DIR)/tests/test_lexer.o $(BUILD_DIR)/sql/lexer.o
$(BUILD_DIR)/test_lexer_fault: $(BUILD_DIR)/tests/test_lexer_fault.o $(BUILD_DIR)/sql/lexer_fault.o
$(BUILD_DIR)/test_page_file: $(BUILD_DIR)/tests/test_page_file.o $(STORAGE_OBJS)
$(BUILD_DIR)/test_catalog_store: $(BUILD_DIR)/tests/test_catalog_store.o $(STORAGE_OBJS)
$(BUILD_DIR)/test_legacy_migrate: $(BUILD_DIR)/tests/test_legacy_migrate.o $(STORAGE_OBJS)

$(BUILD_DIR)/test_result: $(BUILD_DIR)/tests/test_result.o $(BUILD_DIR)/sql/result.o $(BUILD_DIR)/storage/value.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD_DIR)/test_result_fault: $(BUILD_DIR)/tests/test_result_fault.o $(BUILD_DIR)/sql/result_fault.o $(BUILD_DIR)/storage/value.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD_DIR)/sql/result_fault.o: src/sql/result.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEPFLAGS) -Dmalloc=result_test_malloc -Dcalloc=result_test_calloc -Drealloc=result_test_realloc -Dfree=result_test_free -c -o $@ $<

test-result: $(BUILD_DIR)/test_result $(BUILD_DIR)/test_result_fault
	$(TEST_RUNNER) --report $(BUILD_DIR)/fault/test_result.json -- ./$(BUILD_DIR)/test_result
	$(TEST_RUNNER) --report $(BUILD_DIR)/fault/test_result_fault.json -- ./$(BUILD_DIR)/test_result_fault

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

$(BUILD_DIR)/storage/catalog_fault.o: src/storage/catalog.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEPFLAGS) -Dmalloc=catalog_test_malloc -Drealloc=catalog_test_realloc -Dfree=catalog_test_free -c -o $@ $<

$(BUILD_DIR)/sql/lexer_fault.o: src/sql/lexer.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEPFLAGS) -Dmalloc=lexer_test_malloc -Dfree=lexer_test_free -c -o $@ $<

test: $(TEST_BINS)
	@set -e; for binary in $(TEST_BINS); do \
		$(TEST_RUNNER) --report "$(BUILD_DIR)/fault/$${binary##*/}.json" -- "$$binary"; \
	done

test-overflow-file: $(BUILD_DIR)/test_overflow_file
	@directory=$$(mktemp -d "$(BUILD_DIR)/overflow.XXXXXX") && \
	trap 'rmdir "$$directory" 2>/dev/null || true' EXIT HUP INT TERM && \
	$(TEST_RUNNER) --report $(BUILD_DIR)/fault/test_overflow_file.json -- ./$(BUILD_DIR)/test_overflow_file "$$directory/roundtrip.db"

test-value-sqlite: $(BUILD_DIR)/test_value_sqlite
	$(TEST_RUNNER) --report $(BUILD_DIR)/fault/test_value_sqlite.json -- ./$(BUILD_DIR)/test_value_sqlite

test-value-varchar: $(BUILD_DIR)/test_value_varchar
	$(TEST_RUNNER) --report $(BUILD_DIR)/fault/test_value_varchar.json -- ./$(BUILD_DIR)/test_value_varchar

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
	npm_config_cache=$(BUILD_DIR)/npm-cache npm ci --prefix docs/assets --ignore-scripts
	docs/assets/node_modules/.bin/daphnis check docs/assets/sql-scaling-*.dap --strict --no-deprecated --require-data
	docs/assets/node_modules/.bin/daphnis render docs/assets/sql-scaling-*.dap --strict --require-data

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
	$(TEST_RUNNER) --report $(BUILD_DIR)/fault/test_step0.json -- ./$(BUILD_DIR)/test_step0

$(BUILD_DIR)/test_step1: $(BUILD_DIR)/tests/test_step1_sql_ext.o $(OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

test-step1: $(BUILD_DIR)/test_step1
	$(TEST_RUNNER) --report $(BUILD_DIR)/fault/test_step1.json -- ./$(BUILD_DIR)/test_step1

$(BUILD_DIR)/test_prop: $(BUILD_DIR)/tests/test_bptree_property.o $(OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

test-prop: $(BUILD_DIR)/test_prop
	$(TEST_RUNNER) --report $(BUILD_DIR)/fault/test_prop.json -- ./$(BUILD_DIR)/test_prop

$(BUILD_DIR)/test_step2: $(BUILD_DIR)/tests/test_step2_concurrency.o $(OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

test-step2: $(BUILD_DIR)/test_step2
	$(TEST_RUNNER) --report $(BUILD_DIR)/fault/test_step2.json -- ./$(BUILD_DIR)/test_step2

$(BUILD_DIR)/test_lock_table_waiters: tests/test_lock_table_waiters.c src/server/lock_table.c include/server/lock_table.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tests/test_lock_table_waiters.c $(LDLIBS)

test-lock-waiters: $(BUILD_DIR)/test_lock_table_waiters
	$(TEST_RUNNER) --report $(BUILD_DIR)/fault/test_lock_table_waiters.json -- ./$(BUILD_DIR)/test_lock_table_waiters

$(BUILD_DIR)/test_step3: $(BUILD_DIR)/tests/test_step3_regression.o $(OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

test-step3: $(BUILD_DIR)/test_step3
	$(TEST_RUNNER) --report $(BUILD_DIR)/fault/test_step3.json -- ./$(BUILD_DIR)/test_step3

$(BUILD_DIR)/sql_probe: $(BUILD_DIR)/tests/sql_probe.o $(OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

test-pg-compat: $(BUILD_DIR)/sql_probe
	python3 tests/pg_compat.py --probe ./$(BUILD_DIR)/sql_probe

test-legacy-adopt: $(BUILD_DIR)/minidb
	$(TEST_RUNNER) --report $(BUILD_DIR)/fault/test_legacy_adopt.json -- $(PYTHON) tests/test_legacy_adopt.py --minidb ./$(BUILD_DIR)/minidb

test-repl-boundary: $(BUILD_DIR)/minidb
	$(TEST_RUNNER) --report $(BUILD_DIR)/fault/test_repl_boundary.json -- $(PYTHON) tests/test_repl_boundary.py --minidb ./$(BUILD_DIR)/minidb

test-io-cli: $(BUILD_DIR)/minidb
	$(TEST_RUNNER) --report $(BUILD_DIR)/fault/test_io_cli.json -- $(PYTHON) tests/test_io_cli.py --minidb ./$(BUILD_DIR)/minidb

test-http-boundary-cli: $(BUILD_DIR)/minidb
	$(TEST_RUNNER) --timeout 240 --report $(BUILD_DIR)/fault/test_http_boundary_cli.json -- $(PYTHON) tests/test_http_boundary_cli.py --minidb ./$(BUILD_DIR)/minidb --workdir $(BUILD_DIR)/http-boundary-cli

test-runner:
	$(TEST_RUNNER) --report $(BUILD_DIR)/fault/test_runner.json -- $(PYTEST) -q tests/test_fault_runner.py tests/test_fault_oracle.py --basetemp=$(BUILD_DIR)/runner-tests -p no:cacheprovider

$(BUILD_DIR)/test_fault_runtime: $(BUILD_DIR)/tests/test_fault_runtime.o $(BUILD_DIR)/tests/fault_runtime.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD_DIR)/fault_fixture: $(BUILD_DIR)/tests/fault_fixture.o $(BUILD_DIR)/tests/fault_runtime.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

test-fault: $(BUILD_DIR)/test_fault_runtime $(BUILD_DIR)/fault_fixture
	$(TEST_RUNNER) --report $(BUILD_DIR)/fault/test_fault_runtime.json -- ./$(BUILD_DIR)/test_fault_runtime $(BUILD_DIR)/fault/runtime.trace
	FAULT_FIXTURE=$(abspath $(BUILD_DIR)/fault_fixture) $(TEST_RUNNER) --report $(BUILD_DIR)/fault/test_fault_fixture.json -- $(PYTEST) -q tests/test_fault_fixture.py --basetemp=$(BUILD_DIR)/fault/fixture-tests -p no:cacheprovider

HOOK_OBJS = $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/hooks/%.o,$(SRCS))
CONCURRENCY_RUNS ?= 3
ifeq ($(SANITIZE),thread)
DETECTOR_EXPECT = --expect error --expect-code 66
endif

$(BUILD_DIR)/hooks/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEPFLAGS) -DMINIDB_TEST_HOOKS -c -o $@ $<

$(BUILD_DIR)/hooks/tests/%.o: tests/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEPFLAGS) -DMINIDB_TEST_HOOKS -c -o $@ $<

$(BUILD_DIR)/test_concurrency_barrier: $(BUILD_DIR)/hooks/tests/test_concurrency_barrier.o $(HOOK_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD_DIR)/test_concurrency_insert: $(BUILD_DIR)/hooks/tests/test_concurrency_insert.o $(HOOK_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD_DIR)/test_result_allocation: $(BUILD_DIR)/hooks/tests/test_result_allocation.o $(HOOK_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD_DIR)/minidb_hooks: $(BUILD_DIR)/hooks/main.o $(HOOK_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

test-result-allocation: $(BUILD_DIR)/test_result_allocation $(BUILD_DIR)/minidb_hooks
	$(TEST_RUNNER) --report $(BUILD_DIR)/fault/test_result_allocation.json -- ./$(BUILD_DIR)/test_result_allocation
	$(TEST_RUNNER) --report $(BUILD_DIR)/fault/test_result_allocation_cli.json -- $(PYTHON) tests/test_result_allocation_cli.py --minidb ./$(BUILD_DIR)/minidb_hooks --workdir $(BUILD_DIR)/result-allocation-cli

test-concurrency: $(BUILD_DIR)/test_concurrency_barrier $(BUILD_DIR)/test_concurrency_insert
	$(TEST_RUNNER) --report $(BUILD_DIR)/fault/test_concurrency_barrier.json -- ./$(BUILD_DIR)/test_concurrency_barrier
	@set -e; iteration=1; while [ $$iteration -le $(CONCURRENCY_RUNS) ]; do \
		$(TEST_RUNNER) --report "$(BUILD_DIR)/fault/test_concurrency_insert-$$iteration.json" -- ./$(BUILD_DIR)/test_concurrency_insert; \
		iteration=$$((iteration + 1)); \
	done
	$(TEST_RUNNER) --report $(BUILD_DIR)/fault/test_order_detector.json $(DETECTOR_EXPECT) -- ./$(BUILD_DIR)/test_concurrency_barrier --check-order-detector
ifeq ($(SANITIZE),thread)
	$(PYTHON) -c 'from pathlib import Path; import sys; text = Path(sys.argv[1]).read_text(); sys.exit(0 if "WARNING: ThreadSanitizer: lock-order-inversion" in text else 1)' $(BUILD_DIR)/fault/test_order_detector.json.stderr
endif

DATABASE_OBJS = $(STORAGE_OBJS) $(BUILD_DIR)/storage/heap_store.o $(BUILD_DIR)/storage/wal.o \
                $(BUILD_DIR)/storage/durable_pair.o $(BUILD_DIR)/storage/transaction.o \
                $(BUILD_DIR)/sql/database.o $(BUILD_DIR)/sql/database_txn.o \
                $(BUILD_DIR)/sql/sql_statement.o $(BUILD_DIR)/sql/lexer.o $(BUILD_DIR)/sql/result.o
DATABASE_TESTS = test_sql_statement test_heap_store test_database test_database_sqlite test_database_fault test_database_bounded_open
DATABASE_BINS = $(addprefix $(BUILD_DIR)/,$(DATABASE_TESTS))
DATABASE_AUDIT_OBJS = $(patsubst $(BUILD_DIR)/%.o,$(BUILD_DIR)/audit/%.o,$(DATABASE_OBJS))

$(BUILD_DIR)/test_sql_statement: $(BUILD_DIR)/tests/test_sql_statement.o $(BUILD_DIR)/sql/sql_statement_fault.o $(BUILD_DIR)/sql/lexer.o $(BUILD_DIR)/storage/value_statement_fault.o
$(BUILD_DIR)/test_heap_store: $(BUILD_DIR)/tests/test_heap_store.o $(STORAGE_OBJS) $(BUILD_DIR)/storage/heap_store_test.o
$(BUILD_DIR)/test_database: $(BUILD_DIR)/tests/test_database.o $(DATABASE_OBJS)
$(BUILD_DIR)/test_database_sqlite: $(BUILD_DIR)/tests/test_database_sqlite.o $(DATABASE_OBJS)
$(BUILD_DIR)/test_database_fault: $(BUILD_DIR)/tests/test_database_fault.o $(filter-out $(BUILD_DIR)/sql/database.o $(BUILD_DIR)/storage/heap_store.o,$(DATABASE_OBJS)) $(BUILD_DIR)/sql/database_fault.o $(BUILD_DIR)/storage/heap_store_fault.o
$(BUILD_DIR)/test_database_bounded_open: $(BUILD_DIR)/tests/test_database_bounded_open.o $(BUILD_DIR)/tests/alloc_audit.o $(DATABASE_AUDIT_OBJS)
$(DATABASE_BINS):
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD_DIR)/audit/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEPFLAGS) -Dmalloc=audit_malloc -Dcalloc=audit_calloc -Drealloc=audit_realloc -Dfree=audit_free -c -o $@ $<

$(BUILD_DIR)/sql/sql_statement_fault.o: src/sql/sql_statement.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEPFLAGS) -Dmalloc=sqlstmt_test_malloc -Dcalloc=sqlstmt_test_calloc -Dfree=sqlstmt_test_free -c -o $@ $<

$(BUILD_DIR)/storage/value_statement_fault.o: src/storage/value.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEPFLAGS) -Dmalloc=sqlstmt_test_malloc -Dcalloc=sqlstmt_test_calloc -Dfree=sqlstmt_test_free -c -o $@ $<

$(BUILD_DIR)/storage/heap_store_test.o: src/storage/heap_store.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEPFLAGS) -Dmalloc=hs_test_malloc -Dcalloc=hs_test_calloc -Drealloc=hs_test_realloc -c -o $@ $<

$(BUILD_DIR)/sql/database_fault.o: src/sql/database.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEPFLAGS) -Dmalloc=db_test_malloc -Dcalloc=db_test_calloc -Drealloc=db_test_realloc -Dfree=db_test_free -c -o $@ $<

$(BUILD_DIR)/storage/heap_store_fault.o: src/storage/heap_store.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEPFLAGS) -Dmalloc=db_test_malloc -Dcalloc=db_test_calloc -Drealloc=db_test_realloc -Dfree=db_test_free -c -o $@ $<

test-database: $(DATABASE_BINS)
	@set -e; for binary in $(DATABASE_BINS); do \
		$(TEST_RUNNER) --report "$(BUILD_DIR)/fault/$${binary##*/}.json" -- "$$binary"; \
	done

$(BUILD_DIR)/sql/session_test.o: src/sql/session.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEPFLAGS) -Dcalloc=st_calloc -Dfree=st_free -c -o $@ $<

$(BUILD_DIR)/audit/sql/session_test.o: src/sql/session.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEPFLAGS) -Dmalloc=audit_malloc -Dcalloc=audit_calloc -Drealloc=audit_realloc -Dfree=audit_free -DSESSION_TEST_HOOK -c -o $@ $<

$(BUILD_DIR)/test_session: $(BUILD_DIR)/tests/test_session.o $(BUILD_DIR)/sql/session_test.o $(DATABASE_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD_DIR)/test_session_audit: $(BUILD_DIR)/tests/test_session_audit.o $(BUILD_DIR)/tests/alloc_audit.o $(BUILD_DIR)/audit/sql/session_test.o $(DATABASE_AUDIT_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD_DIR)/test_result_ledger: $(BUILD_DIR)/tests/test_result_ledger.o $(BUILD_DIR)/tests/alloc_audit.o $(BUILD_DIR)/audit/sql/result.o $(BUILD_DIR)/storage/value.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD_DIR)/test_heap_ledger: $(BUILD_DIR)/tests/test_heap_ledger.o $(BUILD_DIR)/tests/alloc_audit.o $(BUILD_DIR)/audit/storage/heap_store.o $(BUILD_DIR)/audit/storage/record.o $(filter-out $(BUILD_DIR)/storage/record.o,$(STORAGE_OBJS))
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

test-session: $(BUILD_DIR)/test_session $(BUILD_DIR)/test_session_audit $(BUILD_DIR)/test_result_ledger $(BUILD_DIR)/test_heap_ledger
	$(TEST_RUNNER) --report $(BUILD_DIR)/fault/test_session.json -- ./$(BUILD_DIR)/test_session
	$(TEST_RUNNER) --report $(BUILD_DIR)/fault/test_session_audit.json -- ./$(BUILD_DIR)/test_session_audit
	$(TEST_RUNNER) --report $(BUILD_DIR)/fault/test_result_ledger.json -- ./$(BUILD_DIR)/test_result_ledger
	$(TEST_RUNNER) --report $(BUILD_DIR)/fault/test_heap_ledger.json -- ./$(BUILD_DIR)/test_heap_ledger

$(BUILD_DIR)/test_corrupt_open: $(BUILD_DIR)/tests/test_corrupt_open.o $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

test-corrupt-open: $(BUILD_DIR)/test_corrupt_open $(BUILD_DIR)/minidb
	$(TEST_RUNNER) --report $(BUILD_DIR)/fault/test_corrupt_open.json -- ./$(BUILD_DIR)/test_corrupt_open $(BUILD_DIR)/corrupt-open
	$(TEST_RUNNER) --report $(BUILD_DIR)/fault/test_corrupt_open_cli.json -- $(PYTHON) tests/test_corrupt_open_cli.py --minidb ./$(BUILD_DIR)/minidb --workdir $(BUILD_DIR)/corrupt-open-cli

$(BUILD_DIR)/test_transaction: $(BUILD_DIR)/tests/test_transaction.o $(BUILD_DIR)/storage/transaction.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD_DIR)/test_transaction_fault: $(BUILD_DIR)/tests/test_transaction_fault.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD_DIR)/test_page_file_durable: $(BUILD_DIR)/tests/test_page_file_durable.o $(STORAGE_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD_DIR)/test_durable_pair: $(BUILD_DIR)/tests/test_durable_pair.o $(BUILD_DIR)/storage/durable_pair.o $(BUILD_DIR)/storage/page_file.o $(BUILD_DIR)/storage/wal.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD_DIR)/test_database_txn: $(BUILD_DIR)/tests/test_database_txn.o $(DATABASE_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD_DIR)/test_session_txn: $(BUILD_DIR)/tests/test_session_txn.o $(BUILD_DIR)/sql/session.o $(DATABASE_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

test-transaction: $(BUILD_DIR)/test_transaction $(BUILD_DIR)/test_transaction_fault $(BUILD_DIR)/test_page_file_durable $(BUILD_DIR)/test_durable_pair $(BUILD_DIR)/test_database_txn $(BUILD_DIR)/test_session_txn
	$(TEST_RUNNER) --report $(BUILD_DIR)/fault/test_transaction.json -- ./$(BUILD_DIR)/test_transaction
	$(TEST_RUNNER) --report $(BUILD_DIR)/fault/test_transaction_fault.json -- ./$(BUILD_DIR)/test_transaction_fault
	$(TEST_RUNNER) --report $(BUILD_DIR)/fault/test_page_file_durable.json -- ./$(BUILD_DIR)/test_page_file_durable
	$(TEST_RUNNER) --report $(BUILD_DIR)/fault/test_durable_pair.json -- ./$(BUILD_DIR)/test_durable_pair
	$(TEST_RUNNER) --report $(BUILD_DIR)/fault/test_database_txn.json -- ./$(BUILD_DIR)/test_database_txn
	$(TEST_RUNNER) --report $(BUILD_DIR)/fault/test_session_txn.json -- ./$(BUILD_DIR)/test_session_txn

$(BUILD_DIR)/session_probe: $(BUILD_DIR)/tests/session_probe.o $(BUILD_DIR)/sql/session.o $(DATABASE_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# SQLite 3.51.0과 같은 입력으로 두 연결을 비교한다. 다른 SQLite 버전은 not-run이라 test-all에 넣지 않는다
SQLITE_DIFF_DIR ?= $(BUILD_DIR)/sqlite-session-diff

test-sqlite-session-diff-harness:
	$(PYTEST) -q tests/test_sqlite_session_diff.py --basetemp=$(BUILD_DIR)/sqlite-session-diff-harness -p no:cacheprovider

test-sqlite-session-diff: $(BUILD_DIR)/session_probe test-sqlite-session-diff-harness
	$(PYTHON) tests/sqlite_session_diff.py --probe ./$(BUILD_DIR)/session_probe --workdir $(SQLITE_DIFF_DIR)

$(BUILD_DIR)/test_wal: $(BUILD_DIR)/tests/test_wal.o $(BUILD_DIR)/storage/wal.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD_DIR)/test_wal_crash: $(BUILD_DIR)/tests/test_wal_crash.o $(BUILD_DIR)/storage/wal.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD_DIR)/test_wal_alloc: $(BUILD_DIR)/tests/test_wal_alloc.o
	$(CC) $^ -o $@ $(LDFLAGS)

test-wal: $(BUILD_DIR)/test_wal $(BUILD_DIR)/test_wal_crash $(BUILD_DIR)/test_wal_alloc
	cd $(BUILD_DIR) && $(PYTHON) $(abspath tests/fault_runner.py) --timeout $(TEST_TIMEOUT) --seed $(FAULT_SEED) --report fault/test_wal.json -- ./test_wal
	cd $(BUILD_DIR) && $(PYTHON) $(abspath tests/fault_runner.py) --timeout $(TEST_TIMEOUT) --seed $(FAULT_SEED) --report fault/test_wal_alloc.json -- ./test_wal_alloc
	cd $(BUILD_DIR) && $(PYTHON) $(abspath tests/fault_runner.py) --timeout 180 --seed $(FAULT_SEED) --report fault/test_wal_crash.json -- ./test_wal_crash

test-all: test test-prop test-step0 test-step1 test-step2 test-step3 test-legacy-adopt test-overflow-file test-value-sqlite test-value-varchar test-repl-boundary test-io-cli test-http-boundary-cli test-database test-corrupt-open test-transaction test-wal test-session test-lock-waiters test-ack-queue test-ack-queue-python

clean:
	rm -rf $(BUILD_DIR) build-o2 *.db __test__*.db __test_step3_*.db

.PHONY: test-runner test-fault test-concurrency test-result test-result-allocation test-overflow-file test-value-sqlite test-value-varchar test-repl-boundary test-io-cli test-http-boundary-cli test-sqlite-session-diff test-sqlite-session-diff-harness

-include $(OBJS:.o=.d) $(STORAGE_OBJS:.o=.d) $(BUILD_DIR)/main.d
-include $(wildcard $(BUILD_DIR)/tests/*.d $(BUILD_DIR)/storage/*_fault.d $(BUILD_DIR)/sql/lexer*.d)
-include $(HOOK_OBJS:.o=.d) $(wildcard $(BUILD_DIR)/hooks/tests/*.d)
-include $(BUILD_DIR)/hooks/main.d
-include $(BUILD_DIR)/sql/result.d $(BUILD_DIR)/sql/result_fault.d
-include $(DATABASE_OBJS:.o=.d) $(BUILD_DIR)/sql/database_fault.d $(BUILD_DIR)/sql/sql_statement_fault.d $(BUILD_DIR)/storage/heap_store_test.d
-include $(DATABASE_AUDIT_OBJS:.o=.d)

-include $(BUILD_DIR)/storage/transaction.d
-include $(BUILD_DIR)/storage/wal.d

.PHONY: test-database test-corrupt-open test-transaction test-wal test-session test-lock-waiters

.PHONY: all test test-prop test-step0 test-step1 test-step2 test-step3 test-all run run-server gen bench bench-1m bench-http stress clean

# 선택형 ACK 큐(#140). 큐 파일이 없으면 무시한다.
-include src/queue/queue.mk
