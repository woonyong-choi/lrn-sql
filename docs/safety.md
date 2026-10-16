# 안전성 시험 결과

ToteDB는 commit 도중 프로세스가 멈추거나 쓰기가 실패해도 다시 열었을 때 그 거래 전체가 반영됐거나 전혀 반영되지 않은 상태만 남긴다. 성공을 돌려준 commit은 프로세스가 끝난 뒤에도 남는다. 이 문서는 그 주장을 확인한 시험과 결과, 재현 명령, 확인하지 않은 범위를 적는다.

## 요약

| 확인한 것 | 결과 | 시험 | 재현 |
|---|---|---|---|
| commit 중 모든 쓰기 지점에서 프로세스 종료 | 33곳·57곳(큰 값) 모두 부분 반영 0, 승인한 commit 유실 0 | `tests/test_database_txn.c` `test_crash_points` | `make test-transaction` |
| commit 중 모든 쓰기·fsync 지점의 I/O 실패 | 32곳·56곳(큰 값) 모두 어긋남 0, 결과를 모르는 2곳은 핸들 poison | 같은 시험 | `make test-transaction` |
| 문장마다 할당·I/O 실패 주입 | 7110개 검사 통과 | `tests/test_database_fault.c` | `make test-database` |
| 다시 연 파일의 페이지 회계 | 고아·이중 소유 0 | `database_check_pages`를 쓰는 위 시험들 | `make test-database test-transaction` |
| 쓰는 도중 SIGKILL된 여러 프로세스 | 남은 프로세스가 계속 쓰고, 행 수와 카운터가 같으며, 회계 0 | `tests/test_database_txn.c` `test_processes_killed_while_writing`, `tests/test_multiprocess.py` `test_killed_writer_leaves_no_half_applied_transaction` | `make test-transaction test-ack-queue-python` |
| SIGKILL된 reader | 게시가 다시 진행 | `tests/test_page_file.c` `test_locking_and_unpublished_files` | `make test` |
| 같은 큐를 쓰는 4·16개 프로세스 | 160·640건 유실·중복 0 | `tests/test_multiprocess.py` `test_workers_share_one_queue_without_loss_or_duplicates` | `make test-ack-queue-python` |
| 큐 연산의 I/O·할당 실패 | 1352개 검사 통과 | `tests/test_ack_queue_fault.c` | `make test-ack-queue` |
| claim한 worker의 SIGKILL | lease가 끝난 뒤 다른 worker가 처리, 출고 중복 0 | `tests/test_outbox_example.py` | `make example-outbox` |

## 무엇을 어떻게 시험했나

### 종료 지점

`test_crash_points`는 쓰기 거래 하나(여러 테이블의 INSERT·UPDATE·DELETE)를 실행하며 파일에 쓰거나 `fsync`하는 지점을 모두 센다. 그 뒤 지점 k마다 fork한 자식이 같은 거래를 실행하다 k번째 지점에서 곧바로 끝난다. 부모는 파일을 다시 열어 상태가 거래 전(옛 상태)이거나 거래 뒤(새 상태)인지 확인한다. 자식이 commit 성공을 받았으면 새 상태여야 한다. 큰 값(15000바이트 overflow 체인)을 넣는 판으로 한 번 더 실행한다.

| 판 | 지점 | 종료 | 승인 | 부분 반영·승인 유실 |
|---|---|---|---|---|
| 기본 | 33 | 32 | 1 | 0 |
| 큰 값 | 57 | 56 | 1 | 0 |

### I/O 실패

같은 지점마다 쓰기나 `fsync`가 실패하게 만든다. commit이 성공을 돌려주면 새 상태, 확정 실패면 옛 상태, 결과를 알 수 없으면(제어 페이지를 쓴 뒤 실패) 둘 중 하나이고 그 핸들은 더 쓰지 못한다(poison).

| 판 | 지점 | 주입 | 불확정 | 어긋남 |
|---|---|---|---|---|
| 기본 | 32 | 32 | 2 | 0 |
| 큰 값 | 56 | 56 | 2 | 0 |

`tests/test_database_fault.c`는 문장 종류마다 할당 실패와 I/O 실패를 차례로 넣고, 실패한 문장이 아무것도 바꾸지 않았는지와 다시 연 파일이 정상인지 7110개 검사로 확인한다.

