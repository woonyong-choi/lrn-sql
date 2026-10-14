#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "storage/overflow.h"

#define PAGE_BYTES 256u

typedef struct {
    FILE *file;
    uint32_t next_page;
} file_store_t;

static bool file_allocate(void *context, uint32_t *id)
{
    file_store_t *store = context;
    *id = store->next_page++;
    return true;
}

static bool file_release(void *context, uint32_t id)
{
    (void)context;
    (void)id;
    return true;
}

static bool file_read(void *context, uint32_t id, uint8_t *page)
{
    file_store_t *store = context;
    return fseek(store->file, (long)id * PAGE_BYTES, SEEK_SET) == 0 &&
           fread(page, 1, PAGE_BYTES, store->file) == PAGE_BYTES;
}

static bool file_write(void *context, uint32_t id, const uint8_t *page)
{
    file_store_t *store = context;
    return fseek(store->file, (long)id * PAGE_BYTES, SEEK_SET) == 0 &&
           fwrite(page, 1, PAGE_BYTES, store->file) == PAGE_BYTES;
}

static overflow_io_t io_for(file_store_t *store)
{
    return (overflow_io_t){store, PAGE_BYTES, file_allocate, file_release,
                           file_read, file_write};
}

/*
 * #81: 합성 데이터를 실제 파일에 쓰고 닫은 뒤 새 핸들로 다시 읽는다.
 * 정상 close/reopen만 확인하며 전원 장애·중간 crash 복구는 검증하지 않는다.
 * 사용법: test_overflow_file <새로 만들 파일 경로>. 경로가 없으면 검사하지 않고 종료 2.
 * 성공하면 합성 파일을 지운다.
 * 페이지 0은 예약이라 첫 할당 번호는 1이다. 할당표 복원은 이 검사의 범위가 아니다.
 */
int main(int argc, char **argv)
{
    if (argc != 2) {
        fputs("overflow file: path argument missing, restart not verified\n", stderr);
        return 2;
    }
    enum { SIZE = 20000 };
    uint8_t *bytes = malloc(SIZE), *back = NULL;
    for (size_t i = 0; i < SIZE; i++) bytes[i] = (uint8_t)(i * 7 + (i >> 8));

    file_store_t writer = {fopen(argv[1], "wb+"), 1};
    assert(writer.file != NULL);
    uint8_t zero[PAGE_BYTES] = {0};
    assert(fwrite(zero, 1, PAGE_BYTES, writer.file) == PAGE_BYTES);
    overflow_io_t write_io = io_for(&writer);
    overflow_ref_t ref;
    assert(overflow_write(&write_io, bytes, SIZE, &ref) == OVERFLOW_OK);
    assert(fclose(writer.file) == 0);

    /* 참조 8바이트만 넘겨받은 새 핸들이 같은 값을 읽어야 한다. */
    file_store_t reader = {fopen(argv[1], "rb"), 0};
    assert(reader.file != NULL);
    overflow_io_t read_io = io_for(&reader);
    assert(overflow_read(&read_io, ref, &back) == OVERFLOW_OK);
    assert(memcmp(back, bytes, SIZE) == 0);
    free(back);
    back = NULL;

    /* 파일에서 읽은 뒤 한 바이트가 바뀐 상태는 CORRUPT다. */
    assert(fclose(reader.file) == 0);
    FILE *patch = fopen(argv[1], "rb+");
    assert(patch != NULL && fseek(patch, (long)ref.head * PAGE_BYTES + 100, SEEK_SET) == 0);
    int byte = fgetc(patch);
    assert(byte != EOF && fseek(patch, -1, SEEK_CUR) == 0 && fputc(byte ^ 1, patch) != EOF);
    assert(fclose(patch) == 0);
    reader.file = fopen(argv[1], "rb");
    assert(reader.file != NULL);
    assert(overflow_read(&read_io, ref, &back) == OVERFLOW_CORRUPT && back == NULL);
    fclose(reader.file);
    free(bytes);
    assert(remove(argv[1]) == 0);
    puts("overflow file: normal close/reopen roundtrip; on-disk bit flip is detected (no crash recovery claim)");
    return 0;
}
