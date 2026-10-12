// GBA BGM(MP2K) 검증: 실제 ROM 을 화면 없이 실행해 소리를 WAV 로 저장한다.
//   ref.wav : 원본 그대로 / main.wav : BGM 트랙 무음 처리 / bgm.wav : 내장 MP2K 재생기 출력
// 사용법: gba_bgm_test <rom> <출력폴더> [초] [시작 버튼 누르는 간격(초), 0 이면 입력 없음]
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <mgba/core/blip_buf.h>
#include <mgba/core/config.h>
#include <mgba/core/core.h>
#include <mgba/gba/core.h>
#include <mgba-util/vfs.h>

#include "bgm_mirror.h"
#include "mp2k_engine.h"
#include "rom.h"

static const int RATE = 48000;

struct Bus : GbaBus {
	mCore* c;
	explicit Bus(mCore* core) : c(core) {}
	uint8_t r8(uint32_t a) override { return c->busRead8(c, a); }
	uint16_t r16(uint32_t a) override { return c->busRead16(c, a); }
	uint32_t r32(uint32_t a) override { return c->busRead32(c, a); }
	void w8(uint32_t a, uint8_t v) override { c->busWrite8(c, a, v); }
};

static void saveWav(const std::string& path, const std::vector<int16_t>& s) {
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

static mCore* makeCore(std::vector<uint8_t>& rom, std::vector<uint32_t>& video) {
	mCore* c = GBACoreCreate();
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

static int drain(mCore* c, std::vector<int16_t>* w) {
	blip_t* l = c->getAudioChannel(c, 0);
	blip_t* r = c->getAudioChannel(c, 1);
	int n = blip_samples_avail(l);
	std::vector<int16_t> buf((size_t) n * 2);
	blip_read_samples(l, buf.data(), n, true);
	blip_read_samples(r, buf.data() + 1, n, true);
	if (w) w->insert(w->end(), buf.begin(), buf.end());
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
	if (argc < 3) return 1;
	std::string out = argv[2];
	int seconds = argc > 3 ? std::atoi(argv[3]) : 60;
	double pressEvery = argc > 4 ? std::atof(argv[4]) : 0;
	FILE* f = std::fopen(argv[1], "rb");
	std::fseek(f, 0, SEEK_END);
	std::vector<uint8_t> romData((size_t) std::ftell(f));
	std::fseek(f, 0, SEEK_SET);
	std::fread(romData.data(), 1, romData.size(), f);
	std::fclose(f);

	Rom rom;
	rom.loadFromMemory(romData.data(), romData.size());
	auto cands = findMPlayTables(rom);
	std::printf("candidates: %zu\n", cands.size());
	for (auto& c : cands) std::printf("  table %08X entries=%zu entry0 info=%08X tracks=%08X n=%d\n", c.romAddr, c.entries.size(), c.entries[0].info, c.entries[0].tracks, c.entries[0].numTracks);

	std::vector<uint32_t> v1, v2;
	mCore* ref = makeCore(romData, v1);
	mCore* main = makeCore(romData, v2);
	Bus bus(main);
	Mp2kEngine eng(&rom);
	EngineConfig cfg;
	cfg.outRate = RATE;
	eng.configure(cfg);
	BgmMirror mirror(eng);
	BgmMultiMirror multi(&rom);
	{ EngineConfig mc; mc.outRate = RATE; multi.configure(mc); }
	std::vector<int16_t> refW, mainW, bgmW;
	std::vector<float> fb;

	const int frames = (int) (seconds * 59.7275);
	uint32_t lastHeader = 0xFFFFFFFF;
	for (int fr = 0; fr < frames; ++fr) {
		// 일정 간격으로 START/A 를 눌러 타이틀을 넘긴다
		uint32_t keys = 0;
		if (pressEvery > 0) {
			int period = (int) (pressEvery * 59.7275);
			int ph = fr % period;
			if (ph < 6) keys = (fr / period) % 2 ? 1 /*A*/ : 8 /*START*/;
		}
		ref->setKeys(ref, keys);
		main->setKeys(main, keys);
		ref->runFrame(ref);
		main->runFrame(main);
		if (!multi.located() || !multi.stillValid(bus)) {
			if (multi.located()) { std::printf("frame %d (%.1fs): MP2K lost, stop\n", fr, fr / 59.7275); multi.reset(); }
			if (fr % 30 == 0 && multi.locate(bus, cands)) std::printf("frame %d (%.1fs): located\n", fr, fr / 59.7275);
		}
		if (multi.located()) {
			if (fr % 60 == 1) { SoundInfoSnapshot si = readSoundInfo(bus); if (si.valid) multi.setRuntimeParams(si); }
			multi.onFrame(bus, true);
		}
		// 모든 후보 테이블의 플레이어별 곡 변화를 기록
		{
			static std::vector<std::vector<uint32_t>> last;
			if (last.size() != cands.size()) {
				last.resize(cands.size());
				for (size_t t = 0; t < cands.size(); ++t) last[t].assign(cands[t].entries.size(), 0xFFFFFFFF);
			}
			for (size_t t = 0; t < cands.size(); ++t) {
				for (size_t e = 0; e < cands[t].entries.size(); ++e) {
					uint32_t inf = cands[t].entries[e].info;
					if (!isPlayerInfoValid(bus, inf)) continue;
					uint32_t h = bus.r32(inf);
					if (h != last[t][e]) {
						if (0) std::printf("frame %d (%.1fs): table %zu player %zu (n=%d) header %08X status %08X\n", fr, fr / 59.7275, t, e,
						            cands[t].entries[e].numTracks, h, bus.r32(inf + 4));
						last[t][e] = h;
					}
				}
			}
		}
		if (fr % 600 == 300) {
			uint32_t sip = bus.r32(0x03007FF0);
			std::printf("frame %d: [03007FF0]=%08X ident=%08X\n", fr, sip,
			            (sip >= 0x02000000 && sip < 0x03008000) ? bus.r32(sip) : 0);
		}
		drain(ref, &refW);
		int n = drain(main, &mainW);
		fb.assign((size_t) n * 2, 0.0f);
		multi.render(fb.data(), n);
		for (float x : fb) bgmW.push_back((int16_t) std::lround(std::max(-1.0f, std::min(1.0f, x)) * 24576));
	}
	saveWav(out + "/ref.wav", refW);
	saveWav(out + "/main.wav", mainW);
	saveWav(out + "/bgm.wav", bgmW);
	std::printf("\nsec   ref_rms  main_rms  bgm_rms\n");
	for (int s = 0; s < seconds; ++s) {
		size_t a = (size_t) s * RATE * 2, b = a + RATE * 2;
		std::printf("%3d  %8.0f  %8.0f  %8.0f\n", s, rms(refW, a, b), rms(mainW, a, b), rms(bgmW, a, b));
	}
	return 0;
}
