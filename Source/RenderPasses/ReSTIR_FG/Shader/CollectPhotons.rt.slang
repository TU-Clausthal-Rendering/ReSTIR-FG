#include "Scene/SceneDefines.slangh"
#include "Utils/Math/MathConstants.slangh"

import Scene.Raytracing;
import Scene.Intersection;
import Utils.Math.MathHelpers;
import Utils.Sampling.SampleGenerator;
import Scene.Material.ShadingUtils;
import Rendering.Lights.LightHelpers;
import Utils.Math.PackedFormats;
import Utils.Math.FormatConversion;
import Utils.Color.ColorHelpers;

import Reservoir;
import SurfaceDataFG;
import FinalGatherData;

//For syntax highlighting purposes
#ifndef MODE_FINAL_GATHER
    #define MODE_FINAL_GATHER 0
#endif

//For syntax highlighting purposes
#ifndef CAUSTIC_COLLECTION_MODE
    #define CAUSTIC_COLLECTION_MODE 3
#endif

//Stochastic Collect
#ifndef USE_STOCHASTIC_COLLECT
#define USE_STOCHASTIC_COLLECT 0
#endif

#ifndef STOCH_NUM_PHOTONS
#define STOCH_NUM_PHOTONS 3
#endif


cbuffer PerFrame
{
    uint gFrameCount;         // Frame count since scene was loaded.
    float gAttenuationRadius; // Radius for the distance attenuation
    float2 gPhotonRadius; // x->Global Radius, y-> Caustic Radius
    bool gCollectCaustic;   //Collect flag for caustic photons
    bool gCollectFG;        //Collect flag for final gather sample
}

//Constant settings for the temporal filter. Can be null
cbuffer TemporalFilter{
    uint gTemporalFilterHistoryLimit;
    float gDepthThreshold;              //Depth Threshold
    float gNormalThreshold;             //Normal Threshold
    uint _pad;
}

// Inputs
Texture2D<PackedHitInfo> gVBuffer;
Texture2D<float4> gView;
Texture2D<PackedHitInfo> gFinalGatherHit;
StructuredBuffer<AABB> gPhotonAABB[2];
StructuredBuffer<PhotonDataPacked> gPackedPhotonData[2];
Texture2D<float4> gThp; //Thp for caustic and final gathering


//Caustic Filter
StructuredBuffer<PackedSurfaceDataFG> gSurface;
RWTexture2D<uint4> gCausticReservoir;
RWStructuredBuffer<CausticSample> gCausticSample;
RWTexture2D<uint4> gDirectFGReservoir;
RWStructuredBuffer<CausticSample> gDirectFGSample;

//Inputs Temporal Caustic Filter (can be null)
Texture2D<float2> gMVec;
Texture2D<uint2> gCausticSurface;
Texture2D<uint2> gCausticSurfacePrev;
Texture2D<float4> gCausticPrev;

// Outputs
RWStructuredBuffer<SampleGenerator> gSampleGenState;
RWTexture2D<float4> gColor;
RWStructuredBuffer<FGData> gFGSampleData;
RWTexture2D<float4> gCausticOut;


#if USE_REDUCED_RESERVOIR_FORMAT
    RWTexture2D<uint2> gReservoir;
#else
    RWTexture2D<uint4> gReservoir;
#endif


//Acceleration Structure
RaytracingAccelerationStructure gPhotonAS;


//Constant defines
static const bool kCollectCaustics = CAUSTIC_COLLECTION_MODE != CAUSTIC_COLLECT_MODE_NONE;
static const bool kCollectCausticsForIndirect = CAUSTIC_COLLECTION_INDIRECT;

static const bool kRejectDiffuseSurfaces = REJECT_FGSAMPLE_DIFFUSE_SURFACE;    //Uses same definition as photon generation
static const bool kEmissionToCausticFilter = EMISSION_TO_CAUSTIC_FILTER;

static const uint kRayFlags = RAY_FLAG_SKIP_CLOSEST_HIT_SHADER | RAY_FLAG_SKIP_TRIANGLES;

struct RayDataReservoir{
    PackedSurfaceDataFG surface;
    int idx = -1;
    uint _pad;
    Reservoir r;
    SampleGenerator sg;
}

