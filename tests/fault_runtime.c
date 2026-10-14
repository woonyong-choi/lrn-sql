/* #133: 고정 지점 이름·호출 순번과 seed를 남기는 시험용 runtime. */
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "fault_runtime.h"

static const char *const builtin_sites[] = {
    "alloc.malloc", "alloc.realloc", "lock.wait", "file.pread", "file.pwrite",
    "file.fsync", "socket.write", "txn.begin", "txn.commit", "heap.split",
    "index.split", "wal.append", "wal.flush", "checkpoint.publish", "process.kill"
};

void fault_runtime_init(fault_runtime_t *runtime, FILE *trace) {
    memset(runtime, 0, sizeof(*runtime));
    runtime->trace = trace;
    runtime->seed = 1;
    runtime->random_state = 1;
    runtime->target_call = 1;
    runtime->target_site = -1;
    for (size_t i = 0; i < sizeof(builtin_sites) / sizeof(builtin_sites[0]); ++i) {
        strcpy(runtime->sites[i].name, builtin_sites[i]);
        ++runtime->site_count;
    }
}

int fault_find(const fault_runtime_t *runtime, const char *name) {
    if (name == NULL) return -1;
    for (size_t i = 0; i < runtime->site_count; ++i) {
        if (strcmp(runtime->sites[i].name, name) == 0) return (int)i;
    }
    return -1;
}

int fault_register(fault_runtime_t *runtime, const char *name) {
    if (name == NULL || name[0] == '\0') return -1;
    if (runtime->sequence != 0 || runtime->site_count == FAULT_MAX_SITES) return -1;
    size_t length = strlen(name);
    if (length >= FAULT_NAME_BYTES || fault_find(runtime, name) >= 0) return -1;
    for (size_t i = 0; i < length; ++i) {
        int valid = ('a' <= name[i] && name[i] <= 'z');
        valid |= ('0' <= name[i] && name[i] <= '9');
        valid |= name[i] == '_' || name[i] == '.' || name[i] == '-';
        if (!valid) return -1;
    }
    size_t index = runtime->site_count++;
    memcpy(runtime->sites[index].name, name, length + 1);
    return (int)index;
}

static int parse_number(const char *text, uint64_t *number) {
    if (text == NULL) return 0;
    if (text[0] == '\0') return -1;
    uint64_t value = 0;
    for (size_t i = 0; text[i] != '\0'; ++i) {
        if (text[i] < '0' || text[i] > '9') return -1;
        unsigned digit = (unsigned)(text[i] - '0');
        if (value > (UINT64_MAX - digit) / 10) return -1;
        value = value * 10 + digit;
    }
    *number = value;
    return 0;
}

int fault_configure_env(fault_runtime_t *runtime) {
    if (runtime->sequence != 0 || runtime->trace == NULL) return -1;
    uint64_t seed = 1, call = 1;
    if (parse_number(getenv("FAULT_SEED"), &seed) != 0) return -1;
    if (parse_number(getenv("FAULT_CALL"), &call) != 0 || call == 0) return -1;
    const char *name = getenv("FAULT_SITE");
    int site = fault_find(runtime, name);
    if (name != NULL && site < 0) return -1;
    runtime->seed = seed;
    runtime->random_state = seed;
    runtime->target_call = call;
    runtime->target_site = site;
    return 0;
}

// cost: time O(1), heap O(1), stack O(1), io O(1)
// basis: estimate
int fault_probe(fault_runtime_t *runtime, int site, uint64_t object_id) {
    if (site < 0 || (size_t)site >= runtime->site_count) return -1;
    if (runtime->trace_failed || runtime->trace == NULL) return -1;
    fault_site_t *point = &runtime->sites[site];
    if (runtime->sequence == UINT64_MAX || point->calls == UINT64_MAX) return -1;
    ++point->calls;
    ++runtime->sequence;
    int inject = site == runtime->target_site && point->calls == runtime->target_call;
    int written = fprintf(runtime->trace,
        "{\"sequence\":%" PRIu64 ",\"site\":\"%s\",\"call\":%" PRIu64
        ",\"seed\":%" PRIu64 ",\"object\":%" PRIu64 ",\"inject\":%s}\n",
        runtime->sequence, point->name, point->calls, runtime->seed, object_id,
        inject ? "true" : "false");
    int flushed = fflush(runtime->trace);
    if (written < 0 || flushed != 0) {
        runtime->trace_failed = 1;
        return -1;
    }
    return inject;
}

uint64_t fault_random(fault_runtime_t *runtime) {
    uint64_t value = (runtime->random_state += UINT64_C(0x9e3779b97f4a7c15));
    value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}
