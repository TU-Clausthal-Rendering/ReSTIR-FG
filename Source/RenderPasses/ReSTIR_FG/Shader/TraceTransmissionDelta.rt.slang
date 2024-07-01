#include "Scene/SceneDefines.slangh"
#include "Utils/Math/MathConstants.slangh"

import Scene.Raytracing;
import Scene.Intersection;
import Utils.Math.MathHelpers;
import Utils.Sampling.SampleGenerator;
import Utils.Math.PackedFormats;
import Utils.Math.FormatConversion;
import Rendering.Materials.IsotropicGGX;
import Utils.Color.ColorHelpers;
import Rendering.RTXDI.RTXDI;
import SurfaceDataFG;
import FinalGatherData;

//Syntax Highlight
#ifndef USE_RTXDI
#define USE_RTXDI 1
#endif

#ifndef DEBUG_MASK
#define DEBUG_MASK 1
#endif

cbuffer PerFrame
{
    uint gFrameCount;       // Frame count since scene was loaded.
    uint gMaxBounces;       // Max number of bounces
    bool gRequDiffParts;
    bool gAlphaTest;
}

// Inputs
Texture2D<PackedHitInfo> gInVBuffer;

//Outputs
RWStructuredBuffer<SampleGenerator> gSampleGenState;
RWTexture2D<PackedHitInfo> gOutVBuffer;
RWTexture2D<float4> gOutViewDir;
RWTexture2D<float> gOutRayDist;
RWTexture2D<float4> gOutThp;
RWTexture2D<uint2> gPackedCausticSurface;

//Outputs RTXDI (ReSTIR)
RWTexture2D<PackedHitInfo> gOutVBufferDI;
RWTexture2D<float4> gOutViewDirRayDistDI;
RWTexture2D<float4> gOutThpDI;

RWTexture2D<float4> gOutDiffuseReflectance;
RWTexture2D<float4> gOutSpecularReflectance;

RWTexture2D<float4> gDebugOut;  //Only used when DEBUG_MASK == 1

#define is_valid(name) (is_valid_##name != 0)

static const float kDemodulateMinReflectance = 0.01f;
static const float kRoughnessCutoff = TRACE_TRANS_SPEC_ROUGH_CUTOFF;
static const float kDiffuseCutoff = TRACE_TRANS_SPEC_DIFFUSEPART_CUTOFF;
static const bool kAlphaTest = USE_ALPHA_TEST;

struct TraceData
{
    PackedHitInfo packedHitInfo; //< Reflection albedo of the current hit point
    float distance; ///<Current hit distance
    
