---
name: commit-convention
description: Git 커밋 메시지 작성, 브랜치 전략, PR 워크플로우, merge vs rebase 등 Git 협업 전반을 다루는 스킬. 커밋 메시지 제안이 필요하거나, 브랜치 전략을 결정하거나, PR 설명을 작성하거나, 충돌을 해결하거나, "커밋 메시지", "커밋 컨벤션", "브랜치 전략", "PR 작성"이 언급될 때 활성화.
---

# Git 워크플로우 & 커밋 컨벤션

## 커밋 메시지

### 형식

```text
<type>: <한국어 제목>
```

본문이 필요한 경우:

```text
<type>: <한국어 제목>

<한국어 본문>
```

Breaking Change가 있는 경우:

```text
<type>: <한국어 제목>

<한국어 본문>

BREAKING CHANGE: explain the incompatible change in English
```

### 허용 타입

- `feat`: 사용자 관점의 기능 추가
- `fix`: 버그 수정
- `refactor`: 동작 변화 없이 구조 개선
- `docs`: 문서 변경
- `test`: 테스트 추가 또는 수정
- `chore`: 잡무성 변경, 유지보수
- `style`: 동작에 영향 없는 포맷팅
- `perf`: 성능 개선
- `build`: 빌드 설정, 패키지, 컴파일 구성 변경
- `ci`: CI 설정 변경
- `revert`: 이전 커밋 되돌리기

### 스코프 정책

커밋 제목에 스코프를 사용하지 않는다.
변경된 파일과 한국어 제목으로 영향 범위를 전달한다.

### 제목 규칙

- 한국어로 작성
- 한 줄로 유지
- 마침표로 끝내지 않기
- 행위가 아닌 변경의 결과를 서술
- `수정`, `작업`, `변경`, `업데이트` 같은 모호한 단어 지양
- `자식 노드 비교 순서를 바로잡아`, `타입스크립트 빌드 설정을 추가` 같은 구체적 표현 선호

### 본문 규칙

아래 중 하나라도 해당하면 본문 추가:

- 변경 이유가 명확하지 않을 때
- 영향 범위가 넓을 때
- 마이그레이션 또는 사용 주의사항이 있을 때
- 리뷰어에게 컨텍스트가 필요할 때

한국어로 작성, 간결하고 사실에 기반해 서술.

### 예시

```text
feat: DOM 렌더러 초기 구조를 추가

fix: 자식 노드 재정렬 시 인덱스 계산 오류를 고쳐

docs: README에 타입스크립트 시작 방법을 정리

build: TypeScript 출력 경로와 타입 선언 생성을 설정

refactor: 가상 노드 생성 흐름을 단순화
```

본문 포함 예시:

```text
fix: 자식 노드 재정렬 시 인덱스 계산 오류를 고쳐

키 비교 후 재배치 순서를 다시 계산하도록 바꿔
중첩 목록 갱신에서 잘못된 DOM 이동이 발생하지 않게 한다
```

Breaking Change 예시:

```text
feat: 렌더러 초기화 API를 단순화

기본 사용 흐름을 하나로 맞추기 위해 진입 함수를 통합한다

BREAKING CHANGE: replace createRenderer() with createRoot()
```

### 응답 스타일

커밋 후보를 바로 복사할 수 있는 텍스트 블록으로 반환한다.
필요 시 `이유:` 한 줄을 추가한다.
요약이 모호한 경우 최선의 가정을 하고 간략히 명시한다.

---

## 브랜치 전략

### GitHub Flow (대부분의 프로젝트에 권장)

지속적 배포, 소~중규모 팀에 적합.

```
main (보호, 항상 배포 가능)
  │
  ├── feature/user-auth      → PR → main 병합
  ├── feature/payment-flow   → PR → main 병합
  └── fix/login-bug          → PR → main 병합
```

- `main`은 항상 배포 가능한 상태 유지
- `main`에서 feature 브랜치 생성
- 준비되면 Pull Request 오픈
- 승인 및 CI 통과 후 `main`에 병합
- 병합 즉시 배포

### Trunk-Based Development (고속 팀)

강력한 CI/CD와 Feature Flag를 갖춘 팀에 적합.

- 모든 커밋이 `main` 또는 수명이 짧은 브랜치(1~2일)로 이동
- 미완성 기능은 Feature Flag로 숨김
- CI 통과 후에만 병합
- 하루 여러 번 배포

### GitFlow (릴리즈 주기가 명확한 프로젝트)

```
main (프로덕션 릴리즈)
  └── develop (통합 브랜치)
        ├── feature/user-auth
        ├── release/1.0.0    → main + develop에 병합
        └── hotfix/critical  → main + develop에 병합
```

