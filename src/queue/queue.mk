# 선택형 ACK 큐(#140) 빌드 규칙. Makefile이 -include로 읽는다. DB 코어 빌드에는 큐가 포함되지 않는다.
# DB 코어(DATABASE_OBJS)는 큐 객체를 링크하지 않는다. 큐를 쓰는 바이너리만 QUEUE_OBJS를 더한다.

QUEUE_OBJS = $(BUILD_DIR)/queue/ack_queue.o $(BUILD_DIR)/queue/ack_queue_sql.o
QUEUE_TESTS = test_ack_queue test_ack_queue_fault test_ack_queue_capi
QUEUE_BINS = $(addprefix $(BUILD_DIR)/,$(QUEUE_TESTS))

$(QUEUE_BINS): $(BUILD_DIR)/%: $(BUILD_DIR)/tests/%.o $(QUEUE_OBJS) $(DATABASE_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

test-ack-queue: $(QUEUE_BINS)
	@set -e; for binary in $(QUEUE_BINS); do \
		$(TEST_RUNNER) --report "$(BUILD_DIR)/fault/$${binary##*/}.json" -- "$$binary"; \
	done

-include $(QUEUE_OBJS:.o=.d) $(addprefix $(BUILD_DIR)/tests/,$(addsuffix .d,$(QUEUE_TESTS)))

.PHONY: test-ack-queue

# Python 어댑터가 쓰는 공유 라이브러리. 센티널(sanitizer)을 켜지 않은 최적화 빌드이며 DB 소스와 큐 소스를 한 번에 묶는다.
QUEUE_SHARED_EXT = $(if $(filter Darwin,$(UNAME_S)),dylib,so)
QUEUE_SHARED_LIB = $(BUILD_DIR)/queue-shared/liblrnackq.$(QUEUE_SHARED_EXT)
QUEUE_SHARED_SRCS = $(patsubst $(BUILD_DIR)/%.o,$(SRC_DIR)/%.c,$(DATABASE_OBJS)) \
                    src/queue/ack_queue.c src/queue/ack_queue_sql.c src/queue/ack_queue_capi.c

$(QUEUE_SHARED_LIB): $(QUEUE_SHARED_SRCS) $(wildcard include/*/*.h) $(wildcard src/queue/*.h)
	@mkdir -p $(dir $@)
	$(CC) -O2 -g -fPIC -shared $(WARNFLAGS) $(INCLUDES) -o $@ $(QUEUE_SHARED_SRCS) $(LDLIBS)

# Python 시험은 pytest가 필요하다. ACKQ_PYTHON으로 pytest가 있는 인터프리터를 고른다.
ACKQ_PYTHON ?= $(PYTHON)

test-ack-queue-python: $(QUEUE_SHARED_LIB)
	LRN_ACKQ_LIB=$(abspath $(QUEUE_SHARED_LIB)) PYTHONPATH=python $(ACKQ_PYTHON) -m pytest -q tests/test_ack_queue_python.py tests/test_ack_queue_markchain_contract.py

# 새 엔진·SQLite·PostgreSQL 3자 SQL 차분. SQLDIFF_DSN이 있으면 PostgreSQL까지 반드시 비교한다(psycopg 필요).
test-sql-diff: $(QUEUE_SHARED_LIB)
	LRN_ACKQ_LIB=$(abspath $(QUEUE_SHARED_LIB)) PYTHONPATH=python $(ACKQ_PYTHON) tests/sql_diff.py \
		$(if $(SQLDIFF_DSN),--dsn $(SQLDIFF_DSN) --require-postgres)

.PHONY: test-ack-queue-python test-sql-diff
