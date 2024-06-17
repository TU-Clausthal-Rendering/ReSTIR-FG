import Utils.Math.PackedFormats;
import Utils.Math.FormatConversion;
import Rendering.Materials.IsotropicGGX;
import Rendering.Materials.Fresnel;

#include "Utils/Math/MathConstants.slangh"

/** Structs for the surface data. After the same principle of RTXDI Surface data
  * Reduces the amount of data and calculation that is needed in generation and resampling step. Especially if bias correction is enabled
  * Register presure is reduced through the packed fields in the Surface struct
*/

struct PackedSurfaceDataFG{
    uint2 data0;
    uint2 data1;
    uint2 data2;
};

struct SurfaceFG{
    float3 posW;       //< World position of the hit point     
    float viewDepth;    //< View Depth from camera to hit point. Negative values indicate an invalid Surface
    uint packedNormal;  //< Packed Normal
    uint packedWeights; //< packed weight format:  diffuseProb|roughness|specular|diffuse

    //Not in packed needs to be provided on init
    float3 viewDir;


    // True if data is valid
    property bool valid{
        get {return viewDepth >= 0.f;}
    }

    //Shading world normal
    property float3 normal{
        get { return decodeNormal2x16(packedNormal); }
        set { packedNormal = encodeNormal2x16(newValue); }
    }

     // Diffuse lobe probability.
    property float diffuseProb
    {
        get { return f16tof32(packedWeights);}
        set { packedWeights = (packedWeights & ~0xffff) | (f32tof16(newValue)); }
    }

    // Specular reflectance.
    property float specular
    {
        get { return unpackUnorm8(packedWeights >> 16); }
        set { packedWeights = (packedWeights & ~0xff0000) | (packUnorm8(newValue) << 16);}
    }

    // Specular roughness.
    property float roughness
    {
        get { return unpackUnorm8(packedWeights >> 24); }
        set { packedWeights = (packedWeights & ~0xff000000) | (packUnorm8(newValue) << 24); }
    }

   

    //Empty init
    __init(){
        this = {};
        viewDepth = -1.f;
    }

    //Initialize with packedData and a viewDir
    __init(const PackedSurfaceDataFG packed, const float3 viewDir){
        posW.xy = asfloat(packed.data0);
        posW.z = asfloat(packed.data1.x);
        viewDepth = asfloat(packed.data1.y);
        packedNormal = packed.data2.x;
        packedWeights = packed.data2.y;
        this.viewDir = viewDir;
     }

    //Packs the surface data
    PackedSurfaceDataFG pack(){
        PackedSurfaceDataFG packed = {};
        packed.data0 = asuint(posW.xy);
        packed.data1.x = asuint(posW.z);
        packed.data1.y = asuint(viewDepth);
        packed.data2.x = packedNormal;
        packed.data2.y = packedWeights;

        return packed;
    }

    static SurfaceFG create(float3 posW, float3 normal, float3 viewDir, float viewDepth, float roughness, float diffuse, float specular)
    {
        SurfaceFG s = { };
        s.posW = posW;
        s.viewDir = viewDir;
        s.normal = normal;
        s.viewDepth = viewDepth;
        s.roughness = roughness;
        s.specular = specular;
        float sumWeights = diffuse + specular;
        s.diffuseProb = sumWeights < 1e-7f ? 1.0f : saturate(diffuse / sumWeights);
    
        return s;
    }

