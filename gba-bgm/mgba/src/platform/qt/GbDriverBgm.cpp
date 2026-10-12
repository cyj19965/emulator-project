/* gba-bgm: GB/GBC 게임용 BGM 1배속 유지 */
#include "GbDriverBgm.h"

#include <algorithm>
#include <cstring>

#include <mgba/core/blip_buf.h>
#include <mgba/core/config.h>
#include <mgba/core/core.h>
#include <mgba/gb/core.h>
#include <mgba/internal/gb/gb.h>
#include <mgba/internal/sm83/sm83.h>
#include <mgba-util/vfs.h>

using namespace QGBA;

namespace {

// ---------------------------------------------------------------- ROM 패턴 검색

constexpr int ANY = -1;

bool matchAt(const std::vector<uint8_t>& rom, size_t off, const std::vector<int>& pat) {
	if (off + pat.size() > rom.size()) {
		return false;
	}
	for (size_t i = 0; i < pat.size(); ++i) {
		if (pat[i] != ANY && rom[off + i] != pat[i]) {
			return false;
		}
	}
	return true;
}

// bank:addr (addr 0x4000~0x7FFF 는 bank 영역) -> ROM 오프셋
bool romOffset(const std::vector<uint8_t>& rom, int bank, uint16_t addr, size_t& off) {
	if (addr < 0x4000) {
		off = addr;
	} else if (addr < 0x8000) {
		off = static_cast<size_t>(bank) * 0x4000 + (addr - 0x4000);
	} else {
		return false;
	}
	return off < rom.size();
}

uint16_t word(const std::vector<uint8_t>& rom, size_t off) {
	return static_cast<uint16_t>(rom[off] | (rom[off + 1] << 8));
}

// 포켓몬 금·은·크리스탈(게임프리크) 사운드 엔진. 코드는 pret/pokegold, pret/pokecrystal 기준.
//   PlayMusic / UpdateSound: bank 0 의 래퍼 (hROMBank 저장 -> 오디오 bank 전환 -> 엔진 호출 -> 복원)
//   _UpdateSound 첫머리: ld a,[wMusicPlaying] / and a / ret z / xor a / ld [wCurChannel],a /
//                        ld [wSoundOutput],a / ld bc,wChannel1 / ld hl,CHANNEL_FLAGS1 / add hl,bc / bit 0,[hl]
//   _PlayMusic 첫머리: call MusicOff / ld hl,wMusicID / ld [hl],e / inc hl / ld [hl],d
bool detectGameFreakGsc(const std::vector<uint8_t>& rom, GbDriverProfile& out) {
	const std::vector<int> updateWrapper = {
		0xE5, 0xD5, 0xC5, 0xF5, 0xF0, ANY, 0xF5, 0x3E, ANY, 0xE0, ANY, 0xEA, 0x00, 0x20,
		0xCD, ANY, ANY, 0xF1, 0xE0, ANY, 0xEA, 0x00, 0x20, 0xF1, 0xC1, 0xD1, 0xE1, 0xC9,
	};
	const std::vector<int> playWrapper = {
		0xE5, 0xD5, 0xC5, 0xF5, 0xF0, ANY, 0xF5, 0x3E, ANY, 0xE0, ANY, 0xEA, 0x00, 0x20,
		0x7B, 0xA7, 0x28, 0x05, 0xCD, ANY, ANY, 0x18, 0x03, 0xCD, ANY, ANY, 0xF1,
	};
	const std::vector<int> updateBody = {
		0xFA, ANY, ANY, 0xA7, 0xC8, 0xAF, 0xEA, ANY, ANY, 0xEA, ANY, ANY,
		0x01, ANY, ANY, 0x21, ANY, 0x00, 0x09, 0xCB, 0x46,
	};
	const std::vector<int> nextChannel = {0x21, ANY, 0x00, 0x09, 0x4D, 0x44, 0xFA};
	const std::vector<int> playBody = {0xCD, ANY, ANY, 0x21, ANY, ANY, 0x73, 0x23, 0x72};

	bool haveUpdate = false;
	bool havePlay = false;
	const size_t bank0End = std::min<size_t>(rom.size(), 0x4000);
	for (size_t a = 0; a + 32 < bank0End && !(haveUpdate && havePlay); ++a) {
		if (!haveUpdate && matchAt(rom, a, updateWrapper)) {
			size_t off;
			if (romOffset(rom, rom[a + 8], word(rom, a + 15), off) && matchAt(rom, off, updateBody)) {
				out.updateSound = static_cast<uint16_t>(a);
				out.channelBase = word(rom, off + 13);
				out.flagsOffset = rom[off + 16];
				// 다음 채널로 넘어가는 ld hl,CHANNEL_STRUCT_LENGTH 에서 구조체 크기를 읽는다
				for (size_t s = off; s < off + 512 && s + 8 < rom.size(); ++s) {
					if (matchAt(rom, s, nextChannel)) {
						out.channelStride = rom[s + 1];
						break;
					}
				}
				haveUpdate = out.channelStride != 0;
			}
		}
		if (!havePlay && matchAt(rom, a, playWrapper)) {
			size_t off;
			if (romOffset(rom, rom[a + 8], word(rom, a + 19), off) && matchAt(rom, off, playBody)) {
				out.playMusic = static_cast<uint16_t>(a);
				out.musicId = word(rom, off + 4);
				havePlay = true;
			}
		}
	}
	if (!haveUpdate || !havePlay) {
		return false;
	}
	out.engine = "GameFreak (Pokemon Gold/Silver/Crystal)";
	out.onMask = 0x01;  // SOUND_CHANNEL_ON
	out.sfxChannelStart = 4;
	return true;
}

int16_t clamp16(int v) {
	return static_cast<int16_t>(std::max(-32768, std::min(32767, v)));
}

}

