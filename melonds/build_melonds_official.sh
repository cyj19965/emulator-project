# melonDS 를 공식 Windows 빌드와 같은 방식으로 빌드한다 (MSYS2 UCRT64 셸에서 실행)
# 공식 설정: .github/workflows/build-windows.yml -> UCRT64 + GCC, vcpkg 로 의존 라이브러리를 직접 빌드,
#            프리셋 release-mingw-x86_64 (정적 빌드, 실행 파일 하나)
# 사용: 이 스크립트 옆에 수정한 melonDS 소스를 melonds-src 폴더로 두고
#       WIN_HOME="C:/Users/<사용자 이름>" bash build_melonds_official.sh
cd "$(dirname "$0")/melonds-src" || exit 1
# MSYS2 로그인 셸에는 Windows 기본 환경 변수가 비어 있을 수 있어서, vcpkg 와 빌드 설정이 쓰는 것만 채운다
: "${WIN_HOME:?WIN_HOME 에 Windows 사용자 폴더를 지정하세요 (예: C:/Users/이름)}"
export USERPROFILE="$WIN_HOME"
export LOCALAPPDATA="$WIN_HOME/AppData/Local"
export APPDATA="$WIN_HOME/AppData/Roaming"
export PROCESSOR_ARCHITECTURE=AMD64
# MSYS2 에 SDL2·libarchive·zstd 가 따로 설치돼 있으면 pkg-config 가 그쪽을 먼저 잡는다.
# 공식 빌드 환경처럼 vcpkg 가 만든 라이브러리만 쓰도록 MSYS2 쪽 검색 경로를 뺀다
unset PKG_CONFIG_PATH PKG_CONFIG_SYSTEM_INCLUDE_PATH PKG_CONFIG_SYSTEM_LIBRARY_PATH
cmake --preset=release-mingw-x86_64 > ../melonds-official-cfg.log 2>&1
echo "CFG_EXIT=$?"
tail -5 ../melonds-official-cfg.log
cmake --build --preset=release-mingw-x86_64 > ../melonds-official-build.log 2>&1
echo "BUILD_EXIT=$?"
grep -E "error:|FAILED" ../melonds-official-build.log | head -20
ls -la build/release-mingw-x86_64/*.exe 2>/dev/null
