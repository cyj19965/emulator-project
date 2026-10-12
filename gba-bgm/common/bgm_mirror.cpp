#include "bgm_mirror.h"

#include <algorithm>

namespace {
constexpr uint32_t STATUS_PAUSE = 0x80000000u;
constexpr uint32_t STATUS_TRACK = 0x0000FFFFu;
constexpr uint32_t TRACK_SIZE = 0x50;
}  // namespace

bool isPlayerInfoValid(GbaBus& bus, uint32_t info) {
    uint32_t ident = bus.r32(info + 0x34);
    // MPlayMain 실행 중에는 ident 가 1 증가해 있다
    return ident == kMp2kIdent || ident == kMp2kIdent + 1;
}

uint32_t findBgmPlayerInfo(GbaBus& bus, const std::vector<MPlayTableCandidate>& cands) {
    for (const auto& c : cands) {
        const MPlayEntry& e = c.entries[0];
        if (!isPlayerInfoValid(bus, e.info)) continue;
        if (bus.r8(e.info + 8) != e.numTracks) continue;
        if (bus.r32(e.info + 0x2C) != e.tracks) continue;
        return e.info;
    }
    return 0;
}

BgmSnapshot readBgm(GbaBus& bus, uint32_t info) {
    BgmSnapshot s;
    s.header = bus.r32(info);
    s.status = bus.r32(info + 4);
    s.clock = bus.r32(info + 0x0C);
    s.fadeOI = bus.r16(info + 0x24);
    return s;
}

SoundInfoSnapshot readSoundInfo(GbaBus& bus) {
    SoundInfoSnapshot s;
    uint32_t p = bus.r32(kSoundInfoPtr);
    if (p < 0x02000000u || p >= 0x03008000u) return s;
    uint32_t ident = bus.r32(p);
    if (ident != kMp2kIdent && ident != kMp2kIdent + 1) return s;
    s.valid = true;
    s.reverb = bus.r8(p + 5);
    s.maxChans = bus.r8(p + 6);
    s.masterVolume = bus.r8(p + 7);
    s.dmaPeriod = bus.r8(p + 0x0B);
    s.samplesPerVBlank = (int) bus.r32(p + 0x10);
    return s;
}

void muteBgmTracks(GbaBus& bus, uint32_t info) {
    uint8_t n = bus.r8(info + 8);
    uint32_t tracks = bus.r32(info + 0x2C);
    if (n > 16) n = 16;
    for (uint8_t i = 0; i < n; ++i) {
        uint32_t t = tracks + i * TRACK_SIZE;
        uint8_t flags = bus.r8(t);
        if (!(flags & 0x80)) continue;
        bus.w8(t + 0x12, 0);  // vol
        bus.w8(t + 0x13, 0);  // volX
        bus.w8(t, (uint8_t) (flags | 0x03));  // MPT_FLG_VOLCHG
    }
}

bool isLoopingSong(const Rom& rom, uint32_t header) {
    const uint8_t n = rom.u8(header);
    if (n == 0 || n > 16 || !rom.valid(header, 8 + 4u * n)) return false;
    for (uint8_t t = 0; t < n; ++t) {
        uint32_t p = rom.u32(header + 8 + 4u * t);
        uint8_t running = 0;
        for (int steps = 0; steps < 20000 && rom.valid(p); ++steps) {
            const uint32_t at = p;
            uint8_t cmd = rom.u8(p);
            if (cmd < 0x80) {
                cmd = running;  // 러닝 스테이터스: 이 바이트는 인자
            } else {
                ++p;
                if (cmd >= 0xBD) running = cmd;
            }
            if (cmd >= 0xCF || cmd == 0xCE) {  // 음표 / EOT: 0x80 미만 인자 최대 3개
                for (int i = 0; i < 3 && rom.u8(p) < 0x80; ++i) ++p;
            } else if (cmd >= 0x80 && cmd <= 0xB0) {  // 대기
            } else if (cmd == 0xB1) {  // FINE
                break;
            } else if (cmd == 0xB2) {  // GOTO
                uint32_t target = rom.u32(p);
                if (target <= at) return true;
                break;
            } else if (cmd == 0xB3) {  // PATT
                p += 4;
            } else if (cmd == 0xB4) {  // PEND
            } else if (cmd == 0xB5) {  // REPT
                p += 5;
            } else if (cmd == 0xB9) {  // MEMACC
                uint8_t op = rom.u8(p);
                p += 3;
                if (op >= 6 && op <= 17) p += 4;
            } else if (cmd == 0xCC) {  // PORT
                p += 2;
            } else if (cmd == 0xCD) {  // XCMD
                uint8_t x = rom.u8(p++);
                if (x == 1 || x == 13) p += 4;
                else if (x == 12) p += 2;
                else if (x >= 2 && x <= 11) p += 1;
                else break;
            } else if (cmd >= 0xBA && cmd <= 0xC8) {  // 인자 1개
                p += 1;
            } else {
                break;  // 알 수 없는 명령 (원본도 FINE 처리)
            }
        }
    }
    return false;
}