/** Payload for photon collection. Collect all (48B) and Stochastic Collect (32 + X B) available
*/
#if USE_STOCHASTIC_COLLECT
struct RayData
{
    uint counter;                   //Counter for photons this pixel
    uint photonIdx[STOCH_NUM_PHOTONS];    //Num Photons, variable length. Should be 3 + 4*X for best fit.

    SampleGenerator sg; ///< Per-ray state for the sample generator (up to 16B).
  
    __init(){
        this.counter = 0;
        for(uint i=0; i<STOCH_NUM_PHOTONS; i++)
            this.photonIdx[i] = 0;
    }
};
#else //USE_STOCHASTIC_COLLECT
struct RayData
{
    float3 radiance; ///< Accumulated outgoing radiance from path.
    uint countPhotons; ///< Counts the photons
    PackedHitInfo packedHitInfo; ///< Hit info from vBuffer; Up to 16B

    SampleGenerator sg; ///< Per-ray state for the sample generator (up to 16B).
  
    __init(){
        this.radiance = float3(0);
        this.countPhotons = 0;
    }
};
#endif //USE_STOCHASTIC_COLLECT

struct SphereAttribs
{
    float radiusSq;
};

//Load in shading data from hit and direction
ShadingData loadShadingData(const HitInfo hit, const float3 rayDir, const ITextureSampler lod)
{
    const TriangleHit triangleHit = hit.getTriangleHit();
    VertexData v = gScene.getVertexData(triangleHit);
    uint materialID = gScene.getMaterialID(triangleHit.instanceID);
    ShadingData sd = gScene.materials.prepareShadingData(v, materialID, -rayDir, lod);
    
    return sd;
}

CausticSurface getSurface(uint2 idx, bool prev)
{
    uint2 surfaceData;
    if (prev)
        surfaceData = gCausticSurfacePrev[idx];
    else
        surfaceData = gCausticSurface[idx];
    
    return CausticSurface(surfaceData);  
}

[shader("miss")]
void miss(inout RayData rayData : SV_RayPayload)
{
    //Empty. Is needed for compilation
}

[shader("miss")]
void missRes(inout RayDataReservoir rayDataRes : SV_RayPayload)
{
    //Empty. Is needed for compilation
}

[shader("anyhit")]
void anyHitReservoir(inout RayDataReservoir rayDataRes : SV_RayPayload, SphereAttribs attribs : SV_IntersectionAttributes)
{
    const uint primIndex = PrimitiveIndex();
    //Get Photon data
    PhotonData pd = PhotonData(gPackedPhotonData[InstanceIndex()][primIndex]);
    SurfaceFG surface = SurfaceFG(rayDataRes.surface, -WorldRayDirection());

    //Check if first hit check is required
    if(InstanceIndex() == 0)
    {
        if (!pd.isFirstHit)
            IgnoreHit();
    }

    //Check if photon sample is in valid hemisphere
    float NdotL = dot(pd.dir, surface.normal);
    if (NdotL < kMinCosTheta) 
    {
        IgnoreHit();
    }

    //RIS resampling
    float target = surface.getPdf(pd.dir, luminance(pd.flux)) / NdotL;
    float photonWeight = 1.0 / (M_PI  * attribs.radiusSq);
    if(rayDataRes.r.updateReservoir(photonWeight, target, sampleNext1D(rayDataRes.sg)))
        rayDataRes.idx = primIndex;
    IgnoreHit();
}