    //Gets the one channel average target pdf for Resampling
    float getPdf(const float3 toLight, const float Li = 1.f){
        const float3 N = normal;
        const float3 H = normalize(viewDir + toLight);
        const float NdotV = saturate(dot(N, viewDir));
        const float NdotL = saturate(dot(N, toLight));
        const float NdotH = saturate(dot(N, H));
        const float LdotH = saturate(dot(toLight, H));

        //BRDF terms for target function (DGF Microfaset BRDF)
        float D = evalNdfGGX(roughness * roughness, NdotH);
        float G = evalMaskingSmithGGXSeparable(roughness, NdotL, NdotV);
        
        //float F = specular < 1e-8f ? 0.f :  evalFresnelSchlick(specular, 1.f, LdotH);     //Right way
        float F = specular < 1e-8f ? 0.f : evalFresnelSchlick(specular, 1.f, LdotH) / specular; //Not "right" but less noisier

        //eval brdf terms
        float diffuseBRDF = NdotL * M_1_PI;
        float specularBRDF = D * G * F / (4.f * NdotV);

        //Mix diffuse and specular BRDF
        float brdf = max(0.f, lerp(specularBRDF, diffuseBRDF, diffuseProb));
        return max(0.f, brdf * Li);
    }

    //Gets the one channel average diffuse target pdf for Resampling
    float getDiffusePdf(const float3 toLight, const float Li = 1.f){
        const float3 N = normalize(normal);
        const float NdotL = saturate(dot(N, toLight));
        
        //Eval BSDF
        float diffuseBRDF = NdotL * M_1_PI;
        
        if(diffuseProb > 0)
            diffuseBRDF /= diffuseProb;
        else
            diffuseBRDF = 0.f;
        
        return max(0.f, diffuseBRDF * Li); 
    }

    //ReStir version
    bool isValidNeighbor(const in SurfaceFG neighbor, const float depthThreshold, const float normalThreshold)
    {
        float normalCheck = dot(normal, neighbor.normal);
        float minLinZDiff = viewDepth * depthThreshold;
        float linZDiff = abs(viewDepth - neighbor.viewDepth);
        return ((normalCheck >= normalThreshold) && (linZDiff <= minLinZDiff));
    }
    //ReStir heuristic + material difference
    bool isValidNeighbor(const in SurfaceFG neighbor, const float depthThreshold, const float normalThreshold, const float matThreshold)
    {
        float normalCheck = dot(normal, neighbor.normal);
        float minLinZDiff = viewDepth * depthThreshold;
        float linZDiff = abs(viewDepth - neighbor.viewDepth);
        float matDiff = abs(diffuseProb - neighbor.diffuseProb);

        return ((normalCheck >= normalThreshold) && (linZDiff <= minLinZDiff) && (matThreshold >= matDiff));
    }
};

struct CausticSurface{
    uint packedNormal;
    float viewDepth;
    bool isDelta;

    // True if data is valid
    property bool valid{
        get {return viewDepth > 0.f;}
    }

    //Shading world normal
    property float3 normal{
        get { return decodeNormal2x16(packedNormal); }
        set { packedNormal = encodeNormal2x16(newValue); }
    }

    //Empty init
    __init(){
        isDelta = false;
        viewDepth = 0.f;
    }

    //Initialize with packedData and a viewDir
    __init(const uint2 packed){
        packedNormal = packed.x;
        viewDepth = f16tof32(packed.y & 0xFFFF);
        isDelta = ((packed.y >> 24) & 1) != 0;
     }

    //Packs the surface data
    uint2 pack(){
        uint2 packed = uint2(0);
        packed.x = packedNormal;
        packed.y |= f32tof16(viewDepth) & 0xFFFF;
        if(isDelta)
            packed.y |= 1 << 16;

        return packed;
    }

    static CausticSurface create(float3 normal, float viewDepth, float diffuseProb, bool isDelta)
    {
        CausticSurface s = { };
        s.normal = normal;
        s.viewDepth = viewDepth;
        s.isDelta = isDelta;
    
        return s;
    }
    
    //ReStir heuristic + material difference + delta check (dont reuse outside of delta mats)
    bool isValidNeighbor(const in CausticSurface neighbor, const float depthThreshold, const float normalThreshold)
    {
        float normalCheck = dot(normal, neighbor.normal);
        float minLinZDiff = viewDepth * depthThreshold;
        float linZDiff = abs(viewDepth - neighbor.viewDepth);
        bool deltaCheck = isDelta == neighbor.isDelta; 
        
        return ((normalCheck >= normalThreshold) && (linZDiff <= minLinZDiff) && deltaCheck );
    }
    
};
