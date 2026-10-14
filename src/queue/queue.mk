# 선택형 ACK 큐(#140) 빌드 규칙. Makefile이 -include로 읽는다. 이 파일을 지워도 DB 빌드와 test-all은 바뀌지 않는다.
# DB 코어(DATABASE_OBJS)는 큐 객체를 링크하지 않는다. 큐를 쓰는 바이너리만 QUEUE_OBJS를 더한다.

QUEUE_OBJS = $(BUILD_DIR)/queue/ack_queue.o $(BUILD_DIR)/queue/ack_queue_sql.o
QUEUE_TESTS = test_ack_queue test_ack_queue_fault
QUEUE_BINS = $(addprefix $(BUILD_DIR)/,$(QUEUE_TESTS))

$(QUEUE_BINS): $(BUILD_DIR)/%: $(BUILD_DIR)/tests/%.o $(QUEUE_OBJS) $(DATABASE_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

test-ack-queue: $(QUEUE_BINS)
	@set -e; for binary in $(QUEUE_BINS); do \
		$(TEST_RUNNER) --report "$(BUILD_DIR)/fault/$${binary##*/}.json" -- "$$binary"; \
	done

-include $(QUEUE_OBJS:.o=.d) $(addprefix $(BUILD_DIR)/tests/,$(addsuffix .d,$(QUEUE_TESTS)))

.PHONY: test-ack-queue
