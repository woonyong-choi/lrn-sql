---
type: Map
status: Active
week:
  - vm
systems:
  - Linux
  - Windows
  - PintOS
  - QEMU
tags:
  - domain:os
  - domain:pintos
  - domain:qemu
  - week:vm
  - topic:page-table
  - topic:frame
  - topic:swap
  - topic:mmap
  - layer:memory
  - layer:kernel
related_to:
  - "[[학습-가이드]]"
---

# 3-4주차 Virtual Memory 지도

## 이 지도의 목적

PintOS 3-4주차 VM 학습에서 주소 변환, 페이지 테이블, 프레임, 페이지 폴트, swap, mmap 문서를 찾기 위한 링크 허브다.

## 먼저 볼 것

- [[address-translation-memory]]
- [[page-table-entry-bits-knowledge]]
- [[supplemental-page-table-knowledge]]
- [[바이트-버퍼와-캐스팅-실험|바이트 버퍼와 캐스팅 실험]]

## 핵심 개념

- [[address-translation-memory]]
- [[page-table-entry-bits-knowledge]]: PTE 한 칸이 frame 주소와 present/writable/user/accessed/dirty bit를 함께 담는 방식
- [[supplemental-page-table-knowledge]]: page table만으로는 기억할 수 없는 "원래 합법한 페이지" 정보
- [[mmap-file-backed-page-knowledge]]: 파일 바이트가 유저 가상 주소의 page로 보이는 과정

## 흐름 추적

- [[page-fault-trace]]: #PF가 OS 정책으로 이어지는 관문
- [[frame-eviction-trace]]: 빈 frame이 없을 때 victim page를 내보내고 frame을 재사용하는 흐름
- [[lazy-file-backed-page-trace]]: `mmap()` 직후에는 frame 없이 약속만 남기고 첫 접근 때 file page를 읽는 흐름

## 실험

- [[바이트-버퍼와-캐스팅-실험|바이트 버퍼와 캐스팅 실험]]
- [[swap-lab]]: anonymous page가 swap disk slot으로 나갔다 돌아오는 흐름
