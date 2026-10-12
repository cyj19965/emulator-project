/* gba-bgm: 배속과 관계없이 BGM 을 1배속으로 재생한다. */
#include "BgmReplacer.h"

#include <algorithm>
#include <cstring>

#include <mgba/core/core.h>
#ifdef M_CORE_GBA
#include <mgba/internal/gba/gba.h>
#endif
#ifdef M_CORE_GB
#include <mgba/internal/gb/gb.h>
#endif
#ifdef BUILD_SDL
#include "sdl-audio.h"
#endif

using namespace QGBA;

namespace {

std::mutex s_activeMutex;
BgmReplacer* s_active = nullptr;

// 재생기 출력 1.0 = GBA 믹서 512 단위. mGBA 는 (단위 * volume * 3) >> 4 로 int16 을 만든다.
float engineToInt16(int volume) {
	return 512.0f * volume * 3.0f / 16.0f;
}

int16_t clamp16(float v) {
	if (v > 32767.0f) return 32767;
	if (v < -32768.0f) return -32768;
	return static_cast<int16_t>(v);
}

struct CoreBus : GbaBus {
	explicit CoreBus(mCore* c) : core(c) {}
	mCore* core;
	uint8_t r8(uint32_t a) override { return core->busRead8(core, a); }
	uint16_t r16(uint32_t a) override { return core->busRead16(core, a); }
	uint32_t r32(uint32_t a) override { return core->busRead32(core, a); }
	void w8(uint32_t a, uint8_t v) override { core->busWrite8(core, a, v); }
};

}

BgmReplacer::BgmReplacer(mCore* core)
	: m_liveMulti(&m_rom)
	, m_recMulti(&m_rom)
{
	std::memset(&m_proxy, 0, sizeof(m_proxy));
	loadRom(core);
	std::lock_guard<std::mutex> lock(s_activeMutex);
	s_active = this;
#ifdef BUILD_SDL
	mSDLAudioPostMix = &BgmReplacer::mixActive;
#endif
}

BgmReplacer::~BgmReplacer() {
	std::lock_guard<std::mutex> lock(s_activeMutex);
	if (s_active == this) {
		s_active = nullptr;
	}
}

void BgmReplacer::setEnabled(bool enabled) {
	m_enabled = enabled;
	if (!enabled) {
		m_liveMulti.stopAll();
		m_recMulti.stopAll();
	}
}

void BgmReplacer::setOutputVolume(int volume, bool mute) {
	m_volume = volume;
	m_mute = mute;
}

void BgmReplacer::loadRom(mCore* core) {
#ifdef M_CORE_GB
	if (core->platform(core) == mPLATFORM_GB) {
		struct GB* gb = static_cast<struct GB*>(core->board);
		if (!gb->memory.rom || !gb->memory.romSize) {
			return;
		}
		const uint8_t* data = gb->memory.rom;
		if (m_rom.data.size() == gb->memory.romSize && std::memcmp(m_rom.data.data(), data, gb->memory.romSize) == 0) {
			return;
		}
		m_rom.loadFromMemory(data, gb->memory.romSize);
		m_cands.clear();
		m_gb.reset();
		GbDriverProfile profile;
		if (detectGbDriver(m_rom.data, profile)) {
			m_gb.reset(new GbDriverBgm(m_rom.data, profile));
		}
		return;
	}
#endif
#ifdef M_CORE_GBA
	if (core->platform(core) != mPLATFORM_GBA) {
		return;
	}
	GBA* gba = static_cast<GBA*>(core->board);
	if (!gba->memory.rom || !gba->memory.romSize) {
		return;
	}
	const uint8_t* data = reinterpret_cast<const uint8_t*>(gba->memory.rom);
	if (m_rom.data.size() == gba->memory.romSize && std::memcmp(m_rom.data.data(), data, gba->memory.romSize) == 0) {
		return;
	}
	m_rom.loadFromMemory(data, gba->memory.romSize);
	m_cands = findMPlayTables(m_rom);
#else
	(void) core;
#endif
}

void BgmReplacer::onReset(mCore* core) {
	// 실시간 출력 스레드가 ROM 을 읽는 중일 수 있으므로 같은 잠금 안에서 처리한다
	std::lock_guard<std::mutex> lock(s_activeMutex);
	m_liveMulti.reset();
	m_recMulti.reset();
	loadRom(core);
	if (m_gb) {
		m_gb->onReset();
	}
}

void BgmReplacer::onFrame(mCore* core) {
	if (m_gb) {
		m_gb->onFrame(core, m_enabled);
		return;
	}
	if (!m_enabled || m_cands.empty()) {
		return;
	}
	CoreBus bus(core);
	++m_frame;
	if (!m_liveMulti.located() || !m_liveMulti.stillValid(bus)) {
		if (m_liveMulti.located()) {
			// 게임이 MP2K 를 내려놓음 (모드 전환 등): 대체 재생도 멈춘다
			m_liveMulti.reset();
			m_recMulti.reset();
		}
		if (m_frame % 30 != 0) {
			return;
		}
		if (!m_liveMulti.locate(bus, m_cands)) {
			return;
		}
		m_recMulti.locate(bus, m_cands);
	}
	if (m_frame % 60 == 1) {
		SoundInfoSnapshot si = readSoundInfo(bus);
		if (si.valid) {
			m_liveMulti.setRuntimeParams(si);
			m_recMulti.setRuntimeParams(si);
		}
	}
	m_liveMulti.onFrame(bus, true);
	if (m_recording) {
		m_recMulti.onFrame(bus, false);
	}
}

