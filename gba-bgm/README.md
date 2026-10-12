# gba-bgm: 배속 중에도 BGM을 1배속으로 재생하는 mGBA 수정판

mGBA 0.10.5를 수정해, 배속(Tab)을 켜도 배경음악은 원래 속도로 재생합니다. 효과음은 원본 그대로입니다.

## 동작 방식

| 대상 | 방식 |
|---|---|
| GBA (MP2K/Sappy 엔진) | ROM에서 플레이어 테이블을 찾아, 음악을 재생하는 플레이어만 무음 처리하고 같은 곡을 내장 MP2K 재생기(`common/mp2k_engine.*`)로 1배속 재생. 곡이 반복 구간을 갖거나 트랙이 4개 이상이면 음악으로 판단 |
| GB/GBC (포켓몬 금·은·크리스탈 엔진) | 숨겨진 두 번째 GB 코어에서 게임 자체의 음악 코드(PlayMusic/UpdateSound)만 1배속 실행. 원본은 효과음이 쓰지 않는 하드웨어 채널만 끔. 엔진 진입점과 RAM 주소는 ROM 코드 패턴으로 찾음 |
| 그 밖의 엔진 | 아무것도 바꾸지 않음 (원래 mGBA와 동일) |

- 실시간 출력: 배속과 관계없이 BGM 1배속. mGBA 프로세스의 소리이므로 OBS·Win+G 녹화에 함께 들어감
- Audio/Video → Record A/V: 게임 진행 시간 기준으로 BGM을 섞음
- 메뉴 Audio/Video → "BGM 1배속 유지"로 끄고 켬 (기본 켜짐)

## 폴더 구조

```
gba-bgm/
├─ src/                    ← git 대상
│  ├─ mgba/                수정한 mGBA 0.10.5 소스
│  │   └─ src/platform/qt/BgmReplacer.*, GbDriverBgm.* (신규), CoreController.*, AudioDevice.*, Window.cpp,
│  │      CMakeLists.txt, src/platform/sdl/sdl-audio.* (수정)
│  ├─ common/              MP2K 재생기, ROM 탐색, 게임 상태 동기화
│  ├─ tools/               검증 도구 (가짜 ROM 렌더링, 실제 ROM 헤드리스 비교)
│  ├─ scripts/             빌드·배포 스크립트
│  └─ release_files/       배포본 안내문, 라이선스 원문
├─ work/                   ← git 제외: 빌드 폴더, 참고용 디컴파일 소스, FFmpeg 소스, 원본 mGBA
└─ release/                ← git 제외: 배포용 zip
```

## 빌드 (MSYS2 UCRT64 셸)

경로에 한글이 있으면 Qt의 moc가 실패하므로 이 폴더는 한글 없는 경로에 둡니다.

```
bash src/scripts/build_mgba.sh        # work/build/mgba-qt/mGBA.exe
bash src/scripts/deploy_app.sh        # work/build/app/ (실행 가능한 폴더)
bash src/scripts/build_tests.sh       # work/build/tests/
bash src/scripts/package_release.sh   # release/<이름>.zip
```

필요 패키지: `mingw-w64-ucrt-x86_64-{toolchain,cmake,ninja,ffmpeg,libelf,libepoxy,libzip,lua,pkgconf,qt5,SDL2,ntldd}`.

## 검증 도구

- `synth_test`: 가짜 ROM으로 MP2K 재생기 음높이·박자·빈 곡 무음 확인
- `gba_bgm_test <rom> <출력폴더> [초] [버튼 간격]`: 실제 GBA ROM을 화면 없이 실행해 원본/무음 처리본/대체 BGM을 WAV로 저장
- `gb_bgm_test <rom> <출력폴더> [초]`: GB/GBC 판, 4배속 시나리오 포함

확인한 게임: 포켓몬 파이어레드(북미), 젤다 신들의 트라이포스 & 4개의 검(북미), 포켓몬 골드(북미·한글판).

## 알려진 제약

- GBA: 페이드아웃, 울음소리 중 볼륨 감소는 반영하지 않음
- 곡 시작 첫 프레임에 원본 BGM이 짧게 들릴 수 있음
- 배속 중에는 게임이 곡을 빨리 넘기므로 진화 음악·팡파르 같은 짧은 곡이 잘림
- 마리오 & 루이지 RPG처럼 MP2K를 바탕으로 한 자체 엔진은 지원하지 않음

## 진행 중

- 공식판처럼 exe 하나로 만드는 정적 빌드: `build_ffmpeg_static.sh`(완료) → `build_mgba_static.sh`(미검증)
