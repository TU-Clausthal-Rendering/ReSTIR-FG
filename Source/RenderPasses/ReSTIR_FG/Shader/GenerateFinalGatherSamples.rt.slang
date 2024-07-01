#include "Scene/SceneDefines.slangh"
#include "Utils/Math/MathConstants.slangh"

import Scene.Raytracing;
import Scene.Intersection;
import Utils.Math.MathHelpers;
import Utils.Sampling.SampleGenerator;
import Scene.Material.ShadingUtils;
import Rendering.Materials.Microfacet;
import Rendering.Materials.Fresnel;
import Rendering.Lights.LightHelpers;
import Utils.Math.PackedFormats;
import Utils.Math.FormatConversion;
import Utils.Color.ColorHelpers;

import Reservoir;
import FinalGatherData;
import SurfaceDataFG;
import Hash;

cbuffer PerFrame
{
    uint gFrameCount;       // Frame count since scene was loaded.
    float2 gCollectionRadius;
    float gHashScaleFactor; //Hash scale factor. 1/diameter
    float gAttenuationRadius;
    uint3 _pad;
}

cbuffer Constant{
    uint gHashSize;     //Size is always 2^x
    bool gUseAlphaTest; //Alpha test
    bool gDeltaRejection;   //Accept every non delta surface as diffuse
    uint _pad2;
}  

// Inputs
Texture2D<PackedHitInfo> gVBuffer;
Texture2D<float4> gView;
Texture2D <float>gLinZ;

//Outputs
RWStructuredBuffer<SampleGenerator> gSampleGenState;
RWStructuredBuffer<PackedSurfaceDataFG> gSurfaceData;
RWTexture2D<PackedHitInfo> gFinalGatherHit;
RWTexture2D<uint> gPhotonCullingMask;

//Outputs
#if USE_REDUCED_RESERVOIR_FORMAT
    RWTexture2D<uint2> gReservoir;
#else
    RWTexture2D<uint4> gReservoir;
#endif

static const bool kUsePhotonCulling = USE_PHOTON_CULLING;
static const bool kUseCausticPhotons = USE_CAUSTIC_CULLING; 

struct TraceData
{
    PackedHitInfo packedHitInfo; //< Reflection albedo of the current hit point
    float distance; ///<Current hit distance
    
