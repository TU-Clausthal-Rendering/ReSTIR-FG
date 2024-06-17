#include "Scene/SceneDefines.slangh"
#include "Utils/Math/MathConstants.slangh"

import Utils.Math.PackedFormats;
import Utils.Math.FormatConversion;

import Scene.RaytracingInline;
import Scene.Material.ShadingUtils;
import Utils.Sampling.SampleGenerator;
import Utils.Geometry.GeometryHelpers;
import Utils.Math.MathHelpers;
import Utils.Color.ColorHelpers;

import Reservoir;
import SurfaceDataFG;
import FinalGatherData;

#if USE_REDUCED_RESERVOIR_FORMAT
    RWTexture2D<uint2> gReservoir;
    Texture2D<uint2> gReservoirPrev;
#else
    RWTexture2D<uint4> gReservoir;
    Texture2D<uint4> gReservoirPrev;
#endif

RWStructuredBuffer<FGData> gFGSampleData;
StructuredBuffer<FGData> gFGSampleDataPrev;
StructuredBuffer<PackedSurfaceDataFG> gSurface;
StructuredBuffer<PackedSurfaceDataFG>gSurfacePrev;
Texture2D<float2> gMVec;
Texture2D<float4> gView;
Texture2D<float4> gPrevView;
RWStructuredBuffer<SampleGenerator> gSampleGenState;

#define BIAS_MODE_BASIC 1
#define BIAS_MODE_RAYTRACE 2
//This is for syntax highlighting
#ifndef BIAS_CORRECTION_MODE
    #define BIAS_CORRECTION_MODE 2
#endif

cbuffer PerFrame
{
    uint gFrameCount;                   //Current it for sample gen
}

cbuffer Constant
{
    uint2 gFrameDim;                //Frame Dimensions
    uint gMaxAge;                   //M limit            
    uint gSpatialSamples;           //Number of spatial samples
    float gSamplingRadius;          //Sampling radius
    float gDepthThreshold;          //Relative depth offset
    float gNormalThreshold;         //Maximum cosine between both Normals that is allowed
    uint gDisocclusionBoostSamples; //Number of spatial samples if no temporal surface was found
    float gAttenuationRadius;       //Radius for the attenuation
    float2 gJacobianMinMax;         //Minimum and Maximum values for the jacobian
    uint _pad;
}

struct LightSample{
    float3 dir;
    float Li;
};

struct PhotonLight{
    float4 pos;
    float4 flux;
};

static const float kRayEpsilon = 0.99f;
static const float kRayNearEpsilon = 0.01f;


SurfaceFG getSurface(uint2 idx, bool prev)
{
    float3 viewDir;
    if (prev)
        viewDir = gPrevView[idx].xyz;
    else
        viewDir = gView[idx].xyz;

    PackedSurfaceDataFG surfaceData;
    if (prev)
        surfaceData = gSurfacePrev[index2Dto1D(idx, gFrameDim.x)];
    else
        surfaceData = gSurface[index2Dto1D(idx, gFrameDim.x)];
    
    return SurfaceFG(surfaceData, viewDir);
}

bool getLightSample(in FGData fgData, in float3 worldPos, out LightSample ls, out float sampleDistance)
{
    //Get light position    
    ls.dir = fgData.posW - worldPos;
    sampleDistance = length(ls.dir);
    ls.dir = ls.dir / sampleDistance; //normalize
    
    ls.Li = luminance(fgData.flux);
    float distAttenuation = vplAttenuation(sampleDistance, gAttenuationRadius);
    float cosTheta = dot(normalize(fgData.normal), -ls.dir);
    float jacobian = (cosTheta * distAttenuation) / fgData.jacobianDeterminant;

    //Jacobian is outside of valid values (Solid angle ratio too different)
    if(jacobian < gJacobianMinMax.x || jacobian > gJacobianMinMax.y || cosTheta < kMinCosTheta)
        return false;

    ls.Li *= jacobian;
        
    return ls.Li >= 0; //true if sample is valid
}

