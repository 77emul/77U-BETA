#!/usr/bin/env python3
"""Exercise production MIC lifecycle with a deterministic capture backend.

Verifies permission gating, late permission, guest PCM/mute and 3 ms silence.
No host microphone is opened. Real iOS audio routing still needs a device test.
"""
from pathlib import Path
import argparse
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[3]
parser = argparse.ArgumentParser()
parser.add_argument('--source', type=Path, default=repo/'src/Cafe/OS/libs/mic/mic.cpp')
args = parser.parse_args()
source = args.source.read_text().split('\nnamespace mic\n')[0]
source = '\n'.join(line for line in source.splitlines() if not line.startswith('#include'))
fixture = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <vector>
using uint32=uint32_t;using uint16=uint16_t;using sint16=int16_t;using sint32=int32_t;using uint32be=uint32;
template<class T>using betype=T;
template<class T>struct MEMPTR {
    T* ptr=nullptr;bool IsNull()const{return !ptr;}T* operator->(){return ptr;}T& operator*(){return *ptr;}
    uintptr_t GetMPTR()const{return reinterpret_cast<uintptr_t>(ptr);}
};
#define DEFINE_ENUM_FLAG_OPERATORS(T) inline T operator|(T a,T b){return T(uint32(a)|uint32(b));}inline T& operator|=(T&a,T b){return a=a|b;}
enum class LogType{Force};template<class...T>void cemuLog_log(LogType,const char*,T...){}
uint16_t ring[4096]{};
void* memory_getPointerFromVirtualOffset(uint32 p){assert(p==1);return ring;}
uint16 _swapEndianU16(uint16 v){return __builtin_bswap16(v);}
uint32 GetTickCount(){return 123;}
struct Controller{bool is_mic_active(){return false;}};
struct InputManager{static InputManager& instance(){static InputManager v;return v;}Controller* get_vpad_controller(uint32){return nullptr;}};
struct Config{std::wstring input_device=L"default";int input_volume=75;};
Config& GetConfig(){static Config c;return c;}
bool ready=false,backendAvailable=true,throwOnCreate=false;
unsigned creates=0,plays=0,stops=0,consumes=0;
extern "C" bool CemuBridge_IsMicrophoneCaptureReady(void){return ready;}
class IAudioInputAPI {
public:
    struct DeviceDescription{std::wstring GetIdentifier()const{return L"default";}};
    using DeviceDescriptionPtr=std::shared_ptr<DeviceDescription>;enum AudioInputAPI{Cubeb};
    static bool IsAudioInputAPIAvailable(AudioInputAPI){return backendAvailable;}
    static std::vector<DeviceDescriptionPtr> GetDevices(AudioInputAPI){return {std::make_shared<DeviceDescription>()};}
    static std::unique_ptr<IAudioInputAPI> CreateDevice(AudioInputAPI,DeviceDescriptionPtr,uint32 rate,uint32 channels,uint32 block,uint32 bits){
        assert(rate==32000&&channels==1&&block==96&&bits==16);++creates;
        if(throwOnCreate)throw std::runtime_error("test unavailable input");return std::make_unique<IAudioInputAPI>();
    }
    int volume=0;bool playing=false;
    void SetVolume(int v){volume=v;}
    bool Play(){playing=true;++plays;return true;}bool Stop(){playing=false;++stops;return true;}
    bool ConsumeBlock(sint16* p){assert(playing);for(int i=0;i<96;++i)p[i]=sint16(100+i*13);++consumes;return true;}
};
std::shared_mutex g_audioInputMutex;std::unique_ptr<IAudioInputAPI> g_inputAudio;
PRODUCTION
int main(int argc,char**argv){
    std::string mode=argv[1];ready=mode=="allowed"||mode=="failure";
    throwOnCreate=mode=="failure";
    micRingbuffer_t rb{sizeof(ring),1};uint32 err=123;
    assert(MICInit(0,0,{&rb},{&err})==0&&err==0);
    assert(MICOpen(0)==0);mic_updateOnAXFrame();
    if(mode=="allowed"){
        assert(g_inputAudio&&creates==1&&g_inputAudio->volume==75&&consumes==1);
        assert(MICStatus.drc[0].writeIndex==96);
        for(unsigned i=0;i<96;++i)assert(ring[i]==_swapEndianU16(100+i*13));
        assert(MICSetState(0,uint32(MIC_STATEID::MUTE),1)==0);mic_updateOnAXFrame();
        for(unsigned i=96;i<192;++i)assert(ring[i]==0);
        assert(MICSetState(0,uint32(MIC_STATEID::MUTE),0)==0);
        micStatus_t state{};assert(MICGetStatus(0,{&state})==0&&state.numSamplesAvailable==192);
        assert(MICSetDataConsumed(0,192)==0&&mic_availableSamples(0)==0);
        assert(MICClose(0)==0&&!g_inputAudio->playing);
        auto before=MICStatus.drc[0].writeIndex;mic_updateOnAXFrame();assert(before==MICStatus.drc[0].writeIndex);
        assert(MICOpen(0)==0);mic_updateOnAXFrame();assert(creates==1&&consumes==3);
    }else{
        assert(!g_inputAudio);
#if BOOST_OS_IOS
        assert(MICStatus.drc[0].writeIndex==96); // Exactly one 3 ms silence block.
#endif
        if(mode=="denied")assert(creates==0);
#if HAS_REFRESH
        if(mode=="late"){
            assert(creates==0);ready=true;mic_requestInputDeviceRefresh();mic_updateOnAXFrame();
            assert(g_inputAudio&&creates==1&&consumes==1&&MICStatus.drc[0].writeIndex==192);
            ready=false;mic_requestInputDeviceRefresh();mic_updateOnAXFrame();
            assert(!g_inputAudio&&MICStatus.drc[0].writeIndex==288);
            for(unsigned i=192;i<288;++i)assert(ring[i]==0);
        }
#endif
        if(mode=="failure")assert(creates==2); // Init and explicit Open retry; no crash.
    }
    puts("PASS: microphone permission/lifecycle, 32 kHz mono PCM, guest endian/mute, close/reopen and unavailable capture");
}
'''.replace('PRODUCTION', source)

with tempfile.TemporaryDirectory(prefix='77u-mic-lifecycle-') as temp:
    p = Path(temp)
    (p/'test.cpp').write_text(fixture)
    for ios in (0, 1):
        executable = p/f'test-{ios}'
        subprocess.run(['xcrun', 'clang++', '-std=c++20', '-O1', '-fsanitize=address,undefined',
                        f'-DBOOST_OS_IOS={ios}', '-DHAS_REFRESH='+str(int('void mic_requestInputDeviceRefresh()' in source)),
                        str(p/'test.cpp'), '-o', str(executable)], check=True)
        for mode in (('allowed',) if not ios else ('allowed', 'denied', 'late', 'failure')):
            subprocess.run([str(executable), mode], check=True, timeout=15)

bridge = (repo/'platform/ipados/Bridge/CemuBridge.mm').read_text()
assert 'mic_requestInputDeviceRefresh();' in bridge
assert 'extern "C" bool CemuBridge_IsMicrophoneCaptureReady(void)' in bridge
assert 'session.isInputAvailable && session.sampleRate > 0.0' in bridge
print('PASS: iOS refresh callback queues backend work instead of accessing guest state')

backend = (repo/'src/audio/CubebInputAPI.cpp').read_text()


def method(signature):
    start = backend.index(signature)
    end = backend.index('{', start) + 1
    depth = 1
    while depth:
        depth += (backend[end] == '{') - (backend[end] == '}')
        end += 1
    return backend[start:end]


callback_fixture = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <vector>
using uint8=uint8_t;using sint16=int16_t;struct cubeb_stream{};
enum class LogType{Force};template<class...T>void cemuLog_logDebug(LogType,const char*,T...){}
struct CubebInputAPI {
    unsigned m_channels=1,m_bitsPerSample=16,m_bytesPerBlock=192;
    std::shared_mutex m_mutex;std::vector<uint8> m_buffer;
    static long data_cb(cubeb_stream*,void*,const void*,void*,long);
    bool ConsumeBlock(sint16*);
};
METHODS
int main(){
    CubebInputAPI input;input.m_buffer.reserve(1024);
    sint16 samples[96],output[96];for(int i=0;i<96;++i)samples[i]=int16_t(23+i*31);
    assert(input.data_cb(nullptr,&input,samples,nullptr,96)==96&&input.m_buffer.size()==192);
    assert(input.data_cb(nullptr,&input,nullptr,nullptr,96)==96&&input.m_buffer.size()==192);
    assert(input.data_cb(nullptr,nullptr,samples,nullptr,96)==96);
    assert(input.data_cb(nullptr,&input,samples,nullptr,0)==0&&input.m_buffer.size()==192);
    assert(input.ConsumeBlock(output)&&memcmp(samples,output,sizeof(samples))==0&&input.m_buffer.empty());
    assert(input.ConsumeBlock(output));for(auto sample:output)assert(sample==0);
}
'''.replace('METHODS', method('long CubebInputAPI::data_cb(')+'\n'+method('bool CubebInputAPI::ConsumeBlock('))
with tempfile.TemporaryDirectory(prefix='77u-mic-callback-') as temp:
    p = Path(temp)
    (p/'test.cpp').write_text(callback_fixture)
    subprocess.run(['xcrun', 'clang++', '-std=c++20', '-O1', '-fsanitize=address,undefined',
                    str(p/'test.cpp'), '-o', str(p/'test')], check=True)
    subprocess.run([str(p/'test')], check=True, timeout=15)
print('PASS: production Cubeb PCM buffering, null input/user, empty blocks and silence')
