# PC 세션에 붙여 넣을 지시문

아래 `---` 사이 내용을 PC(Windows) 세션에 그대로 붙여 넣으세요.

---

지금까지 작업한 코드를 내 GitHub 저장소에 올려 줘. 다른 작업은 하지 마.

- 저장소: https://github.com/cyj19965/emulator-project.git
- 브랜치: `claude/kind-galileo-ohujte` (이미 `NOTES.md`, `scripts/` 등이 올라가 있음. 먼저 받은 뒤 그 위에 추가)
- 순서:
  1. 임시 폴더에 위 브랜치를 clone
  2. `gba-bgm/` 폴더에 GBA BGM 프로젝트의 현재 작업 폴더에서 소스만 복사 (예전 폴더 `케르릉\gba-bgm`이 아닌 최신 폴더 기준. 어느 쪽이 최신인지 불분명하면 나에게 물어볼 것) (`.cpp`, `.h`, `.lua`, `.py`, `CMakeLists.txt`, `README.md`)
  3. `melonds/` 폴더에 다음을 저장
     - `BgmShadow.h` (경로 유지: `melonds/src/frontend/qt_sdl/BgmShadow.h`)
     - `sseq_stats.py`
     - `bgm-shadow.patch`: melonDS 소스 폴더에서 `git diff 1.1` 결과 (새 파일 포함)
     - `sound-log-stash.patch`: `git stash show -p` 결과 (stash는 지우거나 적용하지 말 것)
  4. 3DS 작업 중 내가 올리라고 한 것이 있으면 `3ds/`에, 없으면 생략
  5. 올리면 안 되는 것: ROM(`.gba`, `.nds`, `.3ds`, `.cia` 등), 세이브(`.sav`, `.srm`, `.ss*`), `build/`, `dist/`, `.exe`, `.dll`, mGBA·melonDS 원본 소스 전체
  6. commit 후 `git push origin claude/kind-galileo-ohujte`
- 끝나면 올린 파일 목록과 commit 해시만 알려 줘.

---
