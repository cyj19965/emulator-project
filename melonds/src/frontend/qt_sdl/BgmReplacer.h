// gba-bgm: 배속과 관계없이 BGM 을 1배속으로 유지한다 (자체 재생기 방식).
//
// 게임(ARM9)은 사운드 CPU(ARM7)에 "시퀀스 시작/정지/음량" 같은 명령을 보내고, 연주는 ARM7 의 드라이버가 한다.
// 1) 그 명령을 지켜보다가 음악 시퀀스가 시작되면, 같은 시퀀스를 자체 재생기(NitroSynth)로 실제 시간에 연주한다.
// 2) 원본에서는 음악 플레이어의 페이더 명령 값을 무음으로 바꿔, BGM 이 소리 없이 계속 연주되게 한다.
//    (게임이 곡의 끝이나 페이드를 기다리는 동작은 그대로 유지된다)
// 3) 그 뒤 게임이 음악 플레이어에 보내는 명령(음량, 정지, 일시정지 등)은 자체 재생기에도 적용한다.
#ifndef BGMREPLACER_H
#define BGMREPLACER_H

#include <array>
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "NitroSynth.h"
#include "types.h"

namespace melonDS
{
class NDS;
}
class EmuInstance;

class BgmReplacer
{
public:
    explicit BgmReplacer(EmuInstance* inst);
    ~BgmReplacer();

    // 끄면 원본 멜론DS 와 똑같이 동작한다. 바꾼 설정은 다음 곡부터 적용된다.
    void setEnabled(bool on);
    bool isEnabled() const { return enabled; }
    // 메뉴(화면 스레드)에서 바꿀 때: 다음 프레임에 에뮬레이션 스레드가 적용한다
    void requestEnabled(bool on) { wanted = on; }
    bool wantEnabled() const { return wanted; }

    // 연출(반복 없는 긴 곡이 나오는 장면) 중인지. 에뮬레이션 스레드 전용
    bool cutsceneActive() const { return cutsceneNow; }
    // "연출 중에도 배속": 꺼져 있으면 연출 동안에는 배속을 걸어도 1배속으로 돈다
    void setFastForwardCutscenes(bool on) { ffCutscenes = on; }
    bool fastForwardCutscenes() const { return ffCutscenes; }
    // "메뉴 효과음도 1배속": 선택음·결정음 같은 조작음을 자체 재생기로 1배속 연주한다 (다음 소리부터 적용)
    void setMenuSounds1x(bool on) { menuSounds = on; }
    bool menuSounds1x() const { return menuSounds; }

    // 에뮬레이션 스레드: 원본 코어가 프레임 하나를 돌린 뒤
    void afterMainFrame();
    // 에뮬레이션 스레드: 상태를 불러온 직후 (프레임 번호가 그대로여도 추적을 다시 하게 한다)
    void onStateLoaded() { forceReset = true; }
    // 오디오 스레드: 자체 재생기의 소리를 출력에 더한다
    // freshFrames: stream 에서 이번에 새로 채워진 앞부분의 길이 (그 뒤는 지난번 출력이 남아 있다)
    void mixInto(melonDS::s16* stream, int frames, int volume, int freshFrames);

private:
    struct SeqInfo
    {
        int group = -1;  // (사운드 아카이브, 플레이어 번호) 묶음
        int tracks = 0;
        bool loop = false;
        float seconds = 0;  // 반복 없는 곡의 길이 (대략)
        std::string name;
    };
    struct Command
    {
        melonDS::u32 id;
        melonDS::u32 arg[4];
    };
    struct Player
    {
        bool music = false;  // 음악 시퀀스를 자체 재생기로 연주 중인 플레이어
        bool muted = false;  // 원본 쪽 소리를 끄는 명령이 들어갔는지
        // 연출 곡(반복 없는 긴 곡, 대체하지 않는다)을 원본이 연주 중인 플레이어
        bool cutscene = false;
        melonDS::u32 cutsceneStart = 0, cutsceneEnd = 0;  // 시작한 프레임, 길이로 어림한 끝 프레임
        bool sfx = false;     // 메뉴 효과음: 자체 재생기로 1배속 연주 (음악과 달리 다른 곡을 미루거나 끊지 않는다)
        bool jingle = false;  // 팡파레(반복 없는 짧은 곡): 배속 중에도 끝까지 연주한다
        bool started = false;
        // 뒤처진 팡파레가 따라올 때까지 미뤄 둔 동작 (배속 중)
        bool holdStart = false, holdResume = false;
        int holdId = -1;       // 기다리는 팡파레 (자체 재생기 번호)
        double holdUntil = 0;  // 그 팡파레가 여기(초)까지 연주되면 푼다
        int synthId = -1;  // 자체 재생기의 플레이어 번호
        // 게임이 의도한 값 (원본에는 페이더만 무음 값으로 바꿔 넣는다)
        melonDS::u8 prio = 64, volume = 127;
        melonDS::s16 fader = 0;
        std::string name;
    };

