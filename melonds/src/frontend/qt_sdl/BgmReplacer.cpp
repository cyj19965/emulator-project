// gba-bgm: 배속과 관계없이 BGM 을 1배속으로 유지한다 (자체 재생기 방식). 설명은 BgmReplacer.h
#include "BgmReplacer.h"

#include <algorithm>
#include <cstring>

#include "CRC32.h"
#include "EmuInstance.h"
#include "NDS.h"
#include "NDSCart.h"
#include "Platform.h"

using namespace melonDS;

namespace
{

// ARM9 -> ARM7 사운드 명령 번호 (하트골드의 실제 명령 기록으로 확인한 것)
enum : u32
{
    Cmd_StartSeq = 0,
    Cmd_StopSeq = 1,
    Cmd_PrepareSeq = 2,
    Cmd_StartPreparedSeq = 3,
    Cmd_PauseSeq = 4,
    Cmd_SkipSeq = 5,
    Cmd_PlayerParam = 6,
    Cmd_TrackParam = 7,
    Cmd_MuteTrack = 8,
    Cmd_AllocatableChannel = 9,
    Cmd_PlayerLocalVar = 10,
    Cmd_SurroundDecay = 22,
    Cmd_OutputSelector = 25,
    Cmd_InvalidateSeq = 30,
    Cmd_InvalidateWave = 32,
};

constexpr u32 kPxiTagSound = 7;
constexpr u32 kFaderOffset = 6;       // 플레이어 구조체 안의 페이더(s16) 위치
constexpr s32 kFaderMute = -10000;    // 드라이버가 최소 음량으로 잘라 쓰는 값보다 충분히 작은 값
constexpr u32 kNodeSize = 24;         // {u32 next, u32 id, u32 arg[4]}
constexpr size_t kRingFrames = 16384;  // 자체 재생기 출력 버퍼 (프레임)
constexpr float kJingleSeconds = 12.0f;  // 반복 없는 곡이 이보다 짧으면 팡파레로 본다

enum { Kind_Leave = 0, Kind_Music, Kind_Jingle, Kind_LongOneShot, Kind_MenuSfx };

// 메뉴·대화 조작음으로 보는 이름 (사운드 아카이브의 이름표 기준)
bool isMenuSoundName(const std::string& name)
{
    static const char* const keys[] = {"SELECT", "DECIDE", "CANCEL", "CURSOR", "BUTTON", "WIN_OPEN", "MENU", "BEEP"};
    std::string upper = name;
    for (char& c : upper) c = (char)toupper((unsigned char)c);
    for (const char* k : keys)
        if (upper.find(k) != std::string::npos) return true;
    return false;
}

u32 rd32(const u8* p)
{
    u32 v;
    memcpy(&v, p, 4);
    return v;
}

bool readVarLen(const u8* d, u32 size, u32& p, u32& value)
{
    value = 0;
    for (int i = 0; i < 4; i++)
    {
        if (p >= size) return false;
        const u8 b = d[p++];
        value = (value << 7) | (b & 0x7F);
        if (!(b & 0x80)) return true;
    }
    return true;
}

// 트랙 하나를 따라가며 반복(앞으로 되돌아가는 점프)이 있는지 본다.
// 반복이 없으면 ticks 에 트랙의 길이(틱)를, tempo 에 처음 나온 템포를 넣는다 (호출과 횟수 반복은 따라간다)
bool scanTrackLoops(const u8* d, u32 size, u32 base, u32 start, u32& ticks, int& tempo)
{
    u32 p = base + start;
    bool noteWait = true;
    u32 callStack[3], loopPos[3];
    int loopLeft[3];
    int calls = 0, loops = 0;
    ticks = 0;
    for (int steps = 0; steps < 200000 && p < size; steps++)
    {
        const u32 at = p;
        const u8 cmd = d[p++];
        u32 v = 0;
        if (cmd == 0xFF) return false;
        if (cmd == 0x94)
        {
            if (p + 3 > size) return false;
            const u32 target = d[p] | (d[p + 1] << 8) | (d[p + 2] << 16);
            if (base + target <= at) return true;
            p = base + target;  // 앞으로 건너뛰는 점프는 따라간다
            continue;
        }
        if (cmd == 0xA2) continue;  // if: 다음 명령에 붙는 접두
        if (cmd == 0xA0 || cmd == 0xA1)
        {
            // random / var: 하위 명령 뒤에, 마지막 인자 대신 (min,max) 또는 변수 번호가 온다
            if (p >= size) return false;
            const u8 sub = d[p++];
            const bool extra = sub < 0x80 || (sub >= 0xB0 && sub <= 0xBD);
            p += (cmd == 0xA0 ? 4 : 1) + (extra ? 1 : 0);
            continue;
        }
        if (cmd < 0x80)
        {
            p++;
            if (!readVarLen(d, size, p, v)) return false;
            if (noteWait) ticks += v;
        }
        else if (cmd == 0x80)
        {
            if (!readVarLen(d, size, p, v)) return false;
            ticks += v;
        }
        else if (cmd == 0x81)
        {
            if (!readVarLen(d, size, p, v)) return false;
        }
        else if (cmd == 0x93) p += 4;
        else if (cmd == 0x95)
        {
            if (p + 3 > size) return false;
            const u32 target = d[p] | (d[p + 1] << 8) | (d[p + 2] << 16);
            p += 3;
            if (calls < 3)
            {
                callStack[calls++] = p;
                p = base + target;
            }
        }
        else if (cmd >= 0xB0 && cmd <= 0xBD) p += 3;
        else if (cmd == 0xC7)
        {
            if (p >= size) return false;
            noteWait = d[p++] & 1;
        }
        else if (cmd == 0xD4)
        {
            if (p >= size) return false;
            const int count = d[p++];
            if (count == 0) return true;  // 끝없이 되풀이
            if (loops < 3)
            {
                loopPos[loops] = p;
                loopLeft[loops++] = count;
            }
        }
        else if (cmd >= 0xC0 && cmd <= 0xD7) p += 1;
        else if (cmd == 0xE1)
        {
            if (p + 2 > size) return false;
            if (tempo == 0) tempo = d[p] | (d[p + 1] << 8);
            p += 2;
        }
        else if (cmd >= 0xE0 && cmd <= 0xE3) p += 2;
        else if (cmd == 0xFC)
        {
            if (loops > 0)
            {
                if (--loopLeft[loops - 1] > 0)
                    p = loopPos[loops - 1];
                else
                    loops--;
            }
        }
        else if (cmd == 0xFD)
        {
            if (calls == 0) return false;
            p = callStack[--calls];
        }
        else if (cmd == 0xFE) p += 2;
        else return false;  // 모르는 명령
    }
    return false;
}

// SSEQ 파일에서 트랙 수와 반복 여부, (반복이 없을 때) 대략의 길이를 구한다
bool analyzeSseq(const u8* f, u32 size, int& tracks, bool& loop, float& seconds)
{
    if (size < 0x20 || memcmp(f, "SSEQ", 4) != 0) return false;
    const u32 base = rd32(f + 0x18);
    if (base >= size) return false;
    u32 p = base;
    std::vector<u32> starts{0};
    if (f[p] == 0xFE)
    {
        p += 3;
        while (p + 5 <= size && f[p] == 0x93)
        {
            starts.push_back(f[p + 2] | (f[p + 3] << 8) | (f[p + 4] << 16));
            p += 5;
        }
        starts[0] = p - base;
    }
    tracks = (int)starts.size();
    loop = false;
    u32 longest = 0;
    int tempo = 0;
    for (u32 s : starts)
    {
        u32 ticks = 0;
        loop |= scanTrackLoops(f, size, base, s, ticks, tempo);
        longest = std::max(longest, ticks);
    }
    // 4분음표 = 48틱, 템포 = 분당 4분음표 수
    seconds = (float)longest * 60.0f / (48.0f * (float)(tempo > 0 ? tempo : 120));
    return true;
}

} // namespace

