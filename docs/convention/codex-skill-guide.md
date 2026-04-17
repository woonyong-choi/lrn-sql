# Codex Skill 등록 가이드

`docs/convention` 아래 문서들을 Codex가 직접 사용할 수 있는 스킬로 등록하고, 프롬프트에서 호출하는 방법을 정리한 가이드입니다.

## 대상 파일

이 저장소에서는 아래 네 파일을 스킬 원본으로 사용합니다.

- `docs/convention/c-style.md`
- `docs/convention/python-style.md`
- `docs/convention/project-structure.md`
- `docs/convention/commit-convention.md`

이 파일들은 이미 YAML frontmatter의 `name`, `description`을 포함하고 있어 스킬 본문으로 재사용하기 좋습니다.

## Codex 스킬 기본 구조

Codex 스킬은 보통 `~/.codex/skills/<skill-name>/SKILL.md` 구조를 사용합니다.

예시:

```text
~/.codex/skills/
├── c-style/
│   └── SKILL.md
├── python-style/
│   └── SKILL.md
├── project-structure/
│   └── SKILL.md
└── commit-convention/
    └── SKILL.md
```

핵심은 파일명보다 폴더 안의 `SKILL.md`입니다.
즉 `docs/convention/c-style.md`를 그대로 두고,
그 내용을 `~/.codex/skills/c-style/SKILL.md`로 복사하면 됩니다.

## 등록 방법

### 1. 스킬 폴더 만들기

```bash
mkdir -p ~/.codex/skills/c-style
mkdir -p ~/.codex/skills/python-style
mkdir -p ~/.codex/skills/project-structure
mkdir -p ~/.codex/skills/commit-convention
```

### 2. 컨벤션 파일을 `SKILL.md`로 복사하기

현재 저장소 루트에서 실행:

```bash
cp docs/convention/c-style.md ~/.codex/skills/c-style/SKILL.md
cp docs/convention/python-style.md ~/.codex/skills/python-style/SKILL.md
cp docs/convention/project-structure.md ~/.codex/skills/project-structure/SKILL.md
cp docs/convention/commit-convention.md ~/.codex/skills/commit-convention/SKILL.md
```

### 3. 한 번에 등록하기

```bash
mkdir -p \
  ~/.codex/skills/c-style \
  ~/.codex/skills/python-style \
  ~/.codex/skills/project-structure \
  ~/.codex/skills/commit-convention

cp docs/convention/c-style.md ~/.codex/skills/c-style/SKILL.md
cp docs/convention/python-style.md ~/.codex/skills/python-style/SKILL.md
cp docs/convention/project-structure.md ~/.codex/skills/project-structure/SKILL.md
cp docs/convention/commit-convention.md ~/.codex/skills/commit-convention/SKILL.md
```

## 업데이트 방법

이 저장소의 `docs/convention/*.md`를 원본으로 유지하고,
문서를 수정한 뒤 다시 `~/.codex/skills/.../SKILL.md`로 복사하면 됩니다.

예시:

```bash
cp docs/convention/commit-convention.md ~/.codex/skills/commit-convention/SKILL.md
```

새 세션에서 반영하는 것이 가장 안전합니다.
이미 열려 있는 Codex 세션이 있다면 새로 열거나 재시작해서 스킬 목록을 다시 읽게 하면 됩니다.

## 사용하는 방법

스킬 이름을 프롬프트에 직접 언급하면 가장 확실합니다.

### 예시 프롬프트

#### commit-convention

```text
commit-convention 스킬로 이번 변경에 맞는 커밋 메시지를 작성해줘
```

```text
브랜치 전략은 commit-convention 기준으로 따라줘
```

#### c-style

```text
c-style 스킬 기준으로 mm.c를 리팩터링해줘
```

```text
포인터 선언과 들여쓰기는 c-style 컨벤션을 따라줘
```

#### python-style

```text
python-style 스킬 기준으로 이 모듈을 정리해줘
```

```text
pytest 테스트 코드도 python-style에 맞춰 작성해줘
```

#### project-structure

```text
project-structure 스킬 기준으로 폴더 구조를 다시 잡아줘
```

```text
이 파일은 어디에 두는 게 맞는지 project-structure 기준으로 판단해줘
```

## 여러 스킬을 함께 쓰는 방법

필요하면 한 요청에서 여러 개를 같이 부를 수 있습니다.

예시:

```text
project-structure와 c-style 스킬을 같이 적용해서 C 프로젝트 골격을 만들고,
마지막 커밋 메시지는 commit-convention 기준으로 작성해줘
```

## 권장 운영 방식

- 저장소 안의 `docs/convention/*.md`를 팀의 원본 문서로 유지합니다.
- 개인 환경의 `~/.codex/skills/*/SKILL.md`는 실행용 복사본으로 봅니다.
- 컨벤션 문서를 바꿨으면 스킬 복사본도 함께 갱신합니다.
- 스킬 호출이 중요할 때는 프롬프트에 이름을 직접 적습니다.

## 빠른 점검 체크리스트

- `~/.codex/skills/<skill-name>/SKILL.md`가 존재하는가
- 파일 상단 frontmatter에 `name`, `description`이 있는가
- 스킬 이름을 프롬프트에 직접 언급했는가
- 문서 수정 후 새 Codex 세션에서 다시 확인했는가
