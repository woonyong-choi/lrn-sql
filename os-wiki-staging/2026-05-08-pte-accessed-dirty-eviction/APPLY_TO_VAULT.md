---
type: AI Reference
status: Draft
tags:
  - domain:os
  - domain:tools
---

# PTE accessed/dirty eviction Lab overlay 적용 메모

이 overlay는 실제 vault 쓰기가 막힌 세션에서 만든 적용 후보입니다.

## 포함 파일

- `vault-overlay/labs/os/pte-accessed-dirty-eviction-lab.md`
- `vault-overlay/maps/os/concept-to-code-map.md`
- `vault-overlay/maps/os/week-3-4-virtual-memory-map.md`
- `vault-overlay/traces/os/frame-eviction-trace.md`

## 적용 전 확인

실제 vault 기준 HEAD는 작성 시점에 다음이었습니다.

```text
3fd7b09 docs: CPU 레지스터와 syscall snapshot을 설명
```

작성 후 확인 시 실제 vault HEAD가 다음까지 진행되어 있었습니다.

```text
6f9e250 docs: 우선순위 기부 chain 실험을 추가
```

따라서 이 overlay의 `maps/os/*.md`와 `traces/os/frame-eviction-trace.md`는 그대로 복사하면 최신 vault 변경을 덮어쓸 수 있습니다. 새 Lab 파일은 그대로 추가하되, 기존 문서 3개는 `[[pte-accessed-dirty-eviction-lab]]` 링크가 들어간 부분만 수동 병합하세요.

적용 전에는 반드시 다음을 확인하세요.

```bash
git -C /Users/woonyong/vault status --short
```

dirty 변경이 있으면 `maps/os/concept-to-code-map.md`, `maps/os/week-3-4-virtual-memory-map.md`, `traces/os/frame-eviction-trace.md`를 덮어쓰지 말고 수동 병합하세요.

## 적용 의도

새 Lab은 PTE `accessed`/`dirty` bit를 frame eviction 판단으로 연결합니다.

질문 흐름:

- 왜 eviction은 PTE bit를 보나?
- read와 write 뒤 PTE flag 값은 어떻게 달라지나?
- PintOS helper는 어떤 bit를 읽고 지우나?
- QEMU는 이 bit를 OS 정책으로 이해하나, 하드웨어 효과로만 에뮬레이션하나?
- GDB에서 `0x20`, `0x40` bit를 어떻게 확인하나?

## 커밋 메시지 후보

```text
docs: PTE accessed dirty eviction 실험을 추가
```
