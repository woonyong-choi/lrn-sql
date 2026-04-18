# Q16. I/O Bridge 완전 해부 — CPU·DRAM·NIC 의 물리·커널 경로

> CSAPP 6장·10장 + 리눅스 커널 코드 | "I/O bridge" 가 실제 하드웨어에서 어떻게 생겼고 커널이 어떻게 쓰는가 | 심화

## 질문

1. CSAPP 가 말하는 "I/O bridge" 는 현대 하드웨어에서 정확히 어디 있는가?
2. CPU·DRAM·주변장치가 얽히는 세 종류의 주소 공간은 어떻게 구분되는가?
3. PCIe 위에서 비트는 어떤 패킷으로 움직이는가?
4. 리눅스 커널은 DMA·MMIO·IRQ 를 어떤 API·자료구조로 다루는가?
5. `write()` 한 번이 실제로 PCIe TLP 까지 내려가는 경로를 추적하면?

## 답변

### §1. 하드웨어 변천 — Northbridge/Southbridge 에서 IMC+PCH 로

#### CSAPP 시절 (2000년대 초반)

```
[CPU] ── Front-Side Bus (FSB) ── [Northbridge]
                                    │
                            ┌───────┼────────────┐
                            │       │            │
                         [DRAM]  [AGP/GPU]   Hub Interface
                                                 │
                                           [Southbridge]
                                                 │
                                    ┌────┬──────┼──────┬─────┐
                                    │    │      │      │     │
                                  [PCI][USB][SATA][LAN][Audio]
```

- **Northbridge** — CPU 와 고속 장치(메모리·GPU) 를 연결. **메모리 컨트롤러(IMC) 가 여기 있었다.**
- **Southbridge** — 저속 I/O (USB, SATA, LAN, Audio, BIOS).
- 두 브리지는 Hub Interface 라는 내부 버스로 연결.
- CSAPP 가 한 줄로 그린 "I/O bridge" 는 이 **Northbridge + Southbridge** 묶음의 추상화다.

#### 현대 (Sandy Bridge 2011 이후)

```
[CPU 패키지]
  ├─ Core 0 ─┐
  ├─ Core 1 ─┤   LLC (L3 캐시) 공유
  ├─ Core 2 ─┤
  ├─ Core 3 ─┘
  │
  ├─ IMC (통합 메모리 컨트롤러) ─── DDR 채널 ─── [DRAM DIMMs]
  │
  ├─ PCIe Root Complex ─── PCIe 슬롯 ─── [GPU / NVMe / 고속 NIC]
  │
  └─ DMI Link ─── [PCH (Platform Controller Hub)]
                      ├─ USB
                      ├─ SATA
                      ├─ 저속 Ethernet
                      ├─ Audio
                      └─ BIOS/UEFI SPI
```

변화 포인트:

- Northbridge 기능 대부분이 **CPU 다이 안**으로 들어옴 (IMC, PCIe Root Complex).
- Southbridge 의 계승자 = **PCH**. CPU 와는 **DMI 링크** (PCIe x4 상당) 로 연결.
- 고속 장치 (GPU, NVMe, 100G NIC) 는 CPU 와 **직결**. 저속 I/O 는 PCH 경유.

> 그래서 "CPU <-> DRAM 은 직결" 이 맞다. IMC 가 CPU 다이 안에 있어서 중간 홉이 없다. 반대로 NIC 은 IMC 가 아니라 PCIe Root Complex 를 거친다. "I/O bridge" 의 핵심 후손이 바로 이 **PCIe Root Complex + PCH** 이다.

### §2. 세 종류의 주소 공간

CPU 가 `mov [0x...], rax` 를 때릴 때 주소가 어디로 가느냐는 주소 값에 따라 다르다.

| 주소 공간 | 목적지 | 캐싱 | 접근 명령 |
| --- | --- | --- | --- |
| **물리 DRAM 주소** (예 `0x10000000`) | IMC -> DDR -> DRAM | WB (캐시됨) | `mov` |
| **MMIO 주소** (예 `0xFEC00000`) | PCIe Root Complex -> 장치 레지스터 | **UC / WC (non-cacheable)** | `mov` + UC 매핑 |
| **Port I/O 주소** (예 `0x60`) | LPC/PCH 의 포트 버스 | 캐싱 없음 | `in` / `out` 명령어 |

#### 페이지 테이블이 구분하는 법