BgmReplacer::BgmReplacer(EmuInstance* inst) : inst(inst)
{
    ring.resize(kRingFrames * 2);

}

BgmReplacer::~BgmReplacer()
{
}

void BgmReplacer::setEnabled(bool on)
{
    enabled = on;
    wanted = on;
    if (!on)
    {
        // 자체 재생기의 소리를 바로 멈춘다. 원본의 음악 플레이어는 게임이 다음 음량 명령을 보낼 때
        // 원래 음량으로 돌아온다 (그때까지는 무음)
        if (inst->nds && lastNds == inst->nds && findDriverPlayers())
        {
            for (int p = 0; p < 16; p++)
                if (players[p].music && players[p].muted)
                    inst->nds->ARM7Write16(driverPlayers + 0x24 * p + 6, (u16)players[p].fader);
        }
        for (Player& p : players) p.music = p.cutscene = false;
        cutsceneNow = false;
        orphan = {};
        sfxOrphan = {};
        synth.reset();
        std::lock_guard lock(ringMutex);
        ringCount = 0;
    }
    if (on && inst->nds && lastNds == inst->nds && lastFrame > 60)
    {
        resetTracking();
        recoverFromDriver();
    }
}

void BgmReplacer::resetTracking()
{
    players = {};
    cutsceneNow = false;
    orphan = {};
    sfxOrphan = {};
    synth.reset();
    std::lock_guard lock(ringMutex);
    ringCount = 0;
}

