#pragma once
#include "Falcor.h"
#include "RenderGraph/RenderPass.h"
#include "Rendering/RTXDI/RTXDI.h"
#include "Rendering/Lights/EmissiveLightSampler.h"

#include "Rendering/AccelerationStructure/CustomAccelerationStructure.h"

using namespace Falcor;

class ReSTIR_FG_Lite : public RenderPass
{
public:
    FALCOR_PLUGIN_CLASS(ReSTIR_FG_Lite, "ReSTIR_FG_Lite", "More lightweight implementation of ReSTIR-FG with less options");

    static ref<ReSTIR_FG_Lite> create(ref<Device> pDevice, const Properties& props) { return make_ref<ReSTIR_FG_Lite>(pDevice, props); }

    ReSTIR_FG_Lite(ref<Device> pDevice, const Properties& props);

    virtual Properties getProperties() const override;
    virtual RenderPassReflection reflect(const CompileData& compileData) override;
    virtual void compile(RenderContext* pRenderContext, const CompileData& compileData) override {}
    virtual void execute(RenderContext* pRenderContext, const RenderData& renderData) override;
    virtual void renderUI(Gui::Widgets& widget) override;
    virtual void setScene(RenderContext* pRenderContext, const ref<Scene>& pScene) override;
    virtual bool onMouseEvent(const MouseEvent& mouseEvent) override { return false; }
    virtual bool onKeyEvent(const KeyboardEvent& keyEvent) override { return false; }

private:
    struct ResamplingSettings
    {
        bool enable = true;
        uint confidenceCap = 20;                // Maximum confidence allowed
        uint spatialSamples = 1;                // Number of spatial samples
        uint disocclusionBoostExtraSamples = 1; // Number of spatial samples if no temporal surface was found
        float samplingRadius = 20.f;            // Sampling radius in pixel
    };

    //Initializes the emissive sampler used to sample photons
    void prepareLightingStructure(RenderContext* pRenderContext);

    //Initializes and handles all textures and buffers
    void prepareResources(RenderContext* pRenderContext, const RenderData& renderData);

    //Handles the photon acceleration structure
    void preparePhotonAccelerationStructure();

    //Traces the photons and builds the photon Acceleration Structure
    void tracePhotonsPass(RenderContext* pRenderContext, const RenderData& renderData, bool analyticOnly = false, bool buildAS = true);

    // Handles readback of the photon counter and adjusts dispatched photons dynamically
    void handlePhotonCounter(RenderContext* pRenderContext);

    //Generates the initial Samples for the reservoirs (FG) and initializes RTXDI surfaces
    void generateInitialSamplesPass(RenderContext* pRenderContext, const RenderData& renderData);

    //Reservoir Resampling for Final Gather Samples
    void resampleReservoirFGPass(RenderContext* pRenderContext, const RenderData& renderData);

    //Reservoir Resampling for Caustic Samples
    void resampleReservoirCausticPass(RenderContext* pRenderContext, const RenderData& renderData);

    //Evaluate Reservoirs
    void evaluateReservoirsPass(RenderContext* pRenderContext, const RenderData& renderData);

    //Get Materials defines
    DefineList getMaterialDefines();

    //
    // Pointers
    //
    ref<Scene> mpScene;                     // Scene Pointer
    ref<SampleGenerator> mpSampleGenerator; // GPU Sample Gen
    std::unique_ptr<RTXDI> mpRTXDI;         // Ptr to RTXDI for direct use
    RTXDI::Options mRTXDIOptions;           // Options for RTXDI

    std::unique_ptr<EmissiveLightSampler> mpEmissiveLightSampler; // Light Sampler
    std::unique_ptr<CustomAccelerationStructure> mpPhotonAS;      // Accel Pointer

    //
    // Parameters
    //
    uint mFrameCount = 0;
    uint2 mScreenRes = uint2(0, 0);
    bool mResetScreenTex = false;
    bool mOptionsChanged = false;

    // Material Settings
    bool mUseLambertianDiffuse = true;          // Diffuse BSDF used by ReSTIR PT and SuffixReSTIR
    float mSpecularRoughnessThreshold = 0.25f;  // Any material below this is considered specular

    // Light
    bool mHasLights = false;           // True if the scene has any light sources
    bool mHasAnalyticLights = false;   // True if there are analytic lights
    bool mMixedLights = false;         // True if analytic and emissive lights are in the scene