//Photon Collection via anyHit
#if USE_STOCHASTIC_COLLECT
[shader("anyhit")]
void anyHit(inout RayData rayData : SV_RayPayload, SphereAttribs attribs : SV_IntersectionAttributes)
{
    uint primIndex = PrimitiveIndex();
    const uint instanceIndex = InstanceIndex();
    // Get Photon data
    PhotonData pd = PhotonData(gPackedPhotonData[instanceIndex][primIndex]);

    // Change the last bit of prim index to reflect the type
    primIndex = (primIndex & 0x7FFFFFFF) | (instanceIndex << 31);

    //Check if first hit check is required
    if((rayData.counter >> 24 & 1) != 0)
    {
        if (!pd.isFirstHit)
            IgnoreHit();
    }

    //Check for different surfaces
    if (dot(pd.normal,  WorldRayDirection()) < 0.6)
    {
        IgnoreHit();
    }
    
    //Check if photon sample is in valid hemisphere
    if (dot(pd.dir, WorldRayDirection()) <= 0)
    {
        IgnoreHit();
    }

    uint counter = rayData.counter & 0xFFFFFF;  //Mask out the flags
    uint idx = counter;
    counter++;
    rayData.counter++;

    //Reservoir sampling
    //If photon list is full we get a random index between 0 and number of elements (rayData.counter) to stochastically insert
    if (idx >= STOCH_NUM_PHOTONS)
    {
        idx = uint(sampleNext1D(rayData.sg) * counter);
    }
    //Insert if index is within maximum list size
    if (idx < STOCH_NUM_PHOTONS)
        rayData.photonIdx[idx] = primIndex;

    IgnoreHit();
}

#else //USE_STOCHASTIC_COLLECT | Collect all photons

[shader("anyhit")]
void anyHit(inout RayData rayData : SV_RayPayload, SphereAttribs attribs : SV_IntersectionAttributes)
{
    const uint primIndex = PrimitiveIndex();

    //Get Photon data
    PhotonData pd = PhotonData(gPackedPhotonData[InstanceIndex()][primIndex]);
        
    //Get hit data from payload
    const HitInfo hit = HitInfo(rayData.packedHitInfo);
    let lod = ExplicitLodTextureSampler(0.f);
    //World Direction is set to the viewDir we get from the vBuffer
    ShadingData sd = loadShadingData(hit, WorldRayDirection(), lod);

    //Check if first hit check is required
    if((rayData.countPhotons >> 24 & 1) != 0)
    {
        if (!pd.isFirstHit)
            IgnoreHit();
    }
    
    //Check for different surfaces
    if (dot(pd.normal, sd.faceN) < 0.6)
    {
        IgnoreHit();
    }

    //Photon contribution
    uint hints = (uint) MaterialInstanceHints::AdjustShadingNormal;
    let bsdf = gScene.materials.getMaterialInstance(sd, lod, hints);
    let bsdfProperties = bsdf.getProperties(sd);
    float3 f_r = bsdf.eval(sd, pd.dir, rayData.sg); //right sign?
    float NdotL = dot(bsdfProperties.guideNormal, pd.dir);
    float radiusWeight = 1.0 / (attribs.radiusSq);
    if (NdotL > kMinCosTheta)
        rayData.radiance += max(0.f, (radiusWeight * f_r * pd.flux) / NdotL);
    IgnoreHit();
}
#endif //USE_STOCHASTIC_COLLECT

//Checks if the ray start point is inside the sphere.
bool hitSphere(const float3 center, const float radius, const float3 p)
{
    float3 radiusTest = p - center;
    radiusTest = radiusTest * radiusTest;
    float radiusTestF = radiusTest.x + radiusTest.y + radiusTest.z;
    if (radiusTestF < radius * radius)
        return true;
    return false;
}

[shader("intersection")]
void intersection()
{
    //Get start point and prim index
    const float3 origin = ObjectRayOrigin();
    const uint primIndex = PrimitiveIndex();

    //Get Photon AABB. Instance Index 0 is caustic; 1 is global
    AABB photonAABB = gPhotonAABB[InstanceIndex()][primIndex];
    // float radius = InstanceIndex() == 0 ? gPhotonRadius.x : gPhotonRadius.y;
    float radius = (photonAABB.maxPoint.x - photonAABB.minPoint.x) / 2.0;

    //Check for Sphere intersection
    bool tHit = hitSphere(photonAABB.center(), radius, origin);

    //Store radius square in the attribs
    SphereAttribs attribs;
    attribs.radiusSq = radius * radius;

    //Report the hit if the point is in the sphere
    if (tHit)
    {
        ReportHit(RayTCurrent(), 0, attribs);
    }
}

