/* #133: 등록 오류·숫자 경계·trace 실패가 성공 주입으로 바뀌지 않는다. */
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "fault_runtime.h"

static void registration(FILE *trace) {
    fault_runtime_t runtime;
    fault_runtime_init(&runtime, trace);
    const char *required[] = {"txn.begin", "txn.commit", "heap.split", "index.split",
                             "wal.append", "wal.flush", "checkpoint.publish"};
    for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); ++i)
        assert(fault_find(&runtime, required[i]) >= 0);
    assert(fault_register(&runtime, "heap.split") == -1);
    assert(fault_register(&runtime, "bad\"name") == -1);
    assert(fault_register(&runtime, "") == -1);
    int custom = fault_register(&runtime, "fixture.custom");
    assert(custom >= 0);
    assert(fault_probe(&runtime, custom, 99) == 0);
    assert(fault_register(&runtime, "late.site") == -1);
    assert(fault_probe(&runtime, -1, 0) == -1);
    assert(fault_probe(&runtime, FAULT_MAX_SITES, 0) == -1);
}

static void configuration(FILE *trace) {
    fault_runtime_t runtime;
    const char *invalid[] = {"", "-1", "+1", " 1", "1x", "18446744073709551616"};
    unsetenv("FAULT_SITE");
    unsetenv("FAULT_CALL");
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        fault_runtime_init(&runtime, trace);
        setenv("FAULT_SEED", invalid[i], 1);
        assert(fault_configure_env(&runtime) == -1);
    }
    setenv("FAULT_SEED", "18446744073709551615", 1);
    fault_runtime_init(&runtime, trace);
    assert(fault_configure_env(&runtime) == 0 && runtime.seed == UINT64_MAX);
    setenv("FAULT_CALL", "0", 1);
    assert(fault_configure_env(&runtime) == -1);
    setenv("FAULT_CALL", "2", 1);
    setenv("FAULT_SITE", "unknown", 1);
    assert(fault_configure_env(&runtime) == -1);
    setenv("FAULT_SITE", "alloc.realloc", 1);
    assert(fault_configure_env(&runtime) == 0);
    int site = fault_find(&runtime, "alloc.realloc");
    assert(fault_probe(&runtime, site, 4) == 0);
    assert(fault_probe(&runtime, site, 8) == 1);
    assert(fault_probe(&runtime, site, 16) == 0);
    assert(fault_configure_env(&runtime) == -1);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    FILE *trace = fopen(argv[1], "w");
    assert(trace != NULL);
    registration(trace);
    configuration(trace);
    assert(fclose(trace) == 0);

    trace = fopen(argv[1], "r");
    assert(trace != NULL);
    fault_runtime_t runtime;
    fault_runtime_init(&runtime, trace);
    assert(fault_probe(&runtime, 0, 1) == -1);
    assert(runtime.trace_failed);
    clearerr(trace);
    assert(fault_probe(&runtime, 0, 2) == -1);
    assert(runtime.sequence == 1);
    assert(fclose(trace) == 0);
    assert(remove(argv[1]) == 0);
    puts("fault runtime: registry, configuration and trace errors passed");
    return 0;
}
