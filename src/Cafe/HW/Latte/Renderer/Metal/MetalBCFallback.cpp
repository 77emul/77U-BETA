#include "Cafe/HW/Latte/Renderer/Metal/MetalBCFallback.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

namespace
{
uint16_t Read16(const uint8_t* data)
{
    return uint16_t(data[0]) | (uint16_t(data[1]) << 8);
}

uint32_t Read32(const uint8_t* data)
{
    return uint32_t(data[0]) | (uint32_t(data[1]) << 8) |
           (uint32_t(data[2]) << 16) | (uint32_t(data[3]) << 24);
}

void Decode565(uint16_t color, uint8_t* rgb)
{
    const uint8_t r = (color >> 11) & 31;
    const uint8_t g = (color >> 5) & 63;
    const uint8_t b = color & 31;
    rgb[0] = (r << 3) | (r >> 2);
    rgb[1] = (g << 2) | (g >> 4);
    rgb[2] = (b << 3) | (b >> 2);
}

void BuildColorPalette(const uint8_t* block, bool allowTransparency, uint8_t palette[4][4])
{
    const uint16_t c0 = Read16(block);
    const uint16_t c1 = Read16(block + 2);
    std::memset(palette, 0, 16);
    Decode565(c0, palette[0]);
    Decode565(c1, palette[1]);
    palette[0][3] = palette[1][3] = palette[2][3] = 255;
    const bool threeColor = allowTransparency && c0 <= c1;
    palette[3][3] = threeColor ? 0 : 255;
    for (unsigned channel = 0; channel < 3; ++channel)
    {
        if (threeColor)
            palette[2][channel] = (uint16_t(palette[0][channel]) + palette[1][channel]) / 2;
        else
        {
            palette[2][channel] = (2 * uint16_t(palette[0][channel]) + palette[1][channel] + 1) / 3;
            palette[3][channel] = (uint16_t(palette[0][channel]) + 2 * palette[1][channel] + 1) / 3;
        }
    }
}

void DecodeColorBlock(const uint8_t* block, bool allowTransparency, uint8_t rgba[16][4])
{
    uint8_t palette[4][4];
    BuildColorPalette(block, allowTransparency, palette);
    const uint32_t indices = Read32(block + 4);
    for (unsigned pixel = 0; pixel < 16; ++pixel)
        std::memcpy(rgba[pixel], palette[(indices >> (2 * pixel)) & 3], 4);
}

void DecodeChannelBlock(const uint8_t* block, bool isSigned, uint8_t values[16])
{
    int endpoints[2] = {isSigned ? int(int8_t(block[0])) : int(block[0]),
                        isSigned ? int(int8_t(block[1])) : int(block[1])};
    if (isSigned)
    {
        endpoints[0] = std::max(endpoints[0], -127);
        endpoints[1] = std::max(endpoints[1], -127);
    }
    int palette[8] = {endpoints[0], endpoints[1]};
    if (endpoints[0] > endpoints[1])
    {
        for (int i = 2; i < 8; ++i)
            palette[i] = ((8 - i) * endpoints[0] + (i - 1) * endpoints[1]) / 7;
    }
    else
    {
        for (int i = 2; i < 6; ++i)
            palette[i] = ((6 - i) * endpoints[0] + (i - 1) * endpoints[1]) / 5;
        palette[6] = isSigned ? -127 : 0;
        palette[7] = isSigned ? 127 : 255;
    }
    uint64_t indices = 0;
    for (unsigned byte = 0; byte < 6; ++byte)
        indices |= uint64_t(block[2 + byte]) << (8 * byte);
    for (unsigned pixel = 0; pixel < 16; ++pixel)
        values[pixel] = uint8_t(palette[(indices >> (3 * pixel)) & 7]);
}

uint16_t PackBGR5A1(const uint8_t* rgba)
{
    const auto to5 = [](uint8_t v) { return uint16_t((uint32_t(v) * 31 + 127) / 255); };
    return uint16_t(to5(rgba[2]) | (to5(rgba[1]) << 5) | (to5(rgba[0]) << 10) |
                    (rgba[3] >= 128 ? 0x8000 : 0));
}
}