//Lighting Calculation for each photon in the list. This is done seperatly for caustic and global photons
float3 stochPhotonContribution(in const ShadingData sd, in IMaterialInstance bsdf, float3 N , inout RayData rayData, bool isCaustic)
{
    #if USE_STOCHASTIC_COLLECT
        //Get the size of the list to loop through. 
        uint maxIdx = min(rayData.counter, STOCH_NUM_PHOTONS);
        float3 radiance = float3(0);

        //Return when there is no element
        if (maxIdx == 0)
            return radiance;

        //loop through the list and accumulate photons
        for (uint i = 0; i < maxIdx; i++)
        {
            // Get Photon info
            uint photonIndex = rayData.photonIdx[i];
            uint instanceIndex = (photonIndex >> 31) & 1;
            photonIndex = photonIndex & 0x7FFFFFFF;
            PhotonData pd = PhotonData(gPackedPhotonData[instanceIndex][photonIndex]);

            float3 f_r = bsdf.eval(sd, pd.dir, rayData.sg);
            float NdotL = dot(N, pd.dir);
            float radius = instanceIndex == 0 ? gPhotonRadius.x : gPhotonRadius.y;
            float radiusWeight = 1.0 / (radius * radius);
            if (NdotL > kMinCosTheta)
                radiance += max(0.f, (radiusWeight * f_r * pd.flux) / NdotL);
        }

        //Weight output radiance with number of photons for this pixel.
        //The weight equals 1 if there is less or equal than the maximum list size in the photon array. (Reservoir sampling)
        return radiance * (float(rayData.counter) / float(maxIdx));
    #else
        return float3(0);
    #endif
}

