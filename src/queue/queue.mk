# 선택형 ACK 큐(#140) 빌드 규칙. Makefile이 -include로 읽는다. DB 코어 빌드에는 큐가 포함되지 않는다.
# 큐는 세션(session.o)으로 문장을 실행한다.
# DB 코어(DATABASE_OBJS)는 큐 객체를 링크하지 않는다. 큐를 쓰는 바이너리만 QUEUE_OBJS를 더한다.

QUEUE_OBJS = $(BUILD_DIR)/queue/ack_queue.o $(BUILD_DIR)/queue/ack_queue_sql.o
QUEUE_TESTS = test_ack_queue test_ack_queue_fault test_totedb
QUEUE_BINS = $(addprefix $(BUILD_DIR)/,$(QUEUE_TESTS))

$(QUEUE_BINS): $(BUILD_DIR)/%: $(BUILD_DIR)/tests/%.o $(QUEUE_OBJS) $(BUILD_DIR)/sql/session.o $(DATABASE_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

test-ack-queue: $(QUEUE_BINS)
	@set -e; for binary in $(QUEUE_BINS); do \
		$(TEST_RUNNER) --report "$(BUILD_DIR)/fault/$${binary##*/}.json" -- "$$binary"; \
	done

-include $(QUEUE_OBJS:.o=.d) $(addprefix $(BUILD_DIR)/tests/,$(addsuffix .d,$(QUEUE_TESTS)))

.PHONY: test-ack-queue

# Python 어댑터가 쓰는 공유 라이브러리. 센티널(sanitizer)을 켜지 않은 최적화 빌드이며 DB 소스와 큐 소스를 한 번에 묶는다.
TOTEDB_SHARED_EXT = $(if $(filter Darwin,$(UNAME_S)),dylib,so)
TOTEDB_SHARED_LIB = $(BUILD_DIR)/lib/libtotedb.$(TOTEDB_SHARED_EXT)
TOTEDB_SHARED_SRCS = $(patsubst $(BUILD_DIR)/%.o,$(SRC_DIR)/%.c,$(DATABASE_OBJS)) \
                    src/sql/session.c src/queue/ack_queue.c src/queue/ack_queue_sql.c src/capi/totedb.c

$(TOTEDB_SHARED_LIB): $(TOTEDB_SHARED_SRCS) $(wildcard include/*/*.h) $(wildcard src/queue/*.h)
	@mkdir -p $(dir $@)
	$(CC) -O2 -g -fPIC -shared $(WARNFLAGS) $(INCLUDES) -o $@ $(TOTEDB_SHARED_SRCS) $(LDLIBS)

# Python 시험은 pytest가 필요하다. TOTEDB_PYTHON으로 pytest가 있는 인터프리터를 고른다.
TOTEDB_PYTHON ?= $(PYTHON)

# 여러 언어 예제(#201)의 C worker. 공유 라이브러리와 같은 최적화 빌드로 공개 헤더 totedb.h만 쓴다
OUTBOX_CONSUMER = $(BUILD_DIR)/outbox_consumer

$(OUTBOX_CONSUMER): examples/outbox/consumer.c include/totedb.h $(TOTEDB_SHARED_LIB)
	$(CC) -O2 -g $(WARNFLAGS) -Iinclude -o $@ $< -L$(dir $(TOTEDB_SHARED_LIB)) -ltotedb -Wl,-rpath,$(abspath $(dir $(TOTEDB_SHARED_LIB)))

test-ack-queue-python: $(TOTEDB_SHARED_LIB) $(OUTBOX_CONSUMER)
	TOTEDB_LIB=$(abspath $(TOTEDB_SHARED_LIB)) OUTBOX_CONSUMER=$(abspath $(OUTBOX_CONSUMER)) PYTHONPATH=python $(TOTEDB_PYTHON) -m pytest -q tests/test_ack_queue_python.py tests/test_ack_queue_markchain_contract.py tests/test_database_python.py tests/test_multiprocess.py tests/test_outbox_example.py

# Python 생산자와 C worker가 같은 큐를 쓰는 예제를 실행한다
example-outbox: $(TOTEDB_SHARED_LIB) $(OUTBOX_CONSUMER)
	TOTEDB_LIB=$(abspath $(TOTEDB_SHARED_LIB)) PYTHONPATH=python $(TOTEDB_PYTHON) examples/outbox/run.py --consumer $(OUTBOX_CONSUMER)

# 새 엔진·SQLite·PostgreSQL 3자 SQL 차분. SQLDIFF_DSN이 있으면 PostgreSQL까지 반드시 비교한다(psycopg 필요).
test-sql-diff: $(TOTEDB_SHARED_LIB)
	TOTEDB_LIB=$(abspath $(TOTEDB_SHARED_LIB)) PYTHONPATH=python $(TOTEDB_PYTHON) tests/sql_diff.py \
		$(if $(SQLDIFF_DSN),--dsn $(SQLDIFF_DSN) --require-postgres)

.PHONY: test-ack-queue-python test-sql-diff example-outbox
