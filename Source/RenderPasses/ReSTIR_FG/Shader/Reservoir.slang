struct Reservoir{

    uint M;             // Number of samples. Real number equals generatedSamples * M
    float targetFunc; // Target Pdf of current sample
    float weightSum;    // Weight of the reservoir after finalizing (W in paper). Used as weightSum internaly
    uint pad;           // Pad for fitting
    
    //Empty init
    __init(){
        this = {};
        M = 0;
        weightSum = 0.f;
    }

    //Init with full precision
    __init(const uint4 packed){
        M = packed.x;
        targetFunc = asfloat(packed.y);
        weightSum = asfloat(packed.z);
        //Invalidate if nan or inf is present
        if(isinf(weightSum) || isnan(weightSum)){
            weightSum = 0;
            M = 0;
        }
    }

    //Init with reduced targedPDF precision
    __init(const uint2 packed){
        M = packed.x & 0xFFFF;
        targetFunc = f16tof32(packed.x >> 16);
        weightSum = asfloat(packed.y);
        //Invalidate if nan or inf is present
        if(isinf(weightSum) || isnan(weightSum)){
            weightSum = 0;
            M = 0;
        }
    }

    //Pack for uint texture
    uint4 pack(){        
        return uint4(M, asuint(targetFunc), asuint(weightSum), 0);
    }

    uint2 packReduced()
    {
        return uint2((M & 0xFFFF) | (f32tof16(targetFunc) << 16), asuint(weightSum));
    }
    

    //Updates reservoir with new light sample. Returns true if the light sample was selected
    [mutating] bool updateReservoir(const float invSelectPdf,const float lstargetFunc, const float rnd){
        float risWeight = lstargetFunc * invSelectPdf;
        weightSum += risWeight;
        M += 1;
        bool selectSample = (rnd * weightSum < risWeight);
    
        if(selectSample){
            targetFunc = lstargetFunc;
        }
        return selectSample;
    }

    //Combines another reservoir with this reservoir. Returs true if the new reservoir was selected
    [mutating] bool combineReservoir(in const Reservoir other, const float othertargetFunc,const float rnd){
        float normalizationFactor = other.weightSum * other.M;

        float risWeight = othertargetFunc * normalizationFactor;
    
        M += other.M;
    
        weightSum += risWeight;

        bool selectSample = (rnd * weightSum < risWeight);
    
        if(selectSample){
            targetFunc = othertargetFunc;
        }

        return selectSample;
    }

    //Finalizes a sample. Normalizes the Wsum to W (and uses weightSum to store W)
    [mutating] void finalizeSample(float normalizationNumerator, float normalizationDenominator){
        float denominator = targetFunc * normalizationDenominator;
        
        if(denominator > 0)
            weightSum = (weightSum * normalizationNumerator) / denominator;
        else
            weightSum = 0;
    }
};