    /** Create ray payload with default parameters.
    */
    static TraceData create()
    {
        TraceData d;
        d.packedHitInfo = { };
        d.distance = -1;
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

void cullingGetArrayOfCells(float3 position, float radius, out int3 outCells[8]){
    float3 cell = (position.xyz * gHashScaleFactor);
    float radInScale = radius * gHashScaleFactor;
    float3 cellFloor = floor(cell);
    float3 relCellPos = abs(cell - cellFloor);
    //Get offset direction
    int3 offsetCell;
    offsetCell.x = relCellPos.x < 0.5 ? -1 : 1;
    offsetCell.y = relCellPos.y < 0.5 ? -1 : 1;
    offsetCell.z = relCellPos.z < 0.5 ? -1 : 1;

    //Fill the cell array
    outCells[0] = int3(cellFloor);
    outCells[1] = int3(floor(cell + radInScale * float3(offsetCell.x, 0, 0)));
    outCells[2] = int3(floor(cell + radInScale * float3(offsetCell.x, offsetCell.y, 0)));
    outCells[3] = int3(floor(cell + radInScale * float3(offsetCell.x, offsetCell.y, offsetCell.z)));
    outCells[4] = int3(floor(cell + radInScale * float3(offsetCell.x, 0, offsetCell.z)));
    outCells[5] = int3(floor(cell + radInScale * float3(0, offsetCell.y, 0)));
    outCells[6] = int3(floor(cell + radInScale * float3(0, offsetCell.y, offsetCell.z)));
    outCells[7] = int3(floor(cell + radInScale * float3(0, 0, offsetCell.z)));
}

void photonCulling(float3 hitPos, float radius){
    //Get the 7 nearest neighbors (photon can still be in it)
    int3 cells[8];
    cullingGetArrayOfCells(hitPos, radius, cells);

    //Insert them into the hash buffer
    [unroll]
    for(uint i=0; i<8; i++){
        uint h = hash(cells[i]) & (gHashSize - 1);    //calculateHash
        gPhotonCullingMask[ReSTIR_LinearIndexToZCurve(h)] = 1;
    }
}

[shader("miss")]
void miss(inout TraceData rayData : SV_RayPayload)
{
    rayData.distance = -1.f;
}

//Use anyhit for alpha test if activated
[shader("anyhit")]
void anyHit(inout TraceData rayData : SV_RayPayload, BuiltInTriangleIntersectionAttributes attribs : SV_IntersectionAttributes)
{
    if (gUseAlphaTest)
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
    const float3 incomingRayDir = -WorldRayDirection();
    // Evaluate Falcor's material parameters at the hit point.
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
    
    HitInfo hitInfo = HitInfo(gVBuffer[launchIndex]);
    SurfaceFG surface = SurfaceFG();
    Reservoir reservoir = Reservoir();
    TraceData traceData = TraceData::create();
    #if STORE_SAMPLE_GEN_STATE
        SampleGenerator sg = gSampleGenState[index2Dto1D(launchIndex,launchDim.x)];
    #else
        SampleGenerator sg = SampleGenerator(launchIndex, gFrameCount + 3 * 32);
    #endif
    

    if (hitInfo.isValid())
    {
        float4 viewTmp = gView[launchIndex];
        float3 view = -viewTmp.xyz;
        bool collectDirect = viewTmp.w == 0;
        float viewDepth = gLinZ[launchIndex];
        let lod = ExplicitLodTextureSampler(0.f);
        ShadingData sd = loadShadingData(hitInfo, view, lod);
        
        //Use only BRDF
        sd.mtl.setActiveLobes((uint) LobeType::Reflection);

        uint hints = (uint) MaterialInstanceHints::AdjustShadingNormal;
        let bsdf = gScene.materials.getMaterialInstance(sd, lod, hints);
        let bsdfProperties = bsdf.getProperties(sd);

        BSDFSample bsdfSample;
        bool validSample = bsdf.sample(sd, sg, bsdfSample);
                
        //Fill Surface Buffer used for calculating the target PDF
        surface = SurfaceFG::create(sd.computeNewRayOrigin(), bsdfProperties.guideNormal, sd.V, viewDepth, bsdfProperties.roughness,
                                      luminance(bsdfProperties.diffuseReflectionAlbedo), luminance(bsdfProperties.specularReflectance));
        
        reservoir.targetFunc = surface.getPdf(bsdfSample.wo);
    
        reservoir.weightSum = 1.f / bsdfSample.pdf; // 1/selectPDF

        RayDesc tRay;
        tRay.Origin = sd.computeNewRayOrigin();
        tRay.TMin = 0.0001f;
        tRay.TMax = 10000.f;
        tRay.Direction = bsdfSample.wo;
    
        if (validSample)
        {
            TraceRay(gScene.rtAccel, 0, 0xff /* instanceInclusionMask */, 0 /* hitIdx */, rayTypeCount, 0 /* missIdx */, tRay, traceData);
            validSample = traceData.distance > 0;
        }
    
        if (validSample)
        {                      
            reservoir.M = 1; 
            float3 hitPos = tRay.Origin + tRay.Direction * traceData.distance;
    
            if (kUsePhotonCulling)
            {
                photonCulling(hitPos,gCollectionRadius.x);
                if(kUseCausticPhotons || collectDirect)
                {
                    float radius = collectDirect ? gCollectionRadius.x : gCollectionRadius.y;
                    photonCulling(sd.posW, radius);
                }
                    
            }
        }

    
        if (!validSample)
        {
            traceData.packedHitInfo = { };
            reservoir.weightSum = 0.f; //invalidate reseroir
        }
    }
    
    //Store data in buffers
    gFinalGatherHit[launchIndex] = traceData.packedHitInfo;
    gSurfaceData[index2Dto1D(launchIndex, launchDim.x)] = surface.pack(); 
    #if STORE_SAMPLE_GEN_STATE
        gSampleGenState[index2Dto1D(launchIndex, launchDim.x)] = sg;
    #endif //STORE_SAMPLE_GEN_STATE


    #if USE_REDUCED_RESERVOIR_FORMAT
        gReservoir[launchIndex] = reservoir.packReduced();
    #else
        gReservoir[launchIndex] = reservoir.pack();
    #endif
}
