#include "Scene/SceneDefines.slangh"
#include "Utils/Math/MathConstants.slangh"

import Utils.Math.PackedFormats;
import Utils.Math.FormatConversion;

import Scene.RaytracingInline;
import Utils.Sampling.SampleGenerator;
import Rendering.Materials.IsotropicGGX;
import Utils.Math.MathHelpers;
import Utils.Color.ColorHelpers;
import Rendering.RTXDI.RTXDI;

import Reservoir;
import FinalGatherData;

//For Syntax highlighting
#ifndef USE_RTXDI
    #define USE_RTXDI 1
#endif
#ifndef USE_RESTIR_GI
#define USE_RESTIR_GI 1
#endif
#ifndef USE_CAUSTIC_FILTER_RESERVOIR
#define USE_CAUSTIC_FILTER_RESERVOIR 1
#endif

#if USE_REDUCED_RESERVOIR_FORMAT
    RWTexture2D<uint2> gReservoir;
#else
    RWTexture2D<uint4> gReservoir;
#endif


StructuredBuffer<FGData> gFGSampleData;

Texture2D<PackedHitInfo> gVBuffer;
Texture2D<float4> gView;
Texture2D<float4> gCausticRadiance;
Texture2D<float4> gThp;

Texture2D<PackedHitInfo> gVBufferDI;
Texture2D<float4> gViewDI;
Texture2D<float4> gThpDI;

StructuredBuffer<SampleGenerator> gSampleGenState;

RWTexture2D<float4> gOutColor;
RWTexture2D<float4> gOutEmission;
RWTexture2D<float4> gOutDiffuseRadiance;
RWTexture2D<float4> gOutSpecularRadiance;
Texture2D<float4> gOutDiffuseReflectance;
Texture2D<float4> gOutSpecularReflectance;
RWTexture2D<float4> gOutResidualRadiance;

//For view copy
RWTexture2D<float4> gViewDIPrev;
RWTexture2D<float4> gViewPrev;

// Caustic Reservoir (optinal)
StructuredBuffer<CausticSample> gCausticSample;
StructuredBuffer<CausticSample> gDirectFGSample;
#if USE_REDUCED_RESERVOIR_FORMAT
Texture2D<uint2> gCausticReservoir;
Texture2D<uint2> gDirectFGReservoir;
#else
Texture2D<uint4> gCausticReservoir;
Texture2D<uint4> gDirectFGReservoir;
#endif

#define is_valid(name) (is_valid_##name != 0)

cbuffer PerFrame
{
    uint gFrameCount;                   //Current it for sample gen
    float gAttenuationRadius;           //Current global photon radius
    uint2 gFrameDim; //Frame Dimensions for camera dir
}

static const float kRayEpsilon = 0.98f;
static const bool kUseEnvBackground = USE_ENV_BACKROUND;
static const bool kEmissionToCausticFilter = EMISSION_TO_CAUSTIC_FILTER;

ShadingData loadShadingData(const HitInfo hit, const float3 rayDir, const ITextureSampler lod)
{
    const TriangleHit triangleHit = hit.getTriangleHit();
    VertexData v = gScene.getVertexData(triangleHit);
    uint materialID = gScene.getMaterialID(triangleHit.instanceID);
    ShadingData sd = gScene.materials.prepareShadingData(v, materialID, -rayDir, lod);
    
    return sd;
}

void getEmissiveLight(uint idx,in float3 worldPos, out float distance ,out float3 toLight, out float3 Li){
    //Get light position
    Li = float3(0);
    FGData fgData = gFGSampleData[idx];

    if(fgData.valid){
        toLight = fgData.posW - worldPos;
        distance = length(toLight);
        toLight = toLight/ distance;    //normalize

        Li = fgData.flux;
        float distAttenuation = vplAttenuation(distance, gAttenuationRadius);
        float cosTheta = dot(normalize(fgData.normal), -toLight);
        if(cosTheta > kMinCosTheta)
            Li *= (cosTheta * distAttenuation)/fgData.jacobianDeterminant;
    }
    
}