bool BgmPlayerSelector::locate(GbaBus& bus, const std::vector<MPlayTableCandidate>& cands) {
    reset();
    for (const auto& c : cands) {
        const MPlayEntry& e0 = c.entries[0];
        if (!isPlayerInfoValid(bus, e0.info)) continue;
        if (bus.r8(e0.info + 8) != e0.numTracks) continue;
        if (bus.r32(e0.info + 0x2C) != e0.tracks) continue;
        int maxTracks = 0;
        for (const auto& e : c.entries) maxTracks = std::max<int>(maxTracks, e.numTracks);
        const int threshold = std::max(4, (maxTracks + 1) / 2);
        for (const auto& e : c.entries) {
            if (e.numTracks >= threshold && isPlayerInfoValid(bus, e.info)) m_players.push_back(e.info);
        }
        m_state.assign(m_players.size(), State());
        return !m_players.empty();
    }
    return false;
}

bool BgmPlayerSelector::stillValid(GbaBus& bus) const {
    for (uint32_t p : m_players) {
        if (!isPlayerInfoValid(bus, p)) return false;
    }
    return !m_players.empty();
}

void BgmPlayerSelector::reset() {
    m_players.clear();
    m_state.clear();
    m_active = 0;
    m_frame = 0;
    m_loopCache.clear();
}

uint32_t BgmPlayerSelector::update(GbaBus& bus, const Rom& rom, BgmSnapshot& snap, bool& changed) {
    ++m_frame;
    changed = false;
    uint32_t best = 0;
    uint64_t bestStart = 0;
    bool activePlaying = false;
    BgmSnapshot activeSnap, bestSnap;
    for (size_t i = 0; i < m_players.size(); ++i) {
        BgmSnapshot s = readBgm(bus, m_players[i]);
        State& st = m_state[i];
        if (s.header != st.header || s.clock < st.clock) st.startedAt = m_frame;  // 곡 시작/재시작
        st.header = s.header;
        st.clock = s.clock;
        const bool playing = s.header && !(s.status & 0x80000000u) && (s.status & 0xFFFFu);
        if (m_players[i] == m_active) {
            activePlaying = playing;
            activeSnap = s;
        }
        bool looping = false;
        if (playing) {
            auto it = m_loopCache.find(s.header);
            if (it == m_loopCache.end()) it = m_loopCache.emplace(s.header, isLoopingSong(rom, s.header)).first;
            looping = it->second;
        }
        if (playing && looping && st.startedAt >= bestStart) {
            best = m_players[i];
            bestStart = st.startedAt;
            bestSnap = s;
        }
    }
    // 재생 중인 플레이어 중 가장 최근에 시작한 쪽을 따른다. 아무것도 재생 중이 아니면 현재 플레이어 유지
    if (best && best != m_active) {
        uint64_t activeStart = 0;
        for (size_t i = 0; i < m_players.size(); ++i) {
            if (m_players[i] == m_active) activeStart = m_state[i].startedAt;
        }
        if (!activePlaying || bestStart > activeStart) {
            m_active = best;
            changed = true;
            snap = bestSnap;
            return m_active;
        }
    }
    if (!m_active && !m_players.empty()) {
        m_active = m_players[0];
        changed = true;
        activeSnap = readBgm(bus, m_active);
    }
    snap = activeSnap;
    return m_active;
}

void BgmMirror::reset() {
    have_ = false;
    last_ = BgmSnapshot();
}