void BgmReplacer::indexCart()
{
    NDS* nds = inst->nds;
    const auto* cart = nds ? nds->NDSCartSlot.GetCart() : nullptr;
    const u32 checksum = cart ? cart->Checksum() : 0;
    if (cartIndexed && checksum == cartChecksum) return;
    cartIndexed = true;
    cartChecksum = checksum;
    seqIndex.clear();
    groupIsMusic.clear();
    if (!cart) return;

    const u8* rom = cart->GetROM();
    const u32 romlen = cart->GetROMLength();
    static const u8 magic[6] = {'S', 'D', 'A', 'T', 0xFF, 0xFE};
    std::vector<int> groupTotal, groupBig;
    int archives = 0;

    for (const u8* it = rom;;)
    {
        it = std::search(it, rom + romlen, magic, magic + 6);
        if (it == rom + romlen) break;
        const u8* sdat = it;
        it += 4;
        const u32 left = (u32)(rom + romlen - sdat);
        if (left < 0x40) break;
        const u32 symbOff = rd32(sdat + 0x10), symbSize = rd32(sdat + 0x14);
        const u32 infoOff = rd32(sdat + 0x18), fatOff = rd32(sdat + 0x20);
        // 압축 데이터 안에서 우연히 같은 글자가 나온 곳은 건너뛴다
        if (infoOff + 0x40 > left || fatOff + 0x10 > left || memcmp(sdat + infoOff, "INFO", 4) != 0)
            continue;
        const u8* info = sdat + infoOff;
        const u8* fat = sdat + fatOff;
        const u32 seqRec = rd32(info + 8);
        if (infoOff + seqRec + 4 > left) continue;
        const u32 count = rd32(info + seqRec);
        const u32 fatCount = rd32(fat + 8);
        archives++;

        std::unordered_map<u32, int> playerGroup;  // 이 아카이브의 플레이어 번호 -> 묶음 번호
        for (u32 i = 0; i < count && i < 0x10000; i++)
        {
            if (infoOff + seqRec + 8 + 4 * i > left) break;
            const u32 e = rd32(info + seqRec + 4 + 4 * i);
            if (!e || infoOff + e + 12 > left) continue;
            const u32 fileId = info[e] | (info[e + 1] << 8);
            const u32 player = info[e + 9];
            if (fileId >= fatCount || fatOff + 12 + 16 * fileId + 8 > left) continue;
            const u32 foff = rd32(fat + 12 + 16 * fileId), fsize = rd32(fat + 12 + 16 * fileId + 4);
            if (foff >= left || fsize > left - foff || fsize < 0x20) continue;

            SeqInfo si;
            if (!analyzeSseq(sdat + foff, fsize, si.tracks, si.loop, si.seconds)) continue;
            auto g = playerGroup.find(player);
            if (g == playerGroup.end())
            {
                g = playerGroup.emplace(player, (int)groupTotal.size()).first;
                groupTotal.push_back(0);
                groupBig.push_back(0);
            }
            si.group = g->second;
            groupTotal[si.group]++;
            if (si.tracks >= 4) groupBig[si.group]++;

            // 이름표(SYMB)가 있으면 기록용으로 이름을 둔다
            if (symbSize && symbOff + 12 <= left)
            {
                const u8* symb = sdat + symbOff;
                const u32 soff = rd32(symb + 8);
                if (symbOff + soff + 8 + 4 * i <= left && i < rd32(symb + soff))
                {
                    const u32 no = rd32(symb + soff + 4 + 4 * i);
                    if (no && symbOff + no < left)
                    {
                        const char* s = (const char*)symb + no;
                        si.name.assign(s, strnlen(s, std::min<u32>(64, left - symbOff - no)));
                    }
                }
            }
            seqIndex.emplace(CRC32(sdat + foff, (int)fsize), std::move(si));
        }
    }

    // 묶음 안 곡의 과반이 트랙 4개 이상이면 그 묶음 전체를 음악으로 본다
    int music = 0;
    for (size_t g = 0; g < groupTotal.size(); g++)
    {
        groupIsMusic.push_back(groupBig[g] * 2 > groupTotal[g]);
        music += groupIsMusic.back();
    }
}

