#!/bin/bash
# Claude Code 클라우드 환경용 설정 스크립트 (Ubuntu 24.04 기준)
# 환경 설정 > Setup script 에 이 파일 내용을 붙여 넣어 사용합니다.
set -e

SUDO=""
if [ "$(id -u)" -ne 0 ]; then SUDO="sudo"; fi

export DEBIAN_FRONTEND=noninteractive
$SUDO apt-get update -qq

# melonDS 1.1: 공식 build-ubuntu.yml 의존성 목록 그대로
$SUDO apt-get install -y -qq --no-install-recommends \
  cmake ninja-build pkg-config extra-cmake-modules \
  libpcap0.8-dev libsdl2-dev libenet-dev \
  qt6-base-dev qt6-base-private-dev qt6-multimedia-dev libqt6svg6-dev \
  libarchive-dev libzstd-dev libfuse2 libfaad-dev

# mGBA 0.10.5 libretro 코어 / GBA BGM 도구
$SUDO apt-get install -y -qq --no-install-recommends \
  libzip-dev libpng-dev zlib1g-dev libedit-dev libepoxy-dev \
  lua5.4 liblua5.4-dev

# Windows용 교차 컴파일러 (문법·컴파일 확인용. 배포 빌드는 PC의 MSYS2 UCRT64에서 합니다)
$SUDO apt-get install -y -qq --no-install-recommends mingw-w64
