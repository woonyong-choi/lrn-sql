#ifndef STORAGE_LE_H
#define STORAGE_LE_H

#include <stdint.h>
#include <string.h>

/*
 * 디스크 형식의 little-endian 정수 읽기·쓰기. memcpy 한 번으로 읽고 써서 컴파일러가 load·store 하나로
 * 바꾼다(바이트 단위 루프는 최적화되지 않아 B+Tree 노드 검사가 느렸다). 정렬되지 않은 주소도 된다.
 */

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define LE_SWAP16(v) __builtin_bswap16(v)
#define LE_SWAP32(v) __builtin_bswap32(v)
#define LE_SWAP64(v) __builtin_bswap64(v)
#else
#define LE_SWAP16(v) (v)
#define LE_SWAP32(v) (v)
#define LE_SWAP64(v) (v)
#endif

static inline uint16_t le_get_u16(const uint8_t *p)
{
    uint16_t v;
    memcpy(&v, p, sizeof(v));
    return LE_SWAP16(v);
}

static inline uint32_t le_get_u32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return LE_SWAP32(v);
}

static inline uint64_t le_get_u64(const uint8_t *p)
{
    uint64_t v;
    memcpy(&v, p, sizeof(v));
    return LE_SWAP64(v);
}

static inline void le_put_u16(uint8_t *p, uint16_t v)
{
    v = LE_SWAP16(v);
    memcpy(p, &v, sizeof(v));
}

static inline void le_put_u32(uint8_t *p, uint32_t v)
{
    v = LE_SWAP32(v);
    memcpy(p, &v, sizeof(v));
}

static inline void le_put_u64(uint8_t *p, uint64_t v)
{
    v = LE_SWAP64(v);
    memcpy(p, &v, sizeof(v));
}

#endif