### 페이지 회계

`database_check_pages`는 카탈로그에서 도달하는 페이지와 빈 페이지 목록을 대조해 고아(어디서도 가리키지 않는 페이지)와 이중 소유(두 곳이 가리키거나 빈 목록과 겹치는 페이지)를 센다. 종료·I/O 실패 시험과 여러 프로세스 시험은 다시 열 때마다 이 값이 0인지 확인한다.

### 여러 프로세스

- `test_processes_killed_while_writing`: fork한 writer 셋이 큰 값 갱신·행 추가·카운터 증가를 한 거래로 계속 확정한다. 부모가 둘을 쓰는 도중 SIGKILL하고, 그 사이 남은 writer의 카운터가 계속 오르는지, 끝난 뒤 행 수와 카운터가 같은지, 페이지 회계가 0인지 확인한다.
- `test_workers_share_one_queue_without_loss_or_duplicates`: spawn한 worker 4·16개가 claim, 처리 기록, ack를 한 거래로 처리한다. 처리 기록의 기본 키가 작업 id라 같은 작업을 두 번 처리하면 commit이 실패한다.
- `test_reads_do_not_wait_for_another_process_writing`: 다른 프로세스가 4MB를 쓴 거래를 쥔 동안 조회 50번이 모두 0.5초 안에 commit 전 상태를 돌려준다.
- `test_writer_keeps_committing_while_readers_read_continuously`: reader 넷이 쉬지 않고 읽는 동안에도 commit 40번이 혼자일 때의 6배 + 1초 안에 끝난다.
- `test_forked_child_cannot_use_the_parent_handle`: fork한 자식이 부모 핸들로 쓰면 `ForkedHandleError`이고 파일 바이트가 그대로다.

### 결함 주입으로 시험 자체를 확인

새 시험은 해당 결함을 일부러 넣었을 때 실패하는지 확인했다.

| 넣은 결함 | 실패한 시험 |
|---|---|
| 행 체인 해제 생략 | 페이지 회계 고아 4 |
| rollback의 새 페이지 반환 생략 | 페이지 회계 고아 6 |
| 놓은 게시 페이지 곧바로 재사용 | 열린 거래 동안 다른 세션 읽기 실패 |
| 다른 프로세스의 게시 뒤 재적재 생략 | `tests/test_multiprocess.py` 4개 |
| 다른 프로세스의 게시 뒤 빈 목록 재적재 생략 | `test_database_txn` 경계 밖 접근(ASan) |
| fork 자식의 fd 사본 정리 생략 | `test_page_file` 중단 |
| writer 우선 관문 생략 | `test_writer_keeps_committing_while_readers_read_continuously`(31초) |
| SELECT의 writer 잠금 사용 | `test_reads_do_not_wait_for_another_process_writing` |
| 큐 연장의 fence 검사 제거 | `tests/test_ack_queue.c` |
| 예제 worker의 ack 생략 | `tests/test_outbox_example.py` |

## SQLite와의 강제 종료 비교

`bench/sqlite_workload.py`로 SQLite(DELETE journal, `synchronous=FULL`)와 같은 입력을 비교했다(소스 `f04f93e`, 2026-10-16 측정). 1천·1만·10만 행마다 15번, 행 절반을 바꾸는 UPDATE를 보낸 뒤 0~20ms 안에 SIGKILL하고 다시 열어 UPDATE 전부 또는 전무인지 확인했다. 두 엔진 모두 위반 0이었다. 다시 열어 기본 키 조회 하나에 답할 때까지 p50은 ToteDB 2.2~2.5ms, SQLite 2.5~3.2ms였다.

```sh
python3 bench/sqlite_workload.py
python3 bench/sqlite_workload.py --analyze
```

## 검증하지 않은 것

- 전원 차단. 시험은 프로세스 종료와 I/O 오류만 다룬다. 저장 장치가 `fsync` 성공을 돌려준 뒤 쓰기를 버리면(lost write) 이전 세대가 정상으로 열리며 탐지하지 못한다.
- 네트워크 파일 시스템. 잠금은 로컬 파일 시스템의 `flock`을 전제로 한다.
- 여러 writer의 동시 commit. 쓰기 거래는 파일 전체에서 하나씩 직렬로 실행한다.
- Linux 밖의 운영체제. 시험은 Linux에서만 실행한다.