```c
// arch/x86/include/asm/pgtable_types.h (요지)
#define _PAGE_BIT_PWT     3   // Page Write-Through
#define _PAGE_BIT_PCD     4   // Page Cache Disable

// PAT/MTRR 테이블과 조합해서 메모리 타입을 결정
// WB (write-back) / WT / UC / UC- / WC / WP

// 일반 커널 메모리
#define PAGE_KERNEL         __pgprot(__PAGE_KERNEL)         // WB
// MMIO 매핑
#define PAGE_KERNEL_NOCACHE __pgprot(__PAGE_KERNEL_NOCACHE) // UC
// GPU 프레임버퍼 같이 쓰기 결합이 도움되는 영역
#define PAGE_KERNEL_WC      __pgprot(__PAGE_KERNEL_WC)      // WC
```

**MMIO 는 왜 non-cacheable 이어야 하나**
NIC 레지스터 값은 장치 상태에 따라 시시각각 바뀐다. CPU 캐시에 한 번 올려놓으면 "케시된 옛날 값" 을 계속 읽게 돼 장치 상태를 놓친다. 반대로 쓸 때도 캐시에만 머물면 장치에 명령이 전달되지 않는다. 그래서 UC (Uncacheable) 로 매핑해 매 접근이 실제 PCIe 트랜잭션으로 나가게 한다.

#### ioremap — MMIO 를 커널 주소에 붙이기

PCIe 카드는 **BAR (Base Address Register)** 에 자신의 MMIO 물리주소 범위를 선언한다. 커널이 이걸 읽어 커널 가상주소에 매핑한다.

```c
// drivers/net/ethernet/intel/e1000/e1000_main.c (요지)
static int e1000_probe(struct pci_dev *pdev, const struct pci_device_id *ent)
{
    struct e1000_adapter *adapter;
    void __iomem *hw_addr;

    pci_enable_device(pdev);
    pci_request_regions(pdev, "e1000");

    // BAR0 의 MMIO 영역을 커널 가상주소에 UC 매핑
    hw_addr = pci_iomap(pdev, 0, 0);
    //  = ioremap(pci_resource_start(pdev, 0), pci_resource_len(pdev, 0))

    adapter->hw.hw_addr = hw_addr;   // 이후 레지스터 접근에 사용
    // ...
}
```

`__iomem` 은 `sparse` 정적 분석기가 "이 포인터는 MMIO. 직접 역참조 금지, readl/writel 만 허용" 을 강제하려고 붙이는 표식이다.

레지스터 접근:

```c
// include/asm-generic/io.h (요지)
static inline u32 readl(const volatile void __iomem *addr)
{
    u32 val;
    asm volatile("movl %1, %0" : "=r"(val) : "m"(*(volatile u32 __force *)addr));
    return val;
}

static inline void writel(u32 val, volatile void __iomem *addr)
{
    asm volatile("movl %0, %1" : : "r"(val), "m"(*(volatile u32 __force *)addr));
}
```

실제로는 아키텍처마다 배리어가 더 붙어 있다 (x86 은 얇고 ARM 은 두꺼움).

### §3. PCIe 프로토콜 — TLP (Transaction Layer Packet)

PCIe 는 "패킷 교환 망" 이다. CPU <-> 장치의 모든 트래픽이 **TLP** 라는 패킷으로 오간다.

```
PCIe 스택
  ┌──────────────────────────────┐
  │ Transaction Layer (TLP)      │  "누가 누구에게 뭘"
  ├──────────────────────────────┤
  │ Data Link Layer (DLLP)       │  ACK/NAK, credit
  ├──────────────────────────────┤
  │ Physical Layer               │  Serdes, 레인, 인코딩(128b/130b)
  └──────────────────────────────┘
```

#### TLP 종류

| 종류 | 약자 | 용도 |
| --- | --- | --- |
| Memory Read | MRd | CPU 가 MMIO 읽음 / 장치가 DMA 읽음 |
| Memory Write | MWr | CPU 가 MMIO 씀 / 장치가 DMA 씀 |
| I/O Read/Write | IORd/IOWr | 레거시 (거의 안 씀) |
| Config Read/Write | CfgRd/CfgWr | BAR, 벤더 ID 등 PCIe config 공간 접근 |
| Message | Msg | 인터럽트(MSI/MSI-X), PME 등 |
| Completion | Cpl/CplD | 읽기 응답 |

#### TLP 헤더 (Memory Write 4DW 단순화)

