#!/usr/bin/env python3
"""Execute the actual default/migration function without touching real preferences."""
from pathlib import Path
import subprocess,tempfile
repo=Path(__file__).resolve().parents[3]
s=(repo/'platform/ipados/Bridge/CemuBridge.mm').read_text();s=s[s.index('static int ZeldaBOTWInitialFPSLimit('):s.index('void EnsureZeldaBOTWIPhoneThermalSettings(')]
prefix=r'''
#import <Foundation/Foundation.h>
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <map>
#include <cassert>
#include <cstring>
#include <cstdio>
std::string hostModel="iPhone18,1";
int sysctlbyname(const char*,void* out,size_t* size,void*,size_t){if(hostModel.empty())return -1;assert(*size>hostModel.size());memcpy(out,hostModel.c_str(),hostModel.size()+1);return 0;}
@interface TestDefaults:NSObject
+(TestDefaults*)standardUserDefaults;
-(BOOL)boolForKey:(NSString*)key;
-(void)setBool:(BOOL)value forKey:(NSString*)key;
@end
NSMutableDictionary* values;
@implementation TestDefaults
+(TestDefaults*)standardUserDefaults {static TestDefaults* d=[TestDefaults new];return d;}
-(BOOL)boolForKey:(NSString*)key{return [values[key] boolValue];}
-(void)setBool:(BOOL)value forKey:(NSString*)key{values[key]=@(value);}
@end
#define NSUserDefaults TestDefaults
#define cemuLog_log(...) ((void)0)
namespace WindowSystem {bool phone=true;bool IsPhone(){return phone;}}
namespace TitleIdParser {uint64_t MakeBaseTitleId(uint64_t id){return id;}}
struct Pack {
 std::map<std::string,std::string> p;bool enabled=false;int changes=0;
 std::string GetVirtualPath(){return "The Legend of Zelda: Breath of the Wild/Mods/FPS++";}
 std::string GetActivePreset(std::string k){return p[k];}
 void SetActivePreset(std::string k,std::string v,bool){p[k]=v;++changes;}
 void SetEnabled(bool v){enabled=v;}void UpdatePresetVisibility(){}void ValidatePresetSelections(){}
};
namespace GraphicPack2 {std::vector<std::shared_ptr<Pack>> packs;auto& GetGraphicPacks(){return packs;}}
std::mutex s_graphicPackBridgeMutex;int saves=0;void SaveGraphicPackState(){++saves;}
'''
main=r'''
int main(){@autoreleasepool{
 for(int scenario=0;scenario<5;scenario++){
 values=[NSMutableDictionary new];saves=0;WindowSystem::phone=scenario!=3;
 auto p=std::make_shared<Pack>();p->p["Framerate Limit"]="custom existing 30";p->p["Fence Type"]=scenario==2?"Performance Fence (Default)":"Fence Skip";GraphicPack2::packs={p};
 if(scenario!=0)values[@"CemuIOS.BotW60FPSDefaultApplied_v3"]=@YES;
 EnsureZeldaBOTWDefaultFPSPackEnabled(scenario==4?0:0x00050000101C9500ULL);
 if(scenario==0){assert(p->p["Fence Type"]=="Accurate Fence");assert(p->enabled);}
 if(scenario==1){assert(p->p["Fence Type"]=="Accurate Fence");assert(p->p["Framerate Limit"]=="custom existing 30");assert(!p->enabled);assert(p->changes==1);assert(saves==1);EnsureZeldaBOTWDefaultFPSPackEnabled(0x00050000101C9500ULL);assert(saves==1);}
 if(scenario==2){assert(p->p["Fence Type"]=="Performance Fence (Default)");assert(p->changes==0);}
 if(scenario==3||scenario==4){assert(p->p["Fence Type"]=="Fence Skip");assert(p->changes==0);}
 }
 for(auto [model,fps]:std::vector<std::pair<std::string,int>>{{"iPhone13,2",30},{"iPhone14,7",30},{"iPhone15,2",30},{"iPhone15,3",30},{"iPhone15,4",60},{"iPhone16,1",60},{"iPhone18,1",60},{"iPad8,9",30},{"iPad12,1",30},{"iPad13,1",30},{"iPad13,18",30},{"iPad14,1",30},{"iPad15,7",30},{"iPad15,8",30},{"iPad13,4",60},{"iPad13,16",60},{"iPad14,3",60},{"iPad14,8",60},{"iPad15,1",60},{"iPad16,3",60},{"iPhone18,1garbage",60},{"unknown",60},{"",60}}){
 hostModel=model;WindowSystem::phone=model.starts_with("iPhone");values=[NSMutableDictionary new];
 assert(ZeldaBOTWInitialFPSLimit()==fps);
 auto p=std::make_shared<Pack>();GraphicPack2::packs={p};
 EnsureZeldaBOTWDefaultFPSPackEnabled(0x00050000101C9500ULL);
 assert(p->enabled);assert(p->p["FPS Limit"]==(fps==30?"30FPS Limit":"60FPS Limit (Default)"));
 assert(p->p["Framerate Limit"]==(fps==30?"30FPS (ideal for 240/120/60Hz displays)":"60FPS (ideal for 240/120/60Hz displays)"));
 p->p["FPS Limit"]="custom user setting";p->p["Framerate Limit"]="custom user setting";int count=p->changes;
 EnsureZeldaBOTWDefaultFPSPackEnabled(0x00050000101C9500ULL);
 assert(p->changes==count&&p->p["FPS Limit"]=="custom user setting"&&p->p["Framerate Limit"]=="custom user setting");
 }
 puts("PASS: first-install 30/60 defaults across 23 hardware IDs; subsequent launches preserve user settings");
 puts("PASS: new default; one-time phone fence repair; existing FPS/enable preserved; other fences/iPad/titles untouched");
}}
'''
with tempfile.TemporaryDirectory(prefix='botw-fence-') as d:
 d=Path(d);(d/'test.mm').write_text(prefix+s+main)
 subprocess.run(['xcrun','clang++','-std=c++20','-fobjc-arc',str(d/'test.mm'),'-framework','Foundation','-o',str(d/'test')],check=True)
 subprocess.run([str(d/'test')],check=True)
