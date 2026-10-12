// 재생기 자체 검증용: MP2K 곡 하나를 담은 가짜 ROM 을 만들어 WAV 로 렌더링한다.
// 트랙0: DirectSound 사인파(1주기 64샘플 루프)로 A4(440Hz) 1박
// 트랙1: 1박 쉰 뒤 PSG 사각파(채널2, 듀티 50%)로 A5(880Hz) 1박
// 템포 150bpm -> 1박 = 0.4초
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../common/mp2k_engine.h"
#include "../common/rom.h"

static void put32(std::vector<uint8_t>& r, uint32_t off, uint32_t v) {
    for (int i = 0; i < 4; ++i) r[off + i] = (uint8_t) (v >> (i * 8));
}

int main(int argc, char** argv) {
    std::vector<uint8_t> r(0x1000, 0);
    std::memcpy(&r[0xA0], "TESTROM", 7);
    std::memcpy(&r[0xAC], "TEST", 4);

    // gMPlayTable (스캐너 확인용)
    const uint32_t mp[4][3] = {{0x03001000, 0x03002000, 10}, {0x03001040, 0x03002400, 3}, {0x03001080, 0x03002600, 3}, {0x030010C0, 0x03002800, 2}};
    for (int i = 0; i < 4; ++i) {
        put32(r, 0x200 + i * 12, mp[i][0]);
        put32(r, 0x204 + i * 12, mp[i][1]);
        r[0x208 + i * 12] = (uint8_t) mp[i][2];
    }

    // WaveData: 루프, 64샘플 사인 1주기
    const double f60 = 440.0 * 64 / std::pow(2.0, 9.0 / 12.0);
    put32(r, 0x300, 0x40000000);
    put32(r, 0x304, (uint32_t) (f60 * 1024));
    put32(r, 0x308, 0);
    put32(r, 0x30C, 64);
    for (int i = 0; i < 64; ++i) r[0x310 + i] = (uint8_t) (int8_t) std::lround(100 * std::sin(2 * M_PI * i / 64));

    // 음색 그룹
    const uint8_t v0[12] = {0x00, 60, 0, 0, 0x00, 0x03, 0x00, 0x08, 255, 0, 255, 200};
    const uint8_t v1[12] = {0x02, 60, 0, 0, 0x02, 0x00, 0x00, 0x00, 0, 0, 15, 0};
    std::memcpy(&r[0x400], v0, 12);
    std::memcpy(&r[0x40C], v1, 12);

    // 곡 헤더
    r[0x500] = 2;
    put32(r, 0x504, 0x08000400);
    put32(r, 0x508, 0x08000600);
    put32(r, 0x50C, 0x08000700);

    const uint8_t t0[] = {0xBB, 75, 0xBD, 0, 0xBE, 100, 0xE7, 69, 127, 0x98, 0x98, 0x98, 0xB1};
    const uint8_t t1[] = {0xBD, 1, 0xBE, 100, 0x98, 0xE7, 81, 127, 0x98, 0x98, 0xB1};
    std::memcpy(&r[0x600], t0, sizeof(t0));
    std::memcpy(&r[0x700], t1, sizeof(t1));

    if (argc > 2) {
        FILE* rf = std::fopen(argv[2], "wb");
        std::fwrite(r.data(), 1, r.size(), rf);
        std::fclose(rf);
    }

    Rom rom;
    rom.loadFromMemory(r.data(), r.size());

    auto cands = findMPlayTables(rom);
    std::printf("mplay table candidates: %zu\n", cands.size());
    for (auto& c : cands) std::printf("  %08X entries=%zu first info=%08X\n", c.romAddr, c.entries.size(), c.entries[0].info);

    Mp2kEngine eng(&rom);
    EngineConfig cfg;
    eng.configure(cfg);
    if (!eng.start(0x08000500)) {
        std::printf("start failed\n");
        return 1;
    }
    const int frames = 48000 * 2;
    std::vector<float> buf((size_t) frames * 2);
    eng.render(buf.data(), frames);

    // 빈 곡(트랙 0개)으로 바꾸면 즉시 무음이어야 한다
    {
        r[0x800] = 0;  // trackCount 0
        Rom rom2;
        rom2.loadFromMemory(r.data(), r.size());
        Mp2kEngine e2(&rom2);
        e2.configure(cfg);
        e2.start(0x08000500);
        std::vector<float> a(48000 * 2 * 2 / 10);
        e2.render(a.data(), 4800);  // 0.1초 재생
        e2.start(0x08000800);       // 빈 곡
        std::vector<float> b(48000 * 2);
        e2.render(b.data(), 48000 / 2 * 1);
        double before = 0, after = 0;
        for (size_t i = 0; i < 4800 * 2; ++i) before += a[i] * a[i];
        for (size_t i = 4800; i < 48000; ++i) after += b[i] * b[i];
        std::printf("empty-song test: energy before %.3f, after %.6f -> %s\n", before, after, after < 1e-6 ? "SILENT (ok)" : "STILL PLAYING");
    }

    const char* path = argc > 1 ? argv[1] : "synth_test.wav";
    FILE* f = std::fopen(path, "wb");
    uint32_t dataBytes = frames * 4;
    auto w32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto w16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f); w32(36 + dataBytes); std::fwrite("WAVEfmt ", 1, 8, f);
    w32(16); w16(1); w16(2); w32(48000); w32(48000 * 4); w16(4); w16(16);
    std::fwrite("data", 1, 4, f); w32(dataBytes);
    for (float s : buf) w16((uint16_t) (int16_t) std::lround(s * 32767));
    std::fclose(f);
    std::printf("wrote %s\n", path);
    return 0;
}
