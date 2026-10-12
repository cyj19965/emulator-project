# NDS 롬의 SDAT 에서 시퀀스(SSEQ)마다 트랙 수와 반복 여부를 구해 이름 종류별로 정리한다
# 사용: python sseq_stats.py <롬.nds>
import collections
import re
import struct
import sys


def varlen(d, p):
    v = 0
    while True:
        b = d[p]
        p += 1
        v = (v << 7) | (b & 0x7F)
        if not b & 0x80:
            return v, p


def arg_len(cmd, d, p):
    """명령 cmd 의 인자 길이. 모르면 None"""
    if cmd < 0x80:
        return varlen(d, p + 1)[1] - p          # velocity + 가변 길이
    if cmd in (0x80, 0x81):
        return varlen(d, p)[1] - p
    if cmd == 0x93:
        return 4
    if cmd in (0x94, 0x95):
        return 3
    if 0xB0 <= cmd <= 0xBD:
        return 3
    if 0xC0 <= cmd <= 0xD7:
        return 1
    if 0xE0 <= cmd <= 0xE3:
        return 2
    if cmd in (0xFC, 0xFD, 0xFF):
        return 0
    if cmd == 0xFE:
        return 2
    return None


def scan_track(d, base, start, limit=200000):
    """트랙을 따라가며 (반복 여부, 끝까지 읽었는지)"""
    p = base + start
    steps = 0
    while p < len(d) and steps < limit:
        steps += 1
        at = p
        cmd = d[p]
        p += 1
        if cmd == 0xFF:
            return False, True
        if cmd == 0x94:
            target = d[p] | (d[p + 1] << 8) | (d[p + 2] << 16)
            return base + target <= at, True
        if cmd == 0xA2:                      # if: 다음 명령에 붙는 접두
            continue
        if cmd == 0xA0:                      # random: 하위 명령 + 마지막 인자 대신 min,max
            sub = d[p]
            p += 1
            if sub < 0x80:
                p += 1 + 4
            elif 0xB0 <= sub <= 0xBD:
                p += 1 + 4
            else:
                p += 4
            continue
        if cmd == 0xA1:                      # var: 하위 명령 + 마지막 인자 대신 변수 번호
            sub = d[p]
            p += 1
            if sub < 0x80 or 0xB0 <= sub <= 0xBD:
                p += 2
            else:
                p += 1
            continue
        n = arg_len(cmd, d, p)
        if n is None:
            return False, False
        p += n
    return False, False


def analyze(sseq):
    """SSEQ 파일 -> (트랙 수, 반복 여부, 끝까지 읽었는지)"""
    if sseq[:4] != b'SSEQ':
        return None
    base = struct.unpack_from('<I', sseq, 0x18)[0]     # DATA 블록 안의 시퀀스 데이터 위치(0x1C)
    p = base
    starts = [0]
    if sseq[p] == 0xFE:
        p += 3
        while sseq[p] == 0x93:
            off = sseq[p + 2] | (sseq[p + 3] << 8) | (sseq[p + 4] << 16)
            starts.append(off)
            p += 5
        starts[0] = p - base
    loop = False
    complete = True
    for s in starts:
        l, ok = scan_track(sseq, base, s)
        loop |= l
        complete &= ok
    return len(starts), loop, complete


def sequences(d):
    pos = 0
    while True:
        base = d.find(b'SDAT\xff\xfe', pos)
        if base < 0:
            return
        pos = base + 4
        symb_off, symb_size, info_off, info_size, fat_off, fat_size, file_off, file_size = \
            struct.unpack_from('<8I', d, base + 0x10)
        if base + info_off + 4 > len(d) or d[base + info_off:base + info_off + 4] != b'INFO':
            continue
        info, fat, symb = base + info_off, base + fat_off, base + symb_off
        rec = struct.unpack_from('<I', d, info + 8)[0]
        n = struct.unpack_from('<I', d, info + rec)[0]
        soff = struct.unpack_from('<I', d, symb + 8)[0]
        for i in range(n):
            e = struct.unpack_from('<I', d, info + rec + 4 + 4 * i)[0]
            if not e:
                continue
            no = struct.unpack_from('<I', d, symb + soff + 4 + 4 * i)[0]
            name = d[symb + no:d.index(b'\0', symb + no)].decode('ascii', 'replace') if no else 'seq%d' % i
            file_id = struct.unpack_from('<H', d, info + e)[0]
            player = d[info + e + 9]
            foff, fsize = struct.unpack_from('<II', d, fat + 12 + 16 * file_id)
            yield hex(base), name, player, d[base + foff:base + foff + fsize]


def kind(name):
    m = re.match(r'SEQ_(SE|ME|PV|GS_SE|GS_ME|SILENCE)', name)
    if not m:
        return 'BGM'
    return {'GS_SE': 'SE', 'GS_ME': 'ME'}.get(m.group(1), m.group(1))


def main():
    d = open(sys.argv[1], 'rb').read()
    stats = collections.defaultdict(lambda: collections.Counter())
    examples = collections.defaultdict(list)
    for sdat, name, player, sseq in sequences(d):
        r = analyze(sseq)
        if not r:
            continue
        tracks, loop, complete = r
        key = (sdat, kind(name), player)
        stats[key]['n'] += 1
        stats[key]['loop'] += loop
        stats[key]['tracks>=4'] += tracks >= 4
        stats[key]['unparsed'] += not complete
        stats[key]['music_rule'] += (loop or tracks >= 4)
        if len(examples[key]) < 4:
            examples[key].append('%s(%dtr%s)' % (name, tracks, ',loop' if loop else ''))
    print('SDAT      종류  플레이어  곡수  반복  트랙4+  규칙(반복 or 트랙4+)  해석실패  예')
    for key in sorted(stats):
        s = stats[key]
        print('%-9s %-5s %5d %6d %5d %6d %12d %12d   %s' % (
            key[0], key[1], key[2], s['n'], s['loop'], s['tracks>=4'], s['music_rule'], s['unparsed'],
            ' '.join(examples[key])))


if __name__ == '__main__':
    main()
