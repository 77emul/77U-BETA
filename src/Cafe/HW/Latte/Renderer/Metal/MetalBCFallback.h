#pragma once

#include <cstddef>
#include <cstdint>

enum class BCFormat : uint8_t
{
    BC1, BC2, BC3, BC4Unorm, BC4Snorm, BC5Unorm, BC5Snorm,
    // BC1 stored as packed 16-bit BGR5A1 (blue bits 0-4, green 5-9, red 10-14,
    // alpha bit 15). Half the size of RGBA8 for memory-constrained devices.
    BC1Packed16
};

// Expands a linear array of 4x4 BC blocks into tightly packed pixels.
// The destination has 4, 1, or 2 channels for BC1-3, BC4, or BC5 respectively,
// and 2 bytes per pixel for BC1Packed16.
bool DecodeBCImage(BCFormat format, const uint8_t* source, size_t sourceSize,
                   uint32_t width, uint32_t height, uint8_t* destination, size_t destinationSize);
