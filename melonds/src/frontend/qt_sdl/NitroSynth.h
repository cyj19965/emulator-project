// gba-bgm: NDS 표준 사운드 드라이버의 시퀀스(SSEQ) 재생기.
//
// 게임이 메인 메모리에 올려 둔 시퀀스·뱅크(SBNK)·웨이브 아카이브(SWAR)를 그대로 읽어 연주한다.
// ARM7 드라이버가 하는 일(약 5.2ms 마다 템포 계산 -> 틱 진행 -> 채널 음량/음정/좌우 갱신)을 같은 순서로 하고,
// 하드웨어 채널 16개에 해당하는 믹서로 소리를 만든다. 실제 시간 기준으로 돌리므로 게임 배속과 무관하다.
#ifndef NITROSYNTH_H
#define NITROSYNTH_H

#include <array>
#include <vector>

#include "types.h"

class NitroSynth
{
public:
    NitroSynth();

    // 읽을 메모리(원본 코어의 메인 메모리). 주소는 0x02xxxxxx 를 mask 로 접는다
    void setMemory(const melonDS::u8* ram, melonDS::u32 mask);
    void reset();
    // 이 플레이어는 지금 시점의 메모리 사본으로 연주한다 (prepare 뒤에 부른다).
    // 배속 중에는 게임이 팡파레를 이미 끝냈다고 보고 그 데이터를 지우거나 덮어쓸 수 있기 때문이다
    void snapshotMemory(int player);
    // 준비한 곡을 소리 없이 ticks 틱만큼 앞으로 돌린다 (이미 진행 중이던 곡을 그 위치부터 이어 받을 때)
    void skipTicks(int player, melonDS::u32 ticks);

    // 게임이 드라이버에 보내는 명령에 대응
    bool prepare(int player, melonDS::u32 seqBase, melonDS::u32 seqOffset, melonDS::u32 bank);
    void start(int player);
    void stop(int player);
    void pause(int player, bool paused);
    void setPlayerPrio(int player, int prio);
    void setPlayerVolume(int player, int volume);
    void setPlayerFader(int player, int fader);
    void muteTracks(int player, melonDS::u32 trackMask, bool mute);

    melonDS::u32 playerTicks(int player) const { return players[player & 15].ticks; }
    bool playerActive(int player) const { return players[player & 15].active; }  // 곡이 아직 진행 중인지
    melonDS::u64 renderedFrames() const { return rendered; }  // 지금까지 만든 출력 샘플 수
    bool isActive() const;  // 연주 중인 플레이어나 소리 나는 채널이 있는지
    // 스테레오 s16 을 frames 개 만든다 (덮어쓴다)
    void render(melonDS::s16* out, int frames, int sampleRate);

private:
    struct Player;
    struct Track;
    struct Channel;

    struct Track
    {
        bool active = false;
        bool noteWait = true;
        bool mute = false;
        bool tie = false;
        bool porta = false;
        bool cmp = true;
        melonDS::u8 panRange = 127;
        melonDS::s8 pan = 0;
        melonDS::s8 pitchBend = 0;
        melonDS::u8 volume = 127;
        melonDS::u8 expression = 127;
        melonDS::u8 bendRange = 2;
        melonDS::u8 prio = 64;
        melonDS::u8 portaKey = 60;
        melonDS::u8 portaTime = 0;
        melonDS::s8 transpose = 0;
        melonDS::u8 attack = 0xFF, decay = 0xFF, sustain = 0xFF, release = 0xFF;
        melonDS::u16 prgNo = 0;
        melonDS::s16 sweepPitch = 0;
        melonDS::u8 modType = 0, modSpeed = 16, modDepth = 0, modRange = 1;
        melonDS::u16 modDelay = 0;
        melonDS::u32 pos = 0;  // 시퀀스 데이터 안의 위치 (주소)
        melonDS::s32 wait = 0;
        melonDS::u32 callStack[3] = {};
        melonDS::u8 loopCount[3] = {};
        int callDepth = 0;
        int lastChannel = -1;
    };

