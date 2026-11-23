#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <filesystem>
#include <vector>
#include <optional>
#include <sstream>
#include <cstring>
#include <boost/algorithm/string/predicate.hpp>
namespace fs = std::filesystem;
using uint64 = uint64_t;
using uint32 = uint32_t;
using uint8 = uint8_t;
using sint32 = int32_t;
using DWORD = uint32_t;
#if __has_include("input/api/GCController/AppleMotionHandler.h")
#include "input/api/GCController/AppleMotionHandler.h"
#else
#include "input/motion/MotionHandler.h"
class AppleMotionHandler {
    WiiUMotionHandler motion{GyroBiasMode::AlreadyCorrected};
public:
    void reset() { motion.reset(); }
    void processAppleSample(float dt, float gx, float gy, float gz, float ax, float ay, float az) {
        motion.processMotionSample(dt,gx,-gz,gy,ax,-az,ay);
    }
    MotionSample getMotionSample() { return motion.getMotionSample(); }
};
#endif
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
constexpr float pi=3.14159265359f;
MotionSample held(AppleMotionHandler& motion, float degrees) {
    const float angle=degrees*pi/180.f;
    for (int i=0;i<600;++i)
        motion.processAppleSample(1.f/120.f,0,0,0,0,std::sin(angle),-std::cos(angle));
    return motion.getMotionSample();
}
int main() {
    for (float pose : {0.f,30.f,45.f,65.f,90.f}) {
        AppleMotionHandler motion;
        auto sample=held(motion,pose);
        float acc[3]; sample.getVPADAccelerometer(acc);
        check(std::abs(acc[0])<0.001f && std::abs(acc[1]+1.f)<0.001f && std::abs(acc[2])<0.001f,
            "normally held Apple device is not neutral");
        sample=held(motion,pose+10.f);
        sample.getVPADAccelerometer(acc);
        check(std::abs(acc[2]-std::sin(10.f*pi/180.f))<0.001f,"intentional pitch erased");
        check(std::abs(sample.getVPADAccMagnitude()-1.f)<0.001f,"gravity magnitude changed");
        motion.reset(); sample=held(motion,pose+10.f); sample.getVPADAccelerometer(acc);
        check(std::abs(acc[2])<0.001f,"recenter did not learn new holding pose");
    }
    AppleMotionHandler moving;
    moving.processAppleSample(.01f,3,0,0,0,1,0); // not a quiet calibration sample
    auto sample=held(moving,45); float acc[3]; sample.getVPADAccelerometer(acc);
    check(std::abs(acc[2])<0.001f,"movement captured as neutral pose");
    AppleMotionHandler invalid;
    invalid.processAppleSample(.01f,0,0,0,0,0,0); // no gravity: wait for a valid sample
    sample=held(invalid,30); sample.getVPADAccelerometer(acc);
    check(std::abs(acc[2])<0.001f,"zero gravity prevented calibration");
    AppleMotionHandler gyro{GyroBiasMode::AlreadyCorrected};
    held(gyro,45);
    gyro.processAppleSample(.01f,.2f,.3f,.4f,0,std::sqrt(.5f),-std::sqrt(.5f));
    float rates[3]; gyro.getMotionSample().getVPADGyroChange(rates);
    const float units=2.f*pi;
    check(std::abs(rates[0]+.2f/units)<.001f,"pitch gyro changed");
    check(std::abs(rates[1]+(-.4f+.3f)*std::sqrt(.5f)/units)<.001f,"yaw axes not rotated with gravity");
    check(std::abs(rates[2]-(.4f+.3f)*std::sqrt(.5f)/units)<.001f,"roll axes not rotated with gravity");
    puts("PASS: flat/30/45/65/90 degree poses, relative pitch, recenter, movement and zero gravity");
}
