CC      = gcc
WARNFLAGS = -Wall -Wextra -Werror
INCLUDES = -Iinclude
# ── sanitizer 기본값 ──
#
# macOS 26(Darwin 25) 이상에서는 ASan 런타임이 초기화 도중 자기 자신을
# 재진입해(AsanInitInternal → ... → malloc → AsanInitFromRtl) spin lock 에
# 걸린다. 프로세스가 main() 에 닿지 못하므로 ASan 을 켠 빌드는 무엇을 하든
# 멈춘다. ToteDB 의 문제가 아니라 플랫폼 문제이고, 빈 main() 으로도 재현된다.
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

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c -o $@ $<

# 각 테스트 번역 단위도 헤더 의존성을 추적한다.
$(BUILD_DIR)/tests/%.o: tests/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c -o $@ $<

STORAGE_OBJS = $(BUILD_DIR)/storage/record.o $(BUILD_DIR)/storage/record_page.o \
               $(BUILD_DIR)/storage/overflow.o $(BUILD_DIR)/storage/checksum.o $(BUILD_DIR)/storage/value.o $(BUILD_DIR)/storage/catalog.o \
               $(BUILD_DIR)/storage/page_file.o $(BUILD_DIR)/storage/page_cache.o $(BUILD_DIR)/storage/catalog_store.o \
               $(BUILD_DIR)/storage/pk_tree.o $(BUILD_DIR)/storage/text_tree.o
BASIC_TESTS = test_record test_record_fault test_overflow test_overflow_fault test_value test_value_fault \
              test_catalog test_catalog_fault test_lexer test_lexer_fault test_page_file test_catalog_store \
              test_pk_tree test_text_tree
TEST_BINS = $(addprefix $(BUILD_DIR)/,$(BASIC_TESTS))

$(BUILD_DIR)/test_record: $(BUILD_DIR)/tests/test_record.o $(BUILD_DIR)/storage/record.o $(BUILD_DIR)/storage/record_page.o
$(BUILD_DIR)/test_overflow: $(BUILD_DIR)/tests/test_overflow.o $(BUILD_DIR)/storage/overflow.o $(BUILD_DIR)/storage/checksum.o $(BUILD_DIR)/storage/record_page.o
$(BUILD_DIR)/test_overflow_file: $(BUILD_DIR)/tests/test_overflow_file.o $(BUILD_DIR)/storage/overflow.o $(BUILD_DIR)/storage/checksum.o
$(BUILD_DIR)/test_value: $(BUILD_DIR)/tests/test_value.o $(BUILD_DIR)/storage/value.o
$(BUILD_DIR)/storage/value_varchar_hook.o: src/storage/value.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEPFLAGS) -Dmalloc=value_test_malloc -c -o $@ $<

$(BUILD_DIR)/test_value_varchar: $(BUILD_DIR)/tests/test_value_varchar.o $(BUILD_DIR)/storage/value_varchar_hook.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD_DIR)/test_value_sqlite: $(BUILD_DIR)/tests/test_value_sqlite.o $(BUILD_DIR)/storage/value.o
$(BUILD_DIR)/test_value_sqlite: LDLIBS += -lsqlite3
$(BUILD_DIR)/test_record_fault: $(BUILD_DIR)/tests/test_record_fault.o $(BUILD_DIR)/storage/record_fault.o $(BUILD_DIR)/storage/record_page_fault.o
$(BUILD_DIR)/test_overflow_fault: $(BUILD_DIR)/tests/test_overflow_fault.o $(BUILD_DIR)/storage/overflow_fault.o $(BUILD_DIR)/storage/checksum.o
$(BUILD_DIR)/test_value_fault: $(BUILD_DIR)/tests/test_value_fault.o $(BUILD_DIR)/storage/value_fault.o
$(BUILD_DIR)/test_catalog: $(BUILD_DIR)/tests/test_catalog.o $(BUILD_DIR)/storage/catalog.o
$(BUILD_DIR)/test_catalog_fault: $(BUILD_DIR)/tests/test_catalog_fault.o $(BUILD_DIR)/storage/catalog_fault.o
$(BUILD_DIR)/test_lexer: $(BUILD_DIR)/tests/test_lexer.o $(BUILD_DIR)/sql/lexer.o
$(BUILD_DIR)/test_lexer_fault: $(BUILD_DIR)/tests/test_lexer_fault.o $(BUILD_DIR)/sql/lexer_fault.o
$(BUILD_DIR)/test_page_file: $(BUILD_DIR)/tests/test_page_file.o $(STORAGE_OBJS)
$(BUILD_DIR)/test_catalog_store: $(BUILD_DIR)/tests/test_catalog_store.o $(STORAGE_OBJS)
$(BUILD_DIR)/test_pk_tree: $(BUILD_DIR)/tests/test_pk_tree.o $(BUILD_DIR)/storage/pk_tree.o
$(BUILD_DIR)/test_text_tree: $(BUILD_DIR)/tests/test_text_tree.o $(BUILD_DIR)/storage/text_tree.o

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