void BgmMirror::update(const BgmSnapshot& s) {
    const bool paused = (s.status & STATUS_PAUSE) != 0;

    if (s.header == 0) {
        if (eng_.currentSong()) eng_.stop();
        last_ = s;
        have_ = true;
        return;
    }

    // 곡 변경 또는 같은 곡 재시작(MPlayStart 가 clock 을 0 으로 되돌림)
    bool restart = !have_ || s.header != last_.header || s.clock < last_.clock;
    if (restart) {
        if (!have_ && paused) {
            // 연결 시점에 이미 멈춰 있던 곡은 시작하지 않는다
        } else {
            eng_.start(s.header);
        }
    }

    if (paused) {
        bool explicitStop = (s.status & STATUS_TRACK) != 0;  // m4aMPlayStop / 임시 페이드아웃
        bool fadeEnded = have_ && last_.fadeOI != 0;          // 페이드아웃 완료
        // 트랙이 모두 끝난 자연 종료는 외부 재생기가 스스로 끝날 때까지 둔다
        if (explicitStop || fadeEnded) eng_.setPaused(true);
    } else if (eng_.paused()) {
        eng_.setPaused(false);  // MPlayContinue
    }

    last_ = s;
    have_ = true;
}

bool isMusicSong(const Rom& rom, uint32_t header) {
    const uint8_t n = rom.u8(header);
    if (n == 0 || n > 16) return false;
    return n >= 4 || isLoopingSong(rom, header);
}

bool BgmMultiMirror::locate(GbaBus& bus, const std::vector<MPlayTableCandidate>& cands) {
    std::lock_guard<std::mutex> lk(mtx_);
    players_.clear();
    for (const auto& c : cands) {
        const MPlayEntry& e0 = c.entries[0];
        if (!isPlayerInfoValid(bus, e0.info)) continue;
        if (bus.r8(e0.info + 8) != e0.numTracks) continue;
        if (bus.r32(e0.info + 0x2C) != e0.tracks) continue;
        int maxTracks = 0;
        for (const auto& e : c.entries) maxTracks = std::max<int>(maxTracks, e.numTracks);
        const int threshold = std::max(4, (maxTracks + 1) / 2);
        for (const auto& e : c.entries) {
            if (e.numTracks < threshold || !isPlayerInfoValid(bus, e.info)) continue;
            Player p;
            p.info = e.info;
            p.eng.reset(new Mp2kEngine(rom_));
            p.eng->configure(cfg_);
            p.mirror.reset(new BgmMirror(*p.eng));
            players_.push_back(std::move(p));
        }
        return !players_.empty();
    }
    return false;
}

bool BgmMultiMirror::stillValid(GbaBus& bus) const {
    for (const auto& p : players_) {
        if (!isPlayerInfoValid(bus, p.info)) return false;
    }
    return !players_.empty();
}

void BgmMultiMirror::reset() {
    std::lock_guard<std::mutex> lk(mtx_);
    players_.clear();
}

void BgmMultiMirror::stopAll() {
    for (auto& p : players_) {
        p.eng->stop();
        p.mirror->reset();
        p.header = 0xFFFFFFFF;
        p.music = false;
    }
}

void BgmMultiMirror::configure(const EngineConfig& cfg) {
    std::lock_guard<std::mutex> lk(mtx_);
    cfg_ = cfg;
    for (auto& p : players_) {
        uint32_t song = p.eng->currentSong();
        p.eng->configure(cfg);
        if (song && p.music) p.eng->start(song);
    }
}

void BgmMultiMirror::setRuntimeParams(const SoundInfoSnapshot& si) {
    for (auto& p : players_) {
        p.eng->setRuntimeParams(si.maxChans, si.masterVolume, si.reverb, si.samplesPerVBlank, si.dmaPeriod);
    }
}

void BgmMultiMirror::onFrame(GbaBus& bus, bool mute) {
    for (auto& p : players_) {
        BgmSnapshot s = readBgm(bus, p.info);
        if (s.header != p.header) {
            p.header = s.header;
            auto it = musicCache_.find(s.header);
            if (it == musicCache_.end()) it = musicCache_.emplace(s.header, s.header && isMusicSong(*rom_, s.header)).first;
            if (p.music && !it->second) {
                p.eng->stop();  // 음악 -> 효과음/빈 곡: 대체 재생을 멈춘다
                p.mirror->reset();
            }
            p.music = it->second;
        }
        if (!p.music) continue;
        if (mute) muteBgmTracks(bus, p.info);
        p.mirror->update(s);
    }
}

void BgmMultiMirror::render(float* out, int frames) {
    std::lock_guard<std::mutex> lk(mtx_);
    const size_t n = static_cast<size_t>(frames) * 2;
    std::fill(out, out + n, 0.0f);
    if (tmp_.size() < n) tmp_.resize(n);
    for (auto& p : players_) {
        if (!p.eng->currentSong()) continue;
        p.eng->render(tmp_.data(), frames);
        for (size_t i = 0; i < n; ++i) out[i] += tmp_[i];
    }
}
