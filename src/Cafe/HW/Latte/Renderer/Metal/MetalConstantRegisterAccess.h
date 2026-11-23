// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <string_view>

namespace MetalConstantRegisterAccess
{
// R600/Evergreen ISA, section 4.6.3: relatively indexed constant-register
// addresses outside [0,255] return 0x7fffffff in every component. They must
// never become host buffer reads or be clamped to a different guest register.
inline constexpr unsigned RegisterCount = 256;
inline constexpr std::string_view Source = R"MSL(
inline int4 latteReadConstantRegister(constant int4* registers, uint index)
{
    if (index < 256u)
        return registers[index];
    return int4(0x7fffffff);
}
)MSL";
}