void temporalResampling(uint2 pixel, inout SampleGenerator sg, inout Reservoir r, inout FGData fgSample, in SurfaceFG surface)
{
    uint historyLimit = gMaxAge * r.M;

    //Reproject to the previous pixel using the motion vectors
    float2 motionOffset = gMVec[pixel] * float2(gFrameDim);
    float2 prevIdxF = motionOffset + float2(pixel);
    float2 rndOffset = sampleNext2D(sg) - 0.5f;
    prevIdxF += rndOffset;
    
    uint2 prevIdx = uint2(round(prevIdxF));

    //Check if a neighbor surface matches up
    //Try to find a matching surface in the neighborhood of the reprojected pixel
    SurfaceFG temporalSurface;
    bool selectPrevSample = false;
    uint previousM = 0;
    bool foundTemporalSurface = false;
    int2 idx = (0, 0);
    float temporalSearchRadius = 4.f; //actual radius is 2 due to random value in [-0.5,0.5]
    
    for (uint i = 0; i < 9; i++)
    {
        int2 offset = int2(0, 0);
        if (i > 0)
        {
            offset = int2((sampleNext2D(sg) - 0.5f) * temporalSearchRadius);
        }

        idx = prevIdx + offset;

        //Return if index is invalid
        if(any(idx < 0) || any(idx >= gFrameDim))
            continue;
        
        //Get Hit sample from last frame
        temporalSurface = getSurface(idx, true);
        if (!temporalSurface.valid)
            continue;

        //Test surface similarity and discard if too different
        if (!surface.isValidNeighbor(temporalSurface, gDepthThreshold, gNormalThreshold))
            continue;

        //Valid hit was found
        foundTemporalSurface = true;
        break;
    }

    //If temporal surface was found, resample the previous sample
    if (foundTemporalSurface)
    {
        Reservoir prev = Reservoir(gReservoirPrev[idx]);
        prev.M = min(prev.M, historyLimit);
        previousM = prev.M;

        bool sampleValid = true;
        FGData fgPrev = gFGSampleDataPrev[index2Dto1D(idx, gFrameDim.x)];
        //Kill the reservoir if the sample is no longer valid or was replaced
        if (!fgPrev.valid)
        {
            prev.weightSum = 0;
            prev.M = 0;
            sampleValid = false;
        }

        //Get target pdf at current location if reservoir and sample are valid
        float targetFunc = 0;
        bool combine = true;
        if (prev.M > 0 && sampleValid)
        {
            LightSample ls = { };
            float dis = 0;
            if (!getLightSample(fgPrev, surface.posW, ls, dis))
                combine = false;
            targetFunc = surface.getPdf(ls.dir, ls.Li);
        }

        if(combine)
            selectPrevSample = r.combineReservoir(prev, targetFunc, sampleNext1D(sg));
        if (selectPrevSample)
            fgSample = fgPrev;
    }


    //Bias correction via MIS
#if BIAS_CORRECTION_MODE >= BIAS_MODE_BASIC
    float pi = r.targetFunc;
    float piSum = r.targetFunc; //piSum = target * M_initial ; M_initial is always 1 for us

    if (foundTemporalSurface)
    {
        LightSample ls = { };
        float dist = 0;
        bool valid = getLightSample(fgSample, temporalSurface.posW, ls, dist);
        float ps = valid ? temporalSurface.getPdf(ls.dir, ls.Li) : 0.f;

#if BIAS_CORRECTION_MODE >= BIAS_MODE_RAYTRACE
        if (ps > 0)
        {
            SceneRayQuery < 0 > rayQuery;
            dist *= kRayEpsilon;
            float rayNear = dist * kRayNearEpsilon;
            dist -= rayNear;
            const Ray ray = Ray(computeRayOrigin(temporalSurface.posW, temporalSurface.normal), ls.dir, rayNear, dist);
            if (!rayQuery.traceVisibilityRay(ray, RAY_FLAG_NONE, 0xff))
            {
                ps = 0;
            }
        }
#endif // BIAS MODE RAYTRACE
        
        pi = selectPrevSample ? ps : pi;
        piSum += ps * previousM;
    }
    r.finalizeSample(pi, piSum);
    
#else    
    r.finalizeSample(1.0f, float(r.M));
#endif //BIAS_MODE_BASIC
}