template<BCFormat format>
static bool DecodeBCImageImpl(const uint8_t* source, size_t sourceSize,
                   uint32_t width, uint32_t height, uint8_t* destination, size_t destinationSize)
{
    if (!source || !destination || !width || !height)
        return false;
    constexpr bool packed16 = format == BCFormat::BC1Packed16;
    constexpr bool bc1 = format == BCFormat::BC1 || packed16;
    constexpr bool bc4 = format == BCFormat::BC4Unorm || format == BCFormat::BC4Snorm;
    constexpr bool bc5 = format == BCFormat::BC5Unorm || format == BCFormat::BC5Snorm;
    // Bytes per output pixel.
    constexpr size_t channels = bc4 ? 1 : (bc5 || packed16) ? 2 : 4;
    constexpr size_t blockBytes = (bc1 || bc4) ? 8 : 16;
    const size_t blockWidth = (size_t(width) + 3) / 4;
    const size_t blockHeight = (size_t(height) + 3) / 4;
    if (blockWidth > std::numeric_limits<size_t>::max() / blockHeight ||
        blockWidth * blockHeight > sourceSize / blockBytes ||
        size_t(width) > std::numeric_limits<size_t>::max() / height / channels ||
        size_t(width) * height * channels > destinationSize)
        return false;

    constexpr bool signedChannel = format == BCFormat::BC4Snorm || format == BCFormat::BC5Snorm;
    for (size_t by = 0; by < blockHeight; ++by)
    {
        for (size_t bx = 0; bx < blockWidth; ++bx)
        {
            const uint8_t* block = source + (by * blockWidth + bx) * blockBytes;
            uint8_t rgba[16][4];
            uint8_t first[16];
            uint8_t second[16];
            uint16_t packedPalette[4];
            uint32_t packedIndices = 0;
            if constexpr (packed16)
            {
                // Pack the four palette colors once per block, then look up.
                uint8_t palette[4][4];
                BuildColorPalette(block, true, palette);
                for (unsigned entry = 0; entry < 4; ++entry)
                    packedPalette[entry] = PackBGR5A1(palette[entry]);
                packedIndices = Read32(block + 4);
            }
            else if constexpr (bc4 || bc5)
            {
                DecodeChannelBlock(block, signedChannel, first);
                if constexpr (bc5)
                    DecodeChannelBlock(block + 8, signedChannel, second);
            }
            else
            {
                DecodeColorBlock(block + (bc1 ? 0 : 8), bc1, rgba);
                if constexpr (format == BCFormat::BC2)
                {
                    for (unsigned pixel = 0; pixel < 16; ++pixel)
                        rgba[pixel][3] = ((block[pixel / 2] >> (4 * (pixel & 1))) & 15) * 17;
                }
                else if constexpr (format == BCFormat::BC3)
                {
                    DecodeChannelBlock(block, false, first);
                    for (unsigned pixel = 0; pixel < 16; ++pixel)
                        rgba[pixel][3] = first[pixel];
                }
            }
            for (unsigned py = 0; py < 4 && by * 4 + py < height; ++py)
            {
                const size_t count = std::min<size_t>(4, size_t(width) - bx * 4);
                uint8_t* output = destination + ((by * 4 + py) * width + bx * 4) * channels;
                // Each decoder fills its complete block; copy visible rows, including partial mips.
                if constexpr (bc4)
                {
                    if (count == 4) std::memcpy(output, first + py * 4, 4);
                    else std::memcpy(output, first + py * 4, count);
                }
                else if constexpr (packed16)
                {
                    for (size_t px = 0; px < count; ++px)
                    {
                        const uint16_t packed = packedPalette[(packedIndices >> (2 * (py * 4 + px))) & 3];
                        output[px * 2] = uint8_t(packed);
                        output[px * 2 + 1] = uint8_t(packed >> 8);
                    }
                }
                else if constexpr (!bc5)
                {
                    if (count == 4) std::memcpy(output, rgba[py * 4], 16);
                    else std::memcpy(output, rgba[py * 4], count * 4);
                }
                else for (size_t px = 0; px < count; ++px)
                {
                    const unsigned pixel = py * 4 + px;
                    output[px * 2] = first[pixel];
                    output[px * 2 + 1] = second[pixel];
                }
            }
        }
    }
    return true;
}

bool DecodeBCImage(BCFormat format, const uint8_t* source, size_t sourceSize,
                   uint32_t width, uint32_t height, uint8_t* destination, size_t destinationSize)
{
    // Select once per image so block expansion has no per-pixel format branches.
    switch (format)
    {
    case BCFormat::BC1: return DecodeBCImageImpl<BCFormat::BC1>(source, sourceSize, width, height, destination, destinationSize);
    case BCFormat::BC2: return DecodeBCImageImpl<BCFormat::BC2>(source, sourceSize, width, height, destination, destinationSize);
    case BCFormat::BC3: return DecodeBCImageImpl<BCFormat::BC3>(source, sourceSize, width, height, destination, destinationSize);
    case BCFormat::BC4Unorm: return DecodeBCImageImpl<BCFormat::BC4Unorm>(source, sourceSize, width, height, destination, destinationSize);
    case BCFormat::BC4Snorm: return DecodeBCImageImpl<BCFormat::BC4Snorm>(source, sourceSize, width, height, destination, destinationSize);
    case BCFormat::BC5Unorm: return DecodeBCImageImpl<BCFormat::BC5Unorm>(source, sourceSize, width, height, destination, destinationSize);
    case BCFormat::BC5Snorm: return DecodeBCImageImpl<BCFormat::BC5Snorm>(source, sourceSize, width, height, destination, destinationSize);
    case BCFormat::BC1Packed16: return DecodeBCImageImpl<BCFormat::BC1Packed16>(source, sourceSize, width, height, destination, destinationSize);
    }
    return false;
}