// 시퀀스의 종류: 그대로 둘 소리 / 음악(반복 곡) / 팡파레(반복 없는 짧은 곡) / 반복 없는 긴 곡(그대로 둔다)
int BgmReplacer::classify(u32 seqBase, std::string& name)
{
    NDS* nds = inst->nds;
    name = "?";
    if ((seqBase >> 24) != 0x02) return Kind_Leave;
    const u32 off = (seqBase - 0x1C) & nds->MainRAMMask;
    const u32 ramsize = nds->MainRAMMask + 1;
    if (off + 0x20 > ramsize) return Kind_Leave;
    const u8* f = &nds->MainRAM[off];
    if (memcmp(f, "SSEQ", 4) != 0) return Kind_Leave;
    const u32 size = rd32(f + 8);
    if (size < 0x20 || size > 0x100000 || off + size > ramsize) return Kind_Leave;

    bool music, loop = false;
    float seconds = 0;
    auto it = seqIndex.find(CRC32(f, (int)size));
    if (it != seqIndex.end())
    {
        name = it->second.name.empty() ? "(이름 없음)" : it->second.name;
        music = it->second.group >= 0 && groupIsMusic[it->second.group];
        loop = it->second.loop;
        seconds = it->second.seconds;
    }
    else
    {
        // 롬의 아카이브에서 찾지 못한 시퀀스: 트랙 수로만 판단한다
        int tracks = 0;
        name = "(아카이브에 없음)";
        music = analyzeSseq(f, size, tracks, loop, seconds) && tracks >= 4;
    }
    lastSeconds = seconds;
    // 음악 묶음이 아닌 짧은 소리 가운데 이름이 조작음인 것
    if (!music && menuSounds && it != seqIndex.end() && !loop && seconds <= 3.0f && isMenuSoundName(name))
        return Kind_MenuSfx;
    if (!music) return Kind_Leave;
    if (loop) return Kind_Music;
    return seconds <= kJingleSeconds ? Kind_Jingle : Kind_LongOneShot;
}

int BgmReplacer::allocSynth()
{
    for (int id = 0; id < 16; id++)
    {
        bool used = (orphan[id] || sfxOrphan[id]) && synth.playerActive(id);
        for (const Player& pl : players) used |= pl.music && pl.synthId == id;
        if (used) continue;
        orphan[id] = sfxOrphan[id] = false;
        return id;
    }
    return -1;
}

// 자체 재생기에서 아직 연주 중인 팡파레인지
bool BgmReplacer::jingleActive(int id) const
{
    if (id < 0 || !synth.playerActive(id)) return false;
    if (orphan[id]) return true;
    for (const Player& pl : players)
        if (pl.music && pl.jingle && pl.started && pl.synthId == id) return true;
    return false;
}

// 팡파레를 시작한 뒤로 게임 안에서 흐른 시간과, 자체 재생기가 실제로 연주한 시간.
// 1배속에서는 둘이 같고, 배속 중에는 게임 쪽이 앞서 간다
double BgmReplacer::jingleGameSeconds(int id) const
{
    return (double)(lastFrame - jingleFrame[id]) / 59.8261;
}

double BgmReplacer::jingleSynthSeconds(int id) const
{
    return (double)(synth.renderedFrames() - jingleSample[id]) / (double)std::max(inst->audioFreq, 1);
}

// 음악을 시작하거나 이어 가려는데, 게임에서는 이미 더 진행된(또는 끝난) 팡파레가 자체 재생기에서는
// 아직 그만큼 나오지 않았으면: 팡파레가 그 지점까지 나올 때까지 미룬다. 1배속에서는 미뤄지지 않는다
bool BgmReplacer::holdForJingle(Player& pl)
{
    int worst = -1;
    double worstLag = 0.1;
    for (int id = 0; id < 16; id++)
    {
        if (!jingleActive(id)) continue;
        const double lag = jingleGameSeconds(id) - jingleSynthSeconds(id);
        if (lag > worstLag)
        {
            worst = id;
            worstLag = lag;
        }
    }
    if (worst < 0) return false;
    pl.holdId = worst;
    pl.holdUntil = jingleGameSeconds(worst);
    return true;
}