void spatialResampling(uint2 pixel, inout SampleGenerator sg, inout Reservoir r, inout FGData fgSample, in SurfaceFG surface)
{    
    uint numSamples = clamp(gSpatialSamples + 1, 1, 8); //Cap at 8 samples for the later bias correction
    uint cachedIndices[8]; //Store all indeces. 32 Bits per 2D index (16 per element)
    uint cachedM[4];
    int selectedSample = -1; //Index of the selected sample

    //Go through all neighbors
    for (uint i = 0; i < numSamples; i++)
    {
        cachedIndices[i] = 0xFFFFFFFF; //Set to max int
        if (i % 2 == 0)
            cachedM[i / 2] = 0;
        
        int2 idx = int2(pixel);

        float2 randSamp = (sampleNext2D(sg) * 2.f) - 1.f; //random sample in [-1,1]
        int2 spatialOffset = int2(round(randSamp * gSamplingRadius));
        idx += spatialOffset;

        //Outside index is invalid
        if(any(idx < 0) || any(idx >= gFrameDim))
            continue;

        //Get surface and check if valid
        SurfaceFG neighSurface = getSurface(idx, false); //Get surface
        if (!neighSurface.valid)
            continue;
        
        if (!surface.isValidNeighbor(neighSurface, gDepthThreshold, gNormalThreshold))
            continue;

        //Neighbor passed the test, start resampling
        Reservoir sampledReservoir = Reservoir(gReservoirPrev[idx]);    //Current reservoir is stored in prev
        
        bool sampleValid = true;
        FGData fgNeigh = gFGSampleDataPrev[index2Dto1D(idx, gFrameDim.x)];
        //Kill the reservoir if the sample is no longer valid or was replaced
        if (!fgNeigh.valid)
        {
            sampledReservoir.weightSum = 0;
            sampledReservoir.M = 0;
            sampleValid = false;
        }
        //Store indices
        cachedIndices[i] = (idx.x & 0xFFFF) << 16 | (idx.y & 0xFFFF);
        uint M = 0xFFFF & sampledReservoir.M;
        if (i % 2 != 0)
            M = M << 16;
        cachedM[i / 2] |= M;

        //Get neightbor target PDF for current surface
        float sampletargetFunc = 0.f;
        if (sampledReservoir.M > 0 && sampleValid)
        {
            LightSample ls = { };
            float dis;
            if (!getLightSample(fgNeigh, surface.posW, ls, dis))
                continue;
            sampletargetFunc = surface.getPdf(ls.dir, ls.Li);
        }
        //Combine Reservoirs
        if (r.combineReservoir(sampledReservoir, sampletargetFunc, sampleNext1D(sg)))
        {
            selectedSample = i;
            fgSample = fgNeigh;
        }
    }

    //Bias correction
    #if BIAS_CORRECTION_MODE >= BIAS_MODE_BASIC
        
        float pi = r.targetFunc;
        float piSum = r.targetFunc; //piSum = target * M_initial ; M_initial is always 1 for us
        
        //go through all neighbors again
        for (int i = 0; i < numSamples; i++)
        {
            //Skip if we skipped in last loop
            if ((cachedIndices[i]) == 0xFFFFFFFF)
                continue;
            //Get Index
            uint2 idx = uint2(cachedIndices[i] >> 16, (cachedIndices[i] & 0xFFFF));
            //Get Surface
            SurfaceFG biasSurface = getSurface(idx, false);
            if (!biasSurface.valid)
                continue;
        
            //Get light pdf
            float ps = 0.0f;
            LightSample ls = { };
            float dist = 0;

            //Skip Sample if it was rejected by jacobian
            if (!getLightSample(fgSample, biasSurface.posW, ls, dist))
                continue;
            ps = biasSurface.getPdf(ls.dir, ls.Li);
        
            //Trace ray
    #if BIAS_CORRECTION_MODE >= BIAS_MODE_RAYTRACE
            if (ps > 0)
            {
                SceneRayQuery < 0 > rayQuery;
                dist *= kRayEpsilon;
                float rayNear = dist * kRayNearEpsilon;
                dist -= rayNear;
                const Ray ray = Ray(computeRayOrigin(biasSurface.posW, biasSurface.normal), ls.dir, rayNear, dist);
                if (!rayQuery.traceVisibilityRay(ray, RAY_FLAG_CULL_NON_OPAQUE, 0xff))
                {
                    ps = 0.0f;
                }
            }
    #endif // BIAS MODE RAYTRACE
        
            uint M = cachedM[i / 2];
            if (i % 2 == 0)
                M = M & 0xFFFF;
            else
                M = M >> 16;
        
            pi = selectedSample == i ? ps : pi;
            piSum += ps * M;
        }
        r.finalizeSample(pi, piSum);
        
    #else //BIAS MODE BASIC
        r.finalizeSample(1.0f, r.M);
    #endif //BIAS MODE BASIC    
}