test-runner:
	$(TEST_RUNNER) --report $(BUILD_DIR)/fault/test_runner.json -- $(PYTEST) -q tests/test_fault_runner.py tests/test_fault_oracle.py --basetemp=$(BUILD_DIR)/runner-tests -p no:cacheprovider

$(BUILD_DIR)/test_fault_runtime: $(BUILD_DIR)/tests/test_fault_runtime.o $(BUILD_DIR)/tests/fault_runtime.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD_DIR)/fault_fixture: $(BUILD_DIR)/tests/fault_fixture.o $(BUILD_DIR)/tests/fault_runtime.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

test-fault: $(BUILD_DIR)/test_fault_runtime $(BUILD_DIR)/fault_fixture
	$(TEST_RUNNER) --report $(BUILD_DIR)/fault/test_fault_runtime.json -- ./$(BUILD_DIR)/test_fault_runtime $(BUILD_DIR)/fault/runtime.trace
	FAULT_FIXTURE=$(abspath $(BUILD_DIR)/fault_fixture) $(TEST_RUNNER) --report $(BUILD_DIR)/fault/test_fault_fixture.json -- $(PYTEST) -q tests/test_fault_fixture.py --basetemp=$(BUILD_DIR)/fault/fixture-tests -p no:cacheprovider

# SQL 실행기(#198). 파일마다 한 역할이며 공유 타입은 src/sql/database_exec.h에 있다
DATABASE_EXEC_NAMES = database database_expr database_subquery database_ddl database_insert database_select \
                      database_group database_join database_update database_check
DATABASE_EXEC_OBJS = $(addprefix $(BUILD_DIR)/sql/,$(addsuffix .o,$(DATABASE_EXEC_NAMES)))
DATABASE_EXEC_FAULT_OBJS = $(addprefix $(BUILD_DIR)/sql/,$(addsuffix _fault.o,$(DATABASE_EXEC_NAMES)))
DATABASE_OBJS = $(STORAGE_OBJS) $(BUILD_DIR)/storage/heap_store.o $(BUILD_DIR)/storage/transaction.o \
                $(DATABASE_EXEC_OBJS) $(BUILD_DIR)/sql/database_txn.o \
                $(BUILD_DIR)/sql/sql_statement.o $(BUILD_DIR)/sql/sql_eval.o $(BUILD_DIR)/sql/lexer.o $(BUILD_DIR)/sql/result.o
DATABASE_TESTS = test_sql_statement test_heap_store test_database test_database_sqlite test_database_fault test_database_bounded_open test_primary_index
DATABASE_BINS = $(addprefix $(BUILD_DIR)/,$(DATABASE_TESTS))
DATABASE_AUDIT_OBJS = $(patsubst $(BUILD_DIR)/%.o,$(BUILD_DIR)/audit/%.o,$(DATABASE_OBJS))

$(BUILD_DIR)/test_sql_statement: $(BUILD_DIR)/tests/test_sql_statement.o $(BUILD_DIR)/sql/sql_statement_fault.o $(BUILD_DIR)/sql/lexer.o $(BUILD_DIR)/storage/value_statement_fault.o
$(BUILD_DIR)/test_heap_store: $(BUILD_DIR)/tests/test_heap_store.o $(STORAGE_OBJS) $(BUILD_DIR)/storage/heap_store_test.o
$(BUILD_DIR)/test_database: $(BUILD_DIR)/tests/test_database.o $(DATABASE_OBJS)
$(BUILD_DIR)/bench_secondary_index: $(BUILD_DIR)/tests/bench_secondary_index.o $(DATABASE_OBJS)
$(BUILD_DIR)/bench_secondary_index: LDLIBS += -lsqlite3
$(BUILD_DIR)/bench_secondary_index:
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)
$(BUILD_DIR)/bench_primary_range: $(BUILD_DIR)/tests/bench_primary_range.o $(DATABASE_OBJS)
$(BUILD_DIR)/bench_primary_range: LDLIBS += -lsqlite3
$(BUILD_DIR)/bench_primary_range:
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)
$(BUILD_DIR)/bench_free_space: $(BUILD_DIR)/tests/bench_free_space.o $(DATABASE_OBJS)
$(BUILD_DIR)/bench_free_space: LDLIBS += -lsqlite3
$(BUILD_DIR)/bench_free_space:
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)
$(BUILD_DIR)/test_database_sqlite: $(BUILD_DIR)/tests/test_database_sqlite.o $(DATABASE_OBJS)
$(BUILD_DIR)/test_primary_index: $(BUILD_DIR)/tests/test_primary_index.o $(DATABASE_OBJS)
$(BUILD_DIR)/test_database_fault: $(BUILD_DIR)/tests/test_database_fault.o $(filter-out $(DATABASE_EXEC_OBJS) $(BUILD_DIR)/storage/heap_store.o,$(DATABASE_OBJS)) $(DATABASE_EXEC_FAULT_OBJS) $(BUILD_DIR)/storage/heap_store_fault.o
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

$(DATABASE_EXEC_FAULT_OBJS): $(BUILD_DIR)/sql/%_fault.o: src/sql/%.c
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