void causticPhotonCollection(in ShadingData sd, inout RayData rayData, bool collectDirect, bool validSurface){
    const uint2 launchIndex = DispatchRaysIndex().xy;
    const uint2 launchDim = DispatchRaysDimensions().xy;
    let lod = ExplicitLodTextureSampler(0.f);

    float3 causticRadiance = float3(0);
    float3 directRadiance = float3(0);

    //Ray description
    RayDesc ray;
    ray.Origin = sd.posW;
    ray.TMin = 0.0;
    ray.TMax = 0.001;

    #if USE_STOCHASTIC_COLLECT
        ray.Direction = sd.faceN; //we take the face Normal as direction to save payload space
    #else
        ray.Direction = -sd.V; //we take the view dir as direction to save payload space
    #endif //USE_STOCHASTIC_COLLECT

    //Reservoir Payload
    #if (CAUSTIC_COLLECTION_MODE == CAUSTIC_COLLECT_MODE_RESERVOIR)
        RayDataReservoir rayDataRes;
        rayDataRes.r = Reservoir();
        rayDataRes.sg = rayData.sg;
        rayDataRes.surface = gSurface[index2Dto1D(launchIndex, launchDim.x)];
        rayDataRes.idx = -1;
        ray.Direction = -sd.V;
    #endif
    //Second Reservoir Payload for directly resampled photons
    #if (CAUSTIC_COLLECTION_MODE == CAUSTIC_COLLECT_MODE_RESERVOIR) && RESERVOIR_PHOTON_DIRECT
        RayDataReservoir rayDataResDirect;
        rayDataResDirect.r = Reservoir();
        rayDataResDirect.surface = rayDataRes.surface;
        rayDataResDirect.idx = -1;
        ray.Direction = -sd.V;
    #endif

    if(validSurface){
        //Init material model if stochastic collect is used
        #if USE_STOCHASTIC_COLLECT
            uint hints = (uint) MaterialInstanceHints::AdjustShadingNormal;
            let mi = gScene.materials.getMaterialInstance(sd, lod, hints);
            let miProperties = mi.getProperties(sd);
        #endif // USE_STOCHASTIC_COLLECT


        //Collect Caustic Photons as Reservoirs
    #if (CAUSTIC_COLLECTION_MODE == CAUSTIC_COLLECT_MODE_RESERVOIR)
        TraceRay(gPhotonAS, kRayFlags, 2 /* instanceInclusionMask */, 1 /* hitIdx */, 0 /* rayType count */, 0 /* missIdx */, ray, rayDataRes);
        //If a sample is in the reservoir, finalize it
        if(rayDataRes.r.M > 0 && rayDataRes.idx >= 0){
            rayDataRes.r.finalizeSample(1, 1.f);
        }
        rayDataRes.r.M = 1; //Set confidence weight
        rayData.sg = rayDataRes.sg;
        #if USE_STOCHASTIC_COLLECT
            ray.Direction = sd.faceN;
        #endif 
    #else // (CAUSTIC_COLLECTION_MODE == CAUSTIC_COLLECT_MODE_RESERVOIR) | Usual collection
        TraceRay(gPhotonAS, kRayFlags, 2 /* instanceInclusionMask */, 0 /* hitIdx */, 0 /* rayType count */, 0 /* missIdx */, ray, rayData);
        #if USE_STOCHASTIC_COLLECT
            causticRadiance += M_1_PI * stochPhotonContribution(sd, mi, miProperties.guideNormal, rayData, true);
        #else // USE_STOCHASTIC_COLLECT
            causticRadiance += M_1_PI * rayData.radiance;
        #endif // USE_STOCHASTIC_COLLECT
    #endif //(CAUSTIC_COLLECTION_MODE == CAUSTIC_COLLECT_MODE_RESERVOIR)


    //Collect the direct light on some metal surfaces
        if(collectDirect){
        //Clear Payload
        #if USE_STOCHASTIC_COLLECT
            rayData.counter = 0;
            for(uint i=0; i<STOCH_NUM_PHOTONS; i++)
                rayData.photonIdx[i] = 0;
            rayData.counter += 1 << 24; //Flag that only the first hit should be collected
        #else
            rayData.radiance = float3(0);
            rayData.countPhotons += 1 << 24; //Flag that only the first hit should be collected
        #endif
        #if (CAUSTIC_COLLECTION_MODE == CAUSTIC_COLLECT_MODE_RESERVOIR) && RESERVOIR_PHOTON_DIRECT
            ray.Direction = -sd.V;
            rayDataResDirect.sg = rayData.sg;
            TraceRay(gPhotonAS, kRayFlags, 1 /* instanceInclusionMask */, 1 /* hitIdx */, 0 /* rayType count */, 0 /* missIdx */, ray, rayDataResDirect);
            if(rayDataResDirect.r.M > 0 && rayDataResDirect.idx >= 0){
                rayDataResDirect.r.finalizeSample(1, 1.f);
            }
            rayDataResDirect.r.M = 1;
            rayData.sg = rayDataResDirect.sg;
        #else
            TraceRay(gPhotonAS, kRayFlags, 1 /* instanceInclusionMask */, 0 /* hitIdx */, 0 /* rayType count */, 0 /* missIdx */, ray, rayData);
            #if USE_STOCHASTIC_COLLECT
                rayData.counter &= 0xFFFFFF;    //Delete flag
                directRadiance += M_1_PI * stochPhotonContribution(sd, mi, miProperties.guideNormal, rayData, false);
            #else // USE_STOCHASTIC_COLLECT
                rayData.countPhotons = 0; //delete flag
                directRadiance += M_1_PI * rayData.radiance;
            #endif // USE_STOCHASTIC_COLLECT
        #endif
        }
    } //end if(validSurface)

    //Temporal Caustic Filter (non Reservoir based)
    #if (CAUSTIC_COLLECTION_MODE == CAUSTIC_COLLECT_MODE_TEMPORAL)
        //Get Motion Vector and get reprojected pixel
        float2 motionOffset = gMVec[launchIndex] * float2(launchDim);
        float2 prevIdxF = motionOffset + float2(launchIndex);
        float2 rndOffset = sampleNext2D(rayData.sg) - 0.5f;
        prevIdxF += rndOffset;
        int2 tempIndex = int2(round(prevIdxF));
        causticRadiance += directRadiance;

        uint history = 1;
        if (tempIndex.x >= 0 && tempIndex.y >= 0 && tempIndex.x < launchDim.x && tempIndex.y < launchDim.y)
        {
            //Load Surfaces
            CausticSurface currentSurface = getSurface(launchIndex, false);
            CausticSurface prevSurface = getSurface(tempIndex, true);
        
            if (currentSurface.isValidNeighbor(prevSurface, gDepthThreshold, gNormalThreshold))
            {
                //Because the caustic image can be RGBA16F, we need to store the history as float. As values are in a low range this should not introduce any errors
                float4 prev = gCausticPrev[tempIndex];

                uint limit = currentSurface.isDelta ? gTemporalFilterHistoryLimit/2 : gTemporalFilterHistoryLimit;  //Make reflections more reactive as the reprojection is often wrong
                history = min(limit, uint(round(prev.w)) + history);
                prev.xyz *= max(0, (history - 1));
                causticRadiance += prev.xyz;
                causticRadiance /= max(1u, history);
            }
        }

        gCausticOut[launchIndex] = float4(causticRadiance, float(history));

        //Store Reservoir
    #elif (CAUSTIC_COLLECTION_MODE == CAUSTIC_COLLECT_MODE_RESERVOIR)
        CausticSample currSample = {};
        if (rayDataRes.idx >= 0) {
            AABB pAABB = gPhotonAABB[1][rayDataRes.idx];
            PhotonData pd = PhotonData(gPackedPhotonData[1][rayDataRes.idx]);
            currSample.pos = pAABB.center();
            currSample.flux = pd.flux;
            currSample.dir = pd.dir;
        }

        gCausticSample[index2Dto1D(launchIndex, launchDim.x)] = currSample;
        gCausticReservoir[launchIndex] = rayDataRes.r.pack();
        #if RESERVOIR_PHOTON_DIRECT
            currSample = {};
            if (rayDataResDirect.idx >= 0) {
                AABB pAABB = gPhotonAABB[0][rayDataResDirect.idx];
                PhotonData pd = PhotonData(gPackedPhotonData[0][rayDataResDirect.idx]);
                currSample.pos = pAABB.center();
                currSample.flux = pd.flux;
                currSample.dir = pd.dir;
            }
            gDirectFGSample[index2Dto1D(launchIndex, launchDim.x)] = currSample;
            gDirectFGReservoir[launchIndex] = rayDataResDirect.r.pack();
        #else
            gCausticOut[launchIndex] = float4((directRadiance), 1.f);
        #endif
    #else
        gCausticOut[launchIndex] = float4((causticRadiance + directRadiance), 1.f);
    #endif
}