    /** Create ray payload with default parameters.
    */
    static TraceData create()
    {
        TraceData d;
        d.packedHitInfo = {};
        d.distance = 0.f;
        return d;
    }
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

[shader("miss")]
void miss(inout TraceData rayData : SV_RayPayload)
{
    HitInfo hitInfo = { };
    rayData.packedHitInfo = hitInfo.getData();  //Invalid hit
}

//Use anyhit for alpha test if activated
[shader("anyhit")]
void anyHit(inout TraceData rayData : SV_RayPayload, BuiltInTriangleIntersectionAttributes attribs : SV_IntersectionAttributes)
{
    if (kAlphaTest)
    {
        // Alpha test for non-opaque geometry.
        GeometryInstanceID instanceID = getGeometryInstanceID();
        VertexData v = getVertexData(instanceID, PrimitiveIndex(), attribs);
        uint materialID = gScene.getMaterialID(instanceID);
        if (gScene.materials.alphaTest(v, materialID, 0.f))
            IgnoreHit();
    }
}

[shader("closesthit")]
void closestHit(inout TraceData rayData : SV_RayPayload, BuiltInTriangleIntersectionAttributes attribs : SV_IntersectionAttributes){
    TriangleHit triangleHit;
    triangleHit.instanceID = getGeometryInstanceID();
    triangleHit.primitiveIndex = PrimitiveIndex();
    triangleHit.barycentrics = attribs.barycentrics;
    
    rayData.distance =  RayTCurrent();
    rayData.packedHitInfo = HitInfo(triangleHit).getData();
}

[shader("raygeneration")]
void rayGen()
{
    uint2 launchIndex = DispatchRaysIndex().xy;
    uint2 launchDim = DispatchRaysDimensions().xy;

    float3 diffuseReflectance = { };
    float3 specularReflectance = { };
    
    HitInfo hitInfo = HitInfo(gInVBuffer[launchIndex]);
    float3 viewDir = -gScene.camera.computeRayPinhole(launchIndex, launchDim).dir;
    float rayDist = 0.f;
    float3 thp = float3(1);

    //Direct illumination 
    HitInfo hitInfoDI = hitInfo;
    float4 viewDirDistDI = float4(viewDir, rayDist);
    float3 thpDI = thp;

    bool validSurfaceForDI = true;
    uint bouncesAfterValid = 0;
    bool invalidSample = false;
    bool isDiffuse = false;

    TraceData traceData = TraceData::create();
    #if STORE_SAMPLE_GEN_STATE
        SampleGenerator sg = SampleGenerator(launchIndex, gFrameCount);
    #else
        SampleGenerator sg = SampleGenerator(launchIndex, gFrameCount + 2 * 32);
    #endif
    let lod = ExplicitLodTextureSampler(0.f);
    uint wasTracedMask = 0;
    #if CAUSTIC_TEMPORAL_FILTER_ENABLED
        CausticSurface causticSurface;
    #endif
    
    //Get first valid(diffuse) Surface
    for (uint i = 0; ((i < gMaxBounces) || gMaxBounces == 0) && hitInfo.isValid(); i++)
    {
        ShadingData sd = loadShadingData(hitInfo, -viewDir, lod);

        //Get ray distance
        if(i==0){
            rayDist = distance(gScene.camera.getPosition(), sd.posW);
            viewDirDistDI.w = rayDist;
        }
            

        //Get BSDF
        uint hints = (uint) MaterialInstanceHints::AdjustShadingNormal;
        let bsdf = gScene.materials.getMaterialInstance(sd, lod, hints);
        let bsdfProperties = bsdf.getProperties(sd);
        
        //Write the reflectance on first hit
        if (i == 0 && (is_valid(gOutDiffuseReflectance) || is_valid(gOutDiffuseReflectance)))
        {
            diffuseReflectance = max(kDemodulateMinReflectance, bsdfProperties.diffuseReflectionAlbedo + bsdfProperties.diffuseTransmissionAlbedo);
            // For specular an approximate shading model with a single specular lobe is used.
            const float NdotV = saturate(dot(bsdfProperties.guideNormal, sd.V));
            const float ggxAlpha = bsdfProperties.roughness * bsdfProperties.roughness;
            if(bsdfProperties.isTransmissive)
                specularReflectance = float3(1);
            else
            {
                specularReflectance = approxSpecularIntegralGGX(bsdfProperties.specularReflectionAlbedo, ggxAlpha, NdotV);
                specularReflectance = max(kDemodulateMinReflectance, specularReflectance);
            }
        }

        #if CAUSTIC_TEMPORAL_FILTER_ENABLED
            if(i == 0){
                causticSurface.normal = sd.faceN;
                causticSurface.viewDepth = rayDist;
            }
        #endif    
        
        if (gMaxBounces == 0)
            break;
        
        BSDFSample bsdfSample;

        bool validBSDF = bsdf.sample(sd, sg, bsdfSample);
        if (!validBSDF){
            invalidSample = true;
        }

        //Determine if surface is diffuse
        bool isTransmission = validBSDF ? bsdfSample.isLobe(LobeType::Transmission) : false;
        float3 diffuseProb = bsdfProperties.diffuseReflectionAlbedo / (bsdfProperties.diffuseReflectionAlbedo + bsdfProperties.specularReflectionAlbedo + bsdfProperties.diffuseTransmissionAlbedo + bsdfProperties.specularTransmissionAlbedo);
        float meanDiffuseProb = (diffuseProb.x + diffuseProb.y + diffuseProb.z) / 3;
        bool hasDiffuseParts = gRequDiffParts ? (meanDiffuseProb > kDiffuseCutoff) || bsdfProperties.roughness > kRoughnessCutoff : true;
        isDiffuse = hasDiffuseParts;
        
        if(isDiffuse || invalidSample)
            break;
        
        thp *= bsdfSample.weight;   //Update troughput
                
        RayDesc tRay;
        tRay.Origin = sd.computeNewRayOrigin(!isTransmission);
        tRay.TMin = 0.0001f;
        tRay.TMax = 10000.f;
        tRay.Direction = bsdfSample.wo;

        TraceRay(gScene.rtAccel, 0, 0xff /* instanceInclusionMask */, 0 /* hitIdx */, rayTypeCount, 0 /* missIdx */, tRay, traceData);

        #if CAUSTIC_TEMPORAL_FILTER_ENABLED
            //Set is delta flag on caustic surface
            if(i == 0){
                causticSurface.isDelta = true;
            }
        #endif 
        
        //Update info
        hitInfo = HitInfo(traceData.packedHitInfo);
        viewDir = -bsdfSample.wo;
        rayDist += traceData.distance;
        wasTracedMask = 1;
        
        //Write DirectHit surfaces hits only for transmission or delta reflection hits
        if(validSurfaceForDI){
            #if USE_RESTIR_GI
            if(true){
            #else
            if(isTransmission || bsdfSample.isLobe(LobeType::DeltaReflection)){
            #endif
                hitInfoDI = hitInfo;
                viewDirDistDI = float4(viewDir,rayDist);
                thpDI = thp;
            }else{
                validSurfaceForDI = false;
            }
        }else
            bouncesAfterValid++;

    }
            
    //Fill the RTXDI surface if it is enabled
#if USE_RTXDI
    if(hitInfoDI.isValid())
    {
        //load all needed data
        ShadingData sd = loadShadingData(hitInfoDI, -viewDirDistDI.xyz, lod);
        uint hints = (uint) MaterialInstanceHints::AdjustShadingNormal;
        let bsdf = gScene.materials.getMaterialInstance(sd, lod, hints);
        let bsdfProperties = bsdf.getProperties(sd);
    
        gRTXDI.setSurfaceData(launchIndex, sd.computeNewRayOrigin(), bsdfProperties.guideNormal, bsdfProperties.diffuseReflectionAlbedo, bsdfProperties.specularReflectance, bsdfProperties.roughness, viewDirDistDI.w);
    }else{
        gRTXDI.setInvalidSurfaceData(launchIndex);
    }
#endif

    #if CAUSTIC_TEMPORAL_FILTER_ENABLED
        gPackedCausticSurface[launchIndex] = causticSurface.pack();
    #endif

     //Write DI Buffer
    gOutVBufferDI[launchIndex] = hitInfoDI.getData();
    gOutViewDirRayDistDI[launchIndex] = viewDirDistDI;
    gOutThpDI[launchIndex] = float4(thpDI, 1.f);

    //Set surface to invalid if the specular surface did not produce a valid sample
    #if !USE_RESTIR_GI
        if(invalidSample && !isDiffuse)
            hitInfo = {};
    #endif
    //Write
    #if STORE_SAMPLE_GEN_STATE
        gSampleGenState[index2Dto1D(launchIndex, launchDim.x)] = sg;
    #endif //STORE_SAMPLE_GEN_STATE
    gOutVBuffer[launchIndex] = hitInfo.getData();
    gOutViewDir[launchIndex] = float4(viewDir, validSurfaceForDI ? 0 : bouncesAfterValid + 1);  //Store a flag if this surface has direct light
    gOutRayDist[launchIndex] = rayDist;
    gOutThp[launchIndex] = float4(thp, wasTracedMask);
    if (is_valid(gOutDiffuseReflectance)) gOutDiffuseReflectance[launchIndex] = float4(diffuseReflectance, 1.f);
    if (is_valid(gOutSpecularReflectance)) gOutSpecularReflectance[launchIndex] = float4(specularReflectance, 1.f);

    #if DEBUG_MASK
        float3 debugOut = float3(0,0,1);
        if(invalidSample && !isDiffuse)
            debugOut = float3(1);
        //Enable Red channel if it is not a direct hit
        if(wasTracedMask > 0){
            debugOut = float3(0.8,0,0);
            
            if(!validSurfaceForDI)
                debugOut = float3(0,0.6,0);
                
        }

        gDebugOut[launchIndex] = float4(debugOut, 1.f);
    #endif
}
