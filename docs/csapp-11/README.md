# CSAPP 11 학습 패키지

이번 주 네트워크 학습과 수요 코딩회 구현을 한 흐름으로 묶은 문서 묶음입니다.

## 목표

- `CSAPP` 11장 네트워크 내용을 끝까지 읽고, 핵심 개념을 설명할 수 있다.
- Echo client/server -> Tiny 웹서버 -> Proxy 순서로 구현 흐름을 이해한다.
- 월요일부터는 학습 내용을 질문지와 답변으로 정리하고, 동시에 SQL API 서버 구현을 시작한다.
- 수요일 코딩회에서는 네트워크 지식을 활용해 외부 클라이언트가 사용할 수 있는 C 기반 SQL API 서버를 만든다.

## 문서 목록

- `00-roadmap-overview.md`: 세로 키워드 트리 기반 한 장 로드맵, 학습 체크, 화이트보드 설명용
- `01-week-plan.md`: 2026-04-17 ~ 2026-04-22 실행 계획
- `02-keyword-tree.md`: 모든 개념을 하나의 종속 관계 트리로 정리한 통합 키워드 트리
- `03-completion-rubric.md`: 어디까지 이해하면 "학습 완료"인지 판단하는 기준
- `04-sql-api-implementation-bridge.md`: CSAPP 11 내용을 SQL API 서버 구현으로 연결하는 가이드
- `05-ch11-sequential-numeric-walkthrough.md`: 11장을 초보자 관점에서 실제 숫자와 흐름으로 끝까지 설명하는 기준 문서
- `06-resources.md`: 학습 자료, 참고 링크, 숙제 문제 가이드, 디버깅 도구 정리

## 이번 주 핵심 흐름

```text
소켓과 네트워크 기초
-> Echo client/server
-> HTTP와 Web server
-> Tiny 코드 이해
-> Proxy Lab 요구사항 이해
-> 동시성과 캐시 설계
-> SQL API 서버 구현
```

## 가장 중요한 완료 기준

- `socket -> bind -> listen -> accept`와 `socket -> connect` 흐름을 설명할 수 있다.
- HTTP 요청 한 줄과 주요 헤더를 직접 읽고 다시 만들 수 있다.
- Tiny의 `doit`, `parse_uri`, `serve_static`, `serve_dynamic` 흐름을 코드 레벨로 설명할 수 있다.
- Proxy Lab의 순차 처리, 동시 처리, 캐시 요구사항을 설명할 수 있다.
- SQL API 서버에서 "리스닝 소켓, 요청 파싱, 스레드 풀, DB 엔진 호출, 응답 반환" 구조를 설계할 수 있다.
