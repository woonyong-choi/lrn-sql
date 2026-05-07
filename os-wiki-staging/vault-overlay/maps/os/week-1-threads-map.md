---
type: Map
status: Active
week:
  - threads
systems:
  - Linux
  - Windows
  - PintOS
  - QEMU
tags:
  - domain:os
  - domain:pintos
  - domain:qemu
  - week:threads
  - topic:scheduler
  - topic:interrupt
  - layer:kernel
  - layer:cpu
related_to:
  - "[[학습-가이드]]"
---

# 1주차 Threads 지도

## 이 지도의 목적

PintOS 1주차 Threads 학습에서 스케줄링, 인터럽트, 동기화, 컨텍스트 전환으로 이어지는 문서를 찾기 위한 링크 허브다.

## 먼저 볼 것

- [[cpu-register-execution]]
- [[interrupt-timer-qemu]]

## 핵심 개념

- [[cpu-register-execution]]
- [[interrupt-timer-qemu]]

## 흐름 추적

- [[context-switch-trace]]
- [[thread-scheduler-trace]]

## 실험

- [[pintos-intrusive-list-lab]]: `ready_list`가 담는 주소를 눈으로 확인

## 질문

- [[why-intr-yield-on-return]]: 왜 인터럽트 핸들러 안에서 바로 스케줄하지 않나?

## 앞으로 만들 문서

- `priority-donation-knowledge`
- `semaphore-lock-condition-knowledge`