```
 0        8        16       24       31
+--------+--------+--------+--------+
| Fmt/Tp | TC/Flg | Length(payload)  |  <- DW0
+--------+--------+--------+--------+
| Requester ID  | Tag | Byte Enables|  <- DW1
+--------+--------+--------+--------+
| Address High  (64bit)              |  <- DW2
+--------+--------+--------+--------+
| Address Low                        |  <- DW3
+--------+--------+--------+--------+
| Payload ... (N DW)                 |
+--------+--------+--------+--------+
```

- **Requester ID** = 요청 주체 `Bus:Device:Function`. IOMMU 가 이 ID 로 장치별 주소 변환 테이블을 찾는다.
- **Memory Write** 는 응답 없음 (posted). **Memory Read** 는 non-posted -> Completion TLP 로 데이터가 돌아온다.
- Tag 는 여러 개의 outstanding read 요청을 구분하는 태그.

#### BAR 로 MMIO 영역이 할당되는 과정

```
[1] BIOS/펌웨어 POST 시점
     └ 각 PCIe 장치에 Config Write 로 BAR0 에 `0xFFFFFFFF` 쓰기
     └ Config Read 로 다시 읽어 하위 비트를 보면 "내가 필요한 크기" 를 알 수 있음
     └ 시스템 MMIO 주소 공간에서 연속 영역 할당
     └ Config Write 로 BAR0 에 "너의 MMIO 시작주소 = 0xFEBE0000" 기록

[2] OS 부팅 후
     └ pci_resource_start(pdev, 0) 로 그 값 조회
     └ ioremap 으로 커널 가상주소에 UC 매핑
     └ writel/readl 로 접근
```

### §4. DMA 메커니즘 완전 분해

#### DMA 가 하는 일

```
[DMA 없이]
  CPU: for (i=0; i<149; i++) writel(skb[i], NIC_DATA_REG);
       -> 149번의 PCIe Memory Write TLP, 매번 CPU 가 직접

[DMA 로]
  CPU: writel(phys(skb), NIC_TX_DESC_ADDR);
       writel(149, NIC_TX_LEN);
       wmb();
       writel(GO, NIC_DOORBELL);
  NIC: Memory Read TLP 로 DRAM(phys(skb)) 에서 149B 끌어옴
       -> CPU 는 그 사이 다른 일
```

CPU 는 "어디·얼마·시작" 만 알려주고, 데이터 본체 이동은 NIC 의 DMA 엔진이 PCIe Root Complex <-> IMC <-> DRAM 으로 흘린다.

#### 커널 DMA API

```c
// include/linux/dma-mapping.h

// 1) Coherent DMA — 영속 구조 (descriptor ring 같은 것)
void *cpu_addr = dma_alloc_coherent(
    &pdev->dev,        // 장치
    RING_SIZE,         // 크기
    &dma_addr,         // OUT: 장치가 볼 bus/DMA address
    GFP_KERNEL);
// cpu_addr : CPU 가 쓸 커널 가상주소 (UC 매핑, 대체로 WC)
// dma_addr : NIC 에게 줄 주소 (IOMMU 있으면 IOVA, 없으면 phys)

// 2) Streaming DMA — 일회성 (skb 데이터)
dma_addr_t d = dma_map_single(
    &pdev->dev,
    skb->data,
    skb->len,
    DMA_TO_DEVICE);
// ... NIC 가 다 읽은 뒤 ...
dma_unmap_single(&pdev->dev, d, skb->len, DMA_TO_DEVICE);
```

**두 종류인 이유**

- **Coherent** — 항상 CPU <-> 장치가 같은 값을 본다. 캐싱 안 됨 -> 느리지만 양쪽이 자주 오가는 구조(descriptor ring) 에 적합.
- **Streaming** — 캐싱됨 (WB). 대신 map/unmap 호출로 **캐시 flush/invalidate** 를 명시해야 함. 큰 페이로드(skb 본체) 에 적합.

#### dma_map_single 내부

```c
// kernel/dma/mapping.c (요지)
dma_addr_t dma_map_page_attrs(struct device *dev, struct page *page,
                              size_t offset, size_t size,
                              enum dma_data_direction dir, unsigned long attrs)
{
    const struct dma_map_ops *ops = get_dma_ops(dev);
    dma_addr_t addr;

    if (dma_map_direct(dev, ops)) {
        // IOMMU 없음: 물리주소가 곧 DMA 주소
        addr = phys_to_dma(dev, page_to_phys(page) + offset);
    } else {
        // IOMMU: 가상 DMA 주소 할당 + 변환 테이블 등록
        addr = ops->map_page(dev, page, offset, size, dir, attrs);
    }

    // 방향별 캐시 처리 (x86 은 대체로 no-op; ARM/MIPS 는 실제 flush)
    arch_sync_dma_for_device(page_to_phys(page) + offset, size, dir);
    return addr;
}
```