[numthreads(16, 16, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
    bool valid = true;
    //Load Reflectance (was set before)
    float3 diffuseReflectance = { };
    float3 specularReflectance = { };
    if (is_valid(gOutDiffuseReflectance))
        diffuseReflectance = gOutDiffuseReflectance[pixel].xyz;
    if (is_valid(gOutSpecularReflectance))
        specularReflectance = gOutSpecularReflectance[pixel].xyz;
    
    
    //Outputs
    float3 color = { };
    float3 emission = { };
    float3 diffuseIllumination = { };
    float3 specularIllumination = { };
    float hitT = 1.f;
    uint hitTSamples = 0;

    // Variables for RTXDI and ReSTIRFG
    float3 diffuse = { };
    float3 specular = { };
    float3 caustic =  { };
    float distance = 1.f;
    float3 dir = { };
    float3 Li = { };

    bool validSample = false;

    #if STORE_SAMPLE_GEN_STATE
        SampleGenerator sg = gSampleGenState[index2Dto1D(pixel, gFrameDim.x)];
    #else
        SampleGenerator sg = SampleGenerator(pixel, gFrameCount + 9 * 32);
    #endif
    let lod = ExplicitLodTextureSampler(0.f);
    
    float3 viewDir;
    HitInfo hitInfo;
#if USE_RTXDI
    viewDir = -gViewDI[pixel].xyz;
    
    gViewDIPrev[pixel] = float4(-viewDir,1); //copy view
    
    hitInfo = HitInfo(gVBufferDI[pixel]);
    if (hitInfo.isValid())
    {
        validSample |= true;
        float3 thp = gThpDI[pixel].xyz;
            
        //Get Material properties
        uint hints = (uint) MaterialInstanceHints::AdjustShadingNormal;
        ShadingData sd = loadShadingData(hitInfo, viewDir, lod);
        let bsdf = gScene.materials.getMaterialInstance(sd, lod, hints);
        let bsdfProperties = bsdf.getProperties(sd);
        emission += bsdfProperties.emission * thp; //Emission
        //Get Final Sample from RTXDI
        bool rtxdiValid = gRTXDI.getFinalSample(pixel, dir, distance, Li);
        // Check light visibility.
        if (rtxdiValid)
        {
        // Create a DXR 1.1 query object to trace a ray (the <1> means use alpha testing)
            SceneRayQuery < 1 > rayQuery;
            const Ray ray = Ray(sd.computeNewRayOrigin(), dir, 0.f, distance * kRayEpsilon);
            if (!rayQuery.traceVisibilityRay(ray, RAY_FLAG_NONE, 0xff))
                rtxdiValid = false;
        }

        if (rtxdiValid)
        {
            sd.mtl.setActiveLobes((uint) LobeType::DiffuseReflection);
            diffuse += bsdf.eval(sd, dir, sg) * Li * thp;

            sd.mtl.setActiveLobes((uint) LobeType::SpecularReflection);
            specular += bsdf.eval(sd, dir, sg) * Li * thp;

            hitTSamples++;
            hitT = distance;
        }
    }
#endif //RTXDI

    //Load final gather and ReSTIR FG data
    float4 viewTmp = gView[pixel];
    viewDir = -viewTmp.xyz;
    bool needEmissive = viewTmp.w > 1.1 ; //Direct light for indirect flag
    gViewPrev[pixel] = float4(-viewDir,1); //copy view
    hitInfo = {};
    hitInfo = HitInfo(gVBuffer[pixel]);

    bool wasTraced = false;
    
    if (hitInfo.isValid())
    {
        validSample |= true;
        float4 thpTex = gThp[pixel];
        float3 thp = thpTex.xyz;
        wasTraced = thpTex.w > 0.5;

        //Get Material properties
        uint hints = (uint) MaterialInstanceHints::AdjustShadingNormal;
        ShadingData sd = loadShadingData(hitInfo, viewDir, lod);
        let bsdf = gScene.materials.getMaterialInstance(sd, lod, hints);
        let bsdfProperties = bsdf.getProperties(sd);

        //Get reservoir
        bool validIndirectReservoir = true;
        Reservoir r = Reservoir(gReservoir[pixel]);
    
        if (r.weightSum < 0 || r.M == 0)
        {
            validIndirectReservoir = false;
        }

        if(validIndirectReservoir){
            getEmissiveLight(index2Dto1D(pixel, gFrameDim.x), sd.posW, distance, dir, Li);
            Li = max(0.f, Li * r.weightSum);
        }
        
        //Check light visibility with a shadow ray
        if (validIndirectReservoir)
        {
        // Ray Query for shadow test; <1> means enabled alpha testing; TODO: Add static const for alpha test
            SceneRayQuery < 1 > rayQuery;
            const Ray ray = Ray(sd.computeNewRayOrigin(), dir, 0.f, distance * kRayEpsilon);
            if (!rayQuery.traceVisibilityRay(ray, RAY_FLAG_NONE, 0xff))
                validIndirectReservoir = false;
        }

        //For final gather we need to clear invalid reservoirs
        #if USE_FINAL_GATHER
            if (!validIndirectReservoir) {
                r.weightSum = 0;
                r.M = 0;
                gReservoir[pixel] = r.pack();
            }
        #endif
        
        if (validIndirectReservoir)
        {
            sd.mtl.setActiveLobes((uint) LobeType::Diffuse);
            diffuse += bsdf.eval(sd, dir, sg) * Li * thp;
            
            sd.mtl.setActiveLobes((uint) LobeType::SpecularReflection);
            specular += bsdf.eval(sd, dir, sg) * Li * thp;

            hitTSamples++;
            hitT += distance;
            hitT *= 1.f / hitTSamples;
        }
       
        //Add Emissive to additionally traced materials
        if(needEmissive)
            emission += bsdfProperties.emission * thp;

        #if !USE_RESTIR_GI

            #if (USE_CAUSTIC_FILTER_RESERVOIR && !RESERVOIR_PHOTON_DIRECT) || !USE_CAUSTIC_FILTER_RESERVOIR
                caustic += gCausticRadiance[pixel].xyz * thp;
            #endif
            #if USE_CAUSTIC_FILTER_RESERVOIR
                //sd.mtl.setActiveLobes((uint)LobeType::All);
                Reservoir causticReservoir = Reservoir(gCausticReservoir[pixel]);
                float3 causticReservoirRadianceDiffuse = float3(0);
                float3 causticResRadianceSpec = float3(0);
                if (causticReservoir.M > 0) {
                    CausticSample causticSample = gCausticSample[index2Dto1D(pixel, gFrameDim.x)];
                    float NdotL = dot(bsdfProperties.guideNormal, causticSample.dir);
                    if (NdotL > kMinCosTheta){
                        sd.mtl.setActiveLobes((uint) LobeType::Diffuse);
                        float3 f_r = bsdf.eval(sd, causticSample.dir, sg);
                        causticReservoirRadianceDiffuse += max(0.f, (f_r / NdotL) * causticSample.flux * causticReservoir.weightSum);

                        sd.mtl.setActiveLobes((uint) LobeType::SpecularReflection);
                        f_r = bsdf.eval(sd, causticSample.dir, sg);
                        causticResRadianceSpec += max(0.f, (f_r / NdotL) * causticSample.flux * causticReservoir.weightSum);
                    }
                }
                #if RESERVOIR_PHOTON_DIRECT
                    causticReservoir = Reservoir(gDirectFGReservoir[pixel]);
                    if (causticReservoir.M > 0) {
                        CausticSample causticSample = gDirectFGSample[index2Dto1D(pixel, gFrameDim.x)];
                        float NdotL = dot(bsdfProperties.guideNormal, causticSample.dir);
                        if (NdotL > kMinCosTheta && any(causticSample.flux > 0)){
                            sd.mtl.setActiveLobes((uint) LobeType::Diffuse);
                            float3 f_r = bsdf.eval(sd, causticSample.dir, sg);
                            causticReservoirRadianceDiffuse += max(0.f, (f_r / NdotL) * causticSample.flux * causticReservoir.weightSum);

                            sd.mtl.setActiveLobes((uint) LobeType::SpecularReflection);
                            f_r = bsdf.eval(sd, causticSample.dir, sg);
                            causticResRadianceSpec += max(0.f, (f_r / NdotL) * causticSample.flux * causticReservoir.weightSum);
                        }
                    }
                #endif // RESERVOIR_PHOTON_DIRECT
                diffuse += causticReservoirRadianceDiffuse * thp;
                specular += causticResRadianceSpec * thp;
            #endif // USE_CAUSTIC_FILTER_RESERVOIR
        #endif // !USE_RESTIR_GI
    }   

    if(validSample)
    {
        color += diffuse + specular + caustic;    //Add diffuse and specular parts together
        color += emission;  //Do this or demodulate it?
          
        // NRD
        if(wasTraced){
            float3 combined = (diffuse + specular + emission + caustic);
            specularIllumination = combined / specularReflectance;
            emission = float3(0);
        }
        else
        {
            diffuseIllumination = ((diffuse + caustic)) / diffuseReflectance;
            specularIllumination = (specular) / specularReflectance;
        }
    }else
    {
        if (kUseEnvBackground)
        {
            bool invalid = viewDir.x == 0 && viewDir.y == 0 && viewDir.z == 0;
            if (!invalid)
            {
                emission = gScene.envMap.eval(viewDir);
                color = emission;
            }
        }
    }

    if (is_valid(gOutColor)) gOutColor[pixel] = float4(color, 1.f);
    if (is_valid(gOutEmission)) gOutEmission[pixel] = float4(emission, 1.f);
    if (is_valid(gOutDiffuseRadiance)) gOutDiffuseRadiance[pixel] = float4(diffuseIllumination, hitT);
    if (is_valid(gOutSpecularRadiance))gOutSpecularRadiance[pixel] = float4(specularIllumination, hitT);
    if (is_valid(gOutResidualRadiance))  gOutResidualRadiance[pixel] = float4(0.f); //TODO 
}
