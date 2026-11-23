#pragma once

// Compiled only on the first opt-in frame-generation request. The normal
// renderer's utility library and its shader cache are independent of this.
constexpr const char* frameGenerationShaderSource = R"(
#include <metal_stdlib>
using namespace metal;

struct FGVertex { float4 position [[position]]; float2 uv; };
vertex FGVertex fgVertex(uint id [[vertex_id]]) {
    const float2 p[3] = {float2(-1,-3), float2(-1,1), float2(3,1)};
    return {float4(p[id],0,1), float2(p[id].x*.5+.5, .5-p[id].y*.5)};
}
fragment float4 fgCopy(FGVertex v [[stage_in]], texture2d<float> image [[texture(0)]]) {
    constexpr sampler s(coord::normalized, address::clamp_to_edge, filter::linear);
    return image.sample(s,v.uv);
}

// Matching images are capped at 320 pixels across their larger dimension. Flow
// is solved per 8x8 image region, rather than independently for every pixel.
kernel void fgLuma(texture2d<float> image [[texture(0)]],
                   texture2d<float,access::write> luma [[texture(1)]],
                   uint2 p [[thread_position_in_grid]]) {
    if (any(p >= uint2(luma.get_width(),luma.get_height()))) return;
    constexpr sampler s(coord::normalized, address::clamp_to_edge, filter::linear);
    float2 uv=(float2(p)+.5)/float2(luma.get_width(),luma.get_height());
    float2 d=.25/float2(luma.get_width(),luma.get_height());
    float3 c=(image.sample(s,uv+d).rgb+image.sample(s,uv-d).rgb+
              image.sample(s,uv+float2(d.x,-d.y)).rgb+image.sample(s,uv+float2(-d.x,d.y)).rgb)*.25;
    luma.write(float4(dot(c,float3(.299,.587,.114))),p);
}

struct FGReferencePatch { float3 colors[9]; };
inline FGReferencePatch fgLoadPatch(texture2d<float> image, float2 p, float2 inverseSize, float spacing=4.0) {
    constexpr sampler s(coord::normalized,address::clamp_to_edge,filter::linear);
    FGReferencePatch patch;uint i=0;
    for(int y=-1;y<=1;++y) for(int x=-1;x<=1;++x)
        patch.colors[i++]=image.sample(s,(p+float2(x,y)*spacing)*inverseSize).rgb;
    return patch;
}
inline float fgSourcePatch(thread const FGReferencePatch& reference, texture2d<float> b,
                           float2 p, float2 motion, float2 inverseSize, float spacing=4.0) {
    constexpr sampler s(coord::normalized,address::clamp_to_edge,filter::linear);
    float error=0;uint i=0;
    for(int y=-1;y<=1;++y) for(int x=-1;x<=1;++x)
        error+=dot(abs(reference.colors[i++]-b.sample(s,(p+float2(x,y)*spacing+motion)*inverseSize).rgb),float3(1.0/3.0));
    return error/9.0;
}

// A sequential strict cost<best scan over trials in index order picks the first
// minimum; one SIMD group evaluates the trials in parallel (lane = index) and
// applies the same rule, so results equal the original serial search.
inline void fgSimdPick(float cost, float2 v, thread float& best, thread float2& motion) {
    float m=simd_min(cost);
    if(m<best) {
        uint lane=ctz(uint(static_cast<simd_vote::vote_t>(simd_ballot(cost==m))));
        best=m;motion=simd_shuffle(v,ushort(lane));
    }
}
// fgSimdPick within each 16-lane half of the SIMD group, so two independent
// searches (one per half) advance together. Ties pick the lower lane.
inline void fgHalfPick(float cost, float2 v, uint lane, thread float& best, thread float2& motion) {
    float m=cost;uint index=lane;
    for(ushort offset=8;offset>0;offset>>=1) {
        float otherCost=simd_shuffle_xor(m,offset);uint otherIndex=simd_shuffle_xor(index,offset);
        if(otherCost<m||(otherCost==m&&otherIndex<index)) {m=otherCost;index=otherIndex;}
    }
    if(m<best) {best=m;motion=simd_shuffle(v,ushort(index));}
}
// One SIMD group (32 lanes) per matching cell, two cells per threadgroup.
template<bool fast>
kernel void fgFlow(texture2d<float> previous [[texture(0)]], texture2d<float,access::write> field [[texture(2)]],
                   texture2d<float> sourcePrevious [[texture(5)]], texture2d<float> sourceCurrent [[texture(6)]],
                   uint2 group [[threadgroup_position_in_grid]], uint tid [[thread_index_in_threadgroup]]) {
    const uint lane=tid%32;
    const uint2 p=uint2(group.x*2+tid/32,group.y);
    if(any(p>=uint2(field.get_width(),field.get_height()))) return;
    float2 center=(float2(p)+.5)*float2(previous.get_width(),previous.get_height())/float2(field.get_width(),field.get_height());
    float2 inverseSize=1.0/float2(previous.get_width(),previous.get_height());
    const FGReferencePatch reference=fgLoadPatch(sourcePrevious,center,inverseSize);
    // Match the original colors at sparse regional samples. Filtering down
    // to luma resolution otherwise makes a true half-pixel shift score worse
    // than an unrelated repeated texture several regions away.
    float best=fgSourcePatch(reference,sourceCurrent,center,float2(0),inverseSize);
    float2 motion=0;
    if(best==0) {if(lane==0) field.write(float4(0,0,best,best),p);return;}
    // Resolve small/subpixel pans before coarse candidates can lead the
    // search to an unrelated periodic pattern (5x5, row-major).
    {
        float2 v=float2(int(lane%5)-2,int(lane/5)-2);
        float cost=lane<25 ? fgSourcePatch(reference,sourceCurrent,center,v,inverseSize)+length(v)*.00015 : INFINITY;
        fgSimdPick(cost,v,best,motion);
    }
    const uint refinements=fast ? 1 : 2;
    for(uint pass=0;pass<refinements;++pass) {
        // One matching-grid pixel represents up to four output pixels. The
        // second refinement is skipped by wide scouts and by the fast mode.
        if(pass==1&&best>.002&&(p.x%4==0)&&(p.y%4==0)) break;
        for(float step=.5;step>=.125;step*=.5) {
            float2 v=motion+float2(int(lane%3)-1,int(lane/3)-1)*step;
            float cost=lane<9 ? fgSourcePatch(reference,sourceCurrent,center,v,inverseSize)+length(v)*.00015 : INFINITY;
            fgSimdPick(cost,v,best,motion);
        }
    }
    if(lane!=0) return;
    float residual=fgSourcePatch(reference,sourceCurrent,center,motion,inverseSize);
    field.write(float4(motion,best,residual),p);
}
#define FG_FLOW_KERNEL(name,fast) \
template [[host_name(name)]] kernel void fgFlow<fast>(texture2d<float> previous [[texture(0)]], texture2d<float,access::write> field [[texture(2)]], \
    texture2d<float> sourcePrevious [[texture(5)]], texture2d<float> sourceCurrent [[texture(6)]], \
    uint2 group [[threadgroup_position_in_grid]], uint tid [[thread_index_in_threadgroup]]);
FG_FLOW_KERNEL("fgFlow",false)
FG_FLOW_KERNEL("fgFlowFast",true)