#### x86 의 Cache Coherent DMA

x86 은 **DMA 가 캐시 시스템을 snoop** 한다. NIC 이 Memory Read TLP 로 들어올 때 해당 캐시 라인이 L1/L2/L3 에 dirty 로 있으면 **캐시에서** 데이터가 공급된다. 그래서 명시적 flush 가 거의 필요 없다. ARM/MIPS 계열은 snoop 이 약해 소프트웨어로 flush 해야 한다. Linux `arch_sync_dma_for_device` 가 이 차이를 흡수.

#### IOMMU — 장치별 가상 주소 공간

```
호스트 물리 DRAM                          장치가 보는 IOVA
  ┌────────────┐                           ┌────────────┐
  │ 0x100000   │ <────┐         IOMMU      │ 0x1000     │
  │ 0x101000   │      │     (page table    │ 0x2000     │
  │ 0x1FFFFF   │      │      + IOTLB)      │ 0x3000     │
  └────────────┘      │                    └────────────┘
                      └── Requester ID (BDF) 로 컨텍스트 테이블 선택
```

- VM (VT-d) 에서 게스트가 NIC 에 직접 붙는 구조의 기반.
- 잘못된 장치가 엉뚱한 물리 메모리에 DMA 하는 것을 막는 **보안**.
- 커널 API: `iommu_map()`, `iommu_unmap()`; `iommu_domain_alloc`.

### §5. MMIO 와 메모리 배리어

x86 CPU 는 store buffer 가 있어서 연속된 write 가 순서대로 버스로 나간다는 보장이 약하다. NIC 드라이버는 **배리어** 를 명시해야 한다.

```c
// 잘못된 예
writel(buf_phys, TX_DESC_ADDR);
writel(GO,       DOORBELL);
// -> store buffer 가 순서를 뒤집으면 NIC 이 아직 안 쓴 descriptor 를 DMA 함

// 올바른 예
writel(buf_phys, TX_DESC_ADDR);
wmb();   // write memory barrier: 이전 store 완료 후 이후 store
writel(GO, DOORBELL);
```

리눅스 배리어 종류:

| 함수 | 의미 |
| --- | --- |
| `mb()` | full memory barrier (load+store) |
| `wmb()` | write 배리어 |
| `rmb()` | read 배리어 |
| `smp_*` | SMP 간에만 (단일 CPU 에선 컴파일러 배리어) |
| `dma_wmb()` / `dma_rmb()` | DMA 전용. ARM 에서 `wmb` 보다 가벼움 |

### §6. 인터럽트 — Legacy INTx -> MSI -> MSI-X

#### Legacy INTx

```
NIC ─ 물리 와이어 INTA# ─ PCH ─ IOAPIC ─ CPU(LAPIC)
```

A/B/C/D 네 가닥 공유 와이어. 한 벡터에 여러 장치가 공유될 수 있어 ISR 마다 "누가 쏜 건지" 체크 필요.

#### MSI (Message Signaled Interrupt)

**인터럽트 = PCIe Memory Write TLP** 로 재정의. 장치가 미리 설정된 주소(`0xFEExxxxx`) 에 미리 설정된 데이터(벡터 번호) 를 쓰면 LAPIC 이 받아 CPU 를 깨운다. 와이어 공유 문제 해결, 벡터 수 확장.

#### MSI-X

장치당 최대 **2048 개** 벡터. 멀티큐 NIC 은 "RX 큐마다 MSI-X 벡터 1개" 를 할당해 큐별로 다른 CPU core 에 인터럽트 분산 (RSS, Receive Side Scaling).

#### 커널 코드: MSI-X 설정 + IRQ 핸들러 + NAPI