bool QGBA::detectGbDriver(const std::vector<uint8_t>& rom, GbDriverProfile& out) {
	// 새 엔진 프로필은 여기에 추가한다
	if (detectGameFreakGsc(rom, out)) {
		return true;
	}
	return false;
}

// ---------------------------------------------------------------- 숨겨진 GB 코어

GbShadowPlayer::GbShadowPlayer(const std::vector<uint8_t>& rom, const GbDriverProfile& profile, bool captureStream)
	: m_profile(profile)
	, m_rom(rom)
{
	std::memset(&m_capture, 0, sizeof(m_capture));
	mCore* core = GBCoreCreate();
	if (!core) {
		return;
	}
	if (!core->init(core)) {
		core->deinit(core);
		return;
	}
	mCoreInitConfig(core, nullptr);
	m_video.assign(256 * 256, 0);
	core->setVideoBuffer(core, reinterpret_cast<color_t*>(m_video.data()), 256);
	core->setAudioBufferSize(core, 4096);
	struct VFile* vf = VFileFromConstMemory(m_rom.data(), m_rom.size());
	if (!vf || !core->loadROM(core, vf)) {
		mCoreConfigDeinit(&core->config);
		core->deinit(core);
		return;
	}
	core->reset(core);
	if (captureStream) {
		m_capture.self = this;
		m_capture.base.postAudioFrame = &GbShadowPlayer::_capture;
		core->setAVStream(core, &m_capture.base);
	}

	// 게임 코드 대신 실행할 작은 프로그램:
	//   di / ld sp,stackTop / ld de,0 / call PlayMusic(초기화)
	//   loop: halt / nop / xor a / ldh [rIF],a
	//         ld a,[cmd] / and a / jr z,.update / xor a / ld [cmd],a
	//         ld a,[idLo] / ld e,a / ld a,[idHi] / ld d,a / call PlayMusic
	//   .update: call UpdateSound / ld a,[nr50] / ldh [rNR50],a / ld a,[nr51] / ldh [rNR51],a / jr loop
	const uint16_t base = m_profile.driverRam;
	const uint16_t cmd = base + 0x40;
	const uint16_t pm = m_profile.playMusic;
	const uint16_t us = m_profile.updateSound;
	auto lo = [](uint16_t v) { return static_cast<uint8_t>(v & 0xFF); };
	auto hi = [](uint16_t v) { return static_cast<uint8_t>(v >> 8); };
	const uint8_t program[] = {
		0xF3,
		0x31, lo(m_profile.stackTop), hi(m_profile.stackTop),
		0x11, 0x00, 0x00,
		0xCD, lo(pm), hi(pm),
		0x76,                    // +10 loop
		0x00,
		0xAF,
		0xE0, 0x0F,
		0xFA, lo(cmd), hi(cmd),
		0xA7,
		0x28, 0x0F,              // +19 -> +36
		0xAF,
		0xEA, lo(cmd), hi(cmd),
		0xFA, lo(cmd + 1), hi(cmd + 1),
		0x5F,
		0xFA, lo(cmd + 2), hi(cmd + 2),
		0x57,
		0xCD, lo(pm), hi(pm),
		0xCD, lo(us), hi(us),    // +36
		0xFA, lo(cmd + 3), hi(cmd + 3),
		0xE0, 0x24,
		0xFA, lo(cmd + 4), hi(cmd + 4),
		0xE0, 0x25,
		0x18, 0xD7,              // +49 -> +10
	};
	for (size_t i = 0; i < sizeof(program); ++i) {
		core->busWrite8(core, base + i, program[i]);
	}
	core->busWrite8(core, cmd, 0);
	core->busWrite8(core, cmd + 3, 0x77);
	core->busWrite8(core, cmd + 4, 0xFF);
	core->busWrite8(core, 0xFFFF, 0x01);  // IE: VBlank 만

	struct GB* gb = static_cast<struct GB*>(core->board);
	struct SM83Core* cpu = gb->cpu;
	gb->memory.ime = false;
	cpu->pc = base;
	cpu->sp = m_profile.stackTop;
	cpu->halted = false;
	cpu->executionState = SM83_CORE_FETCH;
	cpu->memory.setActiveRegion(cpu, cpu->pc);
	m_core = core;
}

