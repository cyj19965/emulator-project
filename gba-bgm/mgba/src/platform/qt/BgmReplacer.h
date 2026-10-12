/* gba-bgm: 배속과 관계없이 BGM 을 1배속으로 재생한다.
 * 게임의 MP2K BGM 플레이어를 무음 처리하고, 같은 곡을 내장 재생기로 실시간 출력과 녹화 스트림에 섞는다. */
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include <mgba/core/interface.h>

#include "GbDriverBgm.h"
#include "bgm_mirror.h"
#include "mp2k_engine.h"
#include "rom.h"

struct mCore;

namespace QGBA {

class BgmReplacer {
public:
	explicit BgmReplacer(mCore* core);
	~BgmReplacer();

	bool usable() const { return !m_cands.empty() || m_gb; }

	void setEnabled(bool enabled);
	void setOutputVolume(int volume, bool mute);  // mGBA 설정의 volume(0~0x100), mute

	// 에뮬레이션 스레드에서 프레임마다 호출
	void onFrame(mCore* core);
	void onReset(mCore* core);  // 패치 적용 등으로 ROM 이 바뀌었으면 다시 읽는다

	// 녹화 스트림을 감싸 BGM 을 섞는다 (에뮬레이션 시간 기준)
	mAVStream* wrapStream(mAVStream* target, mCore* core);
	void unwrapStream();

	// 실시간 오디오 출력 스레드에서 호출
	static void mixActive(int16_t* stereo, int frames, int sampleRate);

private:
	struct Proxy {
		mAVStream base;
		mAVStream* target;
		BgmReplacer* self;
	};

	void mixRealtime(int16_t* stereo, int frames, int sampleRate);
	void loadRom(mCore* core);
	int16_t recSample(int16_t in, bool right);

	static void _proxyDimensions(mAVStream*, unsigned width, unsigned height);
	static void _proxyRate(mAVStream*, unsigned rate);
	static void _proxyVideo(mAVStream*, const color_t* buffer, size_t stride);
	static void _proxyAudioFrame(mAVStream*, int16_t left, int16_t right);
	static void _proxyAudioBuffer(mAVStream*, struct blip_t* left, struct blip_t* right);

	Rom m_rom;
	std::vector<MPlayTableCandidate> m_cands;
	std::unique_ptr<GbDriverBgm> m_gb;  // GB/GBC 게임일 때

	BgmMultiMirror m_liveMulti;  // 실시간 출력용
	int m_liveRate = 0;
	std::vector<float> m_liveBuf;

	BgmMultiMirror m_recMulti;   // 녹화(에뮬레이션 시간)용
	std::atomic<bool> m_recording{false};
	int m_recRate = 0;
	std::vector<float> m_recBuf;
	size_t m_recPos = 0;
	Proxy m_proxy;

	std::atomic<bool> m_enabled{true};
	std::atomic<int> m_volume{0x100};
	std::atomic<bool> m_mute{false};
	uint64_t m_frame = 0;
};

}