$(BUILD_DIR)/test_transaction: $(BUILD_DIR)/tests/test_transaction.o $(BUILD_DIR)/storage/transaction.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD_DIR)/test_transaction_fault: $(BUILD_DIR)/tests/test_transaction_fault.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD_DIR)/test_database_txn: $(BUILD_DIR)/tests/test_database_txn.o $(DATABASE_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD_DIR)/test_session_txn: $(BUILD_DIR)/tests/test_session_txn.o $(BUILD_DIR)/sql/session.o $(DATABASE_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

test-transaction: $(BUILD_DIR)/test_transaction $(BUILD_DIR)/test_transaction_fault $(BUILD_DIR)/test_database_txn $(BUILD_DIR)/test_session_txn
	$(TEST_RUNNER) --report $(BUILD_DIR)/fault/test_transaction.json -- ./$(BUILD_DIR)/test_transaction
	$(TEST_RUNNER) --report $(BUILD_DIR)/fault/test_transaction_fault.json -- ./$(BUILD_DIR)/test_transaction_fault
	$(TEST_RUNNER) --report $(BUILD_DIR)/fault/test_database_txn.json -- ./$(BUILD_DIR)/test_database_txn
	$(TEST_RUNNER) --report $(BUILD_DIR)/fault/test_session_txn.json -- ./$(BUILD_DIR)/test_session_txn

$(BUILD_DIR)/session_probe: $(BUILD_DIR)/tests/session_probe.o $(BUILD_DIR)/sql/session.o $(DATABASE_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# SQLite 3.51.0과 같은 입력으로 두 연결을 비교한다. 시스템 SQLite 버전과 상관없이 기준 버전 소스를 받아
# (sha256 확인) 공유 라이브러리로 빌드하고 파이썬 sqlite3 모듈이 그것을 쓰게 한다. 네트워크가 필요해 test-all에 넣지 않는다
SQLITE_DIFF_DIR ?= $(BUILD_DIR)/sqlite-session-diff
SQLITE_REF_VERSION := 3510000
SQLITE_REF_URL := https://www.sqlite.org/2025/sqlite-amalgamation-$(SQLITE_REF_VERSION).zip
SQLITE_REF_SHA256 := 1caf7116f2910600d04473ad69d37ec538fa62fa36adccd37b5e0e43647c98be
SQLITE_REF_DIR := $(BUILD_DIR)/sqlite-ref

$(SQLITE_REF_DIR)/libsqlite3.so.0:
	mkdir -p $(SQLITE_REF_DIR)
	curl -sSfL -o $(SQLITE_REF_DIR)/src.zip $(SQLITE_REF_URL)
	echo "$(SQLITE_REF_SHA256)  $(SQLITE_REF_DIR)/src.zip" | sha256sum -c --quiet -
	unzip -qo -d $(SQLITE_REF_DIR) $(SQLITE_REF_DIR)/src.zip
	gcc -O2 -fPIC -shared -Wl,-soname,libsqlite3.so.0 -o $@ \
		$(SQLITE_REF_DIR)/sqlite-amalgamation-$(SQLITE_REF_VERSION)/sqlite3.c -lm -lpthread

test-sqlite-session-diff-harness:
	$(PYTEST) -q tests/test_sqlite_session_diff.py --basetemp=$(BUILD_DIR)/sqlite-session-diff-harness -p no:cacheprovider

test-sqlite-session-diff: $(BUILD_DIR)/session_probe test-sqlite-session-diff-harness $(SQLITE_REF_DIR)/libsqlite3.so.0
	LD_LIBRARY_PATH=$(abspath $(SQLITE_REF_DIR)) $(PYTHON) tests/sqlite_session_diff.py \
		--probe ./$(BUILD_DIR)/session_probe --workdir $(SQLITE_DIFF_DIR)

test-all: test test-overflow-file test-value-sqlite test-value-varchar test-database test-transaction test-session test-ack-queue test-ack-queue-python

clean:
	rm -rf $(BUILD_DIR)

.PHONY: test-runner test-fault test-result test-overflow-file test-value-sqlite test-value-varchar test-sqlite-session-diff test-sqlite-session-diff-harness

-include $(STORAGE_OBJS:.o=.d)
-include $(wildcard $(BUILD_DIR)/tests/*.d $(BUILD_DIR)/storage/*_fault.d $(BUILD_DIR)/sql/lexer*.d)
-include $(BUILD_DIR)/sql/result.d $(BUILD_DIR)/sql/result_fault.d
-include $(DATABASE_OBJS:.o=.d) $(DATABASE_EXEC_FAULT_OBJS:.o=.d) $(BUILD_DIR)/sql/sql_statement_fault.d $(BUILD_DIR)/storage/heap_store_test.d
-include $(DATABASE_AUDIT_OBJS:.o=.d)

-include $(BUILD_DIR)/storage/transaction.d

.PHONY: test-database test-transaction test-session

.PHONY: all test test-all clean

# 선택형 ACK 큐(#140). 큐 파일이 없으면 무시한다.
-include src/queue/queue.mk

# 기본 대상: DB와 큐를 묶은 공유 라이브러리(파이썬 바인딩이 쓴다)
all: $(TOTEDB_SHARED_LIB)