void BgmReplacer::startMusic(int p)
{
    Player& pl = players[p];
    if (pl.sfx)
    {
        // 같은 효과음이 아직 울리고 있으면 끊고 새로 낸다 (배속 중에 겹쳐 쌓이지 않게)
        for (Player& other : players)
            if (&other != &pl && other.music && other.sfx && other.started && other.name == pl.name)
                synth.stop(other.synthId);
        for (int id = 0; id < 16; id++)
        {
            if (sfxOrphan[id] && sfxOrphanName[id] == pl.name) synth.stop(id);
        }
        synth.start(pl.synthId);
        pl.started = true;
        return;
    }
    if (pl.jingle)
    {
        // 새 팡파레가 나올 때, 게임에서는 진작 지나간 앞의 팡파레가 아직 울리고 있으면 멈춘다 (배속 중에 겹치지 않게)
        for (int id = 0; id < 16; id++)
        {
            if (id == pl.synthId || !jingleActive(id)) continue;
            if (jingleGameSeconds(id) - jingleSynthSeconds(id) < 0.25) continue;
            synth.stop(id);
            orphan[id] = false;
        }
        jingleFrame[pl.synthId] = lastFrame;
        jingleSample[pl.synthId] = synth.renderedFrames();
    }
    else
    {
        // 새 곡이 시작된다 = 게임의 장면이 넘어갔다. 게임에서는 이미 지나간 팡파레가 아직 울리고 있으면 끊고
        // 새 곡을 바로 낸다 (멈췄던 곡이 이어지는 경우에만 팡파레를 끝까지 기다린다)
        for (int id = 0; id < 16; id++)
        {
            if (!jingleActive(id) || jingleGameSeconds(id) - jingleSynthSeconds(id) < 0.1) continue;
            synth.stop(id);
            orphan[id] = false;
        }
    }
    synth.start(pl.synthId);
    pl.started = true;
}

// 게임이 이 자리(플레이어)를 다른 소리에 다시 쓴다: 하던 연주를 정리한다
void BgmReplacer::detach(int p)
{
    Player& pl = players[p];
    if (!pl.music) return;
    // 원본에서는 이미 끝난 팡파레가 자체 재생기에서는 아직 나오고 있을 수 있다 (배속 중). 끝까지 두고 본다
    if (pl.sfx && pl.started && synth.playerActive(pl.synthId))
    {
        sfxOrphan[pl.synthId] = true;
        sfxOrphanName[pl.synthId] = pl.name;
    }
    else if (pl.jingle && pl.started && synth.playerActive(pl.synthId))
        orphan[pl.synthId] = true;
    else
        synth.stop(pl.synthId);
    pl.music = false;
}

void BgmReplacer::releaseHeld()
{
    for (int p = 0; p < 16; p++)
    {
        Player& pl = players[p];
        if (!pl.music || !(pl.holdStart || pl.holdResume)) continue;
        if (jingleActive(pl.holdId) && jingleSynthSeconds(pl.holdId) < pl.holdUntil) continue;
        if (pl.holdStart)
        {
            synth.start(pl.synthId);
            pl.started = true;
        }
        if (pl.holdResume) synth.pause(pl.synthId, false);
        pl.holdStart = pl.holdResume = false;
    }
}

// ARM7 사운드 드라이버의 플레이어 배열을 찾는다. 플레이어 하나는 0x24 바이트:
//   +0 상태 비트(0 연주 중, 2 일시정지), +1 자기 번호, +4 우선순위, +5 음량, +6 페이더(s16),
//   +8 트랙 번호 16개, +0x18 템포, +0x20 뱅크 주소
// 배열(16개) 바로 뒤에 트랙 32개(0x40 바이트씩)가 있고, 트랙의 +0x24 가 시퀀스 데이터의 시작 주소다
bool BgmReplacer::findDriverPlayers()
{
    NDS* nds = inst->nds;
    auto valid = [&](u32 x, bool needActive) {
        const u8 flags = nds->ARM7Read8(x);
        if (needActive && !(flags & 1)) return false;
        if (nds->ARM7Read8(x + 1) > 15) return false;
        const u32 bank = nds->ARM7Read32(x + 0x20);
        if ((bank >> 24) != 0x02) return false;
        const u32 off = bank & nds->MainRAMMask;
        if (off + 4 > nds->MainRAMMask + 1 || memcmp(&nds->MainRAM[off], "SBNK", 4) != 0) return false;
        for (u32 i = 0; i < 16; i++)
        {
            const u8 t = nds->ARM7Read8(x + 8 + i);
            if (t != 0xFF && t >= 32) return false;
        }
        return nds->ARM7Read8(x + 8) < 32;
    };
    if (driverPlayers)
    {
        // 전에 찾은 주소가 아직 맞는지: 연주 중인 플레이어가 하나라도 그 모양이면 된다
        for (u32 p = 0; p < 16; p++)
            if (valid(driverPlayers + 0x24 * p, true) && nds->ARM7Read8(driverPlayers + 0x24 * p + 1) == p) return true;
    }
    static const u32 ranges[2][2] = {{0x037F8000, 0x03810000}, {0x02380000, 0x023C0000}};
    for (const auto& r : ranges)
    {
        for (u32 x = r[0]; x + 0x24 <= r[1]; x += 4)
        {
            if (!valid(x, true)) continue;
            driverPlayers = x - 0x24 * nds->ARM7Read8(x + 1);
            return true;
        }
    }
    return false;
}