```c
// drivers/net/ethernet/intel/e1000e/netdev.c (요지)
static int e1000_request_msix(struct e1000_adapter *adapter)
{
    int err, vector = 0;
    struct net_device *netdev = adapter->netdev;

    err = pci_alloc_irq_vectors(adapter->pdev, nvec, nvec, PCI_IRQ_MSIX);
    if (err < 0) return err;

    // RX 큐별 인터럽트
    for (i = 0; i < adapter->num_rx_queues; i++) {
        int irq = pci_irq_vector(adapter->pdev, vector++);
        err = request_irq(irq, e1000_intr_msix_rx, 0,
                          adapter->rx_ring[i]->name,
                          adapter->rx_ring[i]);
    }
    // TX 큐별 인터럽트 (생략)
    return 0;
}

// RX 인터럽트 핸들러
static irqreturn_t e1000_intr_msix_rx(int irq, void *data)
{
    struct e1000_ring *rx_ring = data;
    struct e1000_adapter *adapter = rx_ring->adapter;

    if (napi_schedule_prep(&adapter->napi)) {
        __napi_schedule(&adapter->napi);  // NAPI poll 예약
    }
    return IRQ_HANDLED;
}

// NAPI poll (softirq context 에서 호출됨)
static int e1000_clean(struct napi_struct *napi, int budget)
{
    int work_done = 0;
    // descriptor ring 에서 완료된 것을 최대 budget 개 처리
    e1000_clean_rx_irq(adapter, &work_done, budget);
    e1000_clean_tx_irq(adapter);

    if (work_done < budget) {
        napi_complete_done(napi, work_done);
        // 다시 인터럽트 활성화
        ew32(IMS, IMS_ENABLE_MASK);
    }
    return work_done;
}
```

**인터럽트 핸들러가 바로 패킷을 처리하지 않고 NAPI 로 넘기는 이유**
초당 100만 패킷이 들어오면 인터럽트 100만 번이 CPU 를 죽인다. NAPI 는 "인터럽트 한 번 받으면 폴링 모드로 바꿔서 큐가 빌 때까지 계속 처리" -> 하드 IRQ 빈도를 급감시킨다.

### §7. MMIO 접근의 실제 메커니즘

`writel(value, addr)` 한 줄이 버스까지 내려가는 과정:

```
[1] CPU 가 mov 명령 실행
     └ 가상주소 addr -> 페이지테이블 조회 -> 물리주소
     └ MTRR/PAT 로 메모리 타입 UC 판정

[2] 캐시 우회
     └ UC 영역이라 L1/L2/L3 거치지 않음
     └ store buffer 에 들어가도 coalescing 되지 않음 (strong uncache 의 경우)

[3] 메모리 컨트롤러 vs PCIe RC 선택
     └ 주소 디코더: 이 물리주소 범위는 "MMIO 대역" -> PCIe Root Complex 로

[4] PCIe Root Complex
     └ Memory Write TLP 생성
     └ 해당 BDF 의 장치를 향해 DMI/PCIe 로 송신

[5] NIC
     └ TLP 수신 -> BAR 안의 레지스터 offset 해석 -> 내부 레지스터 업데이트

[6] 완료 (Memory Write 는 posted -> CPU 대기 없음)
```

`readl` 은 non-posted 라서 CPU 가 Completion TLP 가 돌아올 때까지 **실제로 기다린다**. 그래서 MMIO read 는 대체로 수백 ns, DRAM read (~80ns) 보다 느리다.

### §8. 한 프레임 송신 — PCIe TLP 까지 추적

`write(sockfd, buf, 149)` 가 실제 선로에 전기신호로 나가기까지.

```
[1] user:
      write(sockfd, buf, 149)
      └ syscall 트랩 -> 커널 CPL0

[2] 커널 TCP:
      sock_write_iter -> tcp_sendmsg
      └ sk_stream_alloc_skb
          └ kmem_cache_alloc(skbuff_head_cache)     // 224B 메타
          └ page_frag_alloc(149B)                    // 데이터 영역
      └ copy_from_user(skb->data, buf, 149)         // CPU 가 복사

[3] TCP/IP 처리:
      tcp_transmit_skb -> ip_queue_xmit -> dev_queue_xmit
      └ qdisc 에 enqueue -> __qdisc_run

[4] 드라이버 ndo_start_xmit (e1000_xmit_frame 예):
      dma_addr = dma_map_single(dev, skb->data, 149, DMA_TO_DEVICE);
      // -> IOMMU 있으면 IOVA 할당, 없으면 phys

      desc[tail].buffer_addr = dma_addr;
      desc[tail].length      = 149;
      desc[tail].cmd         = EOP | RS;
      tail++;
      wmb();                          // store 순서 보장
      writel(tail, hw_addr + TDT);    // Tail Descriptor register = doorbell

[5] PCIe: Memory Write TLP (CPU -> NIC)
      Fmt/Type = MWr
      Address  = BAR_0 + TDT offset   (예 0xFEBF8388)
      Payload  = tail 값
      -> PCIe Root Complex -> NIC

[6] NIC 펌웨어:
      doorbell 감지 -> TX DMA 엔진 시작
      Memory Read TLP 발행:
        Address = dma_addr  (예 0x00000001A3F40000)
        Length  = 149B
      -> Root Complex -> IMC -> DRAM
      -> Completion TLP (CplD) 로 149B 데이터 수신
      -> NIC 내부 FIFO SRAM 에 저장

[7] NIC MAC 컨트롤러:
      프리앰블 7B + SFD 1B 앞에 붙임
      프레임 끝에 CRC-32 (FCS) 4B 계산해서 추가
      PHY 에 비트 스트림 전달

[8] PHY -> Serdes -> RJ-45 -> 케이블 (전기/광 신호)

[9] TX 완료 후:
      NIC 이 Memory Write TLP 로 descriptor status 를 DRAM 에 writeback
      MSI-X Message TLP 로 인터럽트 전달
      LAPIC -> CPU -> e1000_intr_msix_tx -> napi_schedule
      NAPI poll 에서 dma_unmap_single + dev_kfree_skb
```