void BgmReplacer::mixActive(int16_t* stereo, int frames, int sampleRate) {
	std::lock_guard<std::mutex> lock(s_activeMutex);
	if (s_active) {
		s_active->mixRealtime(stereo, frames, sampleRate);
	}
}

void BgmReplacer::mixRealtime(int16_t* stereo, int frames, int sampleRate) {
	if (!m_enabled || frames <= 0 || sampleRate <= 0) {
		return;
	}
	if (m_gb) {
		if (!m_mute) {
			m_gb->mixRealtime(stereo, frames, sampleRate, m_volume);
		}
		return;
	}
	if (sampleRate != m_liveRate) {
		EngineConfig cfg;
		cfg.outRate = sampleRate;
		m_liveMulti.configure(cfg);
		m_liveRate = sampleRate;
	}
	if (m_liveBuf.size() < static_cast<size_t>(frames) * 2) {
		m_liveBuf.resize(static_cast<size_t>(frames) * 2);
	}
	// 재생기는 실제 출력된 분량만큼만 진행한다 (일시정지 중에는 frames 가 0)
	m_liveMulti.render(m_liveBuf.data(), frames);
	if (m_mute) {
		return;
	}
	const float scale = engineToInt16(m_volume);
	for (int i = 0; i < frames * 2; ++i) {
		stereo[i] = clamp16(stereo[i] + m_liveBuf[i] * scale);
	}
}

// ---------------------------------------------------------------- 녹화 스트림

mAVStream* BgmReplacer::wrapStream(mAVStream* target, mCore* core) {
	if (!target || !usable()) {
		return target;
	}
	m_proxy.target = target;
	m_proxy.self = this;
	m_proxy.base.videoDimensionsChanged = target->videoDimensionsChanged ? &BgmReplacer::_proxyDimensions : nullptr;
	m_proxy.base.audioRateChanged = &BgmReplacer::_proxyRate;
	m_proxy.base.postVideoFrame = target->postVideoFrame ? &BgmReplacer::_proxyVideo : nullptr;
	m_proxy.base.postAudioFrame = target->postAudioFrame ? &BgmReplacer::_proxyAudioFrame : nullptr;
	m_proxy.base.postAudioBuffer = target->postAudioBuffer ? &BgmReplacer::_proxyAudioBuffer : nullptr;

	if (m_gb) {
		m_gb->startRecording();
		m_recording = true;
		return &m_proxy.base;
	}

	int rate = 32768;
#ifdef M_CORE_GBA
	if (core->platform(core) == mPLATFORM_GBA) {
		GBA* gba = static_cast<GBA*>(core->board);
		if (gba->audio.sampleInterval > 0) {
			rate = GBA_ARM7TDMI_FREQUENCY / gba->audio.sampleInterval;
		}
	}
#endif
	EngineConfig cfg;
	cfg.outRate = rate;
	m_recMulti.configure(cfg);
	m_recMulti.stopAll();
	m_recRate = rate;
	m_recBuf.assign(512, 0.0f);
	m_recPos = m_recBuf.size() / 2;  // 처음 호출 때 바로 채운다
	m_recording = true;
	return &m_proxy.base;
}

void BgmReplacer::unwrapStream() {
	m_recording = false;
	if (m_gb) {
		m_gb->stopRecording();
	}
	m_recMulti.stopAll();
	m_proxy.target = nullptr;
}

int16_t BgmReplacer::recSample(int16_t in, bool right) {
	if (!m_enabled) {
		return in;
	}
	const size_t frames = m_recBuf.size() / 2;
	if (!right && m_recPos >= frames) {
		m_recMulti.render(m_recBuf.data(), static_cast<int>(frames));
		m_recPos = 0;
	}
	float v = m_recBuf[m_recPos * 2 + (right ? 1 : 0)];
	if (right) {
		++m_recPos;
	}
	return clamp16(in + v * engineToInt16(0x100));
}

void BgmReplacer::_proxyDimensions(mAVStream* s, unsigned width, unsigned height) {
	Proxy* p = reinterpret_cast<Proxy*>(s);
	p->target->videoDimensionsChanged(p->target, width, height);
}

void BgmReplacer::_proxyRate(mAVStream* s, unsigned rate) {
	Proxy* p = reinterpret_cast<Proxy*>(s);
	BgmReplacer* self = p->self;
	if (!self->m_gb && rate && static_cast<int>(rate) != self->m_recRate) {
		EngineConfig cfg;
		cfg.outRate = static_cast<int>(rate);
		self->m_recMulti.configure(cfg);
		self->m_recRate = static_cast<int>(rate);
	}
	if (p->target->audioRateChanged) {
		p->target->audioRateChanged(p->target, rate);
	}
}

void BgmReplacer::_proxyVideo(mAVStream* s, const color_t* buffer, size_t stride) {
	Proxy* p = reinterpret_cast<Proxy*>(s);
	p->target->postVideoFrame(p->target, buffer, stride);
}

void BgmReplacer::_proxyAudioFrame(mAVStream* s, int16_t left, int16_t right) {
	Proxy* p = reinterpret_cast<Proxy*>(s);
	BgmReplacer* self = p->self;
	if (self->m_gb) {
		if (self->m_enabled) {
			self->m_gb->recSample(left, right);
		}
		p->target->postAudioFrame(p->target, left, right);
		return;
	}
	int16_t l = self->recSample(left, false);
	int16_t r = self->recSample(right, true);
	p->target->postAudioFrame(p->target, l, r);
}

void BgmReplacer::_proxyAudioBuffer(mAVStream* s, struct blip_t* left, struct blip_t* right) {
	Proxy* p = reinterpret_cast<Proxy*>(s);
	p->target->postAudioBuffer(p->target, left, right);
}
