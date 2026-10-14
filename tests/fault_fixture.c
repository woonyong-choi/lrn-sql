/* #133: 실제 malloc/realloc·mutex·파일 쓰기를 감싼 최소 실패 재현 입력. */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fault_runtime.h"

enum { FIXTURE_INJECTED = 42, FIXTURE_ERROR = 70 };

static int allocation(fault_runtime_t *runtime) {
    size_t size = 16 + fault_random(runtime) % 48;
    int probe = fault_probe(runtime, fault_find(runtime, "alloc.malloc"), size);
    if (probe != 0) return probe > 0 ? FIXTURE_INJECTED : FIXTURE_ERROR;
    unsigned char *bytes = malloc(size);
    if (bytes == NULL) return FIXTURE_ERROR;
    memset(bytes, 0x5a, size);
    int status = 0;
    for (unsigned i = 1; i <= 2; ++i) {
        probe = fault_probe(runtime, fault_find(runtime, "alloc.realloc"), size * 2);
        unsigned char *next = probe == 0 ? realloc(bytes, size * 2) : NULL;
        if (next == NULL) {
            status = probe > 0 ? FIXTURE_INJECTED : FIXTURE_ERROR;
            for (size_t j = 0; j < size; ++j) {
                if (bytes[j] != 0x5a) status = FIXTURE_ERROR;
            }
            break;
        }
        bytes = next;
        size *= 2;
        memset(bytes, 0x5a, size);
    }
    free(bytes);
    return status;
}

static int locking(fault_runtime_t *runtime) {
    pthread_mutex_t lock;
    if (pthread_mutex_init(&lock, NULL) != 0) return FIXTURE_ERROR;
    int probe = fault_probe(runtime, fault_find(runtime, "lock.wait"), 1);
    int result = probe > 0 ? EBUSY : pthread_mutex_trylock(&lock);
    int status = 0;
    if (result == 0 && pthread_mutex_unlock(&lock) != 0) status = FIXTURE_ERROR;
    if (result == EBUSY && probe > 0) status = FIXTURE_INJECTED;
    else if (result != 0 || probe < 0) status = FIXTURE_ERROR;
    if (pthread_mutex_destroy(&lock) != 0) status = FIXTURE_ERROR;
    return status;
}

// cost: time O(1), heap O(1), stack O(1), io O(1)
// basis: estimate
static int file_write(fault_runtime_t *runtime, const char *path) {
    int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) return FIXTURE_ERROR;
    int status = 0;
    int probe = fault_probe(runtime, fault_find(runtime, "file.pwrite"), 0);
    if (probe > 0) status = FIXTURE_INJECTED;
    else if (probe < 0 || pwrite(fd, "new", 3, 0) != 3) status = FIXTURE_ERROR;
    if (status == 0) {
        probe = fault_probe(runtime, fault_find(runtime, "file.fsync"), 0);
        if (probe > 0) status = FIXTURE_INJECTED;
        else if (probe < 0 || fsync(fd) != 0) status = FIXTURE_ERROR;
    }
    if (close(fd) != 0) status = FIXTURE_ERROR;
    if (unlink(path) != 0) status = FIXTURE_ERROR;
    return status;
}

int main(int argc, char **argv) {
    if (argc < 2) return FIXTURE_ERROR;
    fault_runtime_t runtime;
    fault_runtime_init(&runtime, stderr);
    if (fault_configure_env(&runtime) != 0) return FIXTURE_ERROR;
    int status = FIXTURE_ERROR;
    if (strcmp(argv[1], "alloc") == 0) status = allocation(&runtime);
    else if (strcmp(argv[1], "lock") == 0) status = locking(&runtime);
    else if (strcmp(argv[1], "file") == 0 && argc == 3) status = file_write(&runtime, argv[2]);
    else if (strcmp(argv[1], "kill") == 0) {
        int probe = fault_probe(&runtime, fault_find(&runtime, "process.kill"), 0);
        if (probe > 0) raise(SIGKILL);
        status = probe == 0 ? 0 : FIXTURE_ERROR;
    }
    printf("{\"status\":%d,\"resources\":{\"allocations\":[],\"locks\":[],\"fd\":[]}}\n", status);
    return status;
}