GbShadowPlayer::~GbShadowPlayer() {
	if (m_core) {
		m_core->setAVStream(m_core, nullptr);
		mCoreConfigDeinit(&m_core->config);
		m_core->deinit(m_core);
	}
}

void GbShadowPlayer::requestSong(uint16_t id) {
	m_pending = id;
}

void GbShadowPlayer::setMutedChannels(uint8_t mask) {
	m_mutedMask = mask;
}

void GbShadowPlayer::setOutputRegs(uint8_t nr50, uint8_t nr51) {
	m_outRegs = (nr50 << 8) | nr51;
}

void GbShadowPlayer::applyRequest() {
	const uint16_t cmd = m_profile.driverRam + 0x40;
	int regs = m_outRegs;
	if (regs >= 0) {
		m_core->busWrite8(m_core, cmd + 3, static_cast<uint8_t>(regs >> 8));
		m_core->busWrite8(m_core, cmd + 4, static_cast<uint8_t>(regs & 0xFF));
	}
	int id = m_pending.exchange(-1);
	if (id >= 0) {
		m_core->busWrite8(m_core, cmd + 1, static_cast<uint8_t>(id & 0xFF));
		m_core->busWrite8(m_core, cmd + 2, static_cast<uint8_t>(id >> 8));
		m_core->busWrite8(m_core, cmd, 1);
	}
	struct GB* gb = static_cast<struct GB*>(m_core->board);
	uint8_t mask = m_mutedMask;
	for (int i = 0; i < 4; ++i) {
		gb->audio.forceDisableCh[i] = (mask >> i) & 1;
	}
}

void GbShadowPlayer::runFrame() {
	if (!m_core) {
		return;
	}
	applyRequest();
	m_core->runFrame(m_core);
}

void GbShadowPlayer::mixInto(int16_t* stereo, int frames, int sampleRate, int volume) {
	if (!m_core || frames <= 0) {
		return;
	}
	blip_t* left = m_core->getAudioChannel(m_core, 0);
	blip_t* right = m_core->getAudioChannel(m_core, 1);
	if (sampleRate != m_rate) {
		blip_set_rates(left, m_core->frequency(m_core), sampleRate);
		blip_set_rates(right, m_core->frequency(m_core), sampleRate);
		m_rate = sampleRate;
	}
	int guard = 0;
	while (blip_samples_avail(left) < frames && guard++ < 16) {
		runFrame();
	}
	int avail = std::min(frames, blip_samples_avail(left));
	std::vector<int16_t> buf(static_cast<size_t>(avail) * 2);
	blip_read_samples(left, buf.data(), avail, true);
	blip_read_samples(right, buf.data() + 1, avail, true);
	for (int i = 0; i < avail * 2; ++i) {
		stereo[i] = clamp16(stereo[i] + buf[i] * volume / 0x100);
	}
}

void GbShadowPlayer::_capture(mAVStream* stream, int16_t left, int16_t right) {
	GbShadowPlayer* self = reinterpret_cast<Capture*>(stream)->self;
	self->m_streamFifo.push_back(left);
	self->m_streamFifo.push_back(right);
	// 원본보다 몇 프레임 이상 앞서면 오래된 것부터 버린다
	while (self->m_streamFifo.size() > 2 * 32768) {
		self->m_streamFifo.pop_front();
	}
}

bool GbShadowPlayer::popStreamSample(int16_t& left, int16_t& right) {
	if (m_streamFifo.size() < 2) {
		return false;
	}
	left = m_streamFifo.front();
	m_streamFifo.pop_front();
	right = m_streamFifo.front();
	m_streamFifo.pop_front();
	return true;
}