// 추적을 처음부터 다시 해야 할 때(상태 불러오기, 기능을 다시 켬): 게임이 지금 연주 중인 곡을 드라이버에서 읽어
// 음악은 같은 위치부터 자체 재생기로 이어 받고, 연출 곡이면 연출 중으로 표시한다
void BgmReplacer::recoverFromDriver()
{
    NDS* nds = inst->nds;
    if (!nds || nds->ConsoleType != 0 || !findDriverPlayers()) return;
    synth.setMemory(nds->MainRAM, nds->MainRAMMask);
    for (int p = 0; p < 16; p++)
    {
        const u32 x = driverPlayers + 0x24 * p;
        const u8 flags = nds->ARM7Read8(x);
        if (!(flags & 1) || nds->ARM7Read8(x + 1) != p) continue;
        const u8 track0 = nds->ARM7Read8(x + 8);
        if (track0 >= 32) continue;
        const u32 seqBase = nds->ARM7Read32(driverPlayers + 0x24 * 16 + 0x40 * track0 + 0x24);
        const u32 bank = nds->ARM7Read32(x + 0x20);

        Player& pl = players[p];
        pl = Player();
        const int kind = classify(seqBase, pl.name);
        if (kind == Kind_LongOneShot)
        {
            pl.cutscene = true;
            pl.cutsceneStart = lastFrame;
            pl.cutsceneEnd = lastFrame + (u32)(lastSeconds * 59.8261f) + 30;
            continue;
        }
        if (kind != Kind_Music && kind != Kind_Jingle) continue;
        pl.synthId = allocSynth();
        if (pl.synthId < 0 || !synth.prepare(pl.synthId, seqBase, 0, bank)) continue;
        pl.music = true;
        pl.jingle = kind == Kind_Jingle;
        // 지금까지 진행한 틱 수는 드라이버가 공유 영역에 써 준다
        u32 ticks = 0;
        if ((sharedWork >> 24) == 0x02)
            ticks = rd32(&nds->MainRAM[(sharedWork + 0x40 + 0x24 * p) & nds->MainRAMMask]);
        if (ticks > 4000000) ticks = 0;
        synth.skipTicks(pl.synthId, ticks);

        pl.prio = nds->ARM7Read8(x + 4);
        pl.volume = nds->ARM7Read8(x + 5);
        const s16 fader = (s16)nds->ARM7Read16(x + 6);
        // 이미 무음 값이 들어 있으면 게임이 의도한 값은 알 수 없다 (다음 음량 명령 때 맞춰진다)
        pl.fader = fader <= kFaderMute ? 0 : fader;
        nds->ARM7Write16(x + 6, (u16)kFaderMute);
        pl.muted = true;
        synth.setPlayerPrio(pl.synthId, pl.prio);
        synth.setPlayerVolume(pl.synthId, pl.volume);
        synth.setPlayerFader(pl.synthId, pl.fader);
        if (pl.jingle)
        {
            synth.snapshotMemory(pl.synthId);
            jingleFrame[pl.synthId] = lastFrame;
            jingleSample[pl.synthId] = synth.renderedFrames();
        }
        synth.start(pl.synthId);
        pl.started = true;
        if (flags & 4) synth.pause(pl.synthId, true);
    }
}

void BgmReplacer::fifoHook(NDS& nds, u32 word)
{
    auto* inst = static_cast<EmuInstance*>(nds.UserData);
    if (inst && inst->bgmReplacer && inst->nds == &nds) inst->bgmReplacer->onSoundFifo(word);
}

