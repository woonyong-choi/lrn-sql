# 예시 문서

# Q11. 페이지 테이블·PTE·주소 번역 전체 파이프라인

> CSAPP 9.6 | 4부. 주소 변환 | 기본

## 질문

1. CPU가 VA를 발행한 순간부터 데이터가 돌아오기까지의 전체 파이프라인을 서술하시오.
2. 이 파이프라인에서 TLB, 페이지 테이블, 캐시가 각각 어느 단계에 개입하는지 설명하시오.
3. [검증] TLB miss + Page hit + Cache miss 시나리오의 전체 흐름을 그리시오.

## 답변

### 최우녕

> CPU가 VA를 발행한 순간부터 데이터가 돌아오기까지의 전체 파이프라인을 서술하시오.

CPU가 mov 같은 메모리 명령을 실행하면 VA가 MMU로 전달된다.
MMU는 VA를 VPN + VPO로 분해하고, VPN으로 TLB를 조회한다.
TLB HIT이면 PPN을 바로 얻고, MISS면 페이지 테이블 워크를 한다.
PPN을 얻으면 PA = (PPN << p) | VPO로 조합한다.
PA로 L1 → L2 → L3 캐시를 탐색하고, 캐시 HIT이면 데이터를 반환,
MISS면 DRAM에 접근해서 캐시에 올리고 반환한다.

만약 페이지 테이블 워크 중 PTE가 invalid(DRAM에 없음)이면
Page Fault가 발생하고 커널이 디스크에서 로드한 뒤 명령어를 재실행한다.

> 이 파이프라인에서 TLB, 페이지 테이블, 캐시가 각각 어느 단계에 개입하는지 설명하시오.

TLB: VA → PA 변환의 첫 번째 단계. VPN으로 PPN을 빠르게 찾는 캐시다.
HIT이면 페이지 테이블을 안 봐도 된다.

페이지 테이블: TLB MISS일 때 개입한다. CR3부터 4단계를 순회해서 PTE를 찾고
PPN을 얻는다. 이때 메모리 접근이 최대 4번 발생한다(각 단계 테이블이 DRAM에 있으므로).

캐시: PA가 결정된 이후 개입한다. 실제 데이터를 가져오는 단계이며,
L1 → L2 → L3 순서로 조회한다.

정리하면: TLB(주소 변환 캐시) → 페이지 테이블(주소 변환 본체) → 캐시(데이터 캐시)
주소 변환과 데이터 접근은 별개의 단계다.

> [검증] TLB miss + Page hit + Cache miss 시나리오의 전체 흐름을 그리시오.

```text
전제: TLB MISS, 페이지 테이블에서 PPN 찾음(DRAM에 있음), 캐시 MISS

━━━ 1. VA 발행 ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

  CPU: mov 명령 → VA 발행 → MMU 전달
  |
  VA → VPN + VPO 분해

━━━ 2. TLB 조회 (MISS) ━━━━━━━━━━━━━━━━━━━━━━━━━

  MMU: VPN으로 TLB 검색
  |
  ㄴ MISS → 페이지 테이블 워크 필요

━━━ 3. 페이지 테이블 워크 (Page HIT) ━━━━━━━━━━━━

  CR3 → PML4 → PDPT → PD → PT
  |
  ㄴ 각 단계마다 DRAM 접근 (최대 4번 메모리 읽기)
  ㄴ PT에서 PTE 획득
  ㄴ Valid = 1, PPN 있음 → Page Fault 없음
  |
  PPN 획득 → TLB에 캐싱 (다음엔 HIT)
  PA = (PPN << 12) | VPO

━━━ 4. 캐시 조회 (MISS) ━━━━━━━━━━━━━━━━━━━━━━━━

  PA로 L1 캐시 탐색
  |
  ㄴ L1 MISS → L2 탐색
     ㄴ L2 MISS → L3 탐색
        ㄴ L3 MISS → DRAM 접근
           |
           ㄴ DRAM에서 캐시라인(64B) 로드
           ㄴ L3 → L2 → L1에 채움
           ㄴ 데이터를 CPU 레지스터에 전달

━━━ 비용 합산 ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

  TLB MISS       : ~0 (워크로 대체)
  테이블 워크     : ~4 × DRAM 접근 = ~800 사이클
  캐시 MISS       : ~200 사이클 (DRAM 1회)
  총합            : ~1000 사이클

  비교:
  ㄴ TLB HIT + Cache HIT     : ~5 사이클
  ㄴ TLB MISS + Page Fault   : ~수백만 사이클 (디스크)
```

## 연결 키워드

- [Translation Flow](../csapp-ch9-virtual-memory/csapp-ch9-keyword-tree.md#4-변환-메커니즘--va--pa가-실제로-일어나는-과정)
- [전체 흐름](../csapp-ch9-virtual-memory/csapp-ch9-keyword-tree.md#4-변환-메커니즘--va--pa가-실제로-일어나는-과정)