여기서 눈여겨볼 흐름 세 개:

1. **CPU 는 데이터 본체를 만지지 않는다.** 유저->커널 복사 한 번만 CPU. 이후는 DMA.
2. **doorbell 도 MMIO write TLP.** "명령 한 번 보냄" 이 곧 PCIe 패킷.
3. **인터럽트도 MMIO write TLP.** 장치가 LAPIC 의 특정 주소에 값을 씀.

### §9. 성능·경합 관점

```
대역폭 비교 (이론치)

  DDR4-3200 (듀얼채널)      약 50 GB/s
  PCIe 4.0 x16             약 32 GB/s
  PCIe 4.0 x4  (NVMe 하나)  약  8 GB/s
  DMI 4.0                  약  8 GB/s  (PCH 경유 총량)
  10Gbps NIC (단일 포트)   약 1.25 GB/s
  100Gbps NIC             약  12 GB/s
```

운영상 핵심 관찰:

- **NUMA** — 듀얼 소켓 서버에서 NIC 이 한 CPU 의 PCIe 에 붙어 있으면 반대쪽 CPU 에서 접근 시 QPI/UPI 홉 비용. `irqbalance`, `taskset`, `set_mempolicy` 로 친화성 조정.
- **Cache line ping-pong** — descriptor 를 여러 CPU 가 쓰면 캐시 라인이 코어 사이를 왔다갔다. 멀티큐 NIC 는 큐별로 core 고정 (RSS + IRQ affinity).
- **Zerocopy** — `sendfile(2)`, `splice(2)`, `MSG_ZEROCOPY`, `io_uring` 로 유저<->커널 복사를 없애면 10G+ 에서 체감이 큼.
- **Interrupt coalescing** — NIC 펌웨어가 "N 개 또는 K μs 마다 한 번씩" IRQ 를 묶어서 쏨. `ethtool -c eth0` 로 조회·조정.

### §10. 커널 계층으로 되짚기

```
하드웨어
  [NIC PCIe 카드]
       │ (TLP)
  [PCIe Root Complex]
       │
  [CPU 다이: IMC / core / cache]
       │
커널 드라이버
  drivers/net/ethernet/...            <- ndo_start_xmit, IRQ 핸들러
       │
  kernel/dma/, include/linux/dma-mapping.h   <- DMA API
       │
  arch/x86/kernel/pci-*                 <- PCI/PCIe 설정
       │
네트워크 스택
  net/core, net/ipv4                    <- skb, qdisc, TCP/IP
       │
시스템콜
  net/socket.c                          <- sock_write_iter
       │
유저공간
  write(sockfd, buf, len)
```

이 스택 전체가 하나의 `write()` 호출에 엮여 동작한다. CSAPP 6·10·11 장의 분절된 내용이 여기서 한 줄로 이어진다.

## 연결 키워드

- [02-keyword-tree.md — 6장 저장 계층 / 10장 시스템 I/O](../../csapp-10/)
- [q08-host-network-pipeline.md](./q08-host-network-pipeline.md) — 송신 파이프라인 상위 시점
- [q05-socket-principle.md](./q05-socket-principle.md) — 소켓 관점의 I/O bridge 언급
- [q09-network-cpu-kernel-handle.md](./q09-network-cpu-kernel-handle.md) — 네 개의 렌즈
- [q04-filesystem.md](./q04-filesystem.md) — 블록 레이어 / 페이지 캐시 쪽의 사촌 주제
