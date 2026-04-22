CC      = gcc
WARNFLAGS = -Wall -Wextra -Werror
INCLUDES = -Iinclude
CFLAGS  = $(WARNFLAGS) -g $(INCLUDES) -fsanitize=address,undefined
LDFLAGS = -fsanitize=address,undefined -lpthread

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
       $(SRC_DIR)/server/thread_pool.c \
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
$(BUILD_DIR)/bench: tools/bench.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(WARNFLAGS) -g $(INCLUDES) -o $@ $< -lpthread

bench: $(BUILD_DIR)/bench
	@echo "서버를 먼저 실행하세요: make run-server"
	./$(BUILD_DIR)/bench 127.0.0.1 8080 4 100

run-server: $(BUILD_DIR)/minidb
	./$(BUILD_DIR)/minidb --server 8080 bench.db

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

$(BUILD_DIR)/test_step2: tests/test_step2_concurrency.c $(OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

test-step2: $(BUILD_DIR)/test_step2
	./$(BUILD_DIR)/test_step2

test-all: test test-step0 test-step1 test-step2

clean:
	rm -rf $(BUILD_DIR) *.db __test__*.db

.PHONY: all test test-step0 test-step1 test-step2 test-all run run-server gen bench clean
