// gba-bgm: NDS 표준 사운드 드라이버의 시퀀스(SSEQ) 재생기. 설명은 NitroSynth.h
#include "NitroSynth.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

using namespace melonDS;

namespace
{

constexpr s32 kEnvMin = -92544;  // 가장 작은 음량(-723, 0.1dB 단위)을 7비트 올린 값
constexpr double kTimerClock = 16756991.0;
// 드라이버가 한 번 도는 주기(초). 설계상 값은 타이머 2728, 64분주(약 5.21ms)인데, 멜론DS 에서 원본 드라이버의
// 틱 진행(공유 영역의 틱 수)을 재 보면 실제 시간 기준으로 약 0.93% 느리다. 원본과 같은 빠르기가 되도록 맞춘다
constexpr double kDriverPeriod = 64.0 * 2728.0 / 33513982.0 * 1.0093;
// 원본 믹서의 배율: 채널 음량 127, 마스터 음량 127 일 때의 크기
constexpr float kMixScale = (127.0f / 128.0f) * (127.0f / 128.0f);

const u16 kAdpcmTable[89] = {
    0x0007, 0x0008, 0x0009, 0x000A, 0x000B, 0x000C, 0x000D, 0x000E, 0x0010, 0x0011, 0x0013, 0x0015,
    0x0017, 0x0019, 0x001C, 0x001F, 0x0022, 0x0025, 0x0029, 0x002D, 0x0032, 0x0037, 0x003C, 0x0042,
    0x0049, 0x0050, 0x0058, 0x0061, 0x006B, 0x0076, 0x0082, 0x008F, 0x009D, 0x00AD, 0x00BE, 0x00D1,
    0x00E6, 0x00FD, 0x0117, 0x0133, 0x0151, 0x0173, 0x0198, 0x01C1, 0x01EE, 0x0220, 0x0256, 0x0292,
    0x02D4, 0x031C, 0x036C, 0x03C3, 0x0424, 0x048E, 0x0502, 0x0583, 0x0610, 0x06AB, 0x0756, 0x0812,
    0x08E0, 0x09C3, 0x0ABD, 0x0BD0, 0x0CFF, 0x0E4C, 0x0FBA, 0x114C, 0x1307, 0x14EE, 0x1706, 0x1954,
    0x1BDC, 0x1EA5, 0x21B6, 0x2515, 0x28CA, 0x2CDF, 0x315B, 0x364B, 0x3BB9, 0x41B2, 0x4844, 0x4F7E,
    0x5771, 0x602F, 0x69CE, 0x7462, 0x7FFF};
const s8 kAdpcmIndex[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

// PSG 파형: 듀티 0~7 (8칸 중 높은 칸 수가 1~7, 마지막은 전부 낮음)
const s8 kPsgHighFrom[8] = {7, 6, 5, 4, 3, 2, 1, 8};

// 어택 값 109~127 에 쓰는 계수 (127 에서 0 까지 역순으로 찾는다)
const u8 kAttackTable[19] = {0x00, 0x01, 0x05, 0x0E, 0x1A, 0x26, 0x33, 0x3F, 0x49, 0x54,
                             0x5C, 0x64, 0x6D, 0x74, 0x7B, 0x7F, 0x84, 0x89, 0x8F};

u32 rd32(const u8* p)
{
    u32 v;
    memcpy(&v, p, 4);
    return v;
}

u16 rd16(const u8* p)
{
    return (u16)(p[0] | (p[1] << 8));
}

// 0~127 의 음량 값을 0.1dB 단위로 (제곱 곡선: 40*log10)
s32 decibelSquare(int v)
{
    static s16 table[128];
    static bool built = false;
    if (!built)
    {
        table[0] = -32768;
        for (int i = 1; i < 128; i++)
            table[i] = (s16)std::max(-723.0, std::round(400.0 * std::log10(i / 127.0)));
        built = true;
    }
    return table[std::clamp(v, 0, 127)];
}

// LFO 용 사인: 한 주기를 128칸으로, 진폭 127
s32 sinIdx(int index)
{
    static s8 table[128];
    static bool built = false;
    if (!built)
    {
        for (int i = 0; i < 128; i++)
            table[i] = (s8)std::lround(127.0 * std::sin(i * 2.0 * 3.14159265358979323846 / 128.0));
        built = true;
    }
    return table[index & 127];
}

} // namespace

NitroSynth::NitroSynth()
{
}

void NitroSynth::setMemory(const u8* r, u32 mask)
{
    ram = mainRam = r;
    ramMask = mask;
}

void NitroSynth::skipTicks(int p, u32 ticks)
{
    p &= 15;
    Player& pl = players[p];
    if (!pl.prepared || !mainRam) return;
    ram = mainRam;
    silent = true;
    for (u32 i = 0; i < ticks && pl.prepared; i++)
    {
        bool any = false;
        for (const Track& tr : pl.tracks) any |= tr.active;
        if (!any) break;
        playerTick(p);
    }
    silent = false;
}

void NitroSynth::snapshotMemory(int p)
{
    p &= 15;
    if (!mainRam) return;
    // 이 플레이어의 채널이 예전 사본을 읽고 있을 수 있으므로 바로 끈다
    for (Channel& ch : channels)
        if (ch.player == p) ch.active = false;
    snapshot[p].assign(mainRam, mainRam + (size_t)ramMask + 1);
    useSnapshot[p] = true;
}

void NitroSynth::reset()
{
    players = {};
    channels = {};
    globalVars = {};
    useSnapshot = {};
    frameAccum = 0;
}

const u8* NitroSynth::mem(u32 addr, u32 len) const
{
    if (!ram || (addr >> 24) != 0x02) return nullptr;
    const u32 off = addr & ramMask;
    if ((u64)off + len > (u64)ramMask + 1) return nullptr;
    return ram + off;
}

u8 NitroSynth::rd8(u32 addr) const
{
    const u8* p = mem(addr, 1);
    return p ? *p : 0xFF;  // 읽을 수 없으면 FIN(0xFF) 으로 읽혀 트랙이 끝난다
}

u16 NitroSynth::random()
{
    rng = rng * 1664525u + 1013904223u;
    return (u16)(rng >> 16);
}

void NitroSynth::setAttack(Channel& ch, int attack)
{
    attack = std::clamp(attack, 0, 127);
    ch.attackCoef = attack < 109 ? 255 - attack : kAttackTable[127 - attack];
}

s32 NitroSynth::decayCoef(int value)
{
    value = std::clamp(value, 0, 127);
    if (value == 127) return 0xFFFF;
    if (value == 126) return 0x3C00;
    if (value < 50) return value * 2 + 1;
    return 0x1E00 / (126 - value);
}

bool NitroSynth::prepare(int p, u32 seqBase, u32 seqOffset, u32 bank)
{
    Player& pl = players[p & 15];
    // 이 플레이어가 내던 소리는 자연스럽게 줄어들게 두고 새로 시작한다
    for (int t = 0; t < 16; t++) releaseTrackChannels(p & 15, t, -1);
    if (useSnapshot[p & 15])
    {
        // 사본으로 연주하던 소리의 여운은 사본을 계속 읽는다. 다음 연주가 사본을 다시 쓰기 전까지만 유효하다
        useSnapshot[p & 15] = false;
    }
    pl = Player();
    const u8* b = mem(bank, 0x40);
    if (!mem(seqBase + seqOffset, 4) || !b || memcmp(b, "SBNK", 4) != 0) return false;
    pl.seqBase = seqBase;
    pl.bank = bank;
    pl.vars.fill(-1);
    pl.tracks[0].active = true;
    pl.tracks[0].pos = seqBase + seqOffset;
    pl.prepared = true;
    return true;
}

void NitroSynth::start(int p)
{
    Player& pl = players[p & 15];
    if (pl.prepared) pl.active = true;
}

void NitroSynth::stop(int p)
{
    Player& pl = players[p & 15];
    for (int t = 0; t < 16; t++) closeTrack(p & 15, t);
    pl.active = false;
    pl.prepared = false;
}

void NitroSynth::pause(int p, bool paused)
{
    Player& pl = players[p & 15];
    if (pl.paused == paused) return;
    pl.paused = paused;
    if (paused)
        for (int t = 0; t < 16; t++) releaseTrackChannels(p & 15, t, 127);
}

void NitroSynth::setPlayerPrio(int p, int prio)
{
    players[p & 15].prio = (u8)prio;
}

void NitroSynth::setPlayerVolume(int p, int volume)
{
    players[p & 15].volume = (u8)std::clamp(volume, 0, 127);
}

void NitroSynth::setPlayerFader(int p, int fader)
{
    players[p & 15].extFader = (s16)std::clamp(fader, -32768, 0);
}

void NitroSynth::muteTracks(int p, u32 trackMask, bool mute)
{
    for (int t = 0; t < 16; t++)
    {
        if (!(trackMask & (1u << t))) continue;
        players[p & 15].tracks[t].mute = mute;
        if (mute) releaseTrackChannels(p & 15, t, 127);
    }
}

bool NitroSynth::isActive() const
{
    for (const Player& pl : players)
        if (pl.active) return true;
    for (const Channel& ch : channels)
        if (ch.active) return true;
    return false;
}

void NitroSynth::releaseTrackChannels(int p, int t, int release)
{
    for (Channel& ch : channels)
    {
        if (!ch.active || ch.player != p || ch.track != t || ch.envStatus == 3) continue;
        if (release >= 0) ch.releaseCoef = decayCoef(release);
        ch.envStatus = 3;
        ch.prio = 1;
    }
    players[p].tracks[t].lastChannel = -1;
}

void NitroSynth::closeTrack(int p, int t)
{
    Track& tr = players[p].tracks[t];
    if (!tr.active) return;
    releaseTrackChannels(p, t, -1);
    tr.active = false;
}

bool NitroSynth::readInstrument(u32 bank, int prg, int key, int& type, u8 param[10]) const
{
    const u8* b = mem(bank, 0x40);
    if (!b || prg < 0 || (u32)prg >= rd32(b + 0x38)) return false;
    const u8* e = mem(bank + 0x3C + 4 * prg, 4);
    if (!e) return false;
    const u32 entry = rd32(e);
    type = entry & 0xFF;
    u32 at = bank + (entry >> 8);

    if (type == 16)
    {
        // 드럼 세트: {낮은 키, 높은 키, 키마다 {종류, 0, 인자 10바이트}}
        const u8* h = mem(at, 2);
        if (!h || key < h[0] || key > h[1]) return false;
        at += 2 + 12 * (key - h[0]);
        const u8* s = mem(at, 12);
        if (!s) return false;
        type = s[0];
        at += 2;
    }
    else if (type == 17)
    {
        // 키 분할: 구간 끝 키 8개, 구간마다 {종류, 0, 인자 10바이트}
        const u8* h = mem(at, 8);
        if (!h) return false;
        int region = -1;
        for (int i = 0; i < 8 && h[i]; i++)
        {
            if (key <= h[i])
            {
                region = i;
                break;
            }
        }
        if (region < 0) return false;
        at += 8 + 12 * region;
        const u8* s = mem(at, 12);
        if (!s) return false;
        type = s[0];
        at += 2;
    }
    if (type < 1 || type > 3) return false;  // 1 PCM, 2 PSG, 3 노이즈
    const u8* prm = mem(at, 10);
    if (!prm) return false;
    memcpy(param, prm, 10);
    return true;
}

bool NitroSynth::startWave(Channel& ch, u32 bank, int waveNo, int warcNo)
{
    if (warcNo < 0 || warcNo > 3) return false;
    // 뱅크 머리 뒤에 {웨이브 아카이브 주소, 다음 연결} 이 4개 있다
    const u8* link = mem(bank + 0x18 + 8 * warcNo, 4);
    if (!link) return false;
    const u32 swar = rd32(link);
    const u8* s = mem(swar, 0x40);
    if (!s || memcmp(s, "SWAR", 4) != 0 || waveNo < 0 || (u32)waveNo >= rd32(s + 0x38)) return false;
    const u8* o = mem(swar + 0x3C + 4 * waveNo, 4);
    if (!o) return false;
    const u32 off = rd32(o);
    const u32 addr = off < 0x02000000 ? swar + off : off;
    const u8* h = mem(addr, 12);
    if (!h) return false;

    const int type = h[0];
    const u32 loopWords = rd16(h + 6);
    const u32 lenWords = rd32(h + 8);
    const u64 bytes = ((u64)loopWords + lenWords) * 4;
    if (type > 2 || bytes < 8 || bytes > 0x400000) return false;
    const u8* data = mem(addr + 12, (u32)bytes);
    if (!data) return false;

    ch.type = type;
    ch.data = data;
    ch.loops = h[1] != 0;
    const u32 timer = rd16(h + 4);
    ch.baseRate = timer ? kTimerClock / timer : rd16(h + 2);
    if (type == 0)
    {
        ch.total = (u32)bytes;
        ch.loopStart = loopWords * 4;
    }
    else if (type == 1)
    {
        ch.total = (u32)bytes / 2;
        ch.loopStart = loopWords * 2;
    }
    else
    {
        // ADPCM: 처음 4바이트는 초깃값(s16)과 표 위치
        ch.total = ((u32)bytes - 4) * 2;
        ch.loopStart = loopWords ? (loopWords * 4 - 4) * 2 : 0;
        ch.adpcmVal = (s16)rd16(data);
        ch.adpcmIdx = std::min<int>(data[2] & 0x7F, 88);
    }
    if (ch.loopStart >= ch.total) ch.loops = false;
    return true;
}

int NitroSynth::allocChannel(int prio)
{
    int best = -1;
    for (int i = 0; i < 16; i++)
    {
        if (!channels[i].active) return i;
        if (best < 0 || channels[i].prio < channels[best].prio ||
            (channels[i].prio == channels[best].prio &&
             channels[i].volLeft + channels[i].volRight < channels[best].volLeft + channels[best].volRight))
            best = i;
    }
    return channels[best].prio <= prio ? best : -1;
}

void NitroSynth::noteOn(int p, int t, int key, int velocity, s32 length)
{
    Player& pl = players[p];
    Track& tr = pl.tracks[t];
    int ci = -1;
    if (silent) return;

    if (tr.tie && tr.lastChannel >= 0)
    {
        Channel& old = channels[tr.lastChannel];
        if (old.active && old.player == p && old.track == t && old.envStatus != 3)
        {
            ci = tr.lastChannel;
            old.key = (u8)key;
            old.velocity = (u8)velocity;
        }
    }
    if (ci < 0)
    {
        int type = 0;
        u8 prm[10];
        if (!readInstrument(pl.bank, tr.prgNo, key, type, prm)) return;
        ci = allocChannel(std::min(255, pl.prio + tr.prio));
        if (ci < 0) return;
        Channel c;
        if (type == 1)
        {
            if (!startWave(c, pl.bank, rd16(prm), rd16(prm + 2))) return;
        }
        else
        {
            // PSG 와 노이즈는 타이머 값 8006 이 원음 높이다
            c.type = type == 2 ? 3 : 4;
            c.duty = prm[0] & 7;
            c.baseRate = kTimerClock / 8006.0;
        }
        c.active = true;
        c.player = p;
        c.track = t;
        c.prio = std::min(255, pl.prio + tr.prio);
        c.key = (u8)key;
        c.origKey = prm[4];
        c.velocity = (u8)velocity;
        c.length = tr.tie ? -1 : length;
        setAttack(c, prm[5]);
        c.decayCoef = decayCoef(prm[6]);
        c.sustain = prm[7];
        c.releaseCoef = decayCoef(prm[8]);
        c.initPan = (int)prm[9] - 64;
        c.envValue = kEnvMin;
        c.envStatus = 0;
        c.frac = 1.0;  // 첫 샘플을 바로 읽는다
        channels[ci] = c;
    }

    Channel& ch = channels[ci];
    if (tr.attack != 0xFF) setAttack(ch, tr.attack);
    if (tr.decay != 0xFF) ch.decayCoef = decayCoef(tr.decay);
    if (tr.sustain != 0xFF) ch.sustain = tr.sustain;
    if (tr.release != 0xFF) ch.releaseCoef = decayCoef(tr.release);

    ch.sweepPitch = tr.sweepPitch;
    if (tr.porta) ch.sweepPitch += ((int)tr.portaKey - key) << 6;
    if (tr.portaTime == 0)
    {
        ch.sweepLength = length;
        ch.autoSweep = false;
    }
    else
    {
        ch.sweepLength = (tr.portaTime * tr.portaTime * std::abs(ch.sweepPitch)) >> 11;
        ch.autoSweep = true;
    }
    ch.sweepCounter = 0;

    ch.lfoType = tr.modType;
    ch.lfoSpeed = tr.modSpeed;
    ch.lfoDepth = tr.modDepth;
    ch.lfoRange = tr.modRange;
    ch.lfoDelay = tr.modDelay;
    tr.lastChannel = ci;
}

bool NitroSynth::trackStep(int p, int t)
{
    Player& pl = players[p];
    Track& tr = pl.tracks[t];
    if (tr.wait > 0)
    {
        tr.wait--;
        if (tr.wait > 0) return true;
    }

    auto u8at = [&]() { return rd8(tr.pos++); };
    auto s16at = [&]() {
        const u8 lo = rd8(tr.pos), hi = rd8(tr.pos + 1);
        tr.pos += 2;
        return (s32)(s16)(lo | (hi << 8));
    };
    auto u24at = [&]() {
        const u32 v = rd8(tr.pos) | (rd8(tr.pos + 1) << 8) | (rd8(tr.pos + 2) << 16);
        tr.pos += 3;
        return v;
    };
    auto varlen = [&]() {
        s32 v = 0;
        for (int i = 0; i < 4; i++)
        {
            const u8 b = u8at();
            v = (v << 7) | (b & 0x7F);
            if (!(b & 0x80)) break;
        }
        return v;
    };
    auto var = [&](int n) -> s16& {
        n &= 31;
        return n < 16 ? pl.vars[n] : globalVars[n - 16];
    };

    for (int guard = 0; guard < 4096 && tr.wait == 0; guard++)
    {
        bool run = true;
        int argKind = 0;  // 0 보통, 1 난수(min,max), 2 변수
        u8 cmd = u8at();
        if (cmd == 0xA2)
        {
            run = tr.cmp;
            cmd = u8at();
        }
        if (cmd == 0xA0)
        {
            argKind = 1;
            cmd = u8at();
        }
        else if (cmd == 0xA1)
        {
            argKind = 2;
            cmd = u8at();
        }
        // 명령의 마지막 인자: 난수/변수 접두가 있으면 그것으로 대신한다
        auto lastArg = [&](int kind) -> s32 {  // kind: 0 u8, 1 s16, 2 가변 길이
            if (argKind == 1)
            {
                const s32 lo = s16at(), hi = s16at();
                const s32 span = hi - lo + 1;
                return span > 0 ? lo + (s32)(((u32)random() * (u32)span) >> 16) : lo;
            }
            if (argKind == 2) return var(u8at());
            return kind == 0 ? (s32)u8at() : kind == 1 ? s16at() : varlen();
        };

        if (cmd < 0x80)
        {
            const int velocity = u8at();
            const s32 length = lastArg(2);
            const int key = std::clamp((int)cmd + tr.transpose, 0, 127);
            if (!run) continue;
            if (!tr.mute) noteOn(p, t, key, velocity, length);
            tr.portaKey = (u8)key;
            if (tr.noteWait) tr.wait = length;
            continue;
        }

        switch (cmd)
        {
        case 0x80: { const s32 v = lastArg(2); if (run) tr.wait = v; break; }
        case 0x81: { const s32 v = lastArg(2); if (run) tr.prgNo = (u16)(v & 0x7FFF); break; }
        case 0x93:
        {
            const int n = u8at() & 15;
            const u32 off = u24at();
            if (run && n != t)
            {
                closeTrack(p, n);
                pl.tracks[n] = Track();
                pl.tracks[n].active = true;
                pl.tracks[n].pos = pl.seqBase + off;
            }
            break;
        }
        case 0x94: { const u32 off = u24at(); if (run) tr.pos = pl.seqBase + off; break; }
        case 0x95:
        {
            const u32 off = u24at();
            if (run && tr.callDepth < 3)
            {
                tr.callStack[tr.callDepth++] = tr.pos;
                tr.pos = pl.seqBase + off;
            }
            break;
        }
        case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6:
        case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD:
        {
            s16& v = var(u8at());
            const s32 a = lastArg(1);
            if (!run) break;
            switch (cmd)
            {
            case 0xB0: v = (s16)a; break;
            case 0xB1: v = (s16)(v + a); break;
            case 0xB2: v = (s16)(v - a); break;
            case 0xB3: v = (s16)(v * a); break;
            case 0xB4: if (a) v = (s16)(v / a); break;
            case 0xB5: v = (s16)(a >= 0 ? v << a : v >> -a); break;
            case 0xB6:
            {
                const s32 r = (s32)(((u32)random() * (u32)(std::abs(a) + 1)) >> 16);
                v = (s16)(a < 0 ? -r : r);
                break;
            }
            case 0xB8: tr.cmp = v == a; break;
            case 0xB9: tr.cmp = v >= a; break;
            case 0xBA: tr.cmp = v > a; break;
            case 0xBB: tr.cmp = v <= a; break;
            case 0xBC: tr.cmp = v < a; break;
            case 0xBD: tr.cmp = v != a; break;
            }
            break;
        }
        case 0xE0: { const s32 v = lastArg(1); if (run) tr.modDelay = (u16)v; break; }
        case 0xE1: { const s32 v = lastArg(1); if (run) pl.tempo = (u16)v; break; }
        case 0xE3: { const s32 v = lastArg(1); if (run) tr.sweepPitch = (s16)v; break; }
        case 0xFC:
            if (run && tr.callDepth > 0)
            {
                u8& count = tr.loopCount[tr.callDepth - 1];
                if (count == 0 || --count != 0)
                    tr.pos = tr.callStack[tr.callDepth - 1];
                else
                    tr.callDepth--;
            }
            break;
        case 0xFD:
            if (run && tr.callDepth > 0) tr.pos = tr.callStack[--tr.callDepth];
            break;
        case 0xFE: tr.pos += 2; break;
        case 0xFF:
            if (run) return false;
            break;
        default:
            if (cmd >= 0xC0 && cmd <= 0xD7)
            {
                const s32 a = lastArg(0);
                if (!run) break;
                switch (cmd)
                {
                case 0xC0: tr.pan = (s8)(a - 64); break;
                case 0xC1: tr.volume = (u8)a; break;
                case 0xC2: pl.volume = (u8)a; break;
                case 0xC3: tr.transpose = (s8)a; break;
                case 0xC4: tr.pitchBend = (s8)a; break;
                case 0xC5: tr.bendRange = (u8)a; break;
                case 0xC6: tr.prio = (u8)a; break;
                case 0xC7: tr.noteWait = a & 1; break;
                case 0xC8:
                    tr.tie = a & 1;
                    releaseTrackChannels(p, t, -1);
                    break;
                case 0xC9:
                    tr.portaKey = (u8)std::clamp(a + tr.transpose, 0, 127);
                    tr.porta = true;
                    break;
                case 0xCA: tr.modDepth = (u8)a; break;
                case 0xCB: tr.modSpeed = (u8)a; break;
                case 0xCC: tr.modType = (u8)a; break;
                case 0xCD: tr.modRange = (u8)a; break;
                case 0xCE: tr.porta = a & 1; break;
                case 0xCF: tr.portaTime = (u8)a; break;
                case 0xD0: tr.attack = (u8)a; break;
                case 0xD1: tr.decay = (u8)a; break;
                case 0xD2: tr.sustain = (u8)a; break;
                case 0xD3: tr.release = (u8)a; break;
                case 0xD4:
                    if (tr.callDepth < 3)
                    {
                        tr.callStack[tr.callDepth] = tr.pos;
                        tr.loopCount[tr.callDepth] = (u8)a;
                        tr.callDepth++;
                    }
                    break;
                case 0xD5: tr.expression = (u8)a; break;
                default: break;  // 0xD6 변수 출력 등: 소리와 무관
                }
                break;
            }
            return false;  // 모르는 명령: 트랙을 끝낸다
        }
    }
    return true;
}

void NitroSynth::playerTick(int p)
{
    Player& pl = players[p];
    bool any = false;
    pl.ticks++;
    for (int t = 0; t < 16; t++)
    {
        if (!pl.tracks[t].active) continue;
        // 음표 길이와, 틱 단위로 세는 스윕
        for (Channel& ch : channels)
        {
            if (!ch.active || ch.player != p || ch.track != t) continue;
            if (ch.length > 0 && --ch.length == 0 && ch.envStatus != 3)
            {
                ch.envStatus = 3;
                ch.prio = 1;
            }
            if (!ch.autoSweep && ch.sweepCounter < ch.sweepLength) ch.sweepCounter++;
        }
        if (!trackStep(p, t))
            closeTrack(p, t);
        else
            any = true;
    }
    if (!any) pl.active = pl.prepared = false;  // 곡이 끝남
}

void NitroSynth::updateChannel(Channel& ch)
{
    // 엔벨로프
    switch (ch.envStatus)
    {
    case 0:
        ch.envValue = -((-ch.envValue * ch.attackCoef) >> 8);
        if (ch.envValue == 0) ch.envStatus = 1;
        break;
    case 1:
    {
        const s32 sustain = decibelSquare(ch.sustain) << 7;
        ch.envValue -= ch.decayCoef;
        if (ch.envValue <= sustain)
        {
            ch.envValue = sustain;
            ch.envStatus = 2;
        }
        break;
    }
    case 3:
        ch.envValue -= ch.releaseCoef;
        if (ch.envValue <= kEnvMin)
        {
            ch.active = false;
            return;
        }
        break;
    default:
        break;
    }

    // 스윕
    s32 sweep = 0;
    if (ch.sweepPitch != 0 && ch.sweepLength > 0 && ch.sweepCounter < ch.sweepLength)
    {
        sweep = (s32)((s64)ch.sweepPitch * (ch.sweepLength - ch.sweepCounter) / ch.sweepLength);
        if (ch.autoSweep) ch.sweepCounter++;
    }

    // LFO
    s32 lfoPitch = 0, lfoVolume = 0, lfoPan = 0;
    if (ch.lfoDepth != 0)
    {
        if (ch.lfoDelayCounter < ch.lfoDelay)
        {
            ch.lfoDelayCounter++;
        }
        else
        {
            const s64 value = (s64)sinIdx(ch.lfoCounter >> 8) * ch.lfoDepth * ch.lfoRange;
            if (ch.lfoType == 0) lfoPitch = (s32)((value << 6) >> 14);
            else if (ch.lfoType == 1) lfoVolume = (s32)((value * 60) >> 14);
            else lfoPan = (s32)((value << 6) >> 14);
            ch.lfoCounter = (u16)((ch.lfoCounter + (ch.lfoSpeed << 6)) & 0x7FFF);
        }
    }

    // 트랙과 플레이어의 음량·음정·좌우 (트랙이 살아 있을 때만 새로 읽는다)
    if (ch.player >= 0 && ch.track >= 0)
    {
        const Player& pl = players[ch.player];
        const Track& tr = pl.tracks[ch.track];
        if (tr.active)
        {
            ch.userPitch = ((s32)tr.pitchBend * tr.bendRange * 64) >> 7;
            ch.userPan = tr.panRange == 127 ? tr.pan : (tr.pan * tr.panRange + 64) >> 7;
            ch.userDecay = decibelSquare(tr.volume) + decibelSquare(tr.expression);
        }
        // 플레이어 음량과 페이더는 곡이 끝난 뒤의 여음에도 계속 적용한다
        const s32 total = ch.userDecay + decibelSquare(pl.volume) + pl.extFader;
        const s32 volume = decibelSquare(ch.velocity) + (ch.envValue >> 7) + total + lfoVolume;
        const float amp = volume <= -723 ? 0.0f : std::pow(10.0f, std::min(volume, 0) / 200.0f);
        const int pan = std::clamp(64 + ch.initPan + ch.userPan + lfoPan, 0, 127);
        ch.volLeft = amp * (128 - pan) / 128.0f * kMixScale;
        ch.volRight = amp * pan / 128.0f * kMixScale;
    }

    const s32 pitch = ((s32)ch.key - ch.origKey) * 64 + sweep + ch.userPitch + lfoPitch;
    ch.rate = ch.baseRate * std::pow(2.0, pitch / 768.0);
}

void NitroSynth::advance(Channel& ch)
{
    if (ch.type == 3)
    {
        ch.ipos = (ch.ipos + 1) & 7;
        ch.cur = ch.ipos >= kPsgHighFrom[ch.duty] ? 0x7FFF : -0x7FFF;
        return;
    }
    if (ch.type == 4)
    {
        if (ch.noise & 1)
        {
            ch.noise = (ch.noise >> 1) ^ 0x6000;
            ch.cur = -0x7FFF;
        }
        else
        {
            ch.noise >>= 1;
            ch.cur = 0x7FFF;
        }
        return;
    }

    ch.ipos++;
    if ((u32)ch.ipos >= ch.total)
    {
        if (!ch.loops)
        {
            ch.active = false;
            ch.cur = 0;
            return;
        }
        ch.ipos = (s32)ch.loopStart;
        ch.adpcmVal = ch.loopVal;
        ch.adpcmIdx = ch.loopIdx;
    }
    if (ch.type == 0)
    {
        ch.cur = (s32)(s8)ch.data[ch.ipos] << 8;
    }
    else if (ch.type == 1)
    {
        ch.cur = (s16)rd16(ch.data + ch.ipos * 2);
    }
    else
    {
        if ((u32)ch.ipos == ch.loopStart)
        {
            ch.loopVal = ch.adpcmVal;
            ch.loopIdx = ch.adpcmIdx;
        }
        const u8 byte = ch.data[4 + (ch.ipos >> 1)];
        const int nibble = (ch.ipos & 1) ? byte >> 4 : byte & 0xF;
        const s32 step = kAdpcmTable[ch.adpcmIdx];
        s32 diff = step >> 3;
        if (nibble & 1) diff += step >> 2;
        if (nibble & 2) diff += step >> 1;
        if (nibble & 4) diff += step;
        if (nibble & 8)
            ch.adpcmVal = std::max(ch.adpcmVal - diff, -0x7FFF);
        else
            ch.adpcmVal = std::min(ch.adpcmVal + diff, 0x7FFF);
        ch.adpcmIdx = std::clamp(ch.adpcmIdx + kAdpcmIndex[nibble & 7], 0, 88);
        ch.cur = ch.adpcmVal;
    }
}

void NitroSynth::driverFrame()
{
    for (int p = 0; p < 16; p++)
    {
        Player& pl = players[p];
        if (!pl.active || pl.paused) continue;
        // 템포 240 이 드라이버 한 번에 한 틱이다
        ram = useSnapshot[p] ? snapshot[p].data() : mainRam;
        u32 counter = pl.tempoCounter + ((u32)pl.tempo * pl.tempoRatio >> 8);
        while (counter >= 240 && pl.active)
        {
            counter -= 240;
            playerTick(p);
        }
        pl.tempoCounter = (u16)std::min<u32>(counter, 0xFFFF);
    }
    ram = mainRam;
    for (Channel& ch : channels)
        if (ch.active) updateChannel(ch);
}

void NitroSynth::render(s16* out, int frames, int sampleRate)
{
    rendered += (u64)std::max(frames, 0);
    const double perFrame = sampleRate * kDriverPeriod;
    for (int i = 0; i < frames; i++)
    {
        if (frameAccum <= 0)
        {
            driverFrame();
            frameAccum += perFrame;
        }
        frameAccum -= 1.0;

        float left = 0, right = 0;
        for (Channel& ch : channels)
        {
            if (!ch.active) continue;
            ch.frac += ch.rate / sampleRate;
            for (int n = 0; n < 256 && ch.frac >= 1.0 && ch.active; n++)
            {
                ch.frac -= 1.0;
                advance(ch);
            }
            left += ch.cur * ch.volLeft;
            right += ch.cur * ch.volRight;
        }
        out[i * 2] = (s16)std::clamp(left, -32768.0f, 32767.0f);
        out[i * 2 + 1] = (s16)std::clamp(right, -32768.0f, 32767.0f);
    }
}
