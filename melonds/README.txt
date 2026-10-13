melonDS 1.1 수정판 (gba-bgm) 버전 1 - 배속 중에도 BGM 1배속 유지

[실행]
  melonDS.exe 를 실행하고 File -> Open ROM 으로 ROM 을 엽니다. (ROM 은 포함되어 있지 않습니다)
  실행 파일 하나로 동작하며, 설정 파일(melonDS.toml)은 처음 실행할 때 실행 파일 옆에 만들어집니다.
  키 설정은 공식 melonDS 와 같이 비어 있습니다. Config -> Input and hotkeys 에서 지정하세요.
  배속 키도 같은 창의 General hotkeys 탭 (Fast forward / Toggle fast forward) 에서 지정합니다.
  배속 수치는 Config -> Interface settings 의 Fast-Forward 항목입니다.

[추가된 메뉴] Config 메뉴 맨 아래
  Keep BGM at 1x speed           BGM 1배속 유지 (기본값 켜짐)
  Fast forward during cutscenes  연출 중에도 배속 (기본값 꺼짐)
  Keep menu sounds at 1x speed   메뉴 효과음 1배속 (기본값 켜짐)

[추가된 단축키] Config -> Input and hotkeys -> General hotkeys (기본값 없음)
  Toggle BGM at 1x speed
  Toggle fast forward during cutscenes

[동작 방식]
  닌텐도 표준 사운드 드라이버(SDAT: SSEQ/SBNK/SWAR)를 쓰는 DS 게임이 대상입니다.
  게임이 음악을 시작하라고 사운드 CPU 에 보내는 명령을 지켜보다가, 원본 음악은 소리만 끄고
  같은 곡을 내장 재생기로 실제 시간에 맞춰 연주합니다. 효과음은 게임 속도를 그대로 따릅니다.
  - 반복되는 곡(필드, 배틀 등)       : 배속과 관계없이 1배속
  - 팡파레(반복 없고 12초 이하)      : 1배속으로 끝까지 연주하고, 멈춰 있던 곡은 그 뒤에 이어집니다.
                                       도중에 게임이 다른 곡을 새로 시작하면 팡파레를 끊고 그 곡을 냅니다.
  - 연출 곡(반복 없고 12초보다 긴 곡) : 바꾸지 않습니다. "연출 중에도 배속"이 꺼져 있으면
                                       이 곡이 나오는 동안에는 배속을 걸어도 1배속으로 진행합니다.
  - 메뉴 효과음                      : 이름에 SELECT, DECIDE, CANCEL, CURSOR, BUTTON, WIN_OPEN, MENU, BEEP 가
                                       들어간 짧은 효과음을 1배속으로 냅니다 (게임에 이름표가 있을 때만).
  - 상태 저장을 불러오거나 기능을 다시 켜면, 게임이 연주 중이던 곡을 같은 위치부터 이어 받습니다.
  음악을 트랙 수로 구분하므로(트랙 4개 이상인 곡이 과반인 묶음을 음악으로 봄) 게임에 따라 맞지 않을 수 있습니다.
  표준 드라이버를 쓰지 않는 게임과 DSi 모드에서는 아무것도 바꾸지 않습니다 (공식 melonDS 와 동일).

[확인한 범위]
  포켓몬스터 Pt 기라티나(한국판): 처음부터 엔딩까지 플레이하며 확인
  포켓몬스터 하트골드(한국판)   : 제목 화면과 도입부만 확인
  그 밖의 게임은 확인하지 않았습니다.

[알려진 제약]
  - 내장 재생기의 소리는 원본과 완전히 같지는 않습니다 (측정한 곡에서 크기 차이 1dB 이내).
  - 내장 재생기의 소리가 원본보다 20~100ms 먼저 나올 수 있습니다.
  - 상태 저장을 불러온 직후에는 다음 곡이나 음량 변화가 있을 때까지 BGM 이 약 2dB 크게 나올 수 있습니다.
  - 배속 중 팡파레가 나올 때 게임이 곧바로 다른 곡을 시작하면 팡파레가 중간에 끊깁니다.
  - 배속 중 메뉴 효과음이 연달아 나오면 앞의 소리를 끊고 새로 시작합니다.
  - 게임이 보내는 명령 중 트랙별 설정, 곡 건너뛰기, 재생 중 변수 변경은 내장 재생기에 반영되지 않습니다.
  - 서명되지 않은 실행 파일이라 Windows 가 실행을 막거나 경고할 수 있습니다.

[공식 melonDS 와 다른 점 (BGM 기능 외)]
  - 한글 등 영어가 아닌 경로의 ROM 을 명령줄 인자로 넘겨도 열립니다.
  - Audio settings 의 Bit depth 가 Auto 일 때 항상 16비트로 출력합니다.
    (공식 1.1 은 처음 연 ROM 은 16비트, 같은 창에서 다시 연 ROM 은 10비트로 나와 효과음이 달라집니다)

[라이선스와 소스]
  이 프로그램은 melonDS 1.1 (GNU General Public License v3) 을 수정한 것이며, 같은 GPL-3.0 으로 배포합니다.
  - licenses\melonDS-GPL-3.0.txt          : melonDS 라이선스 (GPL-3.0)
  - licenses\third-party\*.txt            : 실행 파일에 함께 묶인 라이브러리(Qt6, SDL2 등)와
                                            melonDS 소스에 포함된 서드파티 구성요소의 라이선스
  - source\gba-bgm-melonDS-source.tar.gz  : 이 프로그램의 소스 (수정한 melonDS 전체)
  - source\melonDS-1.1-changes.diff       : melonDS 1.1 (커밋 b86390e) 대비 수정 내용
  빌드 방법은 공식 melonDS 의 Windows 빌드와 같습니다 (MSYS2 UCRT64, vcpkg, 프리셋 release-mingw-x86_64).
