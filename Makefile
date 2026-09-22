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
LDFLAGS = $(SANITIZE_FLAGS) -lpthread

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
	$(CC) $(LDFLAGS) -o $@ $^

$(BUILD_DIR)/main.o: $(SRC_DIR)/main.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

# ── wk07 regression test ──
$(BUILD_DIR)/test_all: tests/test_all.c $(OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

test: $(BUILD_DIR)/test_all
	./$(BUILD_DIR)/test_all

# ── data generator ──
$(BUILD_DIR)/gen_data: tools/gen_data.c $(OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

run: $(BUILD_DIR)/minidb
	./$(BUILD_DIR)/minidb sql.db

N ?= 1000000
gen: $(BUILD_DIR)/gen_data
	./$(BUILD_DIR)/gen_data sql.db $(N)

# ── benchmark ──
#
# bench      규모별 기울기. 개선 전(993d4d8^)·개선 후·인덱스 비활성 세 빌드를
#            같은 소스에서 뽑아 같은 워크로드로 잰다. 약 1분.
#            결과: bench/scaling.md + docs/scaling.svg
# bench-1m   1M 행 단일 규모 측정 (개선 전 INSERT 가 느려 수 분 걸린다)
# bench-http HTTP 서버 경로 부하 생성기 (make run-server 가 먼저 필요)
bench:
	python3 bench/scaling.py

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
$(BUILD_DIR)/test_step0: tests/test_step0_db_execute.c $(OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

test-step0: $(BUILD_DIR)/test_step0
	./$(BUILD_DIR)/test_step0

$(BUILD_DIR)/test_step1: tests/test_step1_sql_ext.c $(OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

test-step1: $(BUILD_DIR)/test_step1
	./$(BUILD_DIR)/test_step1

$(BUILD_DIR)/test_prop: tests/test_bptree_property.c $(OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

test-prop: $(BUILD_DIR)/test_prop
	./$(BUILD_DIR)/test_prop

$(BUILD_DIR)/test_step2: tests/test_step2_concurrency.c $(OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

test-step2: $(BUILD_DIR)/test_step2
	./$(BUILD_DIR)/test_step2

test-all: test test-prop test-step0 test-step1 test-step2

clean:
	rm -rf $(BUILD_DIR) build-o2 *.db __test__*.db

.PHONY: all test test-prop test-step0 test-step1 test-step2 test-all run run-server gen bench bench-1m bench-http stress clean