    //ReSTIR-FG Reservoirs
    ResamplingSettings mResampleSettingsFG = {};
    ResamplingSettings mResampleSettingsCaustic = {};

    uint mFGRayMaxPathLength = 10;                      // Max path length for the final gather ray
    bool mRebuildReservoirBuffer = false;               // Rebuild the reservoir buffer
    bool mClearReservoir = true;                        // Clears both reservoirs
    bool mCanResample = false;                          // Resampling is only allowed if last iterations reservoir was created
    float mRelativeDepthThreshold = 0.15f;              // Relative Depth threshold (is neighbor 0.1 = 10% as near as the current depth)
    float mNormalThreshold = 0.6f;                      // Cosine of maximum angle between both normals allowed
    float mJacobianDistanceThreshold = 0.001f;          // Threshold for Jacobian distances
    bool mUsePathThreshold = false;                     // Enable resampling only if path length are the same
    bool mUsePhotonsForDirectLightInReflections = true; // Uses photons for direct light in reflections, else the final gather sample is used

    //Photon Distribution
    uint mPhotonMaxBounces = 10;                        // Number of photon bounces
    float mGlobalPhotonRejection = 0.3f;                // Probability a global photon is stored
    uint mNumDispatchedPhotons = 2000000;               // Number of photons dispatched
    uint2 mNumMaxPhotons = uint2(400000, 300000);       // Size of the photon buffer
    uint2 mNumMaxPhotonsUI = mNumMaxPhotons;            // For UI, as changing happens with a button
    bool mChangePhotonLightBufferSize = true;           // True if buffer size has changed
    float mASBuildBufferPhotonOverestimate = 1.15f;     // Guard percentage for AS building
    uint2 mCurrentPhotonCount = mNumMaxPhotons;
    float2 mPhotonRadius = float2(0.020f, 0.005f);      // Global/Caustic Radius.
    float mPhotonAnalyticRatio = 0.5f;                  // Analytic photon distribution ratio in a mixed light case. E.g. 0.3 -> 30% analytic, 70% emissive

    bool mUseDynamicPhotonDispatchCount = true;         // Dynamically change the number of photons to fit the max photon number
    uint mPhotonDynamicDispatchMax = 4000000;           // Max value for dynamically dispatched photons
    float mPhotonDynamicGuardPercentage = 0.08f;        // Determines how much space of the buffer is used to guard against buffer overflows
    float mPhotonDynamicChangePercentage = 0.04f;       // The percentage the buffer is increased/decreased per frame

    //
    // Resources
    //
    ref<Buffer> mpPhotonAABB[2];            // Photon AABBs for Acceleration Structure building
    ref<Buffer> mpPhotonData[2];            // Additional Photon data (flux, dir)
    ref<Buffer> mpPhotonCounter;            // Photon Counter
    ref<Buffer> mpPhotonCounterCPU;         // CPU copy of counter for readback
    ref<Buffer> mpFinalGatherReservoir[2];  // Reservoir for the Final Gather sample
    ref<Buffer> mpCausticReservoir[2];      // Reservoir for the Caustic sample
    ref<Texture> mpEmission;                // Emission for paths that travel through highly specular materials

    //
    // Render Passes/Programs
    //
    struct RayTraceProgramHelper
    {
        ref<RtProgram> pProgram;
        ref<RtBindingTable> pBindingTable;
        ref<RtProgramVars> pVars;

        static const RayTraceProgramHelper create()
        {
            RayTraceProgramHelper r;
            r.pProgram = nullptr;
            r.pBindingTable = nullptr;
            r.pVars = nullptr;
            return r;
        }

        void initProgramVars(ref<Device> pDevice, ref<Scene> pScene, ref<SampleGenerator> pSampleGenerator);
    };

    RayTraceProgramHelper mTracePhotonPass;             // Trace Photons and build AS
    RayTraceProgramHelper mGenerateInitialSamplesPass;  // Trace Final Gather rays and collect photons

    ref<ComputePass> mpResampleReservoirFGPass;         // Resampling Pass for Final Gather Reservoirs
    ref<ComputePass> mpResampleReservoirCausticPass;    // Resampling Pass for Caustic Reservoirs
    ref<ComputePass> mpEvaluateReservoirsPass;          // Evaluates ReSTIR DI and FG reservoirs
};