void BgmReplacer::onSoundFifo(u32 word)
{
    // PXI FIFO 값 = (데이터 << 6) | 태그. 사운드의 데이터는 명령 연결 목록의 주소다
    if ((word & 0x1F) != kPxiTagSound || !enabled) return;
    NDS* nds = inst->nds;
    if (nds->ConsoleType != 0) return;
    synth.setMemory(nds->MainRAM, nds->MainRAMMask);

    u32 addr = word >> 6;
    for (int n = 0; n < 256 && (addr >> 24) == 0x02; n++)
    {
        const u32 off = addr & nds->MainRAMMask;
        if (off + kNodeSize > nds->MainRAMMask + 1) break;
        u8* node = &nds->MainRAM[off];
        Command c{rd32(node + 4), {rd32(node + 8), rd32(node + 12), rd32(node + 16), rd32(node + 20)}};
        addr = rd32(node);
        const int p = c.arg[0] & 0xF;
        Player& pl = players[p];

        switch (c.id)
        {
        case Cmd_StartSeq:
        case Cmd_PrepareSeq:
        {
            detach(p);  // 이 플레이어가 하던 연주는 끝난다
            pl = Player();
            const int kind = classify(c.arg[1], pl.name);
            if (kind == Kind_Music || kind == Kind_Jingle || kind == Kind_MenuSfx)
            {
                pl.synthId = allocSynth();
                pl.music = pl.synthId >= 0 && synth.prepare(pl.synthId, c.arg[1], c.arg[2], c.arg[3]);
                pl.jingle = kind == Kind_Jingle;
                pl.sfx = kind == Kind_MenuSfx;
                if (pl.music && pl.jingle) synth.snapshotMemory(pl.synthId);
            }
            if (kind == Kind_LongOneShot)
            {
                pl.cutscene = true;
                pl.cutsceneStart = lastFrame;
                pl.cutsceneEnd = lastFrame + (u32)(lastSeconds * 59.8261f) + 30;
            }
            if (pl.music && c.id == Cmd_StartSeq) startMusic(p);
            break;
        }

        case Cmd_StartPreparedSeq:
            if (pl.music)
            {
                startMusic(p);
            }
            break;

        case Cmd_StopSeq:
            if (pl.music)
            {
                synth.stop(pl.synthId);
            }
            pl.music = false;
            pl.cutscene = false;
            break;

        case Cmd_PauseSeq:
            if (!pl.music) break;
            if (c.arg[1])
            {
                if (pl.holdStart)
                {
                    synth.start(pl.synthId);
                    pl.started = true;
                }
                pl.holdStart = pl.holdResume = false;
                synth.pause(pl.synthId, true);
            }
            else
            {
                if (pl.holdStart) break;  // 아직 시작도 미뤄 둔 상태
                if (!pl.jingle && !pl.sfx && holdForJingle(pl))
                {
                    pl.holdResume = true;
                }
                else
                    synth.pause(pl.synthId, false);
            }
            break;

        case Cmd_PlayerParam:
        {
            if (!pl.music) break;
            // 플레이어 구조체: 4 우선순위(u8), 5 음량(u8), 6 페이더(s16). 게임이 의도한 값은 자체 재생기에 준다
            if (c.arg[1] == 4 && c.arg[3] == 1)
                pl.prio = c.arg[2] & 0xFF;
            else if (c.arg[1] == 5 && c.arg[3] == 1)
                pl.volume = c.arg[2] & 0xFF;
            else if (c.arg[1] == kFaderOffset && c.arg[3] == 2)
                pl.fader = (s16)c.arg[2];
            else
            {
                break;
            }
            synth.setPlayerPrio(pl.synthId, pl.prio);
            synth.setPlayerVolume(pl.synthId, pl.volume);
            synth.setPlayerFader(pl.synthId, pl.fader);
            // 원본에는 "오프셋 4 부터 4바이트" 를 한 번에 쓰는 명령으로 바꿔 넣는다: 우선순위와 음량은 그대로,
            // 페이더만 무음. 곡을 준비할 때 우선순위 설정이 항상 따라오므로, 페이더 명령이 없는 게임도 꺼진다
            const u32 rewritten[3] = {4, (u32)pl.prio | ((u32)pl.volume << 8) | ((u32)(u16)kFaderMute << 16), 4};
            memcpy(node + 12, rewritten, sizeof(rewritten));
            pl.muted = true;
            break;
        }

        case Cmd_MuteTrack:
            if (pl.music)
            {
                synth.muteTracks(pl.synthId, c.arg[1], c.arg[2] != 0);
            }
            break;

        case Cmd_SkipSeq:
        case Cmd_TrackParam:
        case Cmd_PlayerLocalVar:
            // 아직 자체 재생기에 반영하지 않는 명령: 실제로 쓰이는지 기록만 한다
            break;

        case 29:  // 공유 영역 주소 통지
            sharedWork = c.arg[0];
            break;

        default:
            break;
        }
    }
}