//Collect photons for the final gather sample
void fgSampleCollection(float3 genPosW, inout RayData rayData, bool valid){
    const uint2 launchIndex = DispatchRaysIndex().xy;
    const uint2 launchDim = DispatchRaysDimensions().xy;
    let lod = ExplicitLodTextureSampler(0.f);

    PackedHitInfo packedHit = gFinalGatherHit[launchIndex];
    #if USE_STOCHASTIC_COLLECT
        rayData.counter = 0;
        for(uint i=0; i<STOCH_NUM_PHOTONS; i++)
            rayData.photonIdx[i] = 0;
    #else
        rayData.radiance = float3(0);
        rayData.packedHitInfo = packedHit;
    #endif // USE_STOCHASTIC_COLLECT
    HitInfo hit = HitInfo(packedHit);
    valid &= hit.isValid(); //Check if the final gather ray is valid
    BSDFProperties bsdfProperties = {};
    ShadingData sd = {};   

    float3 fgRadiance = float3(0);
    float jacoDeter = -1.f;
    
    if (valid)
    {
        TriangleHit triangleHit = hit.getTriangleHit();
        VertexData v = gScene.getVertexData(triangleHit);

        RayDesc ray;
        ray.TMin = 0.0;
        ray.TMax = 0.00001;
        ray.Direction = v.posW - genPosW; //Sample dir is used here to save payload space
        float distToFG = length(ray.Direction); //Distance for Jacobian determinant
        ray.Direction /= distToFG; //Normalize
        ray.Origin = v.posW; //Update origin
        
        uint materialID = gScene.getMaterialID(triangleHit.instanceID);
        sd = gScene.materials.prepareShadingData(v, materialID, -ray.Direction, lod);
        uint hints = (uint) MaterialInstanceHints::AdjustShadingNormal;
        let bsdf = gScene.materials.getMaterialInstance(sd, lod, hints);
        bsdfProperties = bsdf.getProperties(sd);

        //Jacobian Determinant
        float NdotV = dot(bsdfProperties.guideNormal,sd.V);
        if(NdotV > kMinCosTheta)
            jacoDeter = vplAttenuation(distToFG, gAttenuationRadius) * NdotV;


        #if USE_STOCHASTIC_COLLECT
            ray.Direction = sd.faceN;   //Store faceN in direction
        #endif // USE_STOCHASTIC_COLLECT

        const uint instanceInclusionMask = kCollectCaustics && kCollectCausticsForIndirect ? 0xFF : 1;

        TraceRay(gPhotonAS, kRayFlags, instanceInclusionMask /* instanceInclusionMask */, 0 /* hitIdx */, 0 /* rayType count */, 0 /* missIdx */, ray, rayData);
        #if USE_STOCHASTIC_COLLECT
            fgRadiance += M_1_PI * stochPhotonContribution(sd, bsdf, bsdfProperties.guideNormal, rayData, false);
        #else
            fgRadiance += M_1_PI * rayData.radiance;
        #endif
    }

    //Store ReSTIR Sample
 
    //Init reservoir
    Reservoir r = Reservoir(gReservoir[launchIndex]);
    r.M = 1; //set M to the right value

    float Li = luminance(fgRadiance);
    r.targetFunc *= Li; //Add the missing parts of the targetFunc (Li)

        //Create the final gather sample
    FGData fgData = { };
    bool fgValid = valid;
    if(fgValid){
        fgValid = !isnan(Li) && !isinf(Li);
        fgData.posW = sd.computeNewRayOrigin();
        fgData.normal = bsdfProperties.guideNormal;
        fgData.flux = fgRadiance;
    }
    fgData.jacobianDeterminant = fgValid ? jacoDeter : -1.f;    //Negative jacobian determinant is an invalid sample
        
    if (!fgValid)
    {
        r.M = 0;
        r.weightSum = 0.f;
    }

    #if USE_REDUCED_RESERVOIR_FORMAT
        gReservoir[launchIndex] = r.packReduced();
    #else  //USE_REDUCED_RESERVOIR_FORMAT
        gReservoir[launchIndex] = r.pack();
    #endif //USE_REDUCED_RESERVOIR_FORMAT

    gFGSampleData[index2Dto1D(launchIndex, launchDim.x)] = fgData;
}