void spatiotemporalResampling(uint2 pixel, inout SampleGenerator sg, inout Reservoir r, inout FGData fgSample, in SurfaceFG surface)
{
    uint historyLimit = gMaxAge;

    //Check if motion vectors are inside of the frame
    float2 motionOffset = gMVec[pixel] * float2(gFrameDim);
    float2 prevIdxF = motionOffset + float2(pixel);
    float2 rndOffset = sampleNext2D(sg) - 0.5f;
    prevIdxF += rndOffset;
    
    int2 prevIdx = int2(round(prevIdxF));

    //Get surface from last frame
    bool foundTemporalSurface = false;
    int2 temporalOffset = int2(0, 0);
    const float temporalSearchRadius = 4.f; //actual radius is 2 due to random value in [-0.5,0.5]

    //Try to find a matching surface in the neighborhood of the reprojected pixel
    for (uint i = 0; i < 9; i++)
    {
        int2 offset = int2(0, 0);
        if (i > 0)
        {
            offset = int2((sampleNext2D(sg) - 0.5f) * temporalSearchRadius);
        }

        int2 idx = prevIdx + offset;

        //Return if idx is not valid
        if(any(idx < 0) || any(idx >= gFrameDim))
            continue;
        
        //Get Hit sample from last frame
        SurfaceFG temporalSurface = getSurface(idx, true);
        if (!temporalSurface.valid)
            continue;

        //Test surface similarity and discard if too different
        if (!surface.isValidNeighbor(temporalSurface, gDepthThreshold, gNormalThreshold))
            continue;
        
        //Valid hit was found
        temporalOffset = offset;
        foundTemporalSurface = true;
        break;
    }

    //
    //Spartio Temporal Resampling
    //
    
    uint numSamples = foundTemporalSurface ? gSpatialSamples + 1 : max(gSpatialSamples + 1, gDisocclusionBoostSamples);
    numSamples = clamp(numSamples, 1, 8); //Cap at 8 samples for the later bias correction
    uint cachedIndices[8]; //Store all indeces. 32 Bits per 2D index (16 per element)
    uint cachedM[4];
    
    int selectedSample = -1;
    bool validSample = true;

    //Go through all neighbors
    for (uint i = 0; i < numSamples; i++)
    {
        cachedIndices[i] = 0xFFFFFFFF; //Set to max int
        if (i % 2 == 0)
            cachedM[i / 2] = 0;
        int2 idx = int2(0);

        //Take temporal sample or generate a new neighbor sample
        if (i == 0 && foundTemporalSurface)
        {
            idx = prevIdx + temporalOffset;
        }
        else
        {
            //Get sample from sampling mask
            float2 randSamp = (sampleNext2D(sg) * 2.f) - 1.f; //random sample in [-1,1]
            int2 spatialOffset = int2(round(randSamp * gSamplingRadius));
            idx = prevIdx + spatialOffset;

            //Outside index is invalid
            if(any(idx < 0) || any(idx >= gFrameDim))
                continue;
            
             //Get surface and check if valid
            SurfaceFG neighSurface = getSurface(idx, true);
            if (!neighSurface.valid)
                continue;
        
            if (!surface.isValidNeighbor(neighSurface, gDepthThreshold, gNormalThreshold))
                continue;
        }
            
        //If neighbor surface has passed the tests get reservoir and compair them
        Reservoir sampledReservoir = Reservoir(gReservoirPrev[idx]);
        sampledReservoir.M = min(sampledReservoir.M, historyLimit);

        FGData fgPrev = gFGSampleDataPrev[index2Dto1D(idx, gFrameDim.x)];

        //Kill the reservoir if it is not valid
        bool sampleValid = true;
        if (!fgPrev.valid)
        {
            sampledReservoir.weightSum = 0;
            sampledReservoir.M = 0;
            sampleValid = false;
        }

        //Store indices
        cachedIndices[i] = (idx.x & 0xFFFF) << 16 | (idx.y & 0xFFFF);
        uint M = 0xFFFF & sampledReservoir.M;
        if (i % 2 != 0)
            M = M << 16;
        cachedM[i / 2] |= M;
        
        //Get neightbor light target PDF for current surface
        float sampletargetFunc = 0.f;
        if (sampledReservoir.M > 0 && sampleValid)
        {
            LightSample ls = { };
            float dis;
            if (!getLightSample(fgPrev, surface.posW, ls, dis))
                continue;
            sampletargetFunc = surface.getPdf(ls.dir, ls.Li);
        }
        //Combine Reservoirs
        if (r.combineReservoir(sampledReservoir, sampletargetFunc, sampleNext1D(sg)))
        {
            selectedSample = i;
            fgSample = fgPrev;
        }
    }

  //Bias correction
    #if BIAS_CORRECTION_MODE >= BIAS_MODE_BASIC
        
        float pi = r.targetFunc;
        float piSum = r.targetFunc; //piSum = target * M_initial
        
        //go through all neighbors again
        for (int i = 0; i < numSamples; i++)
        {
            //Skip if we skipped in last loop
            if ((cachedIndices[i]) == 0xFFFFFFFF)
                continue;
            //Get Index
            uint2 idx = uint2((cachedIndices[i] >> 16) & 0xFFFF, (cachedIndices[i] & 0xFFFF));
            //Get Surface
            SurfaceFG biasSurface = getSurface(idx, true);
        
            //Get light pdf
            float ps = 0.0f;
            LightSample ls = { };
            float dist = 0;

            //Skip Sample if it was rejected by jacobian
            if (biasSurface.valid)
            {
                if(getLightSample(fgSample, biasSurface.posW, ls, dist))
                    ps = biasSurface.getPdf(ls.dir, ls.Li);
            }
                    
            //Trace ray
    #if BIAS_CORRECTION_MODE >= BIAS_MODE_RAYTRACE
            if (ps > 0)
            {
                SceneRayQuery < 0 > rayQuery;
                dist *= kRayEpsilon;
                float rayNear = dist * kRayNearEpsilon;
                dist -= rayNear;
                const Ray ray = Ray(computeRayOrigin(biasSurface.posW, biasSurface.normal), ls.dir, rayNear, dist);
                if (!rayQuery.traceVisibilityRay(ray, RAY_FLAG_CULL_NON_OPAQUE, 0xff))
                {
                    ps = 0.0f;
                }
            }
    #endif // BIAS MODE RAYTRACE
    
            uint M = cachedM[i / 2];
            if (i % 2 == 0)
                M = M & 0xFFFF;
            else
                M = (M >> 16) & 0xFFFF;
        
            pi = selectedSample == i ? ps : pi;
            piSum += ps * M;
        }
        r.finalizeSample(pi, piSum);
        
    #else //BIAS MODE BASIC
        r.finalizeSample(1.0f, r.M);
    #endif //BIAS MODE BASIC  
}