    struct Player
    {
        bool active = false;
        bool prepared = false;
        bool paused = false;
        melonDS::u8 volume = 127;
        melonDS::u8 prio = 64;
        melonDS::s16 extFader = 0;
        melonDS::u16 tempo = 120;
        melonDS::u16 tempoRatio = 256;
        melonDS::u16 tempoCounter = 240;
        melonDS::u32 ticks = 0;  // 지금까지 진행한 틱 수
        melonDS::u32 seqBase = 0;
        melonDS::u32 bank = 0;
        std::array<Track, 16> tracks{};
        std::array<melonDS::s16, 16> vars{};
    };

    struct Channel
    {
        bool active = false;
        int player = -1, track = -1;
        int prio = 0;
        // 음원
        int type = 0;  // 0 PCM8, 1 PCM16, 2 ADPCM, 3 PSG, 4 노이즈
        const melonDS::u8* data = nullptr;
        melonDS::u32 total = 0, loopStart = 0;
        bool loops = false;
        double baseRate = 0;  // 원음 높이에서의 초당 샘플 수
        melonDS::s32 ipos = -1;
        double frac = 0;
        melonDS::s32 cur = 0;
        melonDS::s32 adpcmVal = 0, adpcmIdx = 0, loopVal = 0, loopIdx = 0;
        int duty = 0;
        melonDS::u16 noise = 0x7FFF;
        // 음표
        melonDS::u8 key = 60, origKey = 60, velocity = 127;
        melonDS::s32 length = 0;  // 남은 틱 (0 이하면 길이 없음: 타이)
        int initPan = 0;
        // 엔벨로프
        int envStatus = 0;  // 0 어택, 1 디케이, 2 서스테인, 3 릴리스
        melonDS::s32 envValue = 0;
        melonDS::s32 attackCoef = 0, decayCoef = 0, releaseCoef = 0;
        melonDS::u8 sustain = 127;
        // 스윕(포르타멘토)
        melonDS::s32 sweepPitch = 0, sweepLength = 0, sweepCounter = 0;
        bool autoSweep = true;
        // LFO
        melonDS::u8 lfoType = 0, lfoSpeed = 16, lfoDepth = 0, lfoRange = 1;
        melonDS::u16 lfoDelay = 0, lfoDelayCounter = 0, lfoCounter = 0;
        // 트랙·플레이어에서 온 값 (트랙이 닫힌 뒤에는 마지막 값을 유지)
        melonDS::s32 userDecay = 0, userPitch = 0, userPan = 0;
        // 믹서로 넘기는 값 (드라이버 프레임마다 갱신)
        float volLeft = 0, volRight = 0;
        double rate = 0;
    };

    const melonDS::u8* mem(melonDS::u32 addr, melonDS::u32 len) const;
    melonDS::u8 rd8(melonDS::u32 addr) const;

    void driverFrame();
    void playerTick(int p);
    bool trackStep(int p, int t);  // false 면 트랙이 끝남
    void noteOn(int p, int t, int key, int velocity, melonDS::s32 length);
    bool readInstrument(melonDS::u32 bank, int prg, int key, int& type, melonDS::u8 param[10]) const;
    bool startWave(Channel& ch, melonDS::u32 bank, int waveNo, int warcNo);
    int allocChannel(int prio);
    void releaseTrackChannels(int p, int t, int release);
    void closeTrack(int p, int t);
    void updateChannel(Channel& ch);
    void advance(Channel& ch);
    static void setAttack(Channel& ch, int attack);
    static melonDS::s32 decayCoef(int value);
    melonDS::u16 random();

    const melonDS::u8* ram = nullptr;      // 지금 읽는 메모리 (플레이어에 따라 사본으로 바뀐다)
    const melonDS::u8* mainRam = nullptr;  // 게임의 메인 메모리
    std::array<std::vector<melonDS::u8>, 16> snapshot;
    std::array<bool, 16> useSnapshot{};
    melonDS::u32 ramMask = 0;
    std::array<Player, 16> players{};
    std::array<Channel, 16> channels{};
    std::array<melonDS::s16, 16> globalVars{};
    melonDS::u32 rng = 0x12345678;
    melonDS::u64 rendered = 0;
    bool silent = false;  // skipTicks 중: 음표를 내지 않는다
    double frameAccum = 0;  // 다음 드라이버 프레임까지 남은 출력 샘플 수
};

#endif // NITROSYNTH_H