[shader("raygeneration")]
void rayGen()
{
    uint2 launchIndex = DispatchRaysIndex().xy;
    uint2 launchDim = DispatchRaysDimensions().xy;
            
    //Prepare payload
    RayData rayData = RayData();
    #if STORE_SAMPLE_GEN_STATE
        rayData.sg = gSampleGenState[index2Dto1D(launchIndex, launchDim.x)];
    #else
        rayData.sg = SampleGenerator(launchIndex, gFrameCount + 15 * 32);
    #endif

    PackedHitInfo packedHit = gVBuffer[launchIndex];
    #if !USE_STOCHASTIC_COLLECT
    rayData.packedHitInfo = packedHit;
    #endif //!USE_STOCHASTIC_COLLECT
    HitInfo hit = HitInfo(packedHit);
    bool valid = hit.isValid(); //Check if the ray is valid

    //Get vertex data for the world position
    TriangleHit triangleHit = hit.getTriangleHit();
    VertexData v = gScene.getVertexData(triangleHit);
    if(gCollectCaustic){
        float4 viewTmp =gView[launchIndex];
        float3 view = -viewTmp.xyz;
        bool collectDirect = viewTmp.w > 0.1;

        let lod = ExplicitLodTextureSampler(0.f);
        ShadingData fgSD = {};
        if(valid)
            fgSD = loadShadingData(hit, view, lod); //Shading Data for direct hit

        causticPhotonCollection(fgSD, rayData, collectDirect, valid);
    }
    if(gCollectFG)
        fgSampleCollection(v.posW, rayData, valid);

    #if STORE_SAMPLE_GEN_STATE
        gSampleGenState[index2Dto1D(launchIndex, launchDim.x)] = rayData.sg;
    #endif //STORE_SAMPLE_GEN_STATE
}
