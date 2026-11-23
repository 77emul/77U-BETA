#!/usr/bin/env python3
"""Exercise production override loads using the existing real Metal upload fixture."""
from pathlib import Path
import argparse, os, subprocess, tempfile

repo = Path(__file__).resolve().parents[3]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--source', type=Path, default=repo/'src/Cafe/HW/Latte/Core/LatteTextureLoader.cpp')
a = p.parse_args()

def function(text, signature):
    start = text.index(signature)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]

# Reuse the tested production allocator/decoder fixture rather than another allocator model.
test = repo/'platform/ipados/Tests/MetalDirectTextureUploadTests.py'
source = test.read_text().split("with tempfile.TemporaryDirectory(prefix='cemu-direct-upload-')", 1)[0]
namespace = {'__file__': str(test)}
import sys
saved_argv = sys.argv
sys.argv = [str(test)]
try:
    exec(compile(source, str(test), 'exec'), namespace)
finally:
    sys.argv = saved_argv
fixture = namespace['fixture'].split('int main(int argc,char** argv)', 1)[0]
# No optional timing shim is needed for the platform-independent loader body.
fixture += '\n#undef BOOST_OS_IOS\n#define BOOST_OS_IOS 0\n'
loader = a.source.read_text()
fixture += r'''
#include <mach/mach.h>
#include <filesystem>
#include <format>
using sint32=int32_t;using MPTR=uint32;namespace fs=std::filesystem;
namespace fmt {template<class...T>std::string format(const char*,T&&...){return "unused-dump.tga";}}
namespace Latte {enum class E_GX2SURFFMT{RGBA8};enum class E_DIM{Array};enum class E_HWTILEMODE{Linear};}
namespace ActiveSettings {bool DumpTexturesEnabled(){return false;}fs::path GetUserDataPath(const char*){return {};}}
struct LatteTextureLoaderCtx {sint32 width=0,height=0,stepX=1,stepY=1,decodedTexelCountX=0,decodedTexelCountY=0;uint32 minOffsetOutdated=0,maxOffsetOutdated=0;bool dump=false;uint8*dumpRGBA=nullptr;};
void LatteTextureLoader_begin(LatteTextureLoaderCtx*c,uint32,uint32 mip,MPTR,MPTR,Latte::E_GX2SURFFMT,Latte::E_DIM,uint32 w,uint32 h,uint32,uint32,uint32,Latte::E_HWTILEMODE,uint32){c->width=std::max(w>>mip,1u);c->height=std::max(h>>mip,1u);c->maxOffsetOutdated=w*h*4;}
struct LatteTexture {struct{bool hasFormatOverwrite=false,hasResolutionOverwrite=false;}overwriteInfo;uint32 width=8,height=8,depth=3,mipLevels=2,texDataPtrLow=0,texDataPtrHigh=0;bool isDataDefined=false,isDepth=false,hasStencil=false,cpuUploadOccurred=false;Latte::E_GX2SURFFMT format{};unsigned allocations=0;void AllocateOnHost(){++allocations;}};
unsigned trackerResets=0,decodes=0;
void LatteTC_ResetTextureChangeTracker(LatteTexture*,bool){++trackerResets;}
void catchOpenGLError(){}uint8*LatteTextureLoader_GetInput(LatteTextureLoaderCtx*,sint32,sint32){static uint8 data[16];return data;}
void tga_write_rgba(const fs::path&,int,int,uint8*){}
struct TextureDecoder {int getTexelCountX(LatteTextureLoaderCtx*c){return c->width;}int getTexelCountY(LatteTextureLoaderCtx*c){return c->height;}uint32 calculateImageSize(LatteTextureLoaderCtx*c){return c->width*c->height*4;}void decode(LatteTextureLoaderCtx*c,uint8*p){++decodes;memset(p,0x4d,calculateImageSize(c));}void decodePixelToRGBA(uint8*,uint8*,int,int){}};
struct Backend {
 UploadHarness h;MTL::Texture*target;unsigned acquires=0,releases=0,clears=0,uploads=0;
 explicit Backend(MTL::Device*d):h(d){auto*desc=MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatRGBA8Unorm,8,8,true);desc->setTextureType(MTL::TextureType2DArray);desc->setArrayLength(3);desc->setMipmapLevelCount(2);desc->setStorageMode(MTL::StorageModeShared);desc->setUsage(MTL::TextureUsageShaderRead|MTL::TextureUsageRenderTarget);target=d->newTexture(desc);require(target,"target");std::vector<uint8>bytes(8*8*4,0x79);for(unsigned m=0;m<2;++m)for(unsigned s=0;s<3;++s)target->replaceRegion(MTL::Region(0,0,8>>m,8>>m),m,s,bytes.data(),(8>>m)*4,0);}
 ~Backend(){target->release();}
 TextureDecoder*texture_chooseDecodedFormat(Latte::E_GX2SURFFMT,bool,Latte::E_DIM,uint32,uint32){static TextureDecoder d;return &d;}
 void*texture_acquireTextureUploadBufferForTexture(uint32 bytes,LatteTexture*){++acquires;return h.memory.AcquireTextureUploadBuffer(bytes,true);}
 void texture_releaseTextureUploadBuffer(uint8*p){++releases;h.memory.ReleaseTextureUploadBuffer(p);}
 void texture_clearSlice(LatteTexture*,uint32 slice,uint32 mip){++clears;auto*pass=MTL::RenderPassDescriptor::alloc()->init();auto*c=pass->colorAttachments()->object(0);c->setTexture(target);c->setLevel(mip);c->setSlice(slice);c->setLoadAction(MTL::LoadActionClear);c->setStoreAction(MTL::StoreActionStore);c->setClearColor(MTL::ClearColor(0,0,0,0));auto*encoder=h.renderer.GetCommandBuffer()->renderCommandEncoder(pass);encoder->endEncoding();pass->release();}
 void texture_clearDepthSlice(LatteTexture*t,uint32 slice,uint32 mip,bool,bool,float,uint32){texture_clearSlice(t,slice,mip);}
 void texture_clearColorSlice(LatteTexture*t,uint32 slice,uint32 mip,float,float,float,float){texture_clearSlice(t,slice,mip);}
 void texture_loadSlice(LatteTexture*,int w,int height,int,void*data,int slice,int mip,uint32 bytes){++uploads;auto r=h.upload(data,bytes,w,height);auto*enc=h.renderer.GetCommandBuffer()->blitCommandEncoder();enc->copyFromBuffer(r.mtlBuffer,r.bufferOffset,w*4,0,MTL::Size(w,height,1),target,slice,mip,MTL::Origin(0,0,0));enc->endEncoding();}
};
Backend*g_renderer=nullptr;
PRODUCTION
uint64_t footprint(){task_vm_info_data_t v{};mach_msg_type_number_t n=TASK_VM_INFO_COUNT;require(task_info(mach_task_self(),TASK_VM_INFO,(task_info_t)&v,&n)==KERN_SUCCESS,"footprint");return v.phys_footprint;}
int main(){auto*pool=NS::AutoreleasePool::alloc()->init();auto*d=MTL::CreateSystemDefaultDevice();require(d,"device");
for(bool format:{false,true})for(bool resolution:{false,true})for(unsigned mip:{0u,1u})for(unsigned slice:{0u,2u}){
 Backend b(d);g_renderer=&b;LatteTexture tex;tex.overwriteInfo={format,resolution};const bool overridden=format||resolution;
 auto before=footprint();auto metalBefore=d->currentAllocatedSize();auto decodeBefore=decodes,resetBefore=trackerResets;
 LatteTextureLoader_UpdateTextureSliceData(&tex,slice,mip,1000,2000,Latte::E_DIM::Array,8,8,3,2,8,Latte::E_HWTILEMODE::Linear,0,false);
 require(tex.isDataDefined&&tex.allocations==1&&tex.texDataPtrLow==1000&&tex.texDataPtrHigh==1256&&trackerResets==resetBefore+1,"lifecycle/hash tracking");
 require((b.clears==1)==overridden&&(b.uploads==1)==!overridden&&(decodes==decodeBefore+1)==!overridden,"clear/decode selection");
 require(b.acquires==b.releases,"reservation lifecycle");
#if CHECK_NO_UNUSED_UPLOAD
 require(b.acquires==(overridden?0u:1u),"override must not allocate unused upload memory");
#endif
 auto*cb=b.h.renderer.GetCommandBuffer();cb->commit();cb->waitUntilCompleted();require(cb->status()==MTL::CommandBufferStatusCompleted,"Metal completion");b.h.allocator.CleanupBuffer(cb);
 for(unsigned m=0;m<2;++m)for(unsigned s=0;s<3;++s){std::vector<uint8>pixels((8>>m)*(8>>m)*4);b.target->getBytes(pixels.data(),(8>>m)*4,0,MTL::Region(0,0,8>>m,8>>m),m,s);for(auto p:pixels)require(p==((m==mip&&s==slice)?(overridden?0:0x4d):0x79),"changed or untouched GPU pixels");}
}
{Backend b(d);g_renderer=&b;LatteTexture tex;tex.width=4096;tex.height=2048;tex.overwriteInfo.hasResolutionOverwrite=true;auto before=footprint();auto metalBefore=d->currentAllocatedSize();LatteTextureLoader_UpdateTextureSliceData(&tex,0,0,1000,2000,Latte::E_DIM::Array,4096,2048,3,2,4096,Latte::E_HWTILEMODE::Linear,0,false);auto after=footprint();auto metalAfter=d->currentAllocatedSize();auto*cb=b.h.renderer.GetCommandBuffer();cb->commit();cb->waitUntilCompleted();require(cb->status()==MTL::CommandBufferStatusCompleted,"large override clear");b.h.allocator.CleanupBuffer(cb);printf("{\"acquires\":%u,\"footprintDeltaBytes\":%llu,\"metalDeltaBytes\":%zu}\n",b.acquires,(unsigned long long)(after-before),metalAfter-metalBefore);}
puts("PASS: production loader and real staging allocator, ordinary uploads/override clears, nonzero mips/slices, untouched GPU pixels and hash tracking");d->release();pool->release();}
'''.replace('PRODUCTION', function(loader,'void LatteTextureLoader_loadTextureDataIntoSlice(')+'\n'+function(loader,'void LatteTextureLoader_UpdateTextureSliceData('))
with tempfile.TemporaryDirectory(prefix='77u-override-upload-') as tmp:
    tmp=Path(tmp);(tmp/'test.mm').write_text(fixture)
    subprocess.run(['xcrun','clang++','-std=c++20','-O1','-fsanitize=address,undefined',
                    '-DCHECK_NO_UNUSED_UPLOAD='+str(int(a.source.resolve()==(repo/'src/Cafe/HW/Latte/Core/LatteTextureLoader.cpp').resolve())),
                    '-I'+str(repo/'src'),'-I'+str(repo/'dependencies/metal-cpp'),str(tmp/'test.mm'),
                    str(repo/'src/Cafe/HW/Latte/Renderer/Metal/MetalBCFallback.cpp'),
                    '-framework','Metal','-framework','Foundation','-o',str(tmp/'test')],check=True)
    subprocess.run([str(tmp/'test')],env=dict(os.environ,MTL_DEBUG_LAYER='1'),check=True,timeout=60)
