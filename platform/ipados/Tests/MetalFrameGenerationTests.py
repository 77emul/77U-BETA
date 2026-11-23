#!/usr/bin/env python3
"""Run the production FG shaders and buffer leases on a real Metal GPU."""
from pathlib import Path
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[3]
source = r'''
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION
#include "Cafe/HW/Latte/Renderer/Metal/MetalFrameGeneration.cpp"
#include <cassert>
#include <cstdio>
#include <future>
std::atomic<uint64_t> g_metalPresentCount{0};
static void check(bool ok,const char* text) {if(!ok){fprintf(stderr,"FAIL: %s\n",text);std::exit(1);}}
// FG_TEST_FAST: the optional cheaper mode (fast kernels, 8-pixel dense field).
#ifdef FG_TEST_FAST
#define FG_FLOW "fgFlowFast"
#define FG_REGULARIZE "fgRegularizeFast"
#define FG_CAMERA "fgCameraSearchFast"
#define FG_CELL 8
#else
#define FG_TEST_FAST 0
#define FG_FLOW "fgFlow"
#define FG_REGULARIZE "fgRegularize"
#define FG_CAMERA "fgCameraSearch"
#define FG_CELL 4
#endif
// FG_TEST_DECIDE_SCALE>0: production synthesis (fgDecide once per scale x scale
// output pixels, then fgInterpolateDecided) instead of the per-pixel reference.
#ifndef FG_TEST_DECIDE_SCALE
#define FG_TEST_DECIDE_SCALE 0
#endif
#ifndef FG_TEST_WIDTH
#define FG_TEST_WIDTH 320
#define FG_TEST_HEIGHT 180
#endif
constexpr unsigned W=FG_TEST_WIDTH,H=FG_TEST_HEIGHT,LW=std::min(W,320u),LH=(H*LW+W-1)/W;
using Bytes=std::vector<uint8_t>;
static float pattern(float x,float y,int channel) {
 return .48f+.17f*std::sin(x*.17f+channel*.9f)+.13f*std::cos(y*.13f+channel*.7f)+.08f*std::sin(x*.41f+y*.31f+channel);
}
static Bytes image(float dx,float dy) {
 Bytes bytes(W*H*4);
 for(unsigned y=0;y<H;++y)for(unsigned x=0;x<W;++x){
  for(int c=0;c<3;++c)bytes[(y*W+x)*4+c]=uint8_t(std::clamp(pattern(x-dx,y-dy,c),0.f,1.f)*255+.5f);
  bytes[(y*W+x)*4+3]=255;
 }
 return bytes;
}
static MetalPtr<MTL::Texture> upload(MTL::Device* d,MTL::CommandQueue* q,const Bytes& data,bool srgb=false) {
 auto result=Texture(d,W,H,srgb?MTL::PixelFormatBGRA8Unorm_sRGB:MTL::PixelFormatBGRA8Unorm,MTL::TextureUsageShaderRead|MTL::TextureUsageRenderTarget);
 auto buffer=Own(d->newBuffer(data.data(),data.size(),MTL::ResourceStorageModeShared));
 auto cb=q->commandBuffer();auto blit=cb->blitCommandEncoder();
 blit->copyFromBuffer(buffer.get(),0,W*4,W*H*4,MTL::Size(W,H,1),result.get(),0,0,MTL::Origin(0,0,0));
 blit->endEncoding();cb->commit();cb->waitUntilCompleted();check(!cb->error(),"upload");return result;
}
static Bytes download(MTL::Device* d,MTL::CommandQueue* q,MTL::Texture* source) {
 auto b=Own(d->newBuffer(W*H*4,MTL::ResourceStorageModeShared));
 auto cb=q->commandBuffer();auto blit=cb->blitCommandEncoder();
 blit->copyFromTexture(source,0,0,MTL::Origin(0,0,0),MTL::Size(W,H,1),b.get(),0,W*4,W*H*4);
 blit->endEncoding();cb->commit();cb->waitUntilCompleted();check(!cb->error(),"readback");
 auto* p=static_cast<const uint8_t*>(b->contents());return Bytes(p,p+W*H*4);
}
static double mse(const Bytes& a,const Bytes& b) {
 double err=0;unsigned n=0;
 for(unsigned y=24;y<H-24;++y)for(unsigned x=24;x<W-24;++x)for(int c=0;c<3;++c){
  const double delta=int(a[(y*W+x)*4+c])-int(b[(y*W+x)*4+c]);err+=delta*delta;++n;
 }
 return err/n;
}
int main() {
 auto* autorelease=NS::AutoreleasePool::alloc()->init();
 auto device=Own(MTL::CreateSystemDefaultDevice());check(bool(device),"requires a real Metal GPU");
 auto queue=Own(device->newCommandQueue());NS::Error* error=nullptr;
 auto library=Own(device->newLibrary(NS::String::string(frameGenerationShaderSource,NS::UTF8StringEncoding),nullptr,&error));
 if(error)fprintf(stderr,"%s\n",error->localizedDescription()->utf8String());check(bool(library),"production shader compilation");
 auto function=[&](const char* name){return Own(library->newFunction(NS::String::string(name,NS::UTF8StringEncoding)));};
 auto wf=function("fgWideLocal");auto widePipeline=Own(device->newComputePipelineState(wf.get(),&error));
 auto upsampleFn=function("fgUpsample");auto upsamplePipeline=Own(device->newComputePipelineState(upsampleFn.get(),&error));
 auto projectFn=function("fgProject");auto projectPipeline=Own(device->newComputePipelineState(projectFn.get(),&error));
 auto pf=function("fgPropagate");auto propagatePipeline=Own(device->newComputePipelineState(pf.get(),&error));
 auto cs=function(FG_CAMERA);auto cameraSearchPipeline=Own(device->newComputePipelineState(cs.get(),&error));
 auto ms=function("fgModelSeeds");auto modelSeedPipeline=Own(device->newComputePipelineState(ms.get(),&error));
 auto mf=function("fgModelCandidates");
 auto modelPipeline=Own(device->newComputePipelineState(mf.get(),&error));
 auto lf=function("fgLuma"),ff=function(FG_FLOW),gf=function("fgConsensus"),rf=function(FG_REGULARIZE),vf=function("fgVertex"),sf=function("fgInterpolate");
 auto auditFunction=function("fgAudit");auto auditPipeline=Own(device->newComputePipelineState(auditFunction.get(),&error));
 auto auditBuffer=Own(device->newBuffer(8*sizeof(uint32_t),MTL::ResourceStorageModeShared));
 std::array<uint32_t,8> lastAudit{};
 auto luma=Own(device->newComputePipelineState(lf.get(),&error));
 auto flow=Own(device->newComputePipelineState(ff.get(),&error));
 auto consensus=Own(device->newComputePipelineState(gf.get(),&error));
 auto regularize=Own(device->newComputePipelineState(rf.get(),&error));
 auto pd=Own(MTL::RenderPipelineDescriptor::alloc()->init());pd->setVertexFunction(vf.get());pd->setFragmentFunction(sf.get());
 pd->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormatBGRA8Unorm);
 auto render=Own(device->newRenderPipelineState(pd.get(),&error));
 pd->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormatBGRA8Unorm_sRGB);
 auto srgbRender=Own(device->newRenderPipelineState(pd.get(),&error));
 if(FG_TEST_DECIDE_SCALE){
  auto df=function("fgInterpolateDecided");pd->setFragmentFunction(df.get());
  pd->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormatBGRA8Unorm);render=Own(device->newRenderPipelineState(pd.get(),&error));
  pd->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormatBGRA8Unorm_sRGB);srgbRender=Own(device->newRenderPipelineState(pd.get(),&error));
 }
 auto decideFunction=function("fgDecide");auto decidePipeline=Own(device->newComputePipelineState(decideFunction.get(),&error));
 const unsigned DS=FG_TEST_DECIDE_SCALE?FG_TEST_DECIDE_SCALE:1,DW=(W+DS-1)/DS,DH=(H+DS-1)/DS;
 auto decisionOffsets=Texture(device.get(),DW,DH,MTL::PixelFormatRGBA32Float,MTL::TextureUsageShaderRead|MTL::TextureUsageShaderWrite);
 auto decisionModes=Texture(device.get(),DW,DH,MTL::PixelFormatR8Uint,MTL::TextureUsageShaderRead|MTL::TextureUsageShaderWrite);
 check(decidePipeline&&decisionOffsets&&decisionModes,"decision pipeline");
 auto seedFunction=function("fgStaticSeed");auto seedPipeline=Own(device->newComputePipelineState(seedFunction.get(),&error));
 auto overlayMask=Own(device->newBuffer(DW*DH,MTL::ResourceStorageModePrivate));
 const struct{uint32_t width,height;} maskSize{DW,DH};
 check(seedPipeline&&overlayMask,"HUD mask pipeline");
 check(cameraSearchPipeline&&modelSeedPipeline&&upsamplePipeline&&projectPipeline&&widePipeline&&propagatePipeline&&luma&&flow&&modelPipeline&&consensus&&regularize&&render&&auditPipeline&&auditBuffer,"production pipelines");
 std::array<MetalPtr<MTL::Texture>,2> luminance,raw,rawInitial,field,dominant,models,modelSeeds,proposals,initial,resolved;
 for(unsigned i=0;i<2;++i){
  auto usage=MTL::TextureUsageShaderRead|MTL::TextureUsageShaderWrite;
  luminance[i]=Texture(device.get(),LW,LH,MTL::PixelFormatR16Float,usage);
  rawInitial[i]=Texture(device.get(),(LW+7)/8,(LH+7)/8,MTL::PixelFormatRGBA32Float,usage);
  raw[i]=Texture(device.get(),(LW+7)/8,(LH+7)/8,MTL::PixelFormatRGBA16Float,usage);
  field[i]=Texture(device.get(),(LW+FG_CELL-1)/FG_CELL,(LH+FG_CELL-1)/FG_CELL,MTL::PixelFormatRGBA16Float,usage);
  dominant[i]=Texture(device.get(),3,1,MTL::PixelFormatRGBA16Float,usage);
  initial[i]=Texture(device.get(),(LW+FG_CELL-1)/FG_CELL,(LH+FG_CELL-1)/FG_CELL,MTL::PixelFormatRGBA16Float,usage);
  proposals[i]=Texture(device.get(),(LW+15)/16,(LH+15)/16,MTL::PixelFormatRGBA16Float,usage);
  resolved[i]=Texture(device.get(),LW,LH,MTL::PixelFormatRGBA16Float,usage);
  modelSeeds[i]=Texture(device.get(),3,384,MTL::PixelFormatRGBA32Float,usage);
  models[i]=Texture(device.get(),3,384,MTL::PixelFormatRGBA16Float,usage);
 }
 auto output=Texture(device.get(),W,H,MTL::PixelFormatBGRA8Unorm,MTL::TextureUsageRenderTarget|MTL::TextureUsageShaderRead);
 auto srgbOutput=Texture(device.get(),W,H,MTL::PixelFormatBGRA8Unorm_sRGB,MTL::TextureUsageRenderTarget|MTL::TextureUsageShaderRead);
 auto compute=[&](MTL::CommandBuffer* cb,MTL::ComputePipelineState* pipeline,MTL::Texture* a,MTL::Texture* b,MTL::Texture* c,unsigned w,unsigned h,MTL::Texture* hint=nullptr,MTL::Texture* other=nullptr,MTL::Texture* fullA=nullptr,MTL::Texture* fullB=nullptr){
  auto e=cb->computeCommandEncoder();e->setComputePipelineState(pipeline);e->setTexture(a,0);e->setTexture(b,1);if(c)e->setTexture(c,2);
  if(hint)e->setTexture(hint,3);if(other)e->setTexture(other,4);
  if(fullA)e->setTexture(fullA,5);if(fullB)e->setTexture(fullB,6);
  if(pipeline==cameraSearchPipeline.get()||pipeline==widePipeline.get()||pipeline==modelPipeline.get()||pipeline==flow.get()||pipeline==regularize.get()||pipeline==propagatePipeline.get())e->dispatchThreadgroups(MTL::Size(w,h,1),MTL::Size(pipeline==cameraSearchPipeline.get()?256:64,1,1));
  else e->dispatchThreads(MTL::Size(w,h,1),h==1?MTL::Size(64,1,1):MTL::Size(8,8,1));e->endEncoding();
 };
 std::array<MetalPtr<MTL::Buffer>,2> projected;
 for(auto& b:projected){b=Own(device->newBuffer(LW*LH*sizeof(uint32_t),MTL::ResourceStorageModePrivate));check(bool(b),"projection buffer allocation");}
 bool knownSilhouette=false;float knownCamera=0,knownObject=0;
 double gpuTotal=0,gpuMax=0;unsigned gpuSamples=0;
 auto interpolate=[&](const Bytes& previous,const Bytes& current,float t,bool srgb=false,float zoom=0){
  auto a=upload(device.get(),queue.get(),previous,srgb),b=upload(device.get(),queue.get(),current,srgb);
  auto target=srgb?srgbOutput:output;
  auto cb=queue->commandBuffer();
  if(knownSilhouette){
   for(unsigned direction=0;direction<2;++direction){
    std::array<_Float16,12> camera{};camera[0]=(direction?-1:1)*knownCamera*LW/W;camera[2]=1;
    auto model=Own(device->newBuffer(camera.data(),sizeof(camera),MTL::ResourceStorageModeShared));
    auto modelBlit=cb->blitCommandEncoder();modelBlit->copyFromBuffer(model.get(),0,24,24,MTL::Size(3,1,1),dominant[direction].get(),0,0,MTL::Origin(0,0,0));modelBlit->endEncoding();
    unsigned fw=unsigned(field[direction]->width()),fh=unsigned(field[direction]->height());
    std::vector<_Float16> values(fw*fh*4);
    for(unsigned y=0;y<fh;++y)for(unsigned x=0;x<fw;++x){
     float px=(x+.5f)*W/fw,py=(y+.5f)*H/fh;
     float left=W*.45f+(direction?knownObject:0);
     bool body=px>=left&&px<left+56&&py>=H/3&&py<H*2/3;
     unsigned off=(y*fw+x)*4;
     values[off]=(direction?-1:1)*(body?knownObject:knownCamera)*LW/W;
     values[off+1]=0;values[off+2]=1;values[off+3]=0;
    }
    auto data=Own(device->newBuffer(values.data(),values.size()*sizeof(_Float16),MTL::ResourceStorageModeShared));
    auto blit=cb->blitCommandEncoder();blit->copyFromBuffer(data.get(),0,fw*8,fw*fh*8,MTL::Size(fw,fh,1),field[direction].get(),0,0,MTL::Origin(0,0,0));blit->endEncoding();
   }
  }else if(zoom!=0){
   for(unsigned direction=0;direction<2;++direction){
    const unsigned fw=unsigned(field[direction]->width()),fh=unsigned(field[direction]->height());
    const float modelAmount=direction?-zoom/(1+zoom):zoom;
    std::array<_Float16,12> camera{};camera[0]=-modelAmount*.5f*LW/W;camera[1]=-modelAmount*.5f*LH/H;
    camera[2]=1;camera[4]=modelAmount*LW;camera[9]=modelAmount*LH;
    auto model=Own(device->newBuffer(camera.data(),sizeof(camera),MTL::ResourceStorageModeShared));
    auto modelBlit=cb->blitCommandEncoder();modelBlit->copyFromBuffer(model.get(),0,24,24,MTL::Size(3,1,1),dominant[direction].get(),0,0,MTL::Origin(0,0,0));modelBlit->endEncoding();
    std::vector<_Float16> values(fw*fh*4);
    for(unsigned y=0;y<fh;++y)for(unsigned x=0;x<fw;++x){
     const float px=(x+.5f)*W/fw-.5f,py=(y+.5f)*H/fh-.5f;
     const float amount=direction?-zoom/(1+zoom):zoom;
     const unsigned offset=(y*fw+x)*4;
     values[offset]=amount*(px-W*.5f)*LW/W;
     values[offset+1]=amount*(py-H*.5f)*LH/H;
     values[offset+2]=1;values[offset+3]=0;
    }
    auto bytes=Own(device->newBuffer(values.data(),values.size()*sizeof(_Float16),MTL::ResourceStorageModeShared));
    auto blit=cb->blitCommandEncoder();blit->copyFromBuffer(bytes.get(),0,fw*8,fw*fh*8,MTL::Size(fw,fh,1),field[direction].get(),0,0,MTL::Origin(0,0,0));blit->endEncoding();
   }
  }else{
  compute(cb,luma.get(),a.get(),luminance[0].get(),nullptr,LW,LH);
  compute(cb,luma.get(),b.get(),luminance[1].get(),nullptr,LW,LH);
  for(unsigned i=0;i<2;++i){
   compute(cb,flow.get(),luminance[i].get(),luminance[1-i].get(),rawInitial[i].get(),(LW+7)/8,(LH+7)/8,nullptr,nullptr,i?b.get():a.get(),i?a.get():b.get());
   compute(cb,cameraSearchPipeline.get(),rawInitial[i].get(),luminance[i].get(),raw[i].get(),(LW+7)/8,(LH+7)/8,nullptr,nullptr,i?b.get():a.get(),i?a.get():b.get());
   compute(cb,modelSeedPipeline.get(),raw[i].get(),modelSeeds[i].get(),nullptr,384,1);
   compute(cb,modelPipeline.get(),modelSeeds[i].get(),models[i].get(),raw[i].get(),384,1,luminance[i].get(),nullptr,i?b.get():a.get(),i?a.get():b.get());
  }
  compute(cb,consensus.get(),models[0].get(),models[1].get(),dominant[0].get(),1,1,dominant[1].get(),luminance[0].get());
  for(unsigned i=0;i<2;++i){
   compute(cb,regularize.get(),raw[i].get(),luminance[i].get(),initial[i].get(),(LW+FG_CELL-1)/FG_CELL,(LH+FG_CELL-1)/FG_CELL,dominant[i].get(),luminance[1-i].get(),i?b.get():a.get(),i?a.get():b.get());
  }
  for(unsigned i=0;i<2;++i)
   compute(cb,widePipeline.get(),initial[i].get(),luminance[i].get(),proposals[i].get(),(LW+15)/16,(LH+15)/16,dominant[i].get(),nullptr,i?b.get():a.get(),i?a.get():b.get());
  for(unsigned i=0;i<2;++i)
   compute(cb,propagatePipeline.get(),proposals[i].get(),luminance[i].get(),field[i].get(),(LW+FG_CELL-1)/FG_CELL,(LH+FG_CELL-1)/FG_CELL,dominant[i].get(),initial[i].get(),i?b.get():a.get(),i?a.get():b.get());
  }
  for(unsigned i=0;i<2;++i)
   compute(cb,upsamplePipeline.get(),field[i].get(),luminance[i].get(),resolved[i].get(),LW,LH,nullptr,nullptr,i?b.get():a.get(),i?a.get():b.get());
  struct{float inv[2];float t,pad;} projectionParams{{1.f/LW,1.f/LH},t,0};
  auto clear=cb->blitCommandEncoder();
  for(auto& map:projected)clear->fillBuffer(map.get(),NS::Range(0,LW*LH*sizeof(uint32_t)),0);clear->endEncoding();
  for(unsigned i=0;i<2;++i){
   auto e=cb->computeCommandEncoder();e->setComputePipelineState(projectPipeline.get());
   e->setTexture(i?b.get():a.get(),0);e->setTexture(i?a.get():b.get(),1);
   e->setTexture(resolved[i].get(),2);e->setTexture(resolved[1-i].get(),3);e->setTexture(dominant[i].get(),4);
   projectionParams.t=i?1-t:t;e->setBytes(&projectionParams,sizeof(projectionParams),0);e->setBuffer(projected[i].get(),0,1);
   e->dispatchThreads(MTL::Size(LW,LH,1),MTL::Size(8,8,1));e->endEncoding();
  }
  {
   auto fill=cb->blitCommandEncoder();fill->fillBuffer(overlayMask.get(),NS::Range(0,DW*DH),0);fill->endEncoding();
   auto m=cb->computeCommandEncoder();m->setComputePipelineState(seedPipeline.get());
   m->setTexture(a.get(),0);m->setTexture(b.get(),1);m->setBuffer(overlayMask.get(),0,0);m->setBytes(&maskSize,sizeof(maskSize),1);
   m->dispatchThreads(MTL::Size(DW,DH,1),MTL::Size(8,8,1));m->endEncoding();
  }
  if(FG_TEST_DECIDE_SCALE){
   auto d=cb->computeCommandEncoder();d->setComputePipelineState(decidePipeline.get());d->setBuffer(overlayMask.get(),0,3);
   d->setTexture(a.get(),0);d->setTexture(b.get(),1);d->setTexture(resolved[0].get(),2);d->setTexture(resolved[1].get(),3);
   d->setTexture(decisionOffsets.get(),4);d->setTexture(decisionModes.get(),5);
   struct{float inv[2];float t,pad;} dp{{1.f/LW,1.f/LH},t,0};d->setBytes(&dp,sizeof(dp),0);
   d->setBuffer(projected[0].get(),0,1);d->setBuffer(projected[1].get(),0,2);
   d->dispatchThreads(MTL::Size(DW,DH,1),MTL::Size(8,8,1));d->endEncoding();
  }
  auto pass=Own(MTL::RenderPassDescriptor::alloc()->init());pass->colorAttachments()->object(0)->setTexture(target.get());
  pass->colorAttachments()->object(0)->setLoadAction(MTL::LoadActionDontCare);pass->colorAttachments()->object(0)->setStoreAction(MTL::StoreActionStore);
  auto e=cb->renderCommandEncoder(pass.get());e->setRenderPipelineState(srgb?srgbRender.get():render.get());
  e->setFragmentTexture(a.get(),0);e->setFragmentTexture(b.get(),1);
  if(FG_TEST_DECIDE_SCALE){e->setFragmentTexture(decisionOffsets.get(),2);e->setFragmentTexture(decisionModes.get(),3);}
  else{e->setFragmentTexture(resolved[0].get(),2);e->setFragmentTexture(resolved[1].get(),3);e->setFragmentBuffer(overlayMask.get(),0,3);e->setFragmentBytes(&maskSize,sizeof(maskSize),4);}
  struct{float inv[2];float t,pad;} params{{1.f/LW,1.f/LH},t,0};e->setFragmentBytes(&params,sizeof(params),0);
  e->setFragmentBuffer(projected[0].get(),0,1);e->setFragmentBuffer(projected[1].get(),0,2);
  e->drawPrimitives(MTL::PrimitiveTypeTriangle,NS::UInteger(0),NS::UInteger(3));e->endEncoding();
  std::fill_n(static_cast<uint32_t*>(auditBuffer->contents()),8,0u);
  auto diagnostic=cb->computeCommandEncoder();diagnostic->setComputePipelineState(auditPipeline.get());
  diagnostic->setTexture(a.get(),0);diagnostic->setTexture(b.get(),1);
  diagnostic->setTexture(resolved[0].get(),2);diagnostic->setTexture(resolved[1].get(),3);
  diagnostic->setBuffer(projected[0].get(),0,2);diagnostic->setBuffer(projected[1].get(),0,3);
  diagnostic->setBytes(&params,sizeof(params),0);diagnostic->setBuffer(auditBuffer.get(),0,1);
  diagnostic->dispatchThreads(MTL::Size(32,18,1),MTL::Size(8,8,1));diagnostic->endEncoding();
  cb->commit();cb->waitUntilCompleted();
  std::copy_n(static_cast<const uint32_t*>(auditBuffer->contents()),8,lastAudit.begin());
  uint32_t audited=0;for(auto count:lastAudit)audited+=count;check(audited==576,"GPU audit accounts for every sampled pixel");
  if(cb->error())fprintf(stderr,"%s\n",cb->error()->localizedDescription()->utf8String());
  const double ms=(cb->GPUEndTime()-cb->GPUStartTime())*1000;
  gpuTotal+=ms;gpuMax=std::max(gpuMax,ms);++gpuSamples;
  check(!cb->error(),"flow/render GPU validation");return download(device.get(),queue.get(),target.get());
 };
 std::vector<std::array<float,2>> motions{{8,0},{0,6},{6,-6}};
 if(W>320)motions.insert(motions.end(),{{.5,0},{1,0},{2,0},{5,0},{3.5,1.5},{10,-6},{24,16},{40,-24},{64,0},{96,-24}});
 printf("Production flow at %ux%u output, %ux%u matching grid\n",W,H,LW,LH);
 for(auto motion: motions){
  auto a=image(0,0),b=image(motion[0],motion[1]);
  for(float t:{.25f,.5f,.75f}){
   auto generated=interpolate(a,b,t);auto expected=image(motion[0]*t,motion[1]*t);
   const double native=mse(b,expected),actual=mse(generated,expected);
   printf("Motion (%g,%g), t=%g: generated MSE %.2f, repeated-current MSE %.2f\n",motion[0],motion[1],t,actual,native);
   check(actual<native*.6,"generated camera position must improve over frame duplication");
  }
  check(mse(interpolate(a,b,0),a)==0,"t=0 preserves original pixels");
  check(mse(interpolate(a,b,1),b)==0,"t=1 preserves original pixels");
 }
 if(W>320){
 auto zoomImage=[&](float zoom){
  Bytes pixels(W*H*4);
  for(unsigned y=0;y<H;++y)for(unsigned x=0;x<W;++x){
   float px=W*.5f+(x-W*.5f)/(1+zoom),py=H*.5f+(y-H*.5f)/(1+zoom);
   for(unsigned c=0;c<3;++c)pixels[(y*W+x)*4+c]=uint8_t(std::clamp(pattern(px,py,c),0.f,1.f)*255+.5f);
   pixels[(y*W+x)*4+3]=255;
  }return pixels;
 };
 for(float zoom:{.04f,.12f,.2f})for(float t:{.25f,.5f,.75f}){
  auto a=zoomImage(0),b=zoomImage(zoom),truth=zoomImage(zoom*t);
  auto generated=interpolate(a,b,t,false,zoom);
  printf("Exact affine flow: zoom %.2f t %.2f generated MSE %.2f vs repeat %.2f; moved %u, color rejects %u\n",zoom,t,mse(generated,truth),mse(t<.5?a:b,truth),lastAudit[0]+lastAudit[7],lastAudit[4]);
  // Fast mode: a coarser field only has to beat repeating a source frame by far.
  check(FG_TEST_FAST ? mse(generated,truth)<.15*mse(t<.5?a:b,truth) : mse(generated,truth)<1.0,"spatially varying exact flow must reproduce the intermediate camera position");
  check(lastAudit[0]+lastAudit[7]>lastAudit[2]+lastAudit[3]+lastAudit[4],"known correct motion cannot mostly fall back to original pixels");
 }
 }

 if(W>320){
 // Real flow estimation, not injected perfect vectors: different depths can
 // move by very different amounts in one camera frame, including sign changes.
 auto parallaxImage=[&](float t,float lo,float hi){
  Bytes pixels(W*H*4);
  for(unsigned y=0;y<H;++y)for(unsigned x=0;x<W;++x){
   const float shift=lo+(hi-lo)*float(y)/H;
   for(unsigned c=0;c<3;++c)pixels[(y*W+x)*4+c]=uint8_t(std::clamp(pattern(x-t*shift,y,c),0.f,1.f)*255+.5f);
   pixels[(y*W+x)*4+3]=255;
  }return pixels;
 };
 for(auto range:{std::array<float,2>{8,40},{24,56},{48,96},{-48,48}}){
  auto a=parallaxImage(0,range[0],range[1]),b=parallaxImage(1,range[0],range[1]);
  for(float t:{.25f,.5f,.75f}){
   auto truth=parallaxImage(t,range[0],range[1]),generated=interpolate(a,b,t);
   const double repeated=mse(t<.5f?a:b,truth),actual=mse(generated,truth);
   printf("Estimated parallax %g..%g t %.2f generated MSE %.2f repeat %.2f moved %u rejected motion %u color %u\n",range[0],range[1],t,actual,repeated,lastAudit[0]+lastAudit[7],lastAudit[2],lastAudit[4]);
   check(actual<repeated*.3,"estimated perspective motion must improve over repeated original pixels");
   check(lastAudit[0]+lastAudit[7]>lastAudit[2]+lastAudit[3]+lastAudit[4],"perspective motion cannot mostly fall back to source pixels");
  }
 }
 }
 if(W>320){
 auto character=[&](float t,float camera,float movement,unsigned width){
  auto pixels=image(camera*t,0);
  const float left=W*.45f+movement*t;
  for(unsigned y=H/3;y<H*2/3;++y)for(unsigned x=0;x<W;++x){
   const float xx=x-left;
   if(xx<0||xx>=width) continue;
   for(unsigned c=0;c<3;++c){
    float color=.12f+(.1f+c*.08f)*(1.0f+std::sin(xx*.23f+y*.19f+c));
    pixels[(y*W+x)*4+c]=uint8_t(std::clamp(color,0.f,1.f)*255+.5f);
   }
  }return pixels;
 };
 for(auto motion:{std::array<float,2>{8,-24},{32,-32},{64,-48},{-8,24},{-32,32},{-64,48}}){
  for(bool exact:{false,true})for(float t:{.25f,.5f,.75f}){
  knownSilhouette=exact;knownCamera=motion[0];knownObject=motion[1];
  unsigned width=56;
  auto a=character(0,motion[0],motion[1],width),b=character(1,motion[0],motion[1],width),truth=character(t,motion[0],motion[1],width);
  auto generated=interpolate(a,b,t);
  double err=0,repeated=0;unsigned count=0;
  unsigned left=unsigned(W*.45f+motion[1]*t);
  for(unsigned y=H/3+12;y<H*2/3-12;++y)for(unsigned x=left+12;x<left+width-12;++x)for(unsigned c=0;c<3;++c){
   double d=int(generated[(y*W+x)*4+c])-int(truth[(y*W+x)*4+c]);err+=d*d;
   d=int((t<.5f?a:b)[(y*W+x)*4+c])-int(truth[(y*W+x)*4+c]);repeated+=d*d;++count;
  }
  check(err<repeated*.65,"small fast character must improve over source-frame repetition");
  if(exact)check(err/count<1.0,"known silhouette motion must not resolve to the crossing background");
  printf("%s character camera %g object %g t %.2f: MSE %.2f repeat %.2f moved %u rejected %u\n",exact?"Known":"Estimated",motion[0],motion[1],t,err/count,repeated/count,lastAudit[0],lastAudit[2]+lastAudit[4]);
  }
 }
 }
 knownSilhouette=false;
 auto still=image(0,0);check(mse(interpolate(still,still,.5),still)==0,"static imagery remains exact");
 check(lastAudit[1]==576,"static imagery audit does not claim motion");
 // A shared camera estimate must not drag a character moving the other way.
 auto layered=[&](int background,int object){
  auto pixels=image(background,0);
  for(unsigned y=H/3;y<2*H/3;++y)for(int x=int(W/3)+object;x<int(W/3+W/4)+object;++x){
   const float texture=.45f+.22f*std::sin((x-object)*.23f+y*.19f);
   pixels[(y*W+x)*4]=uint8_t(texture*80);
   pixels[(y*W+x)*4+1]=uint8_t(texture*140);
   pixels[(y*W+x)*4+2]=uint8_t(texture*255);
   pixels[(y*W+x)*4+3]=255;
  }
  return pixels;
 };
 auto layerA=layered(0,0),layerB=layered(8,-8),layerExpected=layered(4,-4);
 auto layerGenerated=interpolate(layerA,layerB,.5);
 auto objectError=[&](const Bytes& pixels){
  double error=0;unsigned count=0,margin=std::max(12u,W/32);
  for(unsigned y=H/3+margin;y<2*H/3-margin;++y)
   for(unsigned x=W/3+margin;x<W/3+W/4-margin;++x)for(unsigned c=0;c<3;++c){
    double d=int(pixels[(y*W+x)*4+c])-int(layerExpected[(y*W+x)*4+c]);error+=d*d;++count;
   }
  return error/count;
 };
 printf("Opposing foreground/camera: generated MSE %.2f, repeated MSE %.2f\n",objectError(layerGenerated),objectError(layerB));
 check(objectError(layerGenerated)<objectError(layerB)*.6,"camera consensus preserves independent foreground motion");
 auto colorMotion=image(8,0);
 check(mse(interpolate(still,colorMotion,0,true),still)<.6,"sRGB t=0 preserves source color");
 check(mse(interpolate(still,colorMotion,1,true),colorMotion)<.6,"sRGB t=1 preserves source color");
 check(mse(interpolate(still,still,.5,true),still)<.6,"sRGB static image avoids double gamma");
 auto moved=image(8,0);
 for(unsigned y=40;y<60;++y)for(unsigned x=40;x<100;++x)for(int c=0;c<4;++c)still[(y*W+x)*4+c]=moved[(y*W+x)*4+c]=255;
 auto hud=interpolate(still,moved,.5);
 for(unsigned y=42;y<58;++y)for(unsigned x=42;x<98;++x)for(int c=0;c<4;++c)check(hud[(y*W+x)*4+c]==255,"stationary HUD remains exact");
 // The background moves under the HUD: no displaced copy of the HUD box may
 // appear beside it (the moving pattern itself never reaches near-white).
 unsigned ghosts=0;
 for(unsigned y=40;y<60;++y)for(unsigned x:{36u,37u,38u,39u,100u,101u,102u,103u}){
  const uint8_t* px=&hud[(y*W+x)*4];ghosts+=px[0]>240&&px[1]>240&&px[2]>240;
 }
 printf("HUD over moving background: %u displaced HUD pixels\n",ghosts);
 {
  // Semi-transparent HUD icon (70 % white) over a background moving 8 px:
  // the icon must stay in place; compare with the true t=.5 composite.
  auto bgA=image(0,0),bgB=image(8,0),bgT=image(4,0);
  // A structured icon: opaque 4-px white bars with 50 % soft edges.
  auto overlay=[&](Bytes& im){for(unsigned y=120;y<150;++y)for(unsigned x=200;x<260;++x){
   const unsigned phase=(x-200)%8;const float alpha=phase<4?1.f:(phase==4||phase==7)?.5f:0.f;
   for(int c=0;c<3;++c){uint8_t& v=im[(y*W+x)*4+c];v=uint8_t(v*(1-alpha)+255*alpha+.5f);}}};
  overlay(bgA);overlay(bgB);overlay(bgT);
  auto out=interpolate(bgA,bgB,.5);
  double err=0;unsigned n=0;
  for(unsigned y=116;y<154;++y)for(unsigned x=196;x<264;++x)for(int c=0;c<3;++c){double d=double(out[(y*W+x)*4+c])-bgT[(y*W+x)*4+c];err+=d*d;++n;}
  double repeat=0;for(unsigned y=116;y<154;++y)for(unsigned x=196;x<264;++x)for(int c=0;c<3;++c){double d=double(bgB[(y*W+x)*4+c])-bgT[(y*W+x)*4+c];repeat+=d*d;}
  printf("Semi-transparent HUD: generated MSE %.2f, repeated MSE %.2f\n",err/n,repeat/n);
  check(err/n<repeat/n*1.5,"semi-transparent HUD icon does not tear over a moving background");
 }
 check(ghosts<=4,"moving background does not copy a stationary HUD"); // a copied box gave 42; edge blends allowed
 Bytes cutA(W*H*4,0),cutB(W*H*4,0);
 for(unsigned i=0;i<W*H;++i){cutA[i*4]=255;cutA[i*4+3]=255;cutB[i*4+2]=255;cutB[i*4+3]=255;}
 check(mse(interpolate(cutA,cutB,.5),cutB)==0,"scene cut avoids melting or crossfade");
 // Exercise the production pool: all six leased slots block reuse until an
 // owner releases one. This is the same lease held by completion callbacks.
 auto pool=std::make_shared<FramePool>();
 for(auto& t:pool->textures)t=Texture(device.get(),16,16,MTL::PixelFormatBGRA8Unorm,MTL::TextureUsageShaderRead);
 std::vector<std::shared_ptr<Image>> owners;
 for(unsigned i=0;i<6;++i)owners.push_back(pool->Acquire());
 auto waiter=std::async(std::launch::async,[pool]{return pool->Acquire();});
 check(waiter.wait_for(std::chrono::milliseconds(20))==std::future_status::timeout,"live leases cannot be overwritten");
 const unsigned freeSlot=owners[2]->slot;owners[2].reset();
 check(waiter.wait_for(std::chrono::seconds(2))==std::future_status::ready,"released slot wakes producer");
 auto acquired=waiter.get();check(acquired->slot==freeSlot,"only the released slot is reused");
 // Controller starts/stops and reallocates with no view/tick dependency.
 auto layer=Retain(CA::MetalLayer::layer());layer->setDevice(device.get());layer->setDrawableSize(CGSize{W,H});
 {
  MetalFrameGeneration controller(device.get());
  for(unsigned i=0;i<30;++i){
   auto target=controller.Begin(layer.get(),i%2?W:W/2,H,MTL::PixelFormatBGRA8Unorm);
   check(target,"controller initialization/size change");
   auto cb=queue->commandBuffer();auto blit=cb->blitCommandEncoder();blit->endEncoding();cb->commit();
   controller.Submit(cb,60);controller.Reset();
  }
  check(controller.PresentedFPS()==0,"submission does not invent displayed FPS");
 }
 double sourceRate=-1,displayRate=-1;
 CemuMetal_GetFrameGenerationRates(&sourceRate,&displayRate);
 check(sourceRate==0&&displayRate==0,"inactive FG has no source/output FPS");
 printf("GPU flow + interpolation (test Mac): average %.2f ms, max %.2f ms\n",gpuTotal/gpuSamples,gpuMax);
 puts("PASS: production GPU flow, arbitrary interpolation, exact endpoints/static HUD, scene cut, bounded image leases and controller resets");
 autorelease->release();
}
'''

with tempfile.TemporaryDirectory(prefix="cemu-fg-gpu-") as temp:
    path=Path(temp)
    (path/'test.cpp').write_text(source)
    # decide=2: the Retina production synthesis path (one warp decision per 2x2 pixels).
    for width,height,fast,decide in [(320,180,0,0),(1280,720,0,0),(1280,589,0,0),(1280,720,1,0),(1280,720,0,2),(1280,720,1,2)]:
        subprocess.run(['xcrun','clang++','-std=c++20','-fblocks','-O1',
                        f'-DFG_TEST_WIDTH={width}',f'-DFG_TEST_HEIGHT={height}',*(['-DFG_TEST_FAST=1'] if fast else []),
                        f'-DFG_TEST_DECIDE_SCALE={decide}',
                        '-I'+str(repo/'src'),'-I'+str(repo/'dependencies/metal-cpp'),str(path/'test.cpp'),
                        '-framework','Foundation','-framework','QuartzCore','-framework','Metal',
                        '-o',str(path/'test')],check=True)
        subprocess.run([str(path/'test')],check=True)