void BgmReplacer::afterMainFrame()
{
    NDS* nds = inst->nds;
    if (!nds) return;
    if (wanted != enabled) setEnabled(wanted);
    if (nds->SoundFifoHook != &BgmReplacer::fifoHook) nds->SoundFifoHook = &BgmReplacer::fifoHook;

    // 게임 교체, 리셋, 상태 불러오기: 추적하던 내용이 맞지 않게 되므로 처음부터 다시 본다
    const u32 frame = nds->NumFrames;
    if (nds != lastNds || frame < lastFrame || frame > lastFrame + 1 || forceReset)
    {
        const bool sameGame = nds == lastNds;
        forceReset = false;
        resetTracking();
        lastNds = nds;
        lastFrame = frame;
        indexCart();
        if (!sameGame) driverPlayers = 0;
        if (enabled && frame > 60) recoverFromDriver();
    }
    lastFrame = frame;
    if (enabled) releaseHeld();

    // 연출 곡이 아직 나오는 중인지: 드라이버가 공유 영역에 써 주는 '연주 중인 플레이어' 비트로 확인한다
    // (주소를 모르면 곡의 길이로 어림한다)
    bool cut = false;
    for (int p = 0; p < 16 && enabled; p++)
    {
        Player& pl = players[p];
        if (!pl.cutscene) continue;
        if ((sharedWork >> 24) == 0x02)
        {
            const u32 status = rd32(&nds->MainRAM[(sharedWork + 4) & nds->MainRAMMask]);
            if (frame > pl.cutsceneStart + 30 && !(status & (1u << p))) pl.cutscene = false;
        }
        else if (frame > pl.cutsceneEnd)
            pl.cutscene = false;
        cut |= pl.cutscene;
    }
    cutsceneNow = cut;
    if (!enabled || nds->ConsoleType != 0 || !synth.isActive()) return;

    // 출력 버퍼가 모자란 만큼만 만든다. 오디오 장치가 소리를 가져가는 속도(실제 시간)에 맞춰지므로
    // 원본의 배속과 무관하게 1배속이 된다
    synth.setMemory(nds->MainRAM, nds->MainRAMMask);
    const int rate = inst->audioFreq;
    const size_t target = (size_t)rate * 60 / 1000;  // 약 60ms 분량
    size_t have;
    {
        std::lock_guard lock(ringMutex);
        have = ringCount;
    }
    if (have >= target) return;
    const size_t need = std::min(target - have, kRingFrames - have);
    renderBuf.resize(need * 2);
    synth.render(renderBuf.data(), (int)need, rate);

    std::lock_guard lock(ringMutex);
    size_t write = (ringRead + ringCount) % kRingFrames;
    for (size_t i = 0; i < need && ringCount < kRingFrames; i++)
    {
        ring[write * 2] = renderBuf[i * 2];
        ring[write * 2 + 1] = renderBuf[i * 2 + 1];
        write = (write + 1) % kRingFrames;
        ringCount++;
    }
}

void BgmReplacer::mixInto(s16* stream, int frames, int volume, int freshFrames)
{
    if (frames <= 0) return;
    // 새로 채워지지 않은 뒷부분에는 지난번 출력(원본 + 그때 더한 BGM)이 남아 있다.
    // 그때 더한 BGM 만 도로 빼서, 원본 멜론DS 가 내던 값 그대로로 되돌린다
    if (lastAdded.size() == (size_t)frames * 2)
    {
        for (int i = std::max(freshFrames, 0) * 2; i < frames * 2; i++)
            stream[i] = (s16)std::clamp((s32)stream[i] - lastAdded[i], -32768, 32767);
    }
    lastAdded.assign((size_t)frames * 2, 0);

    std::lock_guard lock(ringMutex);
    for (int i = 0; i < frames && ringCount > 0; i++)
    {
        for (int ch = 0; ch < 2; ch++)
        {
            const s32 before = stream[i * 2 + ch];
            const s32 v = std::clamp(before + (((s32)ring[ringRead * 2 + ch] * volume) >> 8), -32768, 32767);
            stream[i * 2 + ch] = (s16)v;
            lastAdded[i * 2 + ch] = (s16)(v - before);
        }
        ringRead = (ringRead + 1) % kRingFrames;
        ringCount--;
    }
}