// Keep the original four best basins, including deterministic tie order.
inline void fgKeepFour(thread float* costs, thread float2* vectors, thread uint* ids,
                       float cost, float2 vector, uint id) {
    for(uint i=0;i<4;++i) if(cost<costs[i]||(cost==costs[i]&&id<ids[i])) {
        for(uint j=3;j>i;--j) {costs[j]=costs[j-1];vectors[j]=vectors[j-1];ids[j]=ids[j-1];}
        costs[i]=cost;vectors[i]=vector;ids[i]=id;break;
    }
}
// Threads per group (host must match): the camera search uses 256; the
// register-heavy wide local search keeps 64 to avoid spilling.
constant uint FG_SEARCH_THREADS=256;
constant uint FG_WIDE_THREADS=64;
// Each thread's (cost,id)-sorted top four is stored at [tid*4]. A tree merge
// leaves the four smallest (cost,id) entries at [0..3]: exactly what serial
// fgKeepFour insertion of every list yields, since ids are unique.
inline void fgMergeFour(threadgroup float* costs, threadgroup float2* vectors, threadgroup uint* ids, uint tid, uint threads) {
    for(uint stride=threads/2;stride>0;stride/=2) {
        threadgroup_barrier(mem_flags::mem_threadgroup);
        if(tid<stride) {
            float ca[4],cb[4];float2 va[4],vb[4];uint ia[4],ib[4];
            for(uint i=0;i<4;++i) {
                ca[i]=costs[tid*4+i];va[i]=vectors[tid*4+i];ia[i]=ids[tid*4+i];
                cb[i]=costs[(tid+stride)*4+i];vb[i]=vectors[(tid+stride)*4+i];ib[i]=ids[(tid+stride)*4+i];
            }
            uint a=0,b=0;
            for(uint k=0;k<4;++k) {
                bool takeB=cb[b]<ca[a]||(cb[b]==ca[a]&&ib[b]<ia[a]);
                if(takeB) {costs[tid*4+k]=cb[b];vectors[tid*4+k]=vb[b];ids[tid*4+k]=ib[b];++b;}
                else {costs[tid*4+k]=ca[a];vectors[tid*4+k]=va[a];ids[tid*4+k]=ia[a];++a;}
            }
        }
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
}

// Every row of the wide search runs in parallel. Only the four winning
// basins undergo fractional refinement; no candidates or radii are removed.
template<bool fast>
kernel void fgCameraSearch(texture2d<float,access::read> initial [[texture(0)]],
                           texture2d<float> luma [[texture(1)]],
                           texture2d<float,access::write> result [[texture(2)]],
                           texture2d<float> sourcePrevious [[texture(5)]],
                           texture2d<float> sourceCurrent [[texture(6)]],
                           uint2 group [[threadgroup_position_in_grid]],
                           uint tid [[thread_index_in_threadgroup]]) {
    // One group per 4x4 block: only its first cell is a search scout; the
    // other cells pass the local flow through.
    const uint2 p=group*4;
    if(any(p>=uint2(result.get_width(),result.get_height()))) return;
    if(tid>0&&tid<16) {
        uint2 q=p+uint2(tid%4,tid/4);
        if(all(q<uint2(result.get_width(),result.get_height()))) {
            float4 cell=initial.read(q);
            result.write(float4(cell.xy,1.0-smoothstep(.035,.13,cell.w),cell.w),q);
        }
    }
    float4 own=initial.read(p);
    // Fast: skip the wide search where the local flow already matches well.
    if(own.z<=(fast?.01:.002)) {
        if(tid==0) result.write(float4(own.xy,1.0-smoothstep(.035,.13,own.w),own.w),p);
        return;
    }
    float2 size=float2(luma.get_width(),luma.get_height()),inv=1.0/size;
    float2 center=(float2(p)+.5)*size/float2(result.get_width(),result.get_height());
    FGReferencePatch reference=fgLoadPatch(sourcePrevious,center,inv);
    float costs[4]={INFINITY,INFINITY,INFINITY,INFINITY};
    float2 vectors[4]={float2(0),float2(0),float2(0),float2(0)};
    uint ids[4]={0xffffffffu,0xffffffffu,0xffffffffu,0xffffffffu};
    // All threads share the 33x33 grid. Each candidate keeps its row-major
    // id, and the merge orders by (cost, id), so the result does not depend on
    // which thread evaluated a candidate.
    for(uint c=tid;c<33*33;c+=FG_SEARCH_THREADS) {
        uint x=c%33,row=c/33;
        float2 v=(float2(x,row)-16.0)*2.0;
        float cost=fgSourcePatch(reference,sourceCurrent,center,v,inv)+length(v)*.00015;
        fgKeepFour(costs,vectors,ids,cost,v,c);
    }
    threadgroup float sharedCosts[FG_SEARCH_THREADS*4];
    threadgroup float2 sharedVectors[FG_SEARCH_THREADS*4];
    threadgroup uint sharedIds[FG_SEARCH_THREADS*4];
    for(uint i=0;i<4;++i) {uint at=tid*4+i;sharedCosts[at]=costs[i];sharedVectors[at]=vectors[i];sharedIds[at]=ids[i];}
    fgMergeFour(sharedCosts,sharedVectors,sharedIds,tid,FG_SEARCH_THREADS);
    // Refinement: candidates are evaluated by all threads, then each seed's
    // owner scans them in the original order with the original strict '<',
    // so the chosen vector is exactly the sequential result.
    threadgroup float trialCosts[100];
    threadgroup float2 trialVectors[100];
    threadgroup float seedBest[4];
    threadgroup float2 seedVector[4];
    for(uint c=tid;c<100;c+=FG_SEARCH_THREADS) {
        uint k=c%25;
        float2 trial=sharedVectors[c/25]+float2(int(k%5)-2,int(k/5)-2);
        trialCosts[c]=fgSourcePatch(reference,sourceCurrent,center,trial,inv)+length(trial)*.00015;
        trialVectors[c]=trial;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if(tid<4) {
        float2 v=sharedVectors[tid];float best=INFINITY;
        for(uint k=0;k<25;++k) if(trialCosts[tid*25+k]<best) {best=trialCosts[tid*25+k];v=trialVectors[tid*25+k];}
        seedBest[tid]=best;seedVector[tid]=v;
    }
    for(float step=.5;step>=.125;step*=.5) {
        threadgroup_barrier(mem_flags::mem_threadgroup);
        if(tid<36) {
            uint k=tid%9;
            float2 trial=seedVector[tid/9]+float2(int(k%3)-1,int(k/3)-1)*step;
            trialCosts[tid]=fgSourcePatch(reference,sourceCurrent,center,trial,inv)+length(trial)*.00015;
            trialVectors[tid]=trial;
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
        if(tid<4) {
            float best=seedBest[tid];float2 v=seedVector[tid];
            for(uint k=0;k<9;++k) if(trialCosts[tid*9+k]<best) {best=trialCosts[tid*9+k];v=trialVectors[tid*9+k];}
            seedBest[tid]=best;seedVector[tid]=v;
        }
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    float best=own.z;float2 motion=own.xy;
    for(uint i=0;i<4;++i) if(seedBest[i]<best) {best=seedBest[i];motion=seedVector[i];}
    for(float step=.5;step>=.125;step*=.5) {
        threadgroup_barrier(mem_flags::mem_threadgroup);
        if(tid<9) {
            float2 trial=motion+float2(int(tid%3)-1,int(tid/3)-1)*step;
            trialCosts[tid]=fgSourcePatch(reference,sourceCurrent,center,trial,inv)+length(trial)*.00015;
            trialVectors[tid]=trial;
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
        for(uint k=0;k<9;++k) if(trialCosts[k]<best) {best=trialCosts[k];motion=trialVectors[k];}
    }
    if(tid==0) {
        float residual=fgSourcePatch(reference,sourceCurrent,center,motion,inv);
        result.write(float4(motion,1.0-smoothstep(.035,.13,residual),residual),p);
    }
}

template [[host_name("fgCameraSearch")]] kernel void fgCameraSearch<false>(texture2d<float,access::read> initial [[texture(0)]],
    texture2d<float> luma [[texture(1)]], texture2d<float,access::write> result [[texture(2)]],
    texture2d<float> sourcePrevious [[texture(5)]], texture2d<float> sourceCurrent [[texture(6)]],
    uint2 group [[threadgroup_position_in_grid]], uint tid [[thread_index_in_threadgroup]]);
template [[host_name("fgCameraSearchFast")]] kernel void fgCameraSearch<true>(texture2d<float,access::read> initial [[texture(0)]],
    texture2d<float> luma [[texture(1)]], texture2d<float,access::write> result [[texture(2)]],
    texture2d<float> sourcePrevious [[texture(5)]], texture2d<float> sourceCurrent [[texture(6)]],
    uint2 group [[threadgroup_position_in_grid]], uint tid [[thread_index_in_threadgroup]]);

// Camera transforms remain candidates: every tile validates them against
// its source colors, and independent foreground motion may retain its own fit.
inline uint fgHash(uint x) {x^=x>>16;x*=0x7feb352du;x^=x>>15;x*=0x846ca68bu;return x^(x>>16);}

inline float fgModelError(texture2d<float> a, texture2d<float> b, float2 fineSize,
                          uint2 dimensions, float2 origin, float2 ax, float2 ay) {
    constexpr sampler s(coord::normalized,address::clamp_to_edge,filter::linear);
    float error=0;
    for(uint y=0;y<7;++y) for(uint x=0;x<7;++x) {
        uint2 q=min(uint2((float2(x,y)+.5)*float2(dimensions)/7.0),dimensions-1);
        float2 uv=(float2(q)+.5)/float2(dimensions),at=uv-.5;
        float2 destination=uv+(origin+ax*at.x+ay*at.y)/fineSize;
        float3 ca=a.sample(s,uv).rgb,cb=b.sample(s,destination).rgb;
        error+=any(destination<0.0)||any(destination>1.0) ? .2 : min(.2,dot(abs(ca-cb),float3(1.0/3.0)));
    }
    return error/49.0+length(origin)*.00001;
}
// One of the 49 fgModelError terms (index = y*7+x), for parallel evaluation.
inline float fgModelTerm(texture2d<float> a, texture2d<float> b, float2 fineSize,
                         uint2 dimensions, float2 origin, float2 ax, float2 ay, uint index) {
    constexpr sampler s(coord::normalized,address::clamp_to_edge,filter::linear);
    uint x=index%7,y=index/7;
    uint2 q=min(uint2((float2(x,y)+.5)*float2(dimensions)/7.0),dimensions-1);
    float2 uv=(float2(q)+.5)/float2(dimensions),at=uv-.5;
    float2 destination=uv+(origin+ax*at.x+ay*at.y)/fineSize;
    float3 ca=a.sample(s,uv).rgb,cb=b.sample(s,destination).rgb;
    return any(destination<0.0)||any(destination>1.0) ? .2 : min(.2,dot(abs(ca-cb),float3(1.0/3.0)));
}
// Sums the terms in fgModelError's order, so the result is identical.
inline float fgModelSum(threadgroup const float* terms, float2 origin) {
    float error=0;
    for(uint i=0;i<49;++i) error+=terms[i];
    return error/49.0+length(origin)*.00001;
}

// Fit camera seeds separately from expensive image scoring. Identical
// camera transforms are common in static scenes and uniform pans.
kernel void fgModelSeeds(texture2d<float,access::read> raw [[texture(0)]],
                        texture2d<float,access::write> result [[texture(1)]],
                        uint2 p [[thread_position_in_grid]]) {
    if(p.x>=result.get_height()||p.y!=0) return;
    const uint trial=p.x;
    uint2 dimensions=uint2(raw.get_width(),raw.get_height());
    uint2 scoutSize=(dimensions+3)/4;
    uint2 points[3];float2 at[3];float4 values[3];
    for(uint i=0;i<3;++i) {
        uint h=fgHash(trial*3+i+1);
        points[i]=min(uint2(h%scoutSize.x,(h/scoutSize.x)%scoutSize.y)*4,dimensions-1);
        at[i]=(float2(points[i])+.5)/float2(dimensions)-.5;
        values[i]=raw.read(points[i]);
    }
    float2 u=at[1]-at[0],v=at[2]-at[0];
    float determinant=u.x*v.y-u.y*v.x;
    float2 origin=0,ax=0,ay=0;float valid=0;
    if(abs(determinant)>=.04) {
        float2 du=values[1].xy-values[0].xy,dv=values[2].xy-values[0].xy;
        ax=(du*v.y-dv*u.y)/determinant;
        ay=(dv*u.x-du*v.x)/determinant;
        origin=values[0].xy-ax*at[0].x-ay*at[0].y;
        if(length(ax)<=80&&length(ay)<=80&&all(isfinite(origin))) valid=1;
    }

    // Full precision preserves the preexisting candidate calculations.
    result.write(float4(origin,valid,0),uint2(0,trial));
    result.write(float4(ax,0,0),uint2(1,trial));
    result.write(float4(ay,0,0),uint2(2,trial));
}

// Score/refine each distinct transform once. Suppress only exactly equal
// matrices, not nearby camera estimates or independent object movements.
// One threadgroup per trial: the 49 image terms of each score (and both
// signs of a refinement step) run in parallel; acceptance stays sequential.
kernel void fgModelCandidates(texture2d<float,access::read> seeds [[texture(0)]],
                        texture2d<float,access::write> result [[texture(1)]],
                        texture2d<float,access::read> raw [[texture(2)]],
                        texture2d<float> fineLuma [[texture(3)]],
                        texture2d<float> sourcePrevious [[texture(5)]],
                        texture2d<float> sourceCurrent [[texture(6)]],
                        uint2 group [[threadgroup_position_in_grid]],
                        uint tid [[thread_index_in_threadgroup]]) {
    threadgroup float terms[98];
    threadgroup float2 shared[3];
    threadgroup float sharedError;
    threadgroup uint duplicate;
    uint trial=group.x;
    if(trial>=result.get_height()||group.y!=0) return;
    float4 seed=seeds.read(uint2(0,trial));
    float2 origin=seed.xy,ax=seeds.read(uint2(1,trial)).xy,ay=seeds.read(uint2(2,trial)).xy;
    if(tid==0) duplicate=0;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if(seed.z>0) for(uint i=tid;i<trial;i+=64) {
        float4 earlier=seeds.read(uint2(0,i));
        if(earlier.z>0&&all(earlier.xy==origin)&&
           all(seeds.read(uint2(1,i)).xy==ax)&&all(seeds.read(uint2(2,i)).xy==ay)) duplicate=1;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if(seed.z<=0||duplicate) {
        if(tid==0) {
            result.write(float4(0),uint2(0,trial));
            result.write(float4(0),uint2(1,trial));
            result.write(float4(0),uint2(2,trial));
        }
        return;
    }
    uint2 dimensions=uint2(raw.get_width(),raw.get_height());
    const float2 fineSize=float2(fineLuma.get_width(),fineLuma.get_height());
    if(tid<49) terms[tid]=fgModelTerm(sourcePrevious,sourceCurrent,fineSize,dimensions,origin,ax,ay,tid);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if(tid==0) sharedError=fgModelSum(terms,origin);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    float error=sharedError;
    // A nonnegative error cannot improve on an exact match.
    if(error>0&&error<.06) for(float step=.5;step>=.125;step*=.5) {
        for(uint component=0;component<6;++component) {
            float2 o[2],x[2],y[2];
            for(int k=0;k<2;++k) {
                o[k]=origin;x[k]=ax;y[k]=ay;
                float delta=step*(k?1:-1)*(component<2 ? 1.0 : 2.0);
                if(component<2) o[k][component]+=delta;
                else if(component<4) x[k][component-2]+=delta;
                else y[k][component-4]+=delta;
            }
            for(uint c=tid;c<98;c+=64) {
                uint k=c/49;
                terms[c]=fgModelTerm(sourcePrevious,sourceCurrent,fineSize,dimensions,o[k],x[k],y[k],c%49);
            }
            threadgroup_barrier(mem_flags::mem_threadgroup);
            if(tid==0) {
                float2 bestOrigin=origin,bestX=ax,bestY=ay;
                for(uint k=0;k<2;++k) {
                    float candidate=fgModelSum(terms+49*k,o[k]);
                    if(candidate<error) {error=candidate;bestOrigin=o[k];bestX=x[k];bestY=y[k];}
                }
                shared[0]=bestOrigin;shared[1]=bestX;shared[2]=bestY;sharedError=error;
            }
            threadgroup_barrier(mem_flags::mem_threadgroup);
            origin=shared[0];ax=shared[1];ay=shared[2];error=sharedError;
        }
    }
    if(tid!=0) return;
    float weight=1.0/(.001+error);
    result.write(float4(origin,weight,0),uint2(0,trial));
    result.write(float4(ax,0,0),uint2(1,trial));
    result.write(float4(ay,0,0),uint2(2,trial));
}

inline bool fgInverseModel(float2 origin, float2 ax, float2 ay, float2 size,
                           thread float2& reverseOrigin, thread float2& reverseX, thread float2& reverseY) {
    float a=1.0+ax.x/size.x,b=ay.x/size.x,c=ax.y/size.y,d=1.0+ay.y/size.y;
    float determinant=a*d-b*c;
    if(abs(determinant)<.1) return false;
    float2 normalized=origin/size;
    reverseOrigin=float2(-d*normalized.x+b*normalized.y,c*normalized.x-a*normalized.y)/determinant*size;
    reverseX=float2(d/determinant-1.0,-c/determinant)*size;
    reverseY=float2(-b/determinant,a/determinant-1.0)*size;
    return true;
}

kernel void fgConsensus(texture2d<float,access::read> forward [[texture(0)]],
                        texture2d<float,access::read> backward [[texture(1)]],
                        texture2d<float,access::write> resultForward [[texture(2)]],
                        texture2d<float,access::write> resultBackward [[texture(3)]],
                        texture2d<float> fineLuma [[texture(4)]],
                        uint2 p [[thread_position_in_grid]]) {
    if(any(p!=uint2(0))) return;
    float best=0;uint selected=0;bool reversed=false;
    for(uint i=0;i<forward.get_height();++i) {
        float f=forward.read(uint2(0,i)).z,b=backward.read(uint2(0,i)).z;
        if(f>best) {best=f;selected=i;reversed=false;}
        if(b>best) {best=b;selected=i;reversed=true;}
    }
    float4 model=reversed ? backward.read(uint2(0,selected)) : forward.read(uint2(0,selected));
    float2 ax=(reversed ? backward.read(uint2(1,selected)) : forward.read(uint2(1,selected))).xy;
    float2 ay=(reversed ? backward.read(uint2(2,selected)) : forward.read(uint2(2,selected))).xy;
    float2 reverseOrigin,reverseX,reverseY;
    const float2 size=float2(fineLuma.get_width(),fineLuma.get_height());
    bool invertible=fgInverseModel(model.xy,ax,ay,size,reverseOrigin,reverseX,reverseY);
    float confidence=best>0&&invertible ? 1.0-smoothstep(.025,.12,1.0/best-.001) : 0;
    if(!invertible) {reverseOrigin=reverseX=reverseY=0;}
    resultForward.write(float4(reversed?reverseOrigin:model.xy,confidence,0),uint2(0));
    resultForward.write(float4(reversed?reverseX:ax,0,0),uint2(1,0));
    resultForward.write(float4(reversed?reverseY:ay,0,0),uint2(2,0));
    resultBackward.write(float4(reversed?model.xy:reverseOrigin,confidence,0),uint2(0));
    resultBackward.write(float4(reversed?ax:reverseX,0,0),uint2(1,0));
    resultBackward.write(float4(reversed?ay:reverseY,0,0),uint2(2,0));
}

// Wider context resolves a repeating sleeve/cloth texture without forcing
// the background behind the silhouette to share the character's movement.
inline float fgContextCost(thread const FGReferencePatch& context, texture2d<float> image,
                           float2 p, float2 v, float2 inv) {
    constexpr sampler s(coord::normalized,address::clamp_to_edge,filter::linear);
    float3 anchor=image.sample(s,(p+v)*inv).rgb;
    float error=0;uint i=0;
    for(int y=-1;y<=1;++y) for(int x=-1;x<=1;++x) {
        float3 color=image.sample(s,(p+float2(x,y)*8.0+v)*inv).rgb;
        float src=smoothstep(.15,.4,length(context.colors[i]-context.colors[4]));
        float dst=smoothstep(.15,.4,length(color-anchor));
        error+=.08*abs(src-dst)+.25*min(.025,dot(abs(context.colors[i]-color),float3(1.0/3.0)));
        ++i;
    }
    return error/9.0;
}
inline float fgLocalCost(thread const FGReferencePatch& detail, thread const FGReferencePatch& context,
                         texture2d<float> image, float2 p, float2 v, float2 inv) {
    return fgSourcePatch(detail,image,p,v,inv,2.0)+fgContextCost(context,image,p,v,inv)+length(v)*.00015;
}
// Same result as fgLocalCost(...)<best: the context term is never negative,
// so a trial whose detail cost already loses skips the context samples.
inline bool fgLocalImproves(thread const FGReferencePatch& detail, thread const FGReferencePatch& context,
                            texture2d<float> image, float2 p, float2 v, float2 inv, float best, thread float& cost) {
    float patch=fgSourcePatch(detail,image,p,v,inv,2.0),motionCost=length(v)*.00015;
    if(patch+motionCost>=best) return false;
    cost=patch+fgContextCost(context,image,p,v,inv)+motionCost;
    return cost<best;
}

// Refine the local and camera candidates independently. Never average
// contradictory vectors into an unvalidated deformation of the image.
// fast: optional cheaper mode (one refinement start instead of two); the
// default quality kernel is unchanged. One SIMD group per dense cell (two
// cells per threadgroup) evaluates each trial set in parallel; fgSimdPick
// keeps the serial first-minimum rule, so results equal the serial search.
template<bool fast>
kernel void fgRegularize(texture2d<float> raw [[texture(0)]], texture2d<float> luma [[texture(1)]],
                         texture2d<float,access::write> result [[texture(2)]], texture2d<float,access::read> dominant [[texture(3)]],
                         texture2d<float> sourcePrevious [[texture(5)]], texture2d<float> sourceCurrent [[texture(6)]],
                         uint2 group [[threadgroup_position_in_grid]], uint tid [[thread_index_in_threadgroup]]) {
    const uint lane=tid%32;
    const uint2 p=uint2(group.x*2+tid/32,group.y);
    if(any(p>=uint2(result.get_width(),result.get_height()))) return;
    const float2 size=float2(luma.get_width(),luma.get_height());
    const float2 uv=(float2(p)+.5)/float2(result.get_width(),result.get_height());
    const float2 center=uv*size,inv=1.0/size;
    const float spacing=2.0;
    const FGReferencePatch reference=fgLoadPatch(sourcePrevious,center,inv,spacing);
    float zero=fgSourcePatch(reference,sourceCurrent,center,float2(0),inv,spacing);
    if(zero<.002) {if(lane==0) result.write(float4(0,0,1,zero),p);return;}
    const FGReferencePatch context=fgLoadPatch(sourcePrevious,center,inv,8.0);
    float4 camera=dominant.read(uint2(0));
    float2 at=uv-.5;
    float2 model=camera.xy+dominant.read(uint2(1,0)).xy*at.x+dominant.read(uint2(2,0)).xy*at.y;
    float2 cameraMotion=model;
    float modelResidual=fgSourcePatch(reference,sourceCurrent,center,cameraMotion,inv,spacing);
    const float2 offset=float2(int(lane%3)-1,int(lane/3)-1); // 3x3 trial, row-major
    if(camera.z>.65) {
        // A shared camera fit has small quantization error at individual
        // pixels. Refine it cheaply before considering an object-wide search.
        if(modelResidual>=.01) for(float step=.25;step>=.125;step*=.5) {
            float2 v=cameraMotion+offset*step;
            float cost=lane<9 ? fgSourcePatch(reference,sourceCurrent,center,v,inv,spacing) : INFINITY;
            fgSimdPick(cost,v,modelResidual,cameraMotion);
        }
        if(modelResidual<.015&&fgContextCost(context,sourceCurrent,center,cameraMotion,inv)<.006) {
            // Negative w marks a fully checked final proposal. Synthesis uses
            // xy/z only; propagation need not repeat its expensive checks.
            if(lane==0) result.write(float4(cameraMotion,1.0,-max(modelResidual,.0001)),p);
            return;
        }
    }
    float2 motion=0;float best=fgLocalCost(reference,context,sourceCurrent,center,float2(0),inv);
    // The coarse camera grid misses narrow, independently moving characters.
    // Validate all surrounding coarse candidates in a smaller source patch on
    // the dense output grid; do not linearly average object/background motion.
    {
        int2 base=int2(floor(uv*float2(raw.get_width(),raw.get_height())-.5));
        uint2 q=uint2(clamp(base+int2(lane%2,(lane/2)%2),int2(0),int2(raw.get_width()-1,raw.get_height()-1)));
        float4 proposal=raw.read(q);
        float cost=lane<4 ? fgLocalCost(reference,context,sourceCurrent,center,proposal.xy,inv) : INFINITY;
        float rawConfidence=max(0.0,simd_max(lane<4 ? proposal.z : -INFINITY));
        fgSimdPick(cost,proposal.xy,best,motion);
        // On an actual cut, both the global model and surrounding coarse
        // searches reject the colors. Preserve the original instead of doing
        // hundreds of futile searches in every dense tile.
        if(zero>.25&&camera.z<.12&&rawConfidence<.1) {
            if(lane==0) result.write(float4(0,0,0,-1),p);
            return;
        }
    }
    float2 localStart=motion;
    float fastStartCost=best;
    // Fast: refine only the cheaper of the local and camera starts.
    if(fast&&camera.z>=.12) {
        float cost;
        if(fgLocalImproves(reference,context,sourceCurrent,center,model,inv,fastStartCost,cost)) {fastStartCost=cost;localStart=model;}
    }
    // Candidate 0 (local start) refines in lanes 0-15, candidate 1 (camera
    // model, quality mode only) in lanes 16-31; results merge in serial order.
    const uint group16=lane/16,sub=lane%16;
    const bool active=group16==0||(!fast&&camera.z>=.12);
    float2 v=group16 ? model : localStart;
    float localBest=fast ? fastStartCost : fgLocalCost(reference,context,sourceCurrent,center,v,inv);
    const float2 halfOffset=float2(int(sub%3)-1,int(sub/3)-1);
    for(float step=fast?.5:1.0;step>=.125;step*=.5) {
        float2 trial=v+halfOffset*step;
        float cost=active&&sub<9 ? fgLocalCost(reference,context,sourceCurrent,center,trial,inv) : INFINITY;
        fgHalfPick(cost,trial,lane,localBest,v);
    }
    for(ushort candidate=0;candidate<2;++candidate) {
        float candidateBest=simd_shuffle(localBest,ushort(candidate*16));
        float2 candidateMotion=simd_shuffle(v,ushort(candidate*16));
        if(candidate==1&&(fast||camera.z<.12)) continue;
        if(candidateBest<best) {best=candidateBest;motion=candidateMotion;}
    }
    if(lane!=0) return;
    float residual=fgSourcePatch(reference,sourceCurrent,center,motion,inv,spacing);
    result.write(float4(motion,1.0-smoothstep(.035,.13,residual),best),p);
}
#define FG_REGULARIZE_KERNEL(name,fast) \
template [[host_name(name)]] kernel void fgRegularize<fast>(texture2d<float> raw [[texture(0)]], texture2d<float> luma [[texture(1)]], \
    texture2d<float,access::write> result [[texture(2)]], texture2d<float,access::read> dominant [[texture(3)]], \
    texture2d<float> sourcePrevious [[texture(5)]], texture2d<float> sourceCurrent [[texture(6)]], \
    uint2 group [[threadgroup_position_in_grid]], uint tid [[thread_index_in_threadgroup]]);
FG_REGULARIZE_KERNEL("fgRegularize",false)
FG_REGULARIZE_KERNEL("fgRegularizeFast",true)

// The large search has its own pipeline so register use and instruction
// footprint do not penalize every background tile in the common camera pass.
kernel void fgWideLocal(texture2d<float,access::read> initial [[texture(0)]],
                        texture2d<float> luma [[texture(1)]],
                        texture2d<float,access::write> result [[texture(2)]],
                        texture2d<float,access::read> dominant [[texture(3)]],
                        texture2d<float> sourcePrevious [[texture(5)]],
                        texture2d<float> sourceCurrent [[texture(6)]],
                        uint2 p [[threadgroup_position_in_grid]],
                        uint tid [[thread_index_in_threadgroup]]) {
    if(any(p>=uint2(result.get_width(),result.get_height()))) return;
    // At most one expensive search per 4x4 dense block. Pick an unmatched
    // source region inside it, so a narrow character cannot fall between a
    // fixed, widely spaced camera-scout lattice.
    // Dense cells per proposal side: 4 normally, 2 for the coarser fast field.
    const uint cells=max(1u,uint(round(float(initial.get_width())/float(result.get_width()))));
    uint2 selected=min(p*cells,uint2(initial.get_width()-1,initial.get_height()-1));
    float priority=0;
    for(uint y=0;y<cells;++y) for(uint x=0;x<cells;++x) {
        uint2 q=p*cells+uint2(x,y);
        if(any(q>=uint2(initial.get_width(),initial.get_height()))) continue;
        float score=initial.read(q).w;
        if(score>priority) {priority=score;selected=q;}
    }
    // The coarse fast field (cells<4) searches only clearly unmatched blocks.
    if(priority<(cells>=4?.006:.015)) {if(tid==0) result.write(float4(0,0,0,-1),p);return;}
    float4 own=initial.read(selected);
    float2 size=float2(luma.get_width(),luma.get_height()),inv=1.0/size;
    float2 uv=(float2(selected)+.5)/float2(initial.get_width(),initial.get_height()),center=uv*size;
    const float spacing=2.0;
    FGReferencePatch reference=fgLoadPatch(sourcePrevious,center,inv,spacing);
    FGReferencePatch context=fgLoadPatch(sourcePrevious,center,inv,8.0);
    float4 camera=dominant.read(uint2(0));
    float2 at=uv-.5;
    float2 model=camera.xy+dominant.read(uint2(1,0)).xy*at.x+dominant.read(uint2(2,0)).xy*at.y;
    float2 motion=own.xy;
    float best=fgLocalCost(reference,context,sourceCurrent,center,motion,inv);
    float residual=fgSourcePatch(reference,sourceCurrent,center,motion,inv,spacing);
    if(residual<=.025&&fgContextCost(context,sourceCurrent,center,motion,inv)<=.008) {
        if(tid==0) result.write(float4(motion,1.0-smoothstep(.035,.13,residual),residual),p);
        return;
    }
    float costs[4]={INFINITY,INFINITY,INFINITY,INFINITY};
    float2 vectors[4]={float2(0),float2(0),float2(0),float2(0)};
    uint ids[4]={0xffffffffu,0xffffffffu,0xffffffffu,0xffffffffu};
    // The coarse fast field (cells<4) skips the camera-centered rows; the
    // regularize pass already refines every cell from the camera model.
    uint rows=cells>=4&&camera.z>=.12&&length(model)>=4.0 ? 34 : 17;
    // Spread the rows*17 candidates over all threads; ids stay row-major.
    for(uint c=tid;c<rows*17;c+=FG_WIDE_THREADS) {
        uint line=c/17,x=c%17,candidate=line/17,row=line%17;
        float2 origin=candidate ? model : float2(0);
        float2 v=origin+(float2(x,row)-8.0)*2.0;
        // Exact skip: a detail cost above the fourth basin can never be kept.
        float patch=fgSourcePatch(reference,sourceCurrent,center,v,inv,2.0),motionCost=length(v)*.00015;
        if(patch+motionCost>costs[3]) continue;
        float cost=patch+fgContextCost(context,sourceCurrent,center,v,inv)+motionCost;
        fgKeepFour(costs,vectors,ids,cost,v,c);
    }
    threadgroup float sharedCosts[FG_WIDE_THREADS*4];
    threadgroup float2 sharedVectors[FG_WIDE_THREADS*4];
    threadgroup uint sharedIds[FG_WIDE_THREADS*4];
    for(uint i=0;i<4;++i) {uint at=tid*4+i;sharedCosts[at]=costs[i];sharedVectors[at]=vectors[i];sharedIds[at]=ids[i];}
    fgMergeFour(sharedCosts,sharedVectors,sharedIds,tid,FG_WIDE_THREADS);
    // Same exact parallel refinement as fgCameraSearch (start value: seed cost).
    threadgroup float trialCosts[100];
    threadgroup float2 trialVectors[100];
    for(uint c=tid;c<100;c+=FG_WIDE_THREADS) {
        uint k=c%25;
        float2 trial=sharedVectors[c/25]+float2(int(k%5)-2,int(k/5)-2);
        trialCosts[c]=fgLocalCost(reference,context,sourceCurrent,center,trial,inv);
        trialVectors[c]=trial;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if(tid<4) {
        float2 v=sharedVectors[tid];float localBest=sharedCosts[tid];
        for(uint k=0;k<25;++k) if(trialCosts[tid*25+k]<localBest) {localBest=trialCosts[tid*25+k];v=trialVectors[tid*25+k];}
        sharedCosts[tid]=localBest;sharedVectors[tid]=v;
    }
    for(float step=.5;step>=.125;step*=.5) {
        threadgroup_barrier(mem_flags::mem_threadgroup);
        if(tid<36) {
            uint k=tid%9;
            float2 trial=sharedVectors[tid/9]+float2(int(k%3)-1,int(k/3)-1)*step;
            trialCosts[tid]=fgLocalCost(reference,context,sourceCurrent,center,trial,inv);
            trialVectors[tid]=trial;
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
        if(tid<4) {
            float localBest=sharedCosts[tid];float2 v=sharedVectors[tid];
            for(uint k=0;k<9;++k) if(trialCosts[tid*9+k]<localBest) {localBest=trialCosts[tid*9+k];v=trialVectors[tid*9+k];}
            sharedCosts[tid]=localBest;sharedVectors[tid]=v;
        }
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if(tid==0) {
        for(uint i=0;i<4;++i) if(sharedCosts[i]<best) {best=sharedCosts[i];motion=sharedVectors[i];}
        residual=fgSourcePatch(reference,sourceCurrent,center,motion,inv,spacing);
        result.write(float4(motion,1.0-smoothstep(.035,.13,residual),residual),p);
    }
}

// Share individually validated object proposals with adjacent dense tiles.
// Candidate vectors are rechecked, never averaged across a silhouette.
kernel void fgPropagate(texture2d<float,access::read> proposals [[texture(0)]],
                        texture2d<float> luma [[texture(1)]],
                        texture2d<float,access::write> result [[texture(2)]],
                        texture2d<float,access::read> dominant [[texture(3)]],
                        texture2d<float,access::read> initial [[texture(4)]],
                        texture2d<float> sourcePrevious [[texture(5)]],
                        texture2d<float> sourceCurrent [[texture(6)]],
                        uint2 group [[threadgroup_position_in_grid]],
                        uint tid [[thread_index_in_threadgroup]]) {
    // One SIMD group per cell (two per threadgroup); lane = candidate index.
    const uint lane=tid%32;
    const uint2 p=uint2(group.x*2+tid/32,group.y);
    if(any(p>=uint2(result.get_width(),result.get_height()))) return;
    float4 own=initial.read(p);
    if(own.w<0||(own.z>.9&&own.w<.002&&length(own.xy)<.05)) {if(lane==0) result.write(own,p);return;}
    float2 size=float2(luma.get_width(),luma.get_height()),inv=1.0/size;
    float2 uv=(float2(p)+.5)/float2(result.get_width(),result.get_height()),center=uv*size;
    FGReferencePatch detail=fgLoadPatch(sourcePrevious,center,inv,2.0);
    FGReferencePatch context=fgLoadPatch(sourcePrevious,center,inv,8.0);
    float best=fgLocalCost(detail,context,sourceCurrent,center,own.xy,inv);
    float4 camera=dominant.read(uint2(0));
    float2 at=uv-.5;
    float2 model=camera.xy+dominant.read(uint2(1,0)).xy*at.x+dominant.read(uint2(2,0)).xy*at.y;
    if((best<.006&&length(own.xy)<.05)||(best<.025&&camera.z>.65&&length(own.xy-model)<.75)) {if(lane==0) result.write(own,p);return;}
    float2 motion=own.xy;
    const int2 last=int2(proposals.get_width()-1,proposals.get_height()-1);
    int2 block=int2(p/max(1u,uint(round(float(result.get_width())/float(proposals.get_width())))));
    {
        float4 candidate=proposals.read(uint2(clamp(block+int2(int(lane%3)-1,int((lane/3)%3)-1),int2(0),last)));
        float cost=lane<9&&candidate.w>=0 ? fgLocalCost(detail,context,sourceCurrent,center,candidate.xy,inv) : INFINITY;
        fgSimdPick(cost,candidate.xy,best,motion);
    }
    if(lane!=0) return;
    float residual=fgSourcePatch(detail,sourceCurrent,center,motion,inv,2.0);
    result.write(float4(motion,1.0-smoothstep(.035,.13,residual),residual),p);
}

// Interpolating a coarse field across a character outline invents vectors
// between foreground and background. Resolve those discontinuities against
// source colors once per pair, at the bounded matching-image resolution.
// Smooth camera fields retain their original bilinear interpolation.
kernel void fgUpsample(texture2d<float> coarse [[texture(0)]],
                       texture2d<float> luma [[texture(1)]],
                       texture2d<float,access::write> result [[texture(2)]],
                       texture2d<float> previous [[texture(5)]],
                       texture2d<float> current [[texture(6)]],
                       uint2 p [[thread_position_in_grid]]) {
    uint2 size=uint2(result.get_width(),result.get_height());
    if(any(p>=size)) return;
    constexpr sampler s(coord::normalized,address::clamp_to_edge,filter::linear);
    float2 uv=(float2(p)+.5)/float2(size),inv=1.0/float2(size);
    float4 own=coarse.sample(s,uv),candidates[4];
    int2 base=int2(floor(uv*float2(coarse.get_width(),coarse.get_height())-.5));
    float spread=0;uint i=0;
    for(int y=0;y<2;++y) for(int x=0;x<2;++x) {
        uint2 q=uint2(clamp(base+int2(x,y),int2(0),int2(coarse.get_width()-1,coarse.get_height()-1)));
        candidates[i]=coarse.read(q);spread=max(spread,length(candidates[i++].xy-own.xy));
    }
    if(spread<1.5) {result.write(own,p);return;}
    float2 center=float2(p)+.5;
    FGReferencePatch detail=fgLoadPatch(previous,center,inv,.5);
    float best=fgSourcePatch(detail,current,center,own.xy,inv,.5);
    for(uint i=0;i<4;++i) {
        float4 candidate=candidates[i];
        if(candidate.z<.4) continue;
        float cost=fgSourcePatch(detail,current,center,candidate.xy,inv,.5);
        if(cost<best) {best=cost;own=candidate;}
    }
    own.z=min(own.z,1.0-smoothstep(.035,.13,best));
    result.write(own,p);
}

struct FGParams { float2 inverseLumaSize; float t; float unused; };
// Seed inverse warps by projecting source pixels into the intermediate
// image. Starting only at the output coordinate misses fast silhouettes.
// Packed buffer keys use 17 bits for a <=320x320 source position and 15 for
// a bounded confidence/coverage score; atomic max resolves write collisions.
kernel void fgProject(texture2d<float> previous [[texture(0)]],
                       texture2d<float> current [[texture(1)]],
                       texture2d<float> forward [[texture(2)]],
                       texture2d<float> backward [[texture(3)]],
                       texture2d<float,access::read> dominant [[texture(4)]],
                       constant FGParams& params [[buffer(0)]],
                       device atomic_uint* projected [[buffer(1)]],
                       uint2 p [[thread_position_in_grid]]) {
    uint2 size=uint2(round(1.0/params.inverseLumaSize));
    if(any(p>=size)) return;
    constexpr sampler s(coord::normalized,address::clamp_to_edge,filter::linear);
    float2 uv=(float2(p)+.5)/float2(size);
    float4 motion=forward.sample(s,uv);
    float2 endpoint=uv+motion.xy*params.inverseLumaSize;
    if(any(endpoint<0.0)||any(endpoint>1.0)||motion.z<.65) return;
    float4 reverse=backward.sample(s,endpoint);
    float consistency=length(motion.xy+reverse.xy);
    float trust=min(motion.z,reverse.z)*(1.0-smoothstep(.75,2.0,consistency));
    float error=length(previous.sample(s,uv).rgb-current.sample(s,endpoint).rgb);
    if(trust<.65||error>.22) return;
    // Two visible source surfaces can collide in the intermediate image.
    // An independently moving, verified surface gets a small visibility
    // priority over the camera plane, rather than depending on source-index
    // tie order (which otherwise flips when the camera changes direction).
    float4 camera=dominant.read(uint2(0));
    float2 centered=uv-.5;
    float2 cameraMotion=camera.xy+dominant.read(uint2(1,0)).xy*centered.x+dominant.read(uint2(2,0)).xy*centered.y;
    float importance=camera.z>.65 ? mix(.8,1.0,smoothstep(1.0,2.0,length(motion.xy-cameraMotion))) : .8;
    float2 at=float2(p)+params.t*motion.xy;
    int2 base=int2(floor(at));float2 part=fract(at);
    uint index=p.y*size.x+p.x+1;
    for(uint y=0;y<2;++y) for(uint x=0;x<2;++x) {
        int2 target=base+int2(x,y);
        if(any(target<0)||any(target>=int2(size))) continue;
        float coverage=(x?part.x:1.0-part.x)*(y?part.y:1.0-part.y);
        uint quality=uint(clamp(importance*trust*(1.0-error/.3)*coverage,0.0,1.0)*32767.0);
        if(quality==0) continue;
        uint key=(quality<<17)|index;
        atomic_fetch_max_explicit(&projected[target.y*size.x+target.x],key,memory_order_relaxed);
    }
}
inline bool fgProjectedSource(float2 uv, texture2d<float> field,
    constant FGParams& params, const device uint* projected, float fraction,
    thread float2& source) {
    constexpr sampler s(coord::normalized,address::clamp_to_edge,filter::linear);
    uint2 size=uint2(round(1.0/params.inverseLumaSize));
    uint2 at=min(uint2(uv*float2(size)),size-1);
    uint key=projected[at.y*size.x+at.x];
    uint index=key&131071u;
    if(index==0) return false;
    --index;
    float2 original=(float2(index%size.x,index/size.x)+.5)/float2(size);
    float2 motion=field.sample(s,original).xy;
    source=uv-fraction*motion*params.inverseLumaSize;
    // The splat supplies the surface, not a constant vector for the whole
    // pixel. Refine within that surface for perspective/zoom motion.
    for(uint i=0;i<3;++i)
        source=uv-fraction*field.sample(s,source).xy*params.inverseLumaSize;
    return true;
}

// The two inverse searches can land on different surfaces at a moving
// silhouette. A candidate reconstructed from one source must still satisfy
// the same bidirectional/color tests and map back to this output position.
inline bool fgTryPair(float2 uv, float2 uvA, float2 uvB,
    texture2d<float> previous, texture2d<float> current,
    texture2d<float> forward, texture2d<float> backward,
    constant FGParams& params) {
    constexpr sampler s(coord::normalized,address::clamp_to_edge,filter::linear);
    if(any(uvA<0.0)||any(uvA>1.0)||any(uvB<0.0)||any(uvB>1.0)) return false;
    float4 f=forward.sample(s,uvA),r=backward.sample(s,uvB);
    float trust=min(f.z,r.z)*(1.0-smoothstep(.75,2.0,length(f.xy+r.xy)));
    if(trust<.65) return false;
    float2 projected=mix(uvA,uvB,params.t);
    if(length((projected-uv)/params.inverseLumaSize)>.25) return false;
    float4 a=previous.sample(s,uvA),b=current.sample(s,uvB);
    return length(a.rgb-b.rgb)<=.22;
}

// Output color = previous(UA) for mode 0, current(UB) for mode 1, and
// mix(previous(UA), current(UB), t) for mode 2.
struct FGWarp { float2 a, b; uint mode; };
inline FGWarp fgWarp(float2 a, float2 b, uint mode) { FGWarp w; w.a=a; w.b=b; w.mode=mode; return w; }
inline float4 fgSampleWarp(FGWarp w, texture2d<float> previous, texture2d<float> current, float t) {
    constexpr sampler s(coord::normalized,address::clamp_to_edge,filter::linear);
    if(w.mode==0) return previous.sample(s,w.a);
    if(w.mode==1) return current.sample(s,w.b);
    return mix(previous.sample(s,w.a),current.sample(s,w.b),t);
}

// Shared by presentation and the sparse diagnostic probe. Reasons count
// sampled pixels, not nominal interpolation passes or claimed unique frames.
inline FGWarp fgResolveWarp(float2 uv,
    texture2d<float> previous, texture2d<float> current,
    texture2d<float> forward, texture2d<float> backward,
    constant FGParams& params, thread uint& reason, const device uint* projectedA, const device uint* projectedB) {
    constexpr sampler s(coord::normalized,address::clamp_to_edge,filter::linear);
    float t=clamp(params.t,0.0,1.0);
    float4 a=previous.sample(s,uv), b=current.sample(s,uv);
    reason=5;
    if(t<.001) return fgWarp(uv,uv,0);
    if(t>.999) return fgWarp(uv,uv,1);
    reason=1;
    if(length(a.rgb-b.rgb)<.006) return fgWarp(uv,uv,1);
    float2 projectedAUV,projectedBUV;
    bool hasA=fgProjectedSource(uv,forward,params,projectedA,t,projectedAUV);
    bool hasB=fgProjectedSource(uv,backward,params,projectedB,1.0-t,projectedBUV);
    if(hasA&&hasB&&fgTryPair(uv,projectedAUV,projectedBUV,previous,current,forward,backward,params)) {
        reason=0;return fgWarp(projectedAUV,projectedBUV,2);
    }
    if(hasA) {
        float2 endpoint=projectedAUV+forward.sample(s,projectedAUV).xy*params.inverseLumaSize;
        if(fgTryPair(uv,projectedAUV,endpoint,previous,current,forward,backward,params)) {reason=0;return fgWarp(projectedAUV,endpoint,2);}
    }
    if(hasB) {
        float2 endpoint=projectedBUV+backward.sample(s,projectedBUV).xy*params.inverseLumaSize;
        if(fgTryPair(uv,endpoint,projectedBUV,previous,current,forward,backward,params)) {reason=0;return fgWarp(endpoint,projectedBUV,2);}
    }
    // Solve each inverse warp at its own source position. Sampling flow at
    // the intermediate output coordinate is exact only for uniform pans.
    float2 uvA=uv, uvB=uv;
    for(uint i=0;i<4;++i) {
        uvA=uv-t*forward.sample(s,uvA).xy*params.inverseLumaSize;
        uvB=uv-(1.0-t)*backward.sample(s,uvB).xy*params.inverseLumaSize;
    }
    float4 f=forward.sample(s,uvA), reverse=backward.sample(s,uvB);
    bool validA=all(uvA>=0.0)&&all(uvA<=1.0);
    bool validB=all(uvB>=0.0)&&all(uvB<=1.0);
    reason=3;
    if(!validA||!validB) {
        // An image edge can be visible in only one source. A high-confidence
        // one-sided warp is still usable; never sample clamped offscreen data
        // as though it supplied a matching second observation.
        reason=7;
        if(validA&&f.z>.9) return fgWarp(uvA,uvB,0);
        if(validB&&reverse.z>.9) return fgWarp(uvA,uvB,1);
        reason=3;
        // An unwarped source pixel here tears against its warped neighbours
        // during a pan; keep the onscreen side on its own flow instead.
        if(validA&&(t<.5||!validB)) return fgWarp(uvA,uvB,0);
        if(validB) return fgWarp(uvA,uvB,1);
        return fgWarp(uv,uv,t<.5 ? 0 : 1);
    }
    float consistency=length(f.xy+reverse.xy);
    float trust=min(f.z,reverse.z)*(1.0-smoothstep(.75,2.0,consistency));
    float4 warpedA=previous.sample(s,uvA), warpedB=current.sample(s,uvB);
    bool colorMismatch=length(warpedA.rgb-warpedB.rgb)>.22;
    if(trust<.65||colorMismatch) {
        float2 fromB=uvB+reverse.xy*params.inverseLumaSize;
        float2 fromA=uvA+f.xy*params.inverseLumaSize;
        // Same order as before: the nearer source's pair is tried first.
        for(uint k=0;k<2;++k) {
            bool aFirst=(params.t<.5)==(k==0);
            if(aFirst&&fgTryPair(uv,uvA,fromA,previous,current,forward,backward,params)) {reason=0;return fgWarp(uvA,fromA,2);}
            if(!aFirst&&fgTryPair(uv,fromB,uvB,previous,current,forward,backward,params)) {reason=0;return fgWarp(fromB,uvB,2);}
        }
        reason=trust<.65 ? 2 : 4;
        // Same as above: the nearer source along its own flow, not unwarped.
        return fgWarp(uvA,uvB,t<.5 ? 0 : 1);
    }
    reason=length(f.xy)>.05||length(reverse.xy)>.05 ? 0 : 6;
    return fgWarp(uvA,uvB,2);
}
// A static overlay (HUD) is identical in both sources. A moving surface whose
// warp source lands on such static pixels would copy the overlay to a
// displaced position (doubled hearts/icons); take that color from the other
// source instead, or keep the plain current pixel when both are static.
inline bool fgStaticSource(float2 at, float2 uv, texture2d<float> previous, texture2d<float> current,
                           constant FGParams& params) {
    constexpr sampler s(coord::normalized,address::clamp_to_edge,filter::linear);
    return length((at-uv)/params.inverseLumaSize)>.1&&
           length(previous.sample(s,at).rgb-current.sample(s,at).rgb)<.006;
}
inline FGWarp fgAvoidStaticSources(FGWarp w, float2 uv, texture2d<float> previous, texture2d<float> current,
                                   constant FGParams& params) {
    if(w.mode==1&&all(w.b==uv)) return w;
    bool staticA=w.mode!=1&&fgStaticSource(w.a,uv,previous,current,params);
    bool staticB=w.mode!=0&&fgStaticSource(w.b,uv,previous,current,params);
    if(!staticA&&!staticB) return w;
    if(staticA&&!staticB&&w.mode==2) return fgWarp(w.a,w.b,1);
    if(staticB&&!staticA&&w.mode==2) return fgWarp(w.a,w.b,0);
    return fgWarp(uv,uv,1);
}
inline float4 fgResolve(float2 uv,
    texture2d<float> previous, texture2d<float> current,
    texture2d<float> forward, texture2d<float> backward,
    constant FGParams& params, thread uint& reason, const device uint* projectedA, const device uint* projectedB) {
    FGWarp w=fgAvoidStaticSources(fgResolveWarp(uv,previous,current,forward,backward,params,reason,projectedA,projectedB),uv,previous,current,params);
    return fgSampleWarp(w,previous,current,clamp(params.t,0.0,1.0));
}

// HUD mask, per warp-decision pixel. Seeds are bright, near-white pixels that
// are identical in both sources and stand out from their surroundings (HUD
// icons and text); a 2-pixel margin then covers their soft, semi-transparent
// edges and shadows, which mix with the moving background and would tear.
struct FGMaskSize { uint width, height; };
// Seeds are rare, so each one marks its 5x5 neighbourhood in a cleared byte
// mask (scatter) instead of every pixel gathering 25 neighbours.
kernel void fgStaticSeed(texture2d<float> previous [[texture(0)]], texture2d<float> current [[texture(1)]],
    device uchar* mask [[buffer(0)]], constant FGMaskSize& size [[buffer(1)]], uint2 p [[thread_position_in_grid]]) {
    if(p.x>=size.width||p.y>=size.height) return;
    constexpr sampler s(coord::normalized,address::clamp_to_edge,filter::linear);
    const float2 uv=(float2(p)+.5)/float2(size.width,size.height),texel=1.0/float2(size.width,size.height);
    float3 a=previous.sample(s,uv).rgb,b=current.sample(s,uv).rgb;
    if(length(a-b)>=.006||min(a.r,min(a.g,a.b))<=.75) return;
    float contrast=0;
    for(int i=0;i<4;++i) {
        float2 d=float2(i==0?2:i==1?-2:0,i==2?2:i==3?-2:0)*texel;
        contrast=max(contrast,dot(a-previous.sample(s,uv+d).rgb,float3(.299,.587,.114)));
    }
    if(contrast<=.2) return;
    for(int y=-2;y<=2;++y) for(int x=-2;x<=2;++x) {
        int2 q=int2(p)+int2(x,y);
        if(all(q>=0)&&q.x<int(size.width)&&q.y<int(size.height)) mask[q.y*size.width+q.x]=1;
    }
}
inline bool fgMasked(const device uchar* mask, constant FGMaskSize& size, float2 uv) {
    uint2 at=min(uint2(uv*float2(size.width,size.height)),uint2(size.width-1,size.height-1));
    return mask[at.y*size.width+at.x]!=0;
}

fragment float4 fgInterpolate(FGVertex v [[stage_in]],
    texture2d<float> previous [[texture(0)]], texture2d<float> current [[texture(1)]],
    texture2d<float> forward [[texture(2)]], texture2d<float> backward [[texture(3)]],
    constant FGParams& params [[buffer(0)]], const device uint* projectedA [[buffer(1)]],
    const device uint* projectedB [[buffer(2)]], const device uchar* mask [[buffer(3)]],
    constant FGMaskSize& maskSize [[buffer(4)]]) {
    const float t=clamp(params.t,0.0,1.0);
    if(fgMasked(mask,maskSize,v.uv)) return fgSampleWarp(fgWarp(v.uv,v.uv,t<.5 ? 0 : 1),previous,current,t);
    uint reason;
    return fgResolve(v.uv,previous,current,forward,backward,params,reason,projectedA,projectedB);
}

// Warp decisions once per game pixel: a Retina drawable shows each game
// pixel as 2x2 (or more) output pixels that share the same surface motion.
// Colors are still sampled per output pixel, so sharpness is unchanged.
kernel void fgDecide(texture2d<float> previous [[texture(0)]], texture2d<float> current [[texture(1)]],
    texture2d<float> forward [[texture(2)]], texture2d<float> backward [[texture(3)]],
    texture2d<float,access::write> offsets [[texture(4)]], texture2d<uint,access::write> modes [[texture(5)]],
    constant FGParams& params [[buffer(0)]], const device uint* projectedA [[buffer(1)]],
    const device uint* projectedB [[buffer(2)]], const device uchar* mask [[buffer(3)]],
    uint2 p [[thread_position_in_grid]]) {
    if(any(p>=uint2(offsets.get_width(),offsets.get_height()))) return;
    float2 uv=(float2(p)+.5)/float2(offsets.get_width(),offsets.get_height());
    if(mask[p.y*offsets.get_width()+p.x]!=0) {offsets.write(float4(0),p);modes.write(uint4(params.t<.5 ? 0 : 1),p);return;}
    uint reason;
    FGWarp w=fgAvoidStaticSources(fgResolveWarp(uv,previous,current,forward,backward,params,reason,projectedA,projectedB),uv,previous,current,params);
    offsets.write(float4(w.a-uv,w.b-uv),p);
    modes.write(uint4(w.mode),p);
}
fragment float4 fgInterpolateDecided(FGVertex v [[stage_in]],
    texture2d<float> previous [[texture(0)]], texture2d<float> current [[texture(1)]],
    texture2d<float,access::read> offsets [[texture(2)]], texture2d<uint,access::read> modes [[texture(3)]],
    constant FGParams& params [[buffer(0)]]) {
    uint2 size=uint2(offsets.get_width(),offsets.get_height());
    uint2 at=min(uint2(v.uv*float2(size)),size-1);
    float4 d=offsets.read(at);
    return fgSampleWarp(fgWarp(v.uv+d.xy,v.uv+d.zw,modes.read(at).x),previous,current,clamp(params.t,0.0,1.0));
}

kernel void fgAudit(texture2d<float> previous [[texture(0)]], texture2d<float> current [[texture(1)]],
    texture2d<float> forward [[texture(2)]], texture2d<float> backward [[texture(3)]],
    constant FGParams& params [[buffer(0)]], device atomic_uint* counts [[buffer(1)]],
    const device uint* projectedA [[buffer(2)]], const device uint* projectedB [[buffer(3)]],
    uint2 p [[thread_position_in_grid]]) {
    if(any(p>=uint2(32,18))) return;
    uint reason;
    fgResolve((float2(p)+.5)/float2(32,18),previous,current,forward,backward,params,reason,projectedA,projectedB);
    atomic_fetch_add_explicit(&counts[reason],1u,memory_order_relaxed);
}
)";
