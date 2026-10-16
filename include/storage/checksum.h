#ifndef STORAGE_CHECKSUM_H
#define STORAGE_CHECKSUM_H

#include <stddef.h>
#include <stdint.h>

/*
 * CRC32C(Castagnoli, 다항식 0x1EDC6F41). 페이지·overflow 체크섬에 쓴다. crc는 앞 조각의 결과이며 처음은 0이다
 * (crc32c(crc32c(0, a), b) == crc32c(0, a || b)). x86-64의 SSE4.2와 ARMv8 CRC 명령이 있으면 그것을 쓰고, 없으면
 * 표로 계산한다. 결과는 어느 경로든 같다.
 */
uint32_t crc32c(uint32_t crc, const void *data, size_t size);

#endif
