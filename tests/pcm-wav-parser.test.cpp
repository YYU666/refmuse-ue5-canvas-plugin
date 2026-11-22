// Copyright (c) 2026 Refmuse contributors. SPDX-License-Identifier: Apache-2.0
#include "../Source/RefmuseCanvas/Private/RefmuseCanvasPcmWav.h"

#include <cstdlib>
#include <iostream>
#include <vector>

namespace
{
using Bytes = std::vector<std::uint8_t>;
using RefmuseCanvas::FPcmWav;
using RefmuseCanvas::ParsePcmWav;

int Cases = 0;

void Expect(bool Passed, const char* Case)
{
    if (!Passed)
    {
        std::cerr << Case << " failed\n";
        std::exit(1);
    }
    ++Cases;
}

void Write32(Bytes& Buffer, std::size_t Offset, std::uint32_t Value)
{
    for (int Shift = 0; Shift <= 24; Shift += 8)
    {
        Buffer[Offset++] = static_cast<std::uint8_t>(Value >> Shift);
    }
}

void SetRiffSize(Bytes& Buffer)
{
    Write32(Buffer, 4, static_cast<std::uint32_t>(Buffer.size() - 8));
}

Bytes MakeWave(std::uint8_t Channels, std::uint32_t Rate)
{
    Bytes Buffer = {
        'R','I','F','F', 0,0,0,0, 'W','A','V','E', 'f','m','t',' ', 16,0,0,0,
        1,0, Channels,0, 0,0,0,0, 0,0,0,0, static_cast<std::uint8_t>(Channels * 2),0, 16,0,
        'd','a','t','a', 8,0,0,0, 0,0, 0xff,0x7f, 0,0x80, 0xff,0xff,
    };
    SetRiffSize(Buffer);
    Write32(Buffer, 24, Rate);
    Write32(Buffer, 28, Rate * Channels * 2u);
    return Buffer;
}

bool Parse(const Bytes& Buffer, FPcmWav& Info)
{
    return ParsePcmWav(Buffer.data(), Buffer.size(), Info);
}

void Reject(const Bytes& Buffer, const char* Case)
{
    FPcmWav Info{48000, 2, 44, 8};
    Expect(!Parse(Buffer, Info) && !Info.DataBytes, Case);
}
}

int main()
{
    const Bytes Mono = MakeWave(1, 16000);
    const Bytes Stereo = MakeWave(2, 48000);
    FPcmWav Info;
    Expect(Parse(Mono, Info) && Info.SampleRate == 16000 && Info.Channels == 1
        && Info.DataOffset == 44 && Info.FrameCount() == 4, "16-bit mono");
    Expect(Parse(Stereo, Info) && Info.SampleRate == 48000 && Info.Channels == 2
        && Info.DataBytes == 8 && Info.FrameCount() == 2, "16-bit stereo");

    Bytes WithAncillary = Stereo;
    WithAncillary.insert(WithAncillary.begin() + 36, {'J','U','N','K', 1,0,0,0, 7,0});
    SetRiffSize(WithAncillary);
    Expect(Parse(WithAncillary, Info) && Info.DataOffset == 54 && Info.FrameCount() == 2, "padded ancillary chunk");

    Bytes Format18 = Stereo;
    Format18.insert(Format18.begin() + 36, {0,0});
    Write32(Format18, 16, 18);
    SetRiffSize(Format18);
    Expect(Parse(Format18, Info) && Info.DataOffset == 46, "PCM with zero-length format extension");

    for (std::size_t Length = 0; Length < Stereo.size(); ++Length)
    {
        Expect(!ParsePcmWav(Stereo.data(), Length, Info), "truncated input");
    }

    Bytes Invalid = Stereo;
    Invalid[20] = 3;
    Reject(Invalid, "floating-point codec");
    Invalid[20] = 0xfe;
    Invalid[21] = 0xff;
    Reject(Invalid, "extensible WAV");
    Invalid = Stereo;
    Invalid[34] = 24;
    Reject(Invalid, "24-bit samples");
    Reject(MakeWave(3, 48000), "multichannel");
    Reject(MakeWave(1, 1000), "unsupported rate");
    Invalid = Stereo;
    Invalid[28] ^= 1;
    Reject(Invalid, "wrong byte rate");
    Invalid = Stereo;
    Invalid[32] = 2;
    Reject(Invalid, "wrong frame alignment");
    Invalid = Stereo;
    Write32(Invalid, 40, 6);
    Reject(Invalid, "partial stereo frame");
    Invalid = Stereo;
    Write32(Invalid, 40, 0xffffffffu);
    Reject(Invalid, "oversized RIFF chunk");
    Invalid = Stereo;
    Invalid.insert(Invalid.end(), Stereo.begin() + 36, Stereo.end());
    SetRiffSize(Invalid);
    Reject(Invalid, "duplicate data chunk");
    Invalid = Stereo;
    Invalid.insert(Invalid.end(), Stereo.begin() + 12, Stereo.begin() + 36);
    SetRiffSize(Invalid);
    Reject(Invalid, "duplicate format chunk");
    Invalid = Stereo;
    Invalid.insert(Invalid.end(), {'J','U','N','K', 1,0,0,0, 7});
    SetRiffSize(Invalid);
    Reject(Invalid, "missing RIFF pad");
    Invalid = Stereo;
    Invalid[40] = 0;
    Invalid.resize(44);
    SetRiffSize(Invalid);
    Reject(Invalid, "empty data chunk");
    Invalid = Stereo;
    Invalid[3] = 'X';
    Reject(Invalid, "big-endian container");

    std::cout << "PCM WAV parser: " << Cases << " cases passed\n";
}
