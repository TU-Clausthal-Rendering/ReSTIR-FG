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

#define BIAS_CORRECTION 1

#if USE_REDUCED_RESERVOIR_FORMAT
    RWTexture2D<uint2> gReservoir[2];
    Texture2D<uint2> gReservoirPrev[2];
#else
    RWTexture2D<uint4> gReservoir[2];
    Texture2D<uint4> gReservoirPrev[2];
#endif

RWStructuredBuffer<CausticSample> gCausticSample[2];
StructuredBuffer<CausticSample> gCausticSamplePrev[2];
StructuredBuffer<PackedSurfaceDataFG> gSurface;
StructuredBuffer<PackedSurfaceDataFG>gSurfacePrev;
Texture2D<float2> gMVec;
Texture2D<float4> gView;
Texture2D<float4> gPrevView;
RWStructuredBuffer<SampleGenerator> gSampleGenState;

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
    uint gDisocclusionBoostSamples; // Number of spatial samples if no temporal surface was found
    float2 gPhotonRadius;            //Caustic Photon Radius
}

static const float kRayEpsilon = 0.99f;
static const float kRayNearEpsilon = 0.01f;
static const bool kCollectDirectPhotons = RESERVOIR_PHOTON_DIRECT;

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

void temporalResampling(uint2 pixel, inout SampleGenerator sg, inout Reservoir r, inout CausticSample causticSample, in SurfaceFG surface, uint buffIdx = 0)
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
    float temporalSearchRadius = 1.f; //actual radius is 2 due to random value in [-0.5,0.5]
    
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

        //Test if a sample from the temporal surface could be in the domain of the current surface
        if (distance(surface.posW, temporalSurface.posW) > (gPhotonRadius[buffIdx] * 2))
            continue;

        //Valid hit was found
        foundTemporalSurface = true;
        break;
    }

    //If temporal surface was found, resample the previous sample
    if (foundTemporalSurface)
    {
        Reservoir prev = Reservoir(gReservoirPrev[buffIdx][idx]);
        prev.M = min(prev.M, historyLimit);
        previousM = prev.M;

        CausticSample cauPrev = gCausticSamplePrev[buffIdx][index2Dto1D(idx, gFrameDim.x)];
       
        //Get target pdf at current location if reservoir and sample are valid
        float targetFunc = 0;
        if (prev.M > 0)
        {
            //Target Function
            float NdotL = dot(cauPrev.dir, surface.normal);
            if (NdotL > kMinCosTheta)
                targetFunc = surface.getPdf(cauPrev.dir, luminance(cauPrev.flux)) / NdotL;

            // Shift into the domain
            if (distance(cauPrev.pos, surface.posW) > gPhotonRadius[buffIdx])
                targetFunc = 0;

            selectPrevSample = r.combineReservoir(prev, targetFunc, sampleNext1D(sg));
        }

        if (selectPrevSample)
            causticSample = cauPrev;
    }


    //Bias correction via MIS
#if BIAS_CORRECTION
    float pi = r.targetFunc;
    float piSum = r.targetFunc; //piSum = target * M_initial ; M_initial is always 1 for us

    if (foundTemporalSurface)
    {
        float ps = 0.0;
        float NdotL = dot(causticSample.dir, temporalSurface.normal);
        if (NdotL > kMinCosTheta)
            ps = temporalSurface.getPdf(causticSample.dir, luminance(causticSample.flux)) / NdotL;
        if (distance(causticSample.pos, temporalSurface.posW) > gPhotonRadius[buffIdx])
            ps = 0.0;

        pi = selectPrevSample ? ps : pi;
        piSum += ps * previousM;
    }
    r.finalizeSample(pi, piSum);
#else  // BIAS_CORRECTION 
    r.finalizeSample(1.0f, float(r.M));
#endif // BIAS_CORRECTION
}