// ---------------------------------------------------------------- 백엔드

GbDriverBgm::GbDriverBgm(const std::vector<uint8_t>& rom, const GbDriverProfile& profile)
	: m_rom(rom)
	, m_profile(profile)
{
}

GbDriverBgm::~GbDriverBgm() {
	std::lock_guard<std::mutex> lock(m_liveMutex);
	m_live.reset();
}

void GbDriverBgm::onReset() {
	m_resetLive = true;
	m_rec.reset();
	m_lastSong = -1;
}

void GbDriverBgm::releaseMute(mCore* core) {
	struct GB* gb = static_cast<struct GB*>(core->board);
	for (int i = 0; i < 4; ++i) {
		gb->audio.forceDisableCh[i] = false;
	}
	m_muting = false;
}

void GbDriverBgm::onFrame(mCore* core, bool enabled) {
	if (!enabled) {
		if (m_muting) {
			releaseMute(core);
		}
		return;
	}
	const GbDriverProfile& p = m_profile;
	// wMusicID 는 효과음·울음소리 재생 때도 덮어쓰이므로, 켜져 있는 음악 채널(1~4)의 곡 번호를 쓴다
	int song = 0;
	for (int i = 0; i < p.sfxChannelStart; ++i) {
		uint16_t ch = p.channelBase + i * p.channelStride;
		if (core->busRead8(core, ch + p.flagsOffset) & p.onMask) {
			song = core->busRead8(core, ch) | (core->busRead8(core, ch + 1) << 8);
			break;
		}
	}
	uint8_t sfx = 0;
	for (int i = 0; i < 4; ++i) {
		uint16_t flags = p.channelBase + (p.sfxChannelStart + i) * p.channelStride + p.flagsOffset;
		if (core->busRead8(core, flags) & p.onMask) {
			sfx |= 1 << i;
		}
	}
	// 효과음이 쓰지 않는 하드웨어 채널은 음악이므로 원본에서 끈다
	struct GB* gb = static_cast<struct GB*>(core->board);
	for (int i = 0; i < 4; ++i) {
		gb->audio.forceDisableCh[i] = !((sfx >> i) & 1);
	}
	m_muting = true;
	m_liveSong = song;
	m_sfxMask = sfx;
	const int regs = (core->busRead8(core, 0xFF24) << 8) | core->busRead8(core, 0xFF25);
	m_outRegs = regs;

	if (m_recording) {
		if (!m_rec) {
			m_rec.reset(new GbShadowPlayer(m_rom, m_profile, true));
			m_lastSong = -1;
		}
		if (song != m_lastSong) {
			m_rec->requestSong(static_cast<uint16_t>(song));
			m_lastSong = song;
		}
		m_rec->setMutedChannels(sfx);
		m_rec->setOutputRegs(static_cast<uint8_t>(regs >> 8), static_cast<uint8_t>(regs & 0xFF));
		m_rec->runFrame();
	}
}

void GbDriverBgm::mixRealtime(int16_t* stereo, int frames, int sampleRate, int volume) {
	std::lock_guard<std::mutex> lock(m_liveMutex);
	if (m_resetLive.exchange(false)) {
		m_live.reset();
	}
	int song = m_liveSong;
	if (song < 0) {
		return;  // 아직 게임 상태를 읽지 못함
	}
	if (!m_live) {
		m_live.reset(new GbShadowPlayer(m_rom, m_profile, false));
		m_liveRequested = -1;
	}
	if (!m_live->ok()) {
		return;
	}
	if (song != m_liveRequested) {
		m_live->requestSong(static_cast<uint16_t>(song));
		m_liveRequested = song;
	}
	m_live->setMutedChannels(m_sfxMask);
	int regs = m_outRegs;
	if (regs >= 0) {
		m_live->setOutputRegs(static_cast<uint8_t>(regs >> 8), static_cast<uint8_t>(regs & 0xFF));
	}
	m_live->mixInto(stereo, frames, sampleRate, volume);
}

void GbDriverBgm::startRecording() {
	m_recording = true;
}

void GbDriverBgm::stopRecording() {
	m_recording = false;
	m_rec.reset();
}

void GbDriverBgm::recSample(int16_t& left, int16_t& right) {
	if (!m_rec) {
		return;
	}
	int16_t l, r;
	if (m_rec->popStreamSample(l, r)) {
		left = clamp16(left + l);
		right = clamp16(right + r);
	}
}