[numthreads(16, 16, 1)]
void main(uint2 dTid : SV_DispatchThreadID)
{    
    //Check if surface is valid
    SurfaceFG surface = getSurface(dTid, false);
    if (!surface.valid)
        return;

    #if STORE_SAMPLE_GEN_STATE
        SampleGenerator sg = gSampleGenState[index2Dto1D(dTid,gFrameDim.x)];
    #else
        SampleGenerator sg = SampleGenerator(dTid, gFrameCount + 5 * 32);
    #endif
    
    //Load reservoir from current iteration
    Reservoir r = { };
    FGData fgData = FGData();

    //Load current reservoir in 
#if MODE_SPATIAL
    Reservoir current = Reservoir(gReservoirPrev[dTid]);
    if (current.weightSum > 0)
    {
        fgData = gFGSampleDataPrev[index2Dto1D(dTid, gFrameDim.x)];
    }
#else //Mode Temporal or Spatiotemporal
    Reservoir current = Reservoir(gReservoir[dTid]);
    if (current.weightSum > 0)
    {
        fgData = gFGSampleData[index2Dto1D(dTid, gFrameDim.x)];
    }
#endif //MODE_SPATIAL

    r.combineReservoir(current, current.targetFunc, 0.5 /* random */);
    //Do resampling
#if MODE_SPATIOTEMPORAL
    spatiotemporalResampling(dTid, sg, r, fgData, surface);
#elif MODE_TEMPORAL
    temporalResampling(dTid, sg, r, fgData, surface);
#elif MODE_SPATIAL
    spatialResampling(dTid, sg, r, fgData, surface);
#endif //Resampling Mode
    
    //Store reservoir and sample data    
#if USE_REDUCED_RESERVOIR_FORMAT
    gReservoir[dTid] = r.packReduced();
#else
    gReservoir[dTid] = r.pack();
#endif

    gFGSampleData[index2Dto1D(dTid, gFrameDim.x)] = fgData;

    #if STORE_SAMPLE_GEN_STATE
        gSampleGenState[index2Dto1D(dTid, gFrameDim.x)] = sg;
    #endif //STORE_SAMPLE_GEN_STATE
}
