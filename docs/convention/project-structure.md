---
name: project-structure
description: 프로젝트 폴더 구조 컨벤션. 새 프로젝트를 생성하거나 기존 프로젝트에 파일/폴더를 추가할 때 활성화. "폴더 구조", "프로젝트 구조", "디렉터리", "파일 어디에 두지"가 언급될 때 이 스킬을 따른다.
---

# 프로젝트 구조 컨벤션

> 목적: "이 파일 어디에 두지?"를 없앤다. 팀원과 AI 도구가 동일한 규칙으로 파일을 배치한다.

---

## C 프로젝트 구조

시스템 프로그래밍 과제 (malloc lab, proxy lab 등)에 적합한 구조.

```
project-name/
├── Makefile              # 빌드 규칙
├── README.md             # 프로젝트 설명, 빌드/실행 방법
├── .gitignore
│
├── include/              # 공개 헤더 파일 (.h)
│   ├── mm.h
│   └── memlib.h
│
├── src/                  # 소스 파일 (.c)
│   ├── mm.c              # 메인 구현
│   ├── memlib.c          # 메모리 시뮬레이션
│   └── utils.c           # 유틸리티 함수
│
├── tests/                # 테스트
│   ├── test_mm.c
│   └── traces/           # 테스트 입력 데이터
│       ├── short1.rep
│       └── short2.rep
│
├── docs/                 # 문서
│   └── design.md         # 설계 문서
│
└── build/                # 빌드 산출물 (gitignore)
    └── .gitkeep
```

### 규칙

| 항목 | 위치 |
|------|------|
| 헤더 파일 (.h) | `include/` |
| 소스 파일 (.c) | `src/` |
| 테스트 | `tests/` |
| 빌드 산출물 (.o, 바이너리) | `build/` (gitignore) |
| 문서 | `docs/` |
| 설정 파일 | 프로젝트 루트 |

### 파일 크기 가이드

- 한 파일 300줄 이하 권장
- 넘으면 기능 단위로 분리 (예: `mm.c` → `mm_malloc.c` + `mm_free.c` + `mm_utils.c`)

### 작은 과제 (파일 수 < 5)

파일이 적으면 폴더 분리 없이 플랫 구조도 허용:

```
project-name/
├── Makefile
├── mm.c
├── mm.h
├── memlib.c
├── memlib.h
└── README.md
```

---

## Python 프로젝트 구조

웹 백엔드, 알고리즘 과제, 데이터 처리에 적합한 구조.

### 패키지 프로젝트 (중·대규모)

```
project-name/
├── pyproject.toml        # 프로젝트 메타데이터, 도구 설정
├── README.md
├── .gitignore
├── requirements.txt      # 또는 pyproject.toml의 dependencies
│
├── src/                  # 소스 패키지
│   └── myproject/
│       ├── __init__.py
│       ├── main.py       # 진입점
│       ├── models.py     # 데이터 모델
│       ├── services.py   # 비즈니스 로직
│       ├── utils.py      # 유틸리티
│       └── config.py     # 설정값
│
├── tests/                # 테스트
│   ├── conftest.py       # 공통 fixture
│   ├── test_models.py
│   └── test_services.py
│
├── scripts/              # 일회성 스크립트, 마이그레이션
│   └── seed_data.py
│
├── docs/                 # 문서
│   └── api.md
│
└── .env.example          # 환경변수 템플릿 (.env는 gitignore)
```

### 스크립트/과제 프로젝트 (소규모)

```
project-name/
├── README.md
├── requirements.txt
├── .gitignore
│
├── main.py               # 진입점
├── solution.py           # 풀이/구현
├── utils.py              # 유틸리티
│
└── tests/
    └── test_solution.py
```

### Django 프로젝트

```
project-name/
├── manage.py
├── pyproject.toml
├── requirements.txt
├── .gitignore
├── .env.example
│
├── config/               # 프로젝트 설정 (settings, urls, wsgi)
│   ├── __init__.py
│   ├── settings/
│   │   ├── __init__.py
│   │   ├── base.py       # 공통 설정
│   │   ├── local.py      # 개발용
│   │   └── production.py # 배포용
│   ├── urls.py
│   └── wsgi.py
│
├── apps/                 # Django 앱
│   ├── users/
│   │   ├── __init__.py
│   │   ├── models.py
│   │   ├── views.py
│   │   ├── serializers.py
│   │   ├── urls.py
│   │   └── tests.py
│   └── posts/
│       └── ...
│
├── templates/            # HTML 템플릿
├── static/               # 정적 파일 (CSS, JS, 이미지)
│
└── docs/
```

---

## 공통 규칙

### 파일 네이밍

| 언어 | 규칙 | 예시 |
|------|------|------|
| C | snake_case | `free_list.c`, `mem_utils.h` |
| Python | snake_case | `free_list.py`, `test_utils.py` |
| 문서 | kebab-case | `api-design.md`, `coding-style.md` |
| 설정 | 도구 관례 따름 | `Makefile`, `pyproject.toml`, `.gitignore` |

### .gitignore 필수 항목

```gitignore
# C
*.o
*.d
*.exe
build/

# Python
__pycache__/
*.pyc
*.pyo
.venv/
venv/
*.egg-info/
dist/

# IDE
.vscode/
.idea/
*.swp
*.swo
*~

# OS
.DS_Store
Thumbs.db

# 환경
.env
*.local
```

### README.md 필수 섹션

모든 프로젝트 루트에 README.md가 있어야 한다. 최소 포함 내용:

```markdown
# 프로젝트 이름

한 줄 설명.

## 빌드 / 설치

(빌드 또는 설치 명령어)

## 실행

(실행 명령어)

## 테스트

(테스트 실행 명령어)

## 팀원

- 이름 (역할)
```

---

## docs/ 폴더 구조

```
docs/
├── convention/           # 팀 컨벤션 (이 문서들)
│   ├── commit-convention.md
│   ├── c-style.md
│   ├── python-style.md
│   └── project-structure.md
│
├── design/               # 설계 문서
│   └── architecture.md
│
├── questions/            # 학습 검증 질문
│   └── q01-xxx.md
│
└── <topic>/              # 주제별 학습 아카이브
    └── keyword-tree.md
```

---

## 브랜치와 디렉터리 관계

주(week) 단위 과제가 반복되는 크래프톤 정글 환경:

```
repository/
├── docs/                 # 프로젝트 전체에 걸친 문서
│   └── convention/
│
├── week07/               # 주차별 과제 (필요 시)
│   ├── malloc-lab/
│   └── proxy-lab/
│
└── (또는 주차별 별도 레포지토리)
```

주차별 레포지토리를 쓰는 경우, `docs/convention/`은 개인 workspace에 두고 심링크하거나 복사한다.

---

## 빠른 참조

| 질문 | 답 |
|------|-----|
| 헤더 파일 어디에? | `include/` |
| 소스 파일 어디에? | `src/` |
| 테스트 어디에? | `tests/` |
| 문서 어디에? | `docs/` |
| 빌드 산출물 어디에? | `build/` (gitignore) |
| 설정 파일 어디에? | 프로젝트 루트 |
| 파일 이름 규칙? | snake_case (코드), kebab-case (문서) |
| 한 파일 최대 줄수? | C: 300줄, Python: 300줄 |
| .env 커밋? | 절대 금지. .env.example만 커밋 |
