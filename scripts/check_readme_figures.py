#!/usr/bin/env python3
"""README 이미지가 Daphnis 원본에서 생성되는지 검사한다."""

import re
from pathlib import Path


def main() -> int:
    readme = Path("README.md").read_text()
    images = re.findall(r"!\[[^]]*\]\(([^)]+)\)", readme)
    if not images or "<img" in readme.lower():
        print("README 이미지 목록이 없거나 HTML 이미지가 있다")
        return 1

    invalid = []
    for target in images:
        image = Path(target)
        if (
            not target.startswith("docs/assets/")
            or image.suffix != ".svg"
            or not image.is_file()
            or not image.with_suffix(".dap").is_file()
        ):
            invalid.append(target)
    for target in invalid:
        print(f"Daphnis 원본이 없는 README 이미지: {target}")
    print(f"README Daphnis 이미지 {len(images)}개 확인")
    return 1 if invalid else 0


if __name__ == "__main__":
    raise SystemExit(main())
