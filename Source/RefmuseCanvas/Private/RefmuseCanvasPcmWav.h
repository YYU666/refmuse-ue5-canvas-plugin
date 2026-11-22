// Copyright (c) 2026 Refmuse contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace RefmuseCanvas
{
struct FPcmWav
{
    std::uint32_t SampleRate = 0;
    std::uint16_t Channels = 0;
    std::size_t DataOffset = 0;
    std::uint32_t DataBytes = 0;

    std::uint32_t FrameCount() const { return DataBytes / (Channels * 2u); }
};

// Validate the untrusted RIFF structure before sending only PCM samples to UE.
// This accepts PCM format tag 1 with one or two channels and signed 16-bit samples.
inline bool ParsePcmWav(const std::uint8_t* Bytes, std::size_t Size, FPcmWav& Out)
{
    Out = {};
    const auto Read16 = [](const std::uint8_t* P) {
        return std::uint32_t(P[0]) | (std::uint32_t(P[1]) << 8);
    };
    const auto Read32 = [Read16](const std::uint8_t* P) {
        return Read16(P) | (Read16(P + 2) << 16);
    };
    if (!Bytes || Size < 44 || std::memcmp(Bytes, "RIFF", 4) != 0
        || std::memcmp(Bytes + 8, "WAVE", 4) != 0 || Read32(Bytes + 4) != Size - 8)
    {
        return false;
    }

    FPcmWav Wave;
    std::uint32_t BlockAlign = 0;
    std::size_t Offset = 12;
    while (Offset < Size)
    {
        if (Size - Offset < 8) return false;
        const std::uint8_t* Chunk = Bytes + Offset;
        const std::uint32_t ChunkBytes = Read32(Chunk + 4);
        const std::size_t Start = Offset + 8;
        const std::uint32_t Pad = ChunkBytes & 1u;
        if (ChunkBytes > Size - Start || Pad > Size - Start - ChunkBytes) return false;
        const std::uint8_t* Data = Bytes + Start;

        if (std::memcmp(Chunk, "fmt ", 4) == 0)
        {
            if (Wave.Channels || (ChunkBytes != 16 && ChunkBytes != 18)
                || Read16(Data) != 1 || Read16(Data + 14) != 16
                || (ChunkBytes == 18 && Read16(Data + 16) != 0))
            {
                return false;
            }
            const std::uint32_t Channels = Read16(Data + 2);
            const std::uint32_t Rate = Read32(Data + 4);
            BlockAlign = Read16(Data + 12);
            if (Channels < 1 || Channels > 2 || Rate < 8000 || Rate > 192000
                || BlockAlign != Channels * 2 || Read32(Data + 8) != Rate * BlockAlign)
            {
                return false;
            }
            Wave.Channels = static_cast<std::uint16_t>(Channels);
            Wave.SampleRate = Rate;
        }
        else if (std::memcmp(Chunk, "data", 4) == 0)
        {
            if (Wave.DataBytes || !Wave.Channels || ChunkBytes == 0 || ChunkBytes % BlockAlign != 0) return false;
            Wave.DataOffset = Start;
            Wave.DataBytes = ChunkBytes;
        }
        Offset = Start + ChunkBytes + Pad;
    }
    if (!Wave.DataBytes) return false;
    Out = Wave;
    return true;
}
}