void spatiotemporalResampling(uint2 pixel, inout SampleGenerator sg, inout Reservoir r, inout CausticSample causticSample, in SurfaceFG surface, uint buffIdx = 0)
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
    const float temporalSearchRadius = 1.f; //actual radius is 1 due to random value in [-0.5,0.5]

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

        //Test if a sample from the temporal surface could be in the domain of the current surface
        if (distance(surface.posW, temporalSurface.posW) > (gPhotonRadius[buffIdx] * 2))
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

            //Reject if the spatial sample is the same as the temporal sample
            if(all(spatialOffset == temporalOffset))
                continue;

             //Get surface and check if valid
            SurfaceFG neighSurface = getSurface(idx, true);
            if (!neighSurface.valid)
                continue;
        
            if (!surface.isValidNeighbor(neighSurface, gDepthThreshold, gNormalThreshold))
                continue;

            //Test if a sample from the temporal surface could be in the domain of the current surface
            if (distance(surface.posW, neighSurface.posW) > (gPhotonRadius[buffIdx] * 2))
                continue;
        }
            
        //If neighbor surface has passed the tests get reservoir and compair them
        Reservoir sampledReservoir = Reservoir(gReservoirPrev[buffIdx][idx]);
        sampledReservoir.M = min(sampledReservoir.M, historyLimit);

        CausticSample cauPrev = gCausticSamplePrev[buffIdx][index2Dto1D(idx, gFrameDim.x)];

        //Store indices
        cachedIndices[i] = (idx.x & 0xFFFF) << 16 | (idx.y & 0xFFFF);
        uint M = 0xFFFF & sampledReservoir.M;
        if (i % 2 != 0)
            M = M << 16;
        cachedM[i / 2] |= M;
        
        //Get neightbor light target PDF for current surface
        float targetFunc = 0.f;
        if (sampledReservoir.M > 0)
        {
            //Target Function
            float NdotL = dot(cauPrev.dir, surface.normal);
            if (NdotL > kMinCosTheta)
                targetFunc = surface.getPdf(cauPrev.dir, luminance(cauPrev.flux)) / NdotL;

            // Shift into the domain
            if (distance(cauPrev.pos, surface.posW) > gPhotonRadius[buffIdx])
                targetFunc = 0;
        }
        //Combine Reservoirs
        if (r.combineReservoir(sampledReservoir, targetFunc, sampleNext1D(sg)))
        {
            selectedSample = i;
            causticSample = cauPrev;
        }
    }

  //Bias correction
    #if BIAS_CORRECTION
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
            //Check if sample could be generated at the bias surface
            if (biasSurface.valid)
            {
                float NdotL = dot(causticSample.dir, biasSurface.normal);
                if (NdotL > kMinCosTheta)
                    ps = biasSurface.getPdf(causticSample.dir, luminance(causticSample.flux)) / NdotL;
                if (distance(causticSample.pos, biasSurface.posW) > gPhotonRadius[buffIdx])
                    ps = 0.0;
            }
    
            uint M = cachedM[i / 2];
            if (i % 2 == 0)
                M = M & 0xFFFF;
            else
                M = (M >> 16) & 0xFFFF;
        
            pi = selectedSample == i ? ps : pi;
            piSum += ps * M;
        }
        r.finalizeSample(pi, piSum);
        
    #else //BIAS_CORRECTION
        r.finalizeSample(1.0f, r.M);
    #endif //BIAS_CORRECTION
}

[numthreads(16, 16, 1)]
void main(uint2 dTid : SV_DispatchThreadID)
{    
    //Check if surface is valid
    float4 viewDir = gView[dTid];
    SurfaceFG surface = SurfaceFG(gSurface[index2Dto1D(dTid, gFrameDim.x)],viewDir.xyz);

    bool collectDirect = (viewDir.w > 0.1) && kCollectDirectPhotons; // Check for direct photon collection
    if (!surface.valid)
        return;

    #if STORE_SAMPLE_GEN_STATE
        SampleGenerator sg = gSampleGenState[index2Dto1D(dTid, gFrameDim.x)];
    #else
        SampleGenerator sg = SampleGenerator(dTid, gFrameCount + 8 * 32);
    #endif

    //Load reservoir from current iteration
    Reservoir r = {};
    CausticSample causticData = CausticSample();

    //Load current reservoir
    Reservoir current = Reservoir(gReservoir[0][dTid]);
    if (current.M > 0)
    {
        causticData = gCausticSample[0][index2Dto1D(dTid, gFrameDim.x)];
    }
    
    r.combineReservoir(current, current.targetFunc, 0.5 /* random */);

    //Resampling
#if MODE_SPATIOTEMPORAL
    spatiotemporalResampling(dTid, sg, r, causticData, surface);
#elif MODE_TEMPORAL
    temporalResampling(dTid, sg, r, causticData, surface);
#endif //Resampling Mode


    //Store reservoir and sample data    
#if USE_REDUCED_RESERVOIR_FORMAT
    gReservoir[0][dTid] = r.packReduced();
#else
    gReservoir[0][dTid] = r.pack();
#endif
    gCausticSample[0][index2Dto1D(dTid, gFrameDim.x)] = causticData;

    //Second Resample pass for directly collected photons on some metal surfaces
    if(collectDirect)
    {
        r = {};
        causticData = CausticSample();

        //Load current reservoir 
        current = Reservoir(gReservoir[1][dTid]);
        if (current.M > 0)
        {
            causticData = gCausticSample[1][index2Dto1D(dTid, gFrameDim.x)];
        }
        r.combineReservoir(current, current.targetFunc, 0.5 /* random */);

        //Resampling
        #if MODE_SPATIOTEMPORAL
            spatiotemporalResampling(dTid, sg, r, causticData, surface, 1);
        #elif MODE_TEMPORAL
            temporalResampling(dTid, sg, r, causticData, surface, 1);
        #endif //Resampling Mode

    
            //Store reservoir and sample data    
        #if USE_REDUCED_RESERVOIR_FORMAT
            gReservoir[1][dTid] = r.packReduced();
        #else
            gReservoir[1][dTid] = r.pack();
        #endif
            gCausticSample[1][index2Dto1D(dTid, gFrameDim.x)] = causticData;
    }
    
    #if STORE_SAMPLE_GEN_STATE
        gSampleGenState[index2Dto1D(dTid, gFrameDim.x)] = sg;
    #endif //STORE_SAMPLE_GEN_STATE
}
