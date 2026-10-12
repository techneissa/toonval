// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace toonval::test {

// Builds a minimal SoundFont 2 in memory: one preset (bank 0, program 0) whose instrument plays a
// looped sine at full scale, with a 0.3 s release. Lets the synth tests run without the piano
// samples, which live outside git.
inline std::vector<std::uint8_t> tiny_sf2() {
    std::vector<std::uint8_t> out;
    const auto u16 = [](std::vector<std::uint8_t>& v, unsigned x) {
        v.push_back(static_cast<std::uint8_t>(x & 0xFF));
        v.push_back(static_cast<std::uint8_t>((x >> 8) & 0xFF));
    };
    const auto u32 = [&](std::vector<std::uint8_t>& v, std::uint32_t x) {
        u16(v, x & 0xFFFF);
        u16(v, x >> 16);
    };
    const auto name20 = [](std::vector<std::uint8_t>& v, const char* text) {
        char buf[20] = {};
        std::strncpy(buf, text, sizeof buf - 1);
        v.insert(v.end(), buf, buf + 20);
    };
    const auto chunk = [&](const char* id, const std::vector<std::uint8_t>& body) {
        std::vector<std::uint8_t> c(id, id + 4);
        u32(c, static_cast<std::uint32_t>(body.size()));
        c.insert(c.end(), body.begin(), body.end());
        if (body.size() % 2 != 0) {
            c.push_back(0);
        }
        return c;
    };
    const auto list = [&](const char* kind, const std::vector<std::vector<std::uint8_t>>& parts) {
        std::vector<std::uint8_t> body(kind, kind + 4);
        for (const auto& p : parts) {
            body.insert(body.end(), p.begin(), p.end());
        }
        return chunk("LIST", body);
    };

    // 4096 points of a 375 Hz sine at 48 kHz: exactly 32 periods, so the loop is seamless.
    constexpr std::uint32_t kPoints = 4096;
    std::vector<std::uint8_t> smpl;
    for (std::uint32_t i = 0; i < kPoints + 46; ++i) {
        const double s = i < kPoints ? std::sin(2.0 * 3.14159265358979 * i / 128.0) : 0.0;
        u16(smpl, static_cast<std::uint16_t>(static_cast<std::int16_t>(s * 32000.0)));
    }

    std::vector<std::uint8_t> ifil;
    u16(ifil, 2);
    u16(ifil, 1);
    std::vector<std::uint8_t> phdr, pbag, pmod(10, 0), pgen, inst, ibag, imod(10, 0), igen, shdr;
    name20(phdr, "sine");
    u16(phdr, 0); // preset
    u16(phdr, 0); // bank
    u16(phdr, 0); // bag index
    u32(phdr, 0);
    u32(phdr, 0);
    u32(phdr, 0);
    name20(phdr, "EOP");
    u16(phdr, 0);
    u16(phdr, 0);
    u16(phdr, 1);
    u32(phdr, 0);
    u32(phdr, 0);
    u32(phdr, 0);
    u16(pbag, 0);
    u16(pbag, 0);
    u16(pbag, 1);
    u16(pbag, 0);
    u16(pgen, 41); // instrument 0
    u16(pgen, 0);
    u16(pgen, 0); // terminal
    u16(pgen, 0);
    name20(inst, "sine");
    u16(inst, 0);
    name20(inst, "EOI");
    u16(inst, 1);
    u16(ibag, 0);
    u16(ibag, 0);
    u16(ibag, 3);
    u16(ibag, 0);
    u16(igen, 38); // releaseVolEnv: 1200 * log2(0.3 s) = -2084 timecents
    u16(igen, static_cast<std::uint16_t>(static_cast<std::int16_t>(-2084)));
    u16(igen, 54); // sampleModes: loop continuously
    u16(igen, 1);
    u16(igen, 53); // sampleID 0
    u16(igen, 0);
    u16(igen, 0); // terminal
    u16(igen, 0);
    name20(shdr, "sine");
    u32(shdr, 0);
    u32(shdr, kPoints);
    u32(shdr, 0);
    u32(shdr, kPoints);
    u32(shdr, 48000);
    shdr.push_back(66); // original key, close to 375 Hz
    shdr.push_back(0);
    u16(shdr, 0);
    u16(shdr, 1); // mono
    name20(shdr, "EOS");
    shdr.insert(shdr.end(), 26, 0);

    const auto info = list("INFO", {chunk("ifil", ifil)});
    const auto sdta = list("sdta", {chunk("smpl", smpl)});
    const auto pdta = list("pdta", {chunk("phdr", phdr), chunk("pbag", pbag), chunk("pmod", pmod),
                                    chunk("pgen", pgen), chunk("inst", inst), chunk("ibag", ibag),
                                    chunk("imod", imod), chunk("igen", igen), chunk("shdr", shdr)});
    std::vector<std::uint8_t> body{'s', 'f', 'b', 'k'};
    for (const auto* part : {&info, &sdta, &pdta}) {
        body.insert(body.end(), part->begin(), part->end());
    }
    return chunk("RIFF", body);
}

} // namespace toonval::test
