// 숨겨진 코어 단독 진단: 곡을 재생시키며 CPU/엔진 상태를 출력
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <mgba/core/core.h>
#include <mgba/internal/gb/gb.h>
#include <mgba/internal/sm83/sm83.h>
#include "GbDriverBgm.h"
using namespace QGBA;
int main(int argc, char** argv) {
	FILE* f = std::fopen(argv[1], "rb"); std::fseek(f, 0, SEEK_END);
	std::vector<uint8_t> rom((size_t) std::ftell(f)); std::fseek(f, 0, SEEK_SET);
	std::fread(rom.data(), 1, rom.size(), f); std::fclose(f);
	int song = argc > 2 ? std::atoi(argv[2]) : 82;
	GbDriverProfile p; detectGbDriver(rom, p);
	GbShadowPlayer sh(rom, p, false);
	mCore* c = sh.core();
	struct GB* gb = (struct GB*) c->board;
	sh.requestSong((uint16_t) song);
	for (int fr = 0; fr <= 1500; ++fr) {
		sh.runFrame();
		if (fr % 50 == 0) {
			std::printf("fr %4d pc=%04X sp=%04X playing=%02X id=%02X ch1..4 flags=", fr, gb->cpu->pc, gb->cpu->sp,
			            c->busRead8(c, p.channelBase - 1), c->busRead8(c, p.musicId));
			for (int i = 0; i < 4; ++i) std::printf("%02X ", c->busRead8(c, p.channelBase + i * p.channelStride + p.flagsOffset));
			std::printf(" NR50=%02X NR51=%02X NR52=%02X\n", c->busRead8(c, 0xFF24), c->busRead8(c, 0xFF25), c->busRead8(c, 0xFF26));
		}
	}
}