### 전략 선택 기준

| 전략 | 팀 규모 | 배포 주기 | 적합한 상황 |
|------|---------|----------|------------|
| GitHub Flow | 무관 | 지속적 | SaaS, 웹앱, 스타트업 |
| Trunk-Based | 5명+ 숙련 | 하루 여러 번 | 고속 팀, Feature Flag |
| GitFlow | 10명+ | 정기 릴리즈 | 엔터프라이즈, 규제 산업 |

---

## Merge vs Rebase

### Merge (히스토리 보존)

```bash
git checkout main
git merge feature/user-auth
# merge commit이 생성되어 히스토리가 보존됨
```

사용 시점:
- feature 브랜치를 `main`에 병합할 때
- 정확한 히스토리를 보존해야 할 때
- 여러 명이 브랜치에서 작업했을 때
- 이미 push되어 다른 사람이 기반으로 사용 중일 때

### Rebase (선형 히스토리)

```bash
git checkout feature/user-auth
git rebase main
# feature 커밋이 main 위에 재작성됨
```

사용 시점:
- local feature 브랜치를 최신 main으로 업데이트할 때
- 깔끔한 선형 히스토리를 원할 때
- 브랜치가 아직 push되지 않았을 때
- 혼자만 작업 중인 브랜치일 때

### Rebase 하면 안 되는 경우

```
# 절대 rebase 금지:
- 공유 저장소에 push된 브랜치
- 다른 사람이 기반으로 사용 중인 브랜치
- main, develop 같은 보호 브랜치
- 이미 병합된 브랜치
```

---

## Pull Request

### PR 제목 형식

```
<type>: <한국어 설명>

예시:
feat: 기업 사용자를 위한 SSO 지원 추가
fix: 주문 처리의 경쟁 조건 해결
docs: v2 엔드포인트 OpenAPI 명세 추가
```

### PR 설명 템플릿

```markdown
## 무엇을 변경했나요?

변경 내용을 간단히 설명.

## 왜 변경했나요?

변경 동기와 맥락 설명.

## 어떻게 구현했나요?

주목할 만한 구현 세부사항.

## 테스트

- [ ] 단위 테스트 추가/수정
- [ ] 통합 테스트 추가/수정
- [ ] 수동 테스트 완료

## 체크리스트

- [ ] 프로젝트 스타일 가이드 준수
- [ ] 자기 리뷰 완료
- [ ] 복잡한 로직에 주석 추가
- [ ] 문서 업데이트
- [ ] 새로운 경고 없음
- [ ] 로컬에서 테스트 통과

Closes #123
```

---

## 브랜치 명명 규칙

```
feature/user-authentication
feature/JIRA-123-payment-integration

fix/login-redirect-loop
fix/456-null-pointer-exception

hotfix/critical-security-patch
hotfix/database-connection-leak

release/1.2.0
```

---

## 자주 쓰는 Git 명령어

```bash
# 브랜치 생성 및 전환
git checkout -b feature/user-auth

# 최신 main으로 브랜치 업데이트
git fetch origin
git rebase origin/main

# 병합된 브랜치 정리
git branch --merged main | grep -v "^\*\|main" | xargs -n 1 git branch -d
git fetch -p  # 삭제된 원격 브랜치 정리

# 작업 임시 저장
git stash push -m "WIP: 사용자 인증"
git stash pop

# 실수 되돌리기
git reset --soft HEAD~1   # 마지막 커밋 취소 (변경사항 유지)
git revert HEAD           # 공개 브랜치에서 커밋 되돌리기
git commit --amend -m "수정된 메시지"  # 마지막 커밋 메시지 수정
```

---

## 안티패턴

```
❌ main에 직접 커밋
❌ .env, API 키 커밋
❌ 1000줄 이상의 거대한 PR
❌ "update", "fix", "수정" 같은 모호한 커밋 메시지
❌ 공개 브랜치에 force push
❌ 장기 feature 브랜치 (몇 주 이상)
❌ dist/, node_modules/ 커밋
```

---

## 빠른 참조

| 작업 | 명령어 |
|------|--------|
| 브랜치 생성 | `git checkout -b feature/name` |
| 브랜치 삭제 | `git branch -d branch-name` |
| 브랜치 병합 | `git merge branch-name` |
| 히스토리 보기 | `git log --oneline --graph` |
| 변경사항 확인 | `git diff` |
| 스테이징 | `git add -p` |
| 커밋 | `git commit -m "type: 한국어 제목"` |
| 스태시 | `git stash push -m "설명"` |
| 마지막 커밋 취소 | `git reset --soft HEAD~1` |
