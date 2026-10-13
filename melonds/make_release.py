# melonDS 수정판 배포 묶음을 만든다
# 사용: python make_release.py
# 이 스크립트가 있는 폴더 기준으로
#   melonds-src/                 : 수정한 melonDS 소스 (빌드까지 끝난 상태)
#   release_readme.txt           : 배포 묶음에 넣을 읽어주세요 문서
#   ../../release/<이름>/, .zip  : 결과
import glob
import os
import shutil
import subprocess
import tarfile
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__)).replace(os.sep, '/')
SRC = HERE + '/melonds-src'
BUILD = SRC + '/build/release-mingw-x86_64'
NAME = 'gba-bgm-v1-melonDS-1.1-win64'
OUT = os.path.normpath(HERE + '/../../release/' + NAME).replace(os.sep, '/')
NEW_FILES = ['src/frontend/qt_sdl/BgmReplacer.cpp', 'src/frontend/qt_sdl/BgmReplacer.h',
             'src/frontend/qt_sdl/NitroSynth.cpp', 'src/frontend/qt_sdl/NitroSynth.h']
BS = bytes([0x5C])  # 역슬래시


def git(*args):
    return subprocess.run(['git', '-C', SRC] + list(args), capture_output=True).stdout


def scrub_home(data):
    """실행 파일에 들어간 빌드 PC 의 경로에서 사용자 폴더 이름을 같은 길이의 글자로 덮어쓴다.
    (오류 메시지용 소스 경로 문자열이다. 길이가 같아 파일 구조는 바뀌지 않는다)"""
    home = os.path.expanduser('~').replace(os.sep, '/').encode()  # 예: C:/Users/이름
    parent, name = home.rsplit(b'/', 1)
    for sep in (b'/', BS):
        prefix = parent.replace(b'/', sep) + sep
        data = data.replace(prefix + name, prefix + b'x' * len(name))
        assert (prefix + name) not in data
    return data


def main():
    if os.path.exists(OUT):
        shutil.rmtree(OUT)
    os.makedirs(OUT + '/licenses/third-party')
    os.makedirs(OUT + '/source')

    open(OUT + '/melonDS.exe', 'wb').write(scrub_home(open(BUILD + '/melonDS.exe', 'rb').read()))
    shutil.copy(HERE + '/release_readme.txt', OUT + '/읽어주세요.txt')

    # 라이선스: melonDS 본체(GPL-3.0)와 실행 파일에 함께 묶인 라이브러리
    shutil.copy(SRC + '/LICENSE', OUT + '/licenses/melonDS-GPL-3.0.txt')
    for path in sorted(glob.glob(BUILD + '/vcpkg_installed/x64-mingw-static-release/share/*/copyright')):
        lib = os.path.basename(os.path.dirname(path))
        shutil.copy(path, OUT + '/licenses/third-party/%s.txt' % lib)
    for extra in glob.glob(SRC + '/src/**/LICENSE*', recursive=True) + glob.glob(SRC + '/src/**/COPYING*', recursive=True):
        rel = os.path.relpath(extra, SRC + '/src').replace(os.sep, '_').replace('/', '_')
        shutil.copy(extra, OUT + '/licenses/third-party/melonDS-src_%s.txt' % rel)

    # 소스: 공식 1.1 대비 바꾼 내용(diff)과, 바꾼 상태의 소스 전체
    diff = git('diff', 'HEAD')
    for f in NEW_FILES:
        r = subprocess.run(['git', '-C', SRC, 'diff', '--no-index', '--', '/dev/null', f], capture_output=True)
        diff += r.stdout
    open(OUT + '/source/melonDS-1.1-changes.diff', 'wb').write(diff)
    tracked = git('ls-files').decode().split('\n')
    with tarfile.open(OUT + '/source/gba-bgm-melonDS-source.tar.gz', 'w:gz') as tar:
        for f in [x for x in tracked if x] + NEW_FILES:
            full = SRC + '/' + f
            if os.path.isfile(full):
                tar.add(full, arcname='melonDS-1.1-gba-bgm/' + f)

    zpath = OUT + '.zip'
    if os.path.exists(zpath):
        os.remove(zpath)
    with zipfile.ZipFile(zpath, 'w', zipfile.ZIP_DEFLATED) as z:
        for root, _, names in os.walk(OUT):
            for n in names:
                full = os.path.join(root, n)
                z.write(full, NAME + '/' + os.path.relpath(full, OUT).replace(os.sep, '/'))
    count = sum(len(n) for _, _, n in os.walk(OUT))
    print('파일 %d개, zip %.1f MB' % (count, os.path.getsize(zpath) / 1e6))
    print('diff %d 바이트, 새 파일 포함 여부: %s' % (len(diff), all(f.encode() in diff for f in NEW_FILES)))


if __name__ == '__main__':
    main()
