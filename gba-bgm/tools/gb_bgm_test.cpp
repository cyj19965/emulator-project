// GB BGM 백엔드 검증: 실제 ROM 을 화면 없이 실행해 세 가지 소리를 WAV 로 저장한다.
//   ref.wav   : 아무것도 건드리지 않은 원본 소리
//   main.wav  : 음악 채널을 무음 처리한 원본 코어 소리 (효과음만 남아야 함)
//   bgm.wav   : 숨겨진 코어가 만든 1배속 BGM
//   ff_bgm.wav: 4배속으로 게임을 돌리는 동안 같은 실제 시간에 만든 BGM
// 사용법: gb_bgm_test <rom> <출력폴더> [초]
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <mgba/core/blip_buf.h>
#include <mgba/core/config.h>
#include <mgba/core/core.h>
#include <mgba/gb/core.h>
#include <mgba/internal/gb/gb.h>
#include <mgba-util/vfs.h>

#include "GbDriverBgm.h"

using namespace QGBA;

static const int RATE = 48000;

struct Wav {
	std::vector<int16_t> s;
	void save(const std::string& path) {
		FILE* f = std::fopen(path.c_str(), "wb");
		uint32_t bytes = (uint32_t) s.size() * 2;
		auto w32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
		auto w16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
		std::fwrite("RIFF", 1, 4, f); w32(36 + bytes); std::fwrite("WAVEfmt ", 1, 8, f);
		w32(16); w16(1); w16(2); w32(RATE); w32(RATE * 4); w16(4); w16(16);
		std::fwrite("data", 1, 4, f); w32(bytes);
		std::fwrite(s.data(), 2, s.size(), f);
		std::fclose(f);
	}
};

static mCore* makeCore(std::vector<uint8_t>& rom, std::vector<uint32_t>& video) {
	mCore* c = GBCoreCreate();
	c->init(c);
	mCoreInitConfig(c, nullptr);
	video.assign(256 * 256, 0);
	c->setVideoBuffer(c, reinterpret_cast<color_t*>(video.data()), 256);
	c->setAudioBufferSize(c, 8192);
	c->loadROM(c, VFileFromConstMemory(rom.data(), rom.size()));
	c->reset(c);
	blip_set_rates(c->getAudioChannel(c, 0), c->frequency(c), RATE);
	blip_set_rates(c->getAudioChannel(c, 1), c->frequency(c), RATE);
	return c;
}

static int drain(mCore* c, Wav* w) {
	blip_t* l = c->getAudioChannel(c, 0);
	blip_t* r = c->getAudioChannel(c, 1);
	int n = blip_samples_avail(l);
	std::vector<int16_t> buf((size_t) n * 2);
	blip_read_samples(l, buf.data(), n, true);
	blip_read_samples(r, buf.data() + 1, n, true);
	if (w) w->s.insert(w->s.end(), buf.begin(), buf.end());
	return n;
}

static double rms(const std::vector<int16_t>& s, size_t from, size_t to) {
	double a = 0;
	to = std::min(to, s.size());
	if (to <= from) return 0;
	for (size_t i = from; i < to; ++i) a += (double) s[i] * s[i];
	return std::sqrt(a / (to - from));
}

int main(int argc, char** argv) {
	if (argc < 3) {
		std::printf("usage: gb_bgm_test <rom> <outdir> [seconds]\n");
		return 1;
	}
	std::string out = argv[2];
	int seconds = argc > 3 ? std::atoi(argv[3]) : 40;
	FILE* f = std::fopen(argv[1], "rb");
	std::fseek(f, 0, SEEK_END);
	std::vector<uint8_t> rom((size_t) std::ftell(f));
	std::fseek(f, 0, SEEK_SET);
	std::fread(rom.data(), 1, rom.size(), f);
	std::fclose(f);

	GbDriverProfile p;
	if (!detectGbDriver(rom, p)) {
		std::printf("engine not detected\n");
		return 2;
	}
	std::printf("engine=%s\nPlayMusic=%04X UpdateSound=%04X wMusicID=%04X wChannel1=%04X stride=%02X flagsOffset=%d\n",
	            p.engine.c_str(), p.playMusic, p.updateSound, p.musicId, p.channelBase, p.channelStride, p.flagsOffset);

	std::vector<uint32_t> v1, v2;
	mCore* ref = makeCore(rom, v1);
	mCore* main = makeCore(rom, v2);
	GbDriverBgm bgm(rom, p);
	Wav refW, mainW, bgmW;

	const int frames = (int) (seconds * 59.7275);
	int lastSong = -1;
	for (int fr = 0; fr < frames; ++fr) {
		ref->runFrame(ref);
		main->runFrame(main);
		bgm.onFrame(main, true);
		drain(ref, &refW);
		int n = drain(main, &mainW);
		std::vector<int16_t> b((size_t) n * 2, 0);
		bgm.mixRealtime(b.data(), n, RATE, 0x100);
		bgmW.s.insert(bgmW.s.end(), b.begin(), b.end());
		int song = main->busRead8(main, p.musicId) | (main->busRead8(main, p.musicId + 1) << 8);
		if (song != lastSong) {
			std::printf("frame %5d (%.1fs): music id %d\n", fr, fr / 59.7275, song);
			lastSong = song;
		}
		int mask = 0;
		for (int i = 0; i < 8; ++i) {
			if (main->busRead8(main, p.channelBase + i * p.channelStride + p.flagsOffset) & p.onMask) mask |= 1 << i;
		}
		static int lastMask = -1;
		if (mask != lastMask) {
			std::printf("frame %5d (%.1fs): channel on mask %02X\n", fr, fr / 59.7275, mask);
			lastMask = mask;
		}
	}
	refW.save(out + "/ref.wav");
	mainW.save(out + "/main.wav");
	bgmW.save(out + "/bgm.wav");

	// 1초 단위 음량 비교
	std::printf("\nsec   ref_rms  main_rms  bgm_rms\n");
	for (int s = 0; s < seconds; ++s) {
		size_t a = (size_t) s * RATE * 2, b = a + RATE * 2;
		std::printf("%3d  %8.0f  %8.0f  %8.0f\n", s, rms(refW.s, a, b), rms(mainW.s, a, b), rms(bgmW.s, a, b));
	}

	// 4배속: 게임은 실제 시간 1프레임마다 4프레임 진행, BGM 은 실제 시간만큼만 생성
	mCore* ff = makeCore(rom, v1);
	GbDriverBgm bgmFF(rom, p);
	Wav ffW;
	lastSong = -1;
	const int outFrames = frames / 4;
	const int perFrame = RATE * 1000 / 59727;
	for (int fr = 0; fr < outFrames; ++fr) {
		for (int k = 0; k < 4; ++k) {
			ff->runFrame(ff);
			bgmFF.onFrame(ff, true);
			drain(ff, nullptr);
		}
		std::vector<int16_t> b((size_t) perFrame * 2, 0);
		bgmFF.mixRealtime(b.data(), perFrame, RATE, 0x100);
		ffW.s.insert(ffW.s.end(), b.begin(), b.end());
		int song = ff->busRead8(ff, p.musicId) | (ff->busRead8(ff, p.musicId + 1) << 8);
		if (song != lastSong) {
			std::printf("FF real %.1fs (game %.1fs): music id %d\n", fr / 59.7275, fr * 4 / 59.7275, song);
			lastSong = song;
		}
	}
	ffW.save(out + "/ff_bgm.wav");
	std::printf("ff_bgm.wav: %.1fs real time\n", ffW.s.size() / 2.0 / RATE);
	return 0;
}
