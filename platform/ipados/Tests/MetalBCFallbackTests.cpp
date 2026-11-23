#include "Cafe/HW/Latte/Renderer/Metal/MetalBCFallback.h"

#include <array>
#include <cassert>
#include <cstdint>

int main()
{
    // BC1: opaque red, then green, selected by the low two index bits.
    const std::array<uint8_t, 8> bc1 = {0x00, 0xf8, 0xe0, 0x07, 0x04, 0, 0, 0};
    std::array<uint8_t, 64> rgba{};
    assert(DecodeBCImage(BCFormat::BC1, bc1.data(), bc1.size(), 4, 4, rgba.data(), rgba.size()));
    assert((rgba[0] == 255 && rgba[1] == 0 && rgba[2] == 0 && rgba[3] == 255));
    assert((rgba[4] == 0 && rgba[5] == 255 && rgba[6] == 0 && rgba[7] == 255));

    // Four-colour BC1 index 3 is an opaque interpolated colour, not transparency.
    // Course-preview textures use all four indices; index 3 must not expose the UI below.
    const std::array<uint8_t, 8> opaqueFourth = {0x00, 0xf8, 0xe0, 0x07, 0xff, 0xff, 0xff, 0xff};
    assert(DecodeBCImage(BCFormat::BC1, opaqueFourth.data(), opaqueFourth.size(), 4, 4, rgba.data(), rgba.size()));
    for (size_t pixel = 0; pixel < 16; ++pixel)
        assert(rgba[pixel * 4 + 3] == 255);

    // A partial mip must pack only its two visible columns, with no block padding.
    std::array<uint8_t, 16> cropped{};
    assert(DecodeBCImage(BCFormat::BC1, bc1.data(), bc1.size(), 2, 2, cropped.data(), cropped.size()));
    assert((cropped[0] == 255 && cropped[4] == 0 && cropped[5] == 255));

    // BC1 three-colour mode uses index 3 for transparency.
    const std::array<uint8_t, 8> transparent = {0x00, 0, 0xff, 0xff, 0x03, 0, 0, 0};
    assert(DecodeBCImage(BCFormat::BC1, transparent.data(), transparent.size(), 1, 1, rgba.data(), rgba.size()));
    assert(rgba[3] == 0);

    // BC2 explicit alpha nibble 0x5 expands to 0x55.
    const std::array<uint8_t, 16> bc2 = {0x05, 0, 0, 0, 0, 0, 0, 0, 0x00, 0xf8, 0xe0, 0x07, 0, 0, 0, 0};
    assert(DecodeBCImage(BCFormat::BC2, bc2.data(), bc2.size(), 4, 4, rgba.data(), rgba.size()));
    assert((rgba[0] == 255 && rgba[3] == 0x55));

    // BC3/BC4 endpoint-zero alpha/value decoding.
    const std::array<uint8_t, 16> bc3 = {0x22, 0xee, 0, 0, 0, 0, 0, 0, 0x00, 0xf8, 0xe0, 0x07, 0, 0, 0, 0};
    assert(DecodeBCImage(BCFormat::BC3, bc3.data(), bc3.size(), 4, 4, rgba.data(), rgba.size()));
    assert(rgba[3] == 0x22);
    std::array<uint8_t, 16> channel{};
    assert(DecodeBCImage(BCFormat::BC4Unorm, bc3.data(), 8, 4, 4, channel.data(), channel.size()));
    assert(channel[0] == 0x22);

    // BC5 stores two independent BC4 channels.
    const std::array<uint8_t, 16> bc5 = {0x22, 0xee, 0, 0, 0, 0, 0, 0, 0x77, 0xdd, 0, 0, 0, 0, 0, 0};
    std::array<uint8_t, 32> rg{};
    assert(DecodeBCImage(BCFormat::BC5Unorm, bc5.data(), bc5.size(), 4, 4, rg.data(), rg.size()));
    assert((rg[0] == 0x22 && rg[1] == 0x77));

    // Signed BC4 stores a negative endpoint as an R8Snorm byte.
    const std::array<uint8_t, 8> signedBC4 = {0x81, 0x7f, 0, 0, 0, 0, 0, 0};
    assert(DecodeBCImage(BCFormat::BC4Snorm, signedBC4.data(), signedBC4.size(), 4, 4, channel.data(), channel.size()));
    assert(static_cast<int8_t>(channel[0]) == -127);

    // BC1Packed16 stores BGR5A1: blue bits 0-4, green 5-9, red 10-14, alpha bit 15.
    const auto packed = [](const std::array<uint8_t, 32>& p, size_t pixel) {
        return uint16_t(p[pixel * 2] | (p[pixel * 2 + 1] << 8));
    };
    std::array<uint8_t, 32> bgr5a1{};
    assert(DecodeBCImage(BCFormat::BC1Packed16, bc1.data(), bc1.size(), 4, 4, bgr5a1.data(), bgr5a1.size()));
    assert(packed(bgr5a1, 0) == 0xfc00); // opaque red
    assert(packed(bgr5a1, 1) == 0x83e0); // opaque green
    assert(packed(bgr5a1, 2) == 0xfc00);
    // Four-colour index 3 stays opaque; three-colour index 3 is transparent.
    assert(DecodeBCImage(BCFormat::BC1Packed16, opaqueFourth.data(), opaqueFourth.size(), 4, 4, bgr5a1.data(), bgr5a1.size()));
    for (size_t pixel = 0; pixel < 16; ++pixel)
        assert(packed(bgr5a1, pixel) & 0x8000);
    assert(DecodeBCImage(BCFormat::BC1Packed16, transparent.data(), transparent.size(), 1, 1, bgr5a1.data(), bgr5a1.size()));
    assert(!(packed(bgr5a1, 0) & 0x8000));
    // Each pixel matches the RGBA8 decode rounded to 5 bits per channel.
    const std::array<uint8_t, 8> gradient = {0x1f, 0x84, 0xe0, 0x7b, 0xe4, 0x1b, 0x6c, 0xb1};
    assert(DecodeBCImage(BCFormat::BC1, gradient.data(), gradient.size(), 4, 4, rgba.data(), rgba.size()));
    assert(DecodeBCImage(BCFormat::BC1Packed16, gradient.data(), gradient.size(), 4, 4, bgr5a1.data(), bgr5a1.size()));
    for (size_t pixel = 0; pixel < 16; ++pixel)
    {
        const uint16_t value = packed(bgr5a1, pixel);
        const auto close = [](unsigned five, uint8_t eight) {
            const int expanded = int((five << 3) | (five >> 2));
            return expanded - int(eight) <= 4 && int(eight) - expanded <= 4;
        };
        assert(close((value >> 10) & 31, rgba[pixel * 4 + 0]));
        assert(close((value >> 5) & 31, rgba[pixel * 4 + 1]));
        assert(close(value & 31, rgba[pixel * 4 + 2]));
        assert(bool(value & 0x8000) == (rgba[pixel * 4 + 3] >= 128));
    }
    // A partial mip packs only its visible pixels, 2 bytes each.
    std::array<uint8_t, 8> croppedPacked{};
    assert(DecodeBCImage(BCFormat::BC1Packed16, bc1.data(), bc1.size(), 2, 2, croppedPacked.data(), croppedPacked.size()));
    assert(!DecodeBCImage(BCFormat::BC1Packed16, bc1.data(), bc1.size(), 2, 2, croppedPacked.data(), croppedPacked.size() - 1));

    // A truncated source must never be read beyond its allocation.
    assert(!DecodeBCImage(BCFormat::BC3, bc3.data(), 15, 4, 4, rgba.data(), rgba.size()));
    assert(!DecodeBCImage(static_cast<BCFormat>(255), bc3.data(), bc3.size(), 4, 4, rgba.data(), rgba.size()));
    // An odd rectangular mip must not touch the destination guard bytes.
    for (auto format : {BCFormat::BC1, BCFormat::BC2, BCFormat::BC3, BCFormat::BC4Unorm,
                        BCFormat::BC4Snorm, BCFormat::BC5Unorm, BCFormat::BC5Snorm, BCFormat::BC1Packed16})
    {
        const size_t channels = (format == BCFormat::BC4Unorm || format == BCFormat::BC4Snorm) ? 1 :
            (format == BCFormat::BC5Unorm || format == BCFormat::BC5Snorm || format == BCFormat::BC1Packed16) ? 2 : 4;
        std::array<uint8_t, 32> blocks{};
        std::array<uint8_t, 62> pixels;
        pixels.fill(0xa7);
        assert(DecodeBCImage(format, blocks.data(), blocks.size(), 5, 3, pixels.data() + 1, 15 * channels));
        assert(pixels.front() == 0xa7 && pixels[1 + 15 * channels] == 0xa7);
        assert(!DecodeBCImage(format, blocks.data(), blocks.size(), 5, 3, pixels.data() + 1, 15 * channels - 1));
    }
    return 0;
}