    static void fifoHook(melonDS::NDS& nds, melonDS::u32 word);
    void onSoundFifo(melonDS::u32 word);
    void resetTracking();
    void indexCart();
    int classify(melonDS::u32 seqBase, std::string& name);
    int allocSynth();
    bool jingleActive(int id) const;
    double jingleGameSeconds(int id) const;
    double jingleSynthSeconds(int id) const;
    bool holdForJingle(Player& pl);
    void startMusic(int p);
    void detach(int p);
    void releaseHeld();
    bool findDriverPlayers();
    void recoverFromDriver();

    EmuInstance* inst;
    std::atomic<bool> enabled{true};
    std::atomic<bool> wanted{true};
    std::atomic<bool> ffCutscenes{false};
    std::atomic<bool> menuSounds{true};
    bool cutsceneNow = false;
    float lastSeconds = 0;  // classify 가 마지막으로 본 곡의 길이

    // 원본 코어 추적과 자체 재생기 (에뮬레이션 스레드 전용)
    melonDS::NDS* lastNds = nullptr;
    melonDS::u32 lastFrame = 0;
    melonDS::u32 cartChecksum = 0;
    bool cartIndexed = false;
    std::array<Player, 16> players{};
    std::unordered_map<melonDS::u32, SeqInfo> seqIndex;  // CRC32(SSEQ 파일) -> 정보
    std::vector<bool> groupIsMusic;
    NitroSynth synth;
    // 게임이 그 자리를 다른 소리에 다시 쓴 뒤에도 끝까지 연주하게 둔 팡파레 (자체 재생기 번호별)
    std::array<bool, 16> orphan{};
    // 게임이 그 자리를 다시 쓴 뒤에도 끝까지 울리게 둔 메뉴 효과음 (자체 재생기 번호별)
    std::array<bool, 16> sfxOrphan{};
    std::array<std::string, 16> sfxOrphanName{};
    // 팡파레를 시작한 시점 (자체 재생기 번호별): 게임 프레임 번호와 자체 재생기의 출력 샘플 수
    std::array<melonDS::u32, 16> jingleFrame{};
    std::array<melonDS::u64, 16> jingleSample{};
    std::vector<melonDS::s16> renderBuf;

    // 자체 재생기가 만든 소리 (에뮬레이션 스레드가 채우고 오디오 스레드가 꺼낸다)
    std::mutex ringMutex;
    std::vector<melonDS::s16> ring;  // 스테레오
    size_t ringRead = 0, ringCount = 0;  // 프레임 단위
    std::vector<melonDS::s16> lastAdded;  // 지난번에 출력에 더한 값 (오디오 스레드 전용)

    melonDS::u32 driverPlayers = 0;  // ARM7 드라이버의 플레이어 배열 주소 (찾았을 때)
    bool forceReset = false;
    melonDS::u32 sharedWork = 0;  // 드라이버가 상태를 써 주는 공유 영역의 주소
};

#endif // BGMREPLACER_H
