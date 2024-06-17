#include "Scene/SceneDefines.slangh"
#include "Utils/Math/MathConstants.slangh"

import Utils.Math.PackedFormats;
import Utils.Math.FormatConversion;

import Scene.RaytracingInline;
import Utils.Sampling.SampleGenerator;
import Rendering.Lights.LightHelpers;
import Rendering.Materials.IsotropicGGX;
import Utils.Math.MathHelpers;
import Utils.Color.ColorHelpers;

import FinalGatherData;

Texture2D<PackedHitInfo> gVBuffer;
Texture2D<float4> gView;
Texture2D<float4> gThp;
RWTexture2D<float4> gOutColor;
RWTexture2D<float4> gOutDiffuseRadiance;
RWTexture2D<float4> gOutSpecularRadiance;
Texture2D<float4> gOutDiffuseReflectance;
Texture2D<float4> gOutSpecularReflectance;

#define is_valid(name) (is_valid_##name != 0)

cbuffer PerFrame
{
    uint gFrameCount;                   //Current it for sample gen
}

static const float kRayEpsilon = 0.99f;

struct LightSample
{
    float3 Li; ///< Incident radiance at the shading point (unshadowed). This is already divided by the pdf.
    float distance; ///< Ray distance for visibility evaluation (shortened to avoid self-intersection).
    float3 dir; ///< Ray direction for visibility evaluation (normalized).
        
    Ray getVisibilityRay(float3 posW)
    {
        return Ray(posW, dir, 0.f, distance);
    }
};

ShadingData loadShadingData(const HitInfo hit, const float3 rayDir, const ITextureSampler lod)
{
    const TriangleHit triangleHit = hit.getTriangleHit();
    VertexData v = gScene.getVertexData(triangleHit);
    uint materialID = gScene.getMaterialID(triangleHit.instanceID);
    ShadingData sd = gScene.materials.prepareShadingData(v, materialID, -rayDir, lod);
    
    return sd;
}

/** Generates a light sample on the analytic lights.
    \param[in] shading data.
    \param[in,out] sg Sample generator.
    \param[out] ls Struct describing valid samples.
    \return True if the sample is valid and has nonzero contribution, false otherwise.
*/
bool evalAnalyticLight(const in ShadingData sd, inout SampleGenerator sg, const uint lightIndex, out LightSample ls)
{
    ls = { }; // Default initialization to avoid divergence at returns.
    
    // Sample local light source.
    AnalyticLightSample lightSample;
    LightData lightData = gScene.getLight(lightIndex);
    if (!sampleLight(sd.posW,lightData , sg, lightSample))
        return false;
        
    // Setup returned sample.
    ls.Li = lightSample.Li;
    ls.distance = lightSample.distance * kRayEpsilon;
    ls.dir = lightSample.dir;

    return any(ls.Li > 0.f);
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
    float4 diffuseIllumination = { };
    float4 specularIllumination = { };
    if (is_valid(gOutColor))
        color = gOutColor[pixel].xyz;
    if (is_valid(gOutDiffuseRadiance))
        diffuseIllumination = gOutDiffuseRadiance[pixel];
    if (is_valid(gOutSpecularRadiance))
        specularIllumination = gOutSpecularRadiance[pixel];
    
    SampleGenerator sg = SampleGenerator(pixel, gFrameCount + 4 * 32);

    //Get shading data and view buffer
    float3 viewDir = -gView[pixel].xyz;
    HitInfo hitInfo = HitInfo(gVBuffer[pixel]);
    if (hitInfo.isValid())
    {
        float3 thp = gThp[pixel].xyz;
        let lod = ExplicitLodTextureSampler(0.f);
        
        //Get Material properties
        uint hints = (uint) MaterialInstanceHints::AdjustShadingNormal;
        ShadingData sd = loadShadingData(hitInfo, viewDir, lod);
        let bsdf = gScene.materials.getMaterialInstance(sd, lod, hints);
        let bsdfProperties = bsdf.getProperties(sd);
        
        //Diffuse and specular
        float3 diffuse = { };
        float3 specular = { };

        //Loop over all lights
        for (uint i = 0; i < gScene.getLightCount(); i++)
        {
            LightSample ls = { };
            if(evalAnalyticLight(sd, sg, i, ls))
            {
                //Visibility test
                SceneRayQuery < 1 > rayQuery;
                const Ray ray = ls.getVisibilityRay(sd.computeNewRayOrigin());
                if (!rayQuery.traceVisibilityRay(ray, RAY_FLAG_NONE, 0xff))
                    continue;

                //Shade
                sd.mtl.setActiveLobes((uint) LobeType::Diffuse);
                diffuse += bsdf.eval(sd, ls.dir, sg) * ls.Li;
            
                sd.mtl.setActiveLobes((uint) LobeType::SpecularReflection);
                specular += bsdf.eval(sd, ls.dir, sg) * ls.Li;
            }
        }

        //Store output in textures
        if (is_valid(gOutColor)){
            color += (diffuse + specular) * thp;
            gOutColor[pixel] = float4(color, 1.f);
        }

        if (is_valid(gOutDiffuseRadiance)){
            diffuseIllumination.xyz += (diffuse * thp) / diffuseReflectance;
            gOutDiffuseRadiance[pixel] = diffuseIllumination;
        }

        if (is_valid(gOutSpecularRadiance)){
            specularIllumination.xyz += (specular * thp) / specularReflectance;
            gOutSpecularRadiance[pixel] = specularIllumination;
        }     
    }
}
