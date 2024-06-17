import Utils.Math.PackedFormats;
import Utils.Math.FormatConversion;

#ifndef USE_REDUCED_PD_FORMAT
#define USE_REDUCED_PD_FORMAT 0
#endif

uint index2Dto1D(uint2 idx, uint width) {
    return idx.x + width * idx.y;
}

//Final Gather Data. 64 bytes
struct FGData{
    float3 posW;
    uint packedNormal;
    float3 flux;
    float jacobianDeterminant; 

    //Empty init
    __init(){
        posW = float3(0);
        packedNormal = 0;
        flux = float3(0);
        jacobianDeterminant = -1.f;
    }
    
    //Shading world normal
    property float3 normal{
        get { return decodeNormal2x16(packedNormal); }
        set { packedNormal = encodeNormal2x16(newValue); }
    }

    property bool valid{
    get{ return (jacobianDeterminant >= 0.f);}
    }
};

struct CausticSample {
    float3 pos;
    float3 dir;
    float3 flux;

    // Empty init
    __init() {
    pos = float3(0);
    flux = float3(0);
    dir = float3(0);
    }
}

struct PhotonDataPacked{
    uint4 data1;
#if !USE_REDUCED_PD_FORMAT
    uint4 data2;
#endif
}

struct PhotonData{
    float3 flux;
    uint packedNormal;  //Normal packed as 2x8snorm
    float3 dir;
    bool isFirstHit;

    __init(){
        flux = float3(0);
        dir = float3(0);
        packedNormal = 0;
        isFirstHit = false;
    }
    
    __init(PhotonDataPacked packedData){
    #if USE_REDUCED_PD_FORMAT
        uint4 data = packedData.data1;
        flux.xy = float2(f16tof32((data.x >> 16) & 0xFFFF), f16tof32(data.x & 0xFFFF));
        flux.z = f16tof32((data.y >> 16) & 0xFFFF);
        dir.x = f16tof32(data.y & 0xFFFF);
        dir.yz = float2(f16tof32((data.z >> 16) & 0xFFFF), f16tof32(data.z & 0xFFFF));
        packedNormal = data.w & 0xFFFF;
        isFirstHit = ((data.w >> 16) & 1) == 1 ? true : false;
    #else //USE_REDUCED_PD_FORMAT
        uint4 data = packedData.data1;
        flux = float3(asfloat(data.x), asfloat(data.y), asfloat(data.z));
        packedNormal = data.w;
        data = packedData.data2;
        dir = float3(asfloat(data.x), asfloat(data.y), asfloat(data.z));
        isFirstHit = data.w > 0;
    #endif //USE_REDUCED_PD_FORMAT
    }

    property float3 normal{
        get { return decodeNormal2x8(packedNormal); }
        set { packedNormal = encodeNormal2x8(newValue); }
    }

    PhotonDataPacked pack(){
        PhotonDataPacked ppd;
    #if USE_REDUCED_PD_FORMAT
        uint4 data;
        data.x = (f32tof16(flux.x) << 16u) | f32tof16(flux.y);
        data.y = (f32tof16(flux.z) << 16u) | f32tof16(dir.x);
        data.z = (f32tof16(dir.y) << 16u) | f32tof16(dir.z);
        data.w = packedNormal | (isFirstHit ? 1<<16 : 0);
        ppd.data1 = data;
    #else //USE_REDUCED_PD_FORMAT
        ppd.data1 = uint4(asuint(flux.x), asuint(flux.y), asuint(flux.z), packedNormal);
        uint firstHitVal = isFirstHit ? 1 : 0;
        ppd.data2 = uint4(asuint(dir.x), asuint(dir.y),asuint(dir.z), firstHitVal);
    #endif //USE_REDUCED_PD_FORMAT
        return ppd;
    }
};

//Helper functions for light pdf tex index calculations

// "Explodes" an integer, i.e. inserts a 0 between each bit.  Takes inputs up to 16 bit wide.
//      For example, 0b11111111 -> 0b1010101010101010
uint ReSTIR_IntegerExplode(uint x)
{
    x = (x | (x << 8)) & 0x00FF00FF;
    x = (x | (x << 4)) & 0x0F0F0F0F;
    x = (x | (x << 2)) & 0x33333333;
    x = (x | (x << 1)) & 0x55555555;
    return x;
}

// Reverse of ReSTIR_IntegerExplode, i.e. takes every other bit in the integer and compresses
// those bits into a dense bit field. Takes 32-bit inputs, produces 16-bit outputs.
//    For example, 0b'abcdefgh' -> 0b'0000bdfh'
uint ReSTIR_IntegerCompact(uint x)
{
    x = (x & 0x11111111) | ((x & 0x44444444) >> 1);
    x = (x & 0x03030303) | ((x & 0x30303030) >> 2);
    x = (x & 0x000F000F) | ((x & 0x0F000F00) >> 4);
    x = (x & 0x000000FF) | ((x & 0x00FF0000) >> 8);
    return x;
}

// Converts a 2D position to a linear index following a Z-curve pattern.
uint ReSTIR_ZCurveToLinearIndex(uint2 xy)
{
    return ReSTIR_IntegerExplode(xy[0]) | (ReSTIR_IntegerExplode(xy[1]) << 1);
}

// Converts a linear to a 2D position following a Z-curve pattern.
uint2 ReSTIR_LinearIndexToZCurve(uint index)
{
    return uint2(
        ReSTIR_IntegerCompact(index),
        ReSTIR_IntegerCompact(index >> 1));
}

// Nonsingular Point Light Attenuation by cem yuksel. Reduces bright spots near corners. Simplified version from website
// http://www.cemyuksel.com/research/pointlightattenuation/
//Inputs: Distance d and vpl radius r
//Output: Attenuation
float vplAttenuation(float d, float r){
    float drSq = d*d + r * r;
    float denom = drSq + d * sqrt(drSq);
    return 2.f/denom;
}
