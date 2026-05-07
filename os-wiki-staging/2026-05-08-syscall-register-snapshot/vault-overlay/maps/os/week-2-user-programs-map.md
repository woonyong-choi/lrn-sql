---
type: Map
status: Active
week:
  - user-programs
systems:
  - Linux
  - Windows
  - PintOS
  - QEMU
tags:
  - domain:os
  - domain:pintos
  - domain:qemu
  - week:user-programs
  - topic:syscall
  - topic:process
  - topic:elf
  - topic:fd
  - layer:user
  - layer:kernel
related_to:
  - "[[학습-가이드]]"
---

# 2주차 User Programs 지도

## 이 지도의 목적

PintOS 2주차 User Programs 학습에서 유저 모드, ELF 로딩, 시스템 콜, 파일 디스크립터, 프로세스 생명주기 문서를 찾기 위한 링크 허브다.

## 먼저 볼 것

- [[elf-loader-knowledge]]
- [[argument-passing-lab]]
- [[syscall-end-to-end]]
- [[syscall-register-snapshot-trace]]
- [[file-descriptor-knowledge]]
- [[cpu-register-execution]]

## 핵심 개념

- [[elf-loader-knowledge]]: 실행 파일의 program header가 유저 가상 page mapping으로 바뀌는 지점
- [[cpu-register-execution]]
- [[syscall-register-snapshot-trace]]: `RAX/RDI/RSI/RDX`가 `struct intr_frame`의 필드로 고정되는 지점
- [[file-descriptor-knowledge]]: fd 숫자가 “무엇을 가리키는지”부터 잡기
- [[fd-table-fork-duplication-knowledge]]: `fork()`가 fd table과 열린 파일 위치를 어떻게 물려주는지 확인

## 흐름 추적

- [[syscall-end-to-end]]
- [[syscall-register-snapshot-trace]]
- [[user-pointer-validation-trace]]
- [[process-wait-exit-trace]]: `exit(status)`가 부모의 `wait(pid)` 반환값이 되는 흐름

## 실험

- [[argument-passing-lab]]: `argc/argv`가 초기 유저 스택의 바이트와 포인터로 놓이는 방식
- [[fd-close-reuse-lab]]: `close(fd)` 뒤 같은 번호가 재사용되는 순간과 `remove(name)`이 열린 fd와 분리되는 이유
- [[바이트-버퍼와-캐스팅-실험|바이트 버퍼와 캐스팅 실험]]
