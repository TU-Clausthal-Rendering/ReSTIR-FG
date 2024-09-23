/***************************************************************************
 # Copyright (c) 2015-23, NVIDIA CORPORATION. All rights reserved.
 #
 # Redistribution and use in source and binary forms, with or without
 # modification, are permitted provided that the following conditions
 # are met:
 #  * Redistributions of source code must retain the above copyright
 #    notice, this list of conditions and the following disclaimer.
 #  * Redistributions in binary form must reproduce the above copyright
 #    notice, this list of conditions and the following disclaimer in the
 #    documentation and/or other materials provided with the distribution.
 #  * Neither the name of NVIDIA CORPORATION nor the names of its
 #    contributors may be used to endorse or promote products derived
 #    from this software without specific prior written permission.
 #
 # THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS "AS IS" AND ANY
 # EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 # IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 # PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 # CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 # EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 # PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 # PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 # OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 # (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 # OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 **************************************************************************/
#include "Falcor.h"
#include "Core/API/NativeHandleTraits.h"
#include "RenderGraph/RenderPassStandardFlags.h"

#include "NRDPassBase.h"
#include "NRDPasses/NRDPassNormal.h"
#include "NRDPasses/NRDPassOcclusion.h"
#include "NRDPasses/NRDPassShadow.h"
#include "RenderPasses/Shared/Denoising/NRDConstants.slang"

extern "C" FALCOR_API_EXPORT void registerPlugin(PluginRegistry& registry)
{
    registry.registerClass<RenderPass, NRDPassNormal>();
    registry.registerClass<RenderPass, NRDPassOcclusion>();
    registry.registerClass<RenderPass, NRDPassShadow>();
}

namespace
{
    const char kShaderPackRadiance[] = "RenderPasses/NRDPass/PackRadiance.cs.slang";

    // Serialized parameters.
    const char kEnabled[] = "enabled";
    const char kMethod[] = "method";
    const char kOutputSize[] = "outputSize";

    // Common settings.
    const char kWorldSpaceMotion[] = "worldSpaceMotion";
    const char kDisocclusionThreshold[] = "disocclusionThreshold";

    // Pack radiance settings.
    const char kMaxIntensity[] = "maxIntensity";

    //Relax settings TODO add more
    const std::string kPropsRelaxAntilagAccelerationAmount = "RelaxAntilagAccelerationAmount";
    const std::string kPropsRelaxAntilagSpatialSigmaScale = "RelaxAntilagSpatialSigmaScale";
    const std::string kPropsRelaxAntilagTemporalSigmaScale = "RelaxAntilagTemporalSigmaScale";
    const std::string kPropsRelaxAntilagResetAmount = "RelaxAntilagResetAmount";
    const std::string kPropsRelaxDiffusePrepassBlurRadius = "RelaxDiffusePrepassBlurRadius";
    const std::string kPropsRelaxSpecularPrepassBlurRadius = "RelaxSpecularPrepassBlurRadius";
    const std::string kPropsRelaxDiffuseMaxAccumulatedFrameNum = "RelaxDiffuseMaxAccumulatedFrameNum";
    const std::string kPropsRelaxSpecularMaxAccumulatedFrameNum = "RelaxSpecularMaxAccumulatedFrameNum";
    const std::string kPropsRelaxEnableAntiFirefly = "RelaxEnableAntiFirefly";

    //Reblur settings TODO add

    //Sigma settings TODO

    const Gui::DropdownList kDropdownNormal = {
        {(uint)NRDPassBase::DenoisingMethod::RelaxDiffuseSpecular, "RelaxDiffuseSpecular"},
        {(uint)NRDPassBase::DenoisingMethod::ReblurDiffuseSpecular, "ReblurDiffuseSpecular"},
        {(uint)NRDPassBase::DenoisingMethod::RelaxDiffuse, "RelaxDiffuse"},
        {(uint)NRDPassBase::DenoisingMethod::RelaxSpecular, "RelaxSpecular"},
        {(uint)NRDPassBase::DenoisingMethod::ReblurDiffuse, "ReblurDiffuse"},
        {(uint)NRDPassBase::DenoisingMethod::ReblurSpecular, "ReblurSpecular"},
    };

    const Gui::DropdownList kDropdownOcclusion = {
        {(uint)NRDPassBase::DenoisingMethod::ReblurOcclusionDiffuse, "OcclusionDiffuse"},
        {(uint)NRDPassBase::DenoisingMethod::ReblurOcclusionSpecular, "OcclusionSpecular"},
        {(uint)NRDPassBase::DenoisingMethod::ReblurOcclusionDiffuseSpecular, "OcclusionDiffuseSpecular"},
    };

}

NRDPassBase::NRDPassBase(ref<Device> pDevice, const Properties& props)
    : RenderPass(pDevice)
{
    mpDevice->requireD3D12();

    mRecreateDenoiser = true;

    // Overwrite some defaults coming from the NRD SDK.
    mRelaxSettings.antilagSettings.accelerationAmount = 0.3f;
    mRelaxSettings.antilagSettings.spatialSigmaScale = 4.0f;
    mRelaxSettings.antilagSettings.temporalSigmaScale = 0.2f;
    mRelaxSettings.antilagSettings.resetAmount = 0.7f;

    mRelaxSettings.diffusePrepassBlurRadius = 30.f;
    mRelaxSettings.specularPrepassBlurRadius = 40.f;

    mRelaxSettings.diffuseMaxAccumulatedFrameNum = 50;
    mRelaxSettings.specularMaxAccumulatedFrameNum = 40;

    mRelaxSettings.enableAntiFirefly = true;
    
    // Deserialize pass from dictionary.
    for (const auto& [key, value] : props)
    {
        if (key == kEnabled)
            mEnabled = value;
        else if (key == kMethod)
            mDenoisingMethod = value;
        else if (key == kOutputSize)
            mOutputSizeSelection = value;

        // Common settings.
        else if (key == kWorldSpaceMotion)
            mWorldSpaceMotion = value;
        else if (key == kDisocclusionThreshold)
            mDisocclusionThreshold = value;

        // Pack radiance settings.
        else if (key == kMaxIntensity)
            mMaxIntensity = value;

        // ReLAX settings
        else if (key == kPropsRelaxAntilagAccelerationAmount)
            mRelaxSettings.antilagSettings.accelerationAmount = value;
        else if (key == kPropsRelaxAntilagSpatialSigmaScale)
            mRelaxSettings.antilagSettings.spatialSigmaScale = value;
        else if (key == kPropsRelaxAntilagTemporalSigmaScale)
            mRelaxSettings.antilagSettings.temporalSigmaScale = value;
        else if (key == kPropsRelaxAntilagResetAmount)
            mRelaxSettings.antilagSettings.resetAmount = value;
        else if (key == kPropsRelaxDiffusePrepassBlurRadius)
            mRelaxSettings.diffusePrepassBlurRadius = value;
        else if (key == kPropsRelaxSpecularPrepassBlurRadius)
            mRelaxSettings.specularPrepassBlurRadius = value;
        else if (key == kPropsRelaxDiffuseMaxAccumulatedFrameNum)
            mRelaxSettings.diffuseMaxAccumulatedFrameNum = value;
        else if (key == kPropsRelaxSpecularMaxAccumulatedFrameNum)
            mRelaxSettings.specularMaxAccumulatedFrameNum = value;
        else if (key == kPropsRelaxEnableAntiFirefly)
            mRelaxSettings.enableAntiFirefly = value;

        //Reblur settings TODO

        //Sigma settings TODO
        else
        {
            logWarning("Unknown property '{}' in NRD properties.", key);
        }
    }
}

Properties NRDPassBase::getProperties() const
{
    Properties props;

    props[kEnabled] = mEnabled;
    props[kMethod] = mDenoisingMethod;
    props[kOutputSize] = mOutputSizeSelection;

    // Common settings.
    props[kWorldSpaceMotion] = mWorldSpaceMotion;
    props[kDisocclusionThreshold] = mDisocclusionThreshold;

    // Pack radiance settings.
    props[kMaxIntensity] = mMaxIntensity;

    // ReLAX settings
    props[kPropsRelaxAntilagAccelerationAmount] = mRelaxSettings.antilagSettings.accelerationAmount;
    props[kPropsRelaxAntilagSpatialSigmaScale] = mRelaxSettings.antilagSettings.spatialSigmaScale;
    props[kPropsRelaxAntilagTemporalSigmaScale] = mRelaxSettings.antilagSettings.temporalSigmaScale;
    props[kPropsRelaxAntilagResetAmount] = mRelaxSettings.antilagSettings.resetAmount;
    props[kPropsRelaxDiffusePrepassBlurRadius] = mRelaxSettings.diffusePrepassBlurRadius;
    props[kPropsRelaxSpecularPrepassBlurRadius] = mRelaxSettings.specularPrepassBlurRadius;
    props[kPropsRelaxDiffuseMaxAccumulatedFrameNum] = mRelaxSettings.diffuseMaxAccumulatedFrameNum;
    props[kPropsRelaxSpecularMaxAccumulatedFrameNum] = mRelaxSettings.specularMaxAccumulatedFrameNum;
    props[kPropsRelaxEnableAntiFirefly] = mRelaxSettings.enableAntiFirefly;

    return props;
}

void NRDPassBase::reflectBase(const uint2 ioSize, RenderPassReflection& reflector) {
    reflector.addInput(kInputViewZ, "View Z");
    reflector.addInput(kInputNormalRoughnessMaterialID, "World normal, roughness, and material ID");
    reflector.addInput(kInputMotionVectors, "Motion vectors");

    reflector.addOutput(kOutputValidation, "Validation Layer for debug purposes")
        .format(ResourceFormat::RGBA32Float)
        .texture2D(ioSize.x, ioSize.y)
        .flags(RenderPassReflection::Field::Flags::Optional);
}

void NRDPassBase::compile(RenderContext* pRenderContext, const CompileData& compileData)
{
    mScreenSize = RenderPassHelpers::calculateIOSize(mOutputSizeSelection, mScreenSize, compileData.defaultTexDims);
    if (mScreenSize.x == 0 || mScreenSize.y == 0)
        mScreenSize = compileData.defaultTexDims;
    mFrameIndex = 0;
}

static bool isReblur(NRDPassBase::DenoisingMethod denoisingMethod)
{
    bool check = false;
    check |= denoisingMethod == NRDPassBase::DenoisingMethod::ReblurDiffuse;
    check |= denoisingMethod == NRDPassBase::DenoisingMethod::ReblurSpecular;
    check |= denoisingMethod == NRDPassBase::DenoisingMethod::ReblurDiffuseSpecular;
    return check;
}

static bool isRelax(NRDPassBase::DenoisingMethod denoisingMethod)
{
    bool check = false;
    check |= denoisingMethod == NRDPassBase::DenoisingMethod::RelaxDiffuse;
    check |= denoisingMethod == NRDPassBase::DenoisingMethod::RelaxSpecular;
    check |= denoisingMethod == NRDPassBase::DenoisingMethod::RelaxDiffuseSpecular;
    return check;
}

static bool isOcclusion(NRDPassBase::DenoisingMethod denoisingMethod)
{
    bool check = false;
    check |= denoisingMethod == NRDPassBase::DenoisingMethod::ReblurOcclusionDiffuse;
    check |= denoisingMethod == NRDPassBase::DenoisingMethod::ReblurOcclusionSpecular;
    check |= denoisingMethod == NRDPassBase::DenoisingMethod::ReblurOcclusionDiffuseSpecular;
    return check;
}

static bool denoiserIsDiffuse(NRDPassBase::DenoisingMethod denoisingMethod)
{
    bool check = false;
    check |= denoisingMethod == NRDPassBase::DenoisingMethod::ReblurOcclusionDiffuse;
    check |= denoisingMethod == NRDPassBase::DenoisingMethod::ReblurOcclusionDiffuseSpecular;
    check |= denoisingMethod == NRDPassBase::DenoisingMethod::RelaxDiffuse;
    check |= denoisingMethod == NRDPassBase::DenoisingMethod::RelaxDiffuseSpecular;
    check |= denoisingMethod == NRDPassBase::DenoisingMethod::ReblurDiffuse;
    check |= denoisingMethod == NRDPassBase::DenoisingMethod::ReblurDiffuseSpecular;
    return check;
}

static bool denoiserIsSpecular(NRDPassBase::DenoisingMethod denoisingMethod)
{
    bool check = false;
    check |= denoisingMethod == NRDPassBase::DenoisingMethod::ReblurOcclusionSpecular;
    check |= denoisingMethod == NRDPassBase::DenoisingMethod::ReblurOcclusionDiffuseSpecular;
    check |= denoisingMethod == NRDPassBase::DenoisingMethod::RelaxSpecular;
    check |= denoisingMethod == NRDPassBase::DenoisingMethod::RelaxDiffuseSpecular;
    check |= denoisingMethod == NRDPassBase::DenoisingMethod::ReblurSpecular;
    check |= denoisingMethod == NRDPassBase::DenoisingMethod::ReblurDiffuseSpecular;
    return check;
}

void NRDPassBase::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    if (!mpScene) return;

    //Check if a dict NRD refresh flag was set and overwrite enabled
    auto& dict = renderData.getDictionary();
    auto nrdEnableFlag = dict.getValue(kRenderPassEnableNRD, NRDEnableFlags::None);
    if (mEnabled && (nrdEnableFlag == NRDEnableFlags::NRDDisabled))
        mEnabled = false;
    else if (!mEnabled && (nrdEnableFlag == NRDEnableFlags::NRDEnabled))
        mEnabled = true;

    if (mEnabled)
    {
        executeInternal(pRenderContext, renderData);
    }
    else
    {
        if (isRelax(mDenoisingMethod) || isReblur(mDenoisingMethod))
        {
            if (denoiserIsDiffuse(mDenoisingMethod))
                pRenderContext->blit(renderData.getTexture(kInputDiffuseRadianceHitDist)->getSRV(), renderData.getTexture(kOutputFilteredDiffuseRadianceHitDist)->getRTV());
            if (denoiserIsSpecular(mDenoisingMethod))
                pRenderContext->blit(renderData.getTexture(kInputSpecularRadianceHitDist)->getSRV(), renderData.getTexture(kOutputFilteredSpecularRadianceHitDist)->getRTV());
        }
        if (mDenoisingMethod == DenoisingMethod::Sigma)
        {
            pRenderContext->clearTexture(renderData.getTexture(kOutputFilteredShadow).get(), float4(1, 1, 1, 1));
        }
        if (isOcclusion(mDenoisingMethod))
        {
            if (denoiserIsDiffuse(mDenoisingMethod))
                pRenderContext->blit(renderData.getTexture(kInputDiffuseHitDist)->getSRV(),renderData.getTexture(kOutputFilteredDiffuseOcclusion)->getRTV());
            if (denoiserIsSpecular(mDenoisingMethod))
                pRenderContext->blit(renderData.getTexture(kInputSpecularHitDist)->getSRV(),renderData.getTexture(kOutputFilteredSpecularOcclusion)->getRTV());
        }
            
    }

    //Update dict flag if options changed
    if (mOptionsChanged)
    {
        dict[Falcor::kRenderPassNRDOutputInYCoCg] = mDenoisingMethod == DenoisingMethod::ReblurDiffuseSpecular ? NRDEnableFlags::NRDEnabled : NRDEnableFlags::NRDDisabled;
        dict[Falcor::kRenderPassUseNRDDebugLayer] = mEnableValidationLayer ? NRDEnableFlags::NRDEnabled : NRDEnableFlags::NRDDisabled;
        mOptionsChanged = false;
    }
    else
    {
        dict[Falcor::kRenderPassNRDOutputInYCoCg] = NRDEnableFlags::None;
        dict[Falcor::kRenderPassUseNRDDebugLayer] = NRDEnableFlags::None;
    }
}

nrd::HitDistanceReconstructionMode getNRDHitDistanceReconstructionMode(NRDPassBase::HitDistanceReconstructionMode& falcorHitDistMode)
{
    switch (falcorHitDistMode)
    {
    case NRDPassBase::HitDistanceReconstructionMode::OFF:
        return nrd::HitDistanceReconstructionMode::OFF;
    case NRDPassBase::HitDistanceReconstructionMode::AREA3X3:
        return nrd::HitDistanceReconstructionMode::AREA_3X3;
    case NRDPassBase::HitDistanceReconstructionMode::AREA5X5:
        return nrd::HitDistanceReconstructionMode::AREA_5X5;
    }
    //Should not happen
    return nrd::HitDistanceReconstructionMode::OFF;
}

void NRDPassBase::renderUI(Gui::Widgets& widget)
{
    const nrd::LibraryDesc& nrdLibraryDesc = nrd::GetLibraryDesc();
    char name[256];
    _snprintf_s(name, 255, "NRD Library v%u.%u.%u", nrdLibraryDesc.versionMajor, nrdLibraryDesc.versionMinor, nrdLibraryDesc.versionBuild);
    widget.text(name);

    widget.checkbox("Enabled", mEnabled);

    widget.text("Common:");
    widget.checkbox("Motion : world space", mWorldSpaceMotion);
    widget.tooltip("Else 2.5D Motion Vectors are assumed");
    widget.var("Disocclusion threshold (%) x 100", mDisocclusionThreshold, 1.0f, 2.0f, 0.01f, false, "%.2f");
    mOptionsChanged |= widget.checkbox("Enable Debug Layer", mEnableValidationLayer);
    widget.checkbox("Enable Debug Split Screen", mEnableSplitScreen);
    widget.tooltip("Enables \" noisy input / denoised output \" comparison [0; 1]");
    if (mEnableSplitScreen)
        widget.var("Split Screen Value", mSplitScreenValue, 0.0f, 1.0f, 0.01f, false, "%.2f");

    widget.text("Pack radiance:");
    widget.var("Max intensity", mMaxIntensity, 0.f, 100000.f, 1.f, false, "%.0f");

    //TODO make this more save as some inputs are probably not set
    if (isReblur(mDenoisingMethod) || isRelax(mDenoisingMethod))
        mRecreateDenoiser = widget.dropdown("Denoising method", kDropdownNormal, (uint&)mDenoisingMethod);
    else if (isOcclusion(mDenoisingMethod))
        mRecreateDenoiser = widget.dropdown("Denoising method (ReBLUR)", kDropdownOcclusion, (uint&)mDenoisingMethod);

    if (isRelax(mDenoisingMethod))
    {
        // ReLAX diffuse/specular settings.
        if (auto group = widget.group("ReLAX Settings"))
        {
            if (auto group2 = group.group("Antilag Settings"))
            {
                group2.text(
                    "IMPORTANT: History acceleration and reset amounts for specular are made 2x-3x weaker than values for diffuse below \n"
                    "due to specific specular logic that does additional history acceleration and reset"
                );
                group2.var("Acceleration Amount", mRelaxSettings.antilagSettings.accelerationAmount, 0.f, 1.f, 0.01f, false, "%.2f");
                group2.tooltip("[0; 1] - amount of history acceleration if history clamping happened in pixel");
                group2.var("Spatial Sigma Scale", mRelaxSettings.antilagSettings.spatialSigmaScale, 0.f, 256.f, 0.01f, false, "%.2f");
                group2.tooltip("(> 0) - history is being reset if delta between history and raw input is larger than spatial sigma + temporal sigma");
                group2.var("Temporal Sigma Scale", mRelaxSettings.antilagSettings.temporalSigmaScale, 0.f, 256.f, 0.01f, false, "%.2f");
                group2.tooltip("(> 0) - history is being reset if delta between history and raw input is larger than spatial sigma + temporal sigma");
                group2.var("Reset Amount", mRelaxSettings.antilagSettings.resetAmount, 0.f, 1.f, 0.01f, false, "%.2f");
                group2.tooltip("[0; 1] - amount of history reset, 0.0 - no reset, 1.0 - full reset");
            }
            group.text("Prepass:");
            group.var("Diffuse blur radius", mRelaxSettings.diffusePrepassBlurRadius, 0.0f, 100.0f, 1.0f, false, "%.0f");
            group.tooltip("(pixels) - pre-accumulation spatial reuse pass blur radius (0 = disabled, must be used in case of probabilistic sampling)");
            group.var("Specular blur radius", mRelaxSettings.specularPrepassBlurRadius, 0.0f, 100.0f,1.0f, false, "%.0f");
            group.tooltip("(pixels) - pre-accumulation spatial reuse pass blur radius (0 = disabled, must be used in case of probabilistic sampling)");

            group.text("Reprojection:");
            group.var("Diffuse max accumulated frames", mRelaxSettings.diffuseMaxAccumulatedFrameNum, 0u, nrd::RELAX_MAX_HISTORY_FRAME_NUM);
            group.var("Diffuse responsive max accumulated frames", mRelaxSettings.diffuseMaxFastAccumulatedFrameNum, 0u, nrd::RELAX_MAX_HISTORY_FRAME_NUM);
            group.var("Specular max accumulated frames", mRelaxSettings.specularMaxAccumulatedFrameNum, 0u, nrd::RELAX_MAX_HISTORY_FRAME_NUM);
            group.var("Specular responsive max accumulated frames", mRelaxSettings.specularMaxFastAccumulatedFrameNum, 0u, nrd::RELAX_MAX_HISTORY_FRAME_NUM);
            group.slider("History Fix Frames", mRelaxSettings.historyFixFrameNum, 0u, 3u);
            group.tooltip("[0; 3] - number of reconstructed frames after history reset (less than \" maxFastAccumulatedFrameNum \")"); 

            group.text("A-trous edge stopping:");
            group.var("Diffuse Phi Luminance", mRelaxSettings.diffusePhiLuminance, 0.f, 256.f, 0.01f, false, "%.2f");
            group.var("Specular Phi Luminance", mRelaxSettings.specularPhiLuminance, 0.f, 256.f, 0.01f, false, "%.2f");            
            group.var("Diffuse Lobe Angle Fraction", mRelaxSettings.diffuseLobeAngleFraction, 0.f, 1.f, 0.01f, false, "%.2f");
            group.tooltip("(normalized %) - base fraction of diffuse or specular lobe angle used to drive normal based rejection");
            group.var("Specular Lobe Angle Fraction", mRelaxSettings.specularLobeAngleFraction, 0.f, 1.f, 0.01f, false, "%.2f");
            group.tooltip("(normalized %) - base fraction of diffuse or specular lobe angle used to drive normal based rejection");
            group.var("Roughness Fraction", mRelaxSettings.roughnessFraction, 0.f, 1.f, 0.01f, false, "%.2f");
            group.tooltip("(normalized %) - base fraction of center roughness used to drive roughness based rejection");
            group.var("Specular Variance Boost", mRelaxSettings.specularVarianceBoost, 0.f, 64.f, 0.01f, false, "%.2f");
            group.tooltip("(>= 0) - how much variance we inject to specular if reprojection confidence is low");
            group.var("Specular Lobe Angle Slack", mRelaxSettings.specularLobeAngleSlack, 0.f, 0.9f, 0.01f, false, "%.2f");
            group.tooltip("(degrees) - slack for the specular lobe angle used in normal based rejection of specular during A-Trous passes");
            group.var("History Fix Edge Stopping Normal Power", mRelaxSettings.historyFixEdgeStoppingNormalPower, 0.01f, 64.f, 0.01f, false, "%.2f");
            group.tooltip("(> 0) - normal edge stopper for history reconstruction pass");
            group.var("History Fix Color Box Sigma Scale", mRelaxSettings.historyClampingColorBoxSigmaScale, 1.f, 3.0f, 0.01f, false, "%.2f");
            group.tooltip("[1; 3] - standard deviation scale of color box for clamping main \" slow \" history to responsive \" fast \" history");
            group.var("Spatial Variance Estimation History Threshold", mRelaxSettings.spatialVarianceEstimationHistoryThreshold, 0u, nrd::RELAX_MAX_HISTORY_FRAME_NUM);
            group.tooltip("(>= 0) - history length threshold below which spatial variance estimation will be executed");
            group.var("A-trous Iterations", mRelaxSettings.atrousIterationNum, 2u, 8u);
            group.tooltip("[2; 8] - number of iterations for A-Trous wavelet transform");
            group.var("Diffuse Min Luminance Weight", mRelaxSettings.diffuseMinLuminanceWeight, 0.f, 1.0f, 0.01f, false, "%.2f");
            group.tooltip("[0; 1] - A-trous edge stopping Luminance weight minimum");
            group.var("Specular Min Luminance Weight", mRelaxSettings.specularMinLuminanceWeight, 0.f, 1.0f, 0.01f, false, "%.2f");
            group.tooltip("[0; 1] - A-trous edge stopping Luminance weight minimum");
            group.var("Depth Threshold", mRelaxSettings.depthThreshold, 0.f, 1.0f, 0.01f, false, "%.2f");
            group.tooltip("(normalized %) - Depth threshold for spatial passes");

            group.text("Relaxation Settings:");
            group.var("CD Relaxation Multiplier", mRelaxSettings.confidenceDrivenRelaxationMultiplier, 0.f, 1.0f, 0.01f, false, "%.2f"); //TODO range?
            group.tooltip("CD (Confidence Driven). Confidence inputs can affect spatial blurs, relaxing some weights in areas with low confidence");
            group.var("CD Relaxation Luminance Edge Stopping", mRelaxSettings.confidenceDrivenLuminanceEdgeStoppingRelaxation, 0.f, 1.0f, 0.01f, false, "%.2f");
            group.tooltip("CD (Confidence Driven). Confidence inputs can affect spatial blurs, relaxing some weights in areas with low confidence");
            group.var("CD Relaxation Normal Edge Stopping", mRelaxSettings.confidenceDrivenNormalEdgeStoppingRelaxation, 0.f, 1.0f, 0.01f, false, "%.2f");
            group.tooltip("CD (Confidence Driven). Confidence inputs can affect spatial blurs, relaxing some weights in areas with low confidence");

            group.var("Relaxation Luminance Edge Stopping", mRelaxSettings.luminanceEdgeStoppingRelaxation, 0.f, 1.0f, 0.01f, false, "%.2f");
            group.tooltip("How much we relax roughness based rejection for spatial filter in areas where specular reprojection is low");
            group.var("Relaxation Normal Edge Stopping", mRelaxSettings.normalEdgeStoppingRelaxation, 0.f, 1.0f, 0.01f, false, "%.2f");
            group.tooltip("How much we relax roughness based rejection for spatial filter in areas where specular reprojection is low");
            group.var("Relaxation Roughness Edge Stopping", mRelaxSettings.roughnessEdgeStoppingRelaxation, 0.f, 1.0f, 0.01f, false, "%.2f");
            group.tooltip("How much we relax rejection for spatial filter based on roughness and view vector");
            group.checkbox("Enable Roughness Edge Stopping", mRelaxSettings.enableRoughnessEdgeStopping);

            group.text("Misc:");
            group.dropdown("Hit Distance Reconstruction", mHitDistanceReconstructionMode);
            getNRDHitDistanceReconstructionMode(mHitDistanceReconstructionMode); // Set NRD setting
            group.checkbox("Anti-Firefly Filter", mRelaxSettings.enableAntiFirefly);
            group.checkbox("Material test for diffuse", mRelaxSettings.enableMaterialTestForDiffuse);
            group.checkbox("Material test for specular", mRelaxSettings.enableMaterialTestForSpecular);
        }
    }
    else if (isReblur(mDenoisingMethod) || isOcclusion(mDenoisingMethod)) //TODO specialize occlusion settings
    {
        if (auto group = widget.group("ReBLUR"))
        {
            
            const float kEpsilon = 0.0001f;
            if (auto group2 = group.group("Hit distance"))
            {
                group2.text(
                    "Normalized hit distance = saturate( \"hit distance\" / f ), where: \n f = ( A + viewZ * B ) * lerp( 1.0, C, exp2( D * "
                    "roughness ^ 2 ) ), see \"NRD.hlsl/REBLUR_FrontEnd_GetNormHitDist\""
                );
                group2.var("A", mReblurSettings.hitDistanceParameters.A, 0.01f, 256.0f, 0.01f, false, "%.2f");
                group2.tooltip("(units > 0) - constant value");
                group2.var("B", mReblurSettings.hitDistanceParameters.B, kEpsilon, 256.0f, 0.01f, false, "%.2f");
                group2.tooltip("(> 0) - viewZ based linear scale (1 m - 10 cm, 10 m - 1 m, 100 m - 10 m)");
                group2.var("C", mReblurSettings.hitDistanceParameters.C, 1.0f, 256.0f, 0.01f, false, "%.2f");
                group2.tooltip("(>= 1) - roughness based scale, use values > 1 to get bigger hit distance for low roughness");
                group2.var("D", mReblurSettings.hitDistanceParameters.D, -256.0f, 0.0f, 0.01f, false, "%.2f");
                group2.tooltip(
                    "(<= 0) - absolute value should be big enough to collapse \" exp2(D * roughness ^ 2) \" to \" ~0 \" for roughness = 1"
                );
            }
        
            if (auto group2 = group.group("Antilag settings"))
            {
                group2.var("Luminance Sigma Scale", mReblurSettings.antilagSettings.luminanceSigmaScale, 1.0f, 3.0f, 0.01f, false, "%.2f");
                group2.var("hit Distance Sigma Scale", mReblurSettings.antilagSettings.hitDistanceSigmaScale, 1.0f, 3.0f, 0.01f, false, "%.2f");
                group2.var("Luminance Antilag Power", mReblurSettings.antilagSettings.luminanceAntilagPower, kEpsilon, 1.0f, 0.0001f, false, "%.4f");
                group2.var("hit Distance Antilag Power", mReblurSettings.antilagSettings.hitDistanceAntilagPower, kEpsilon, 1.0f, 0.0001f,false, "%.4f");
            }

            group.var("Max accumulated frame num", mReblurSettings.maxAccumulatedFrameNum, 0u, nrd::REBLUR_MAX_HISTORY_FRAME_NUM);
            group.var("Max fast accumulated frame num", mReblurSettings.maxFastAccumulatedFrameNum, 0u, nrd::REBLUR_MAX_HISTORY_FRAME_NUM);
            group.slider("History Fix frame num", mReblurSettings.historyFixFrameNum, 0u, 3u);
            group.var("Prepass Diffuse Blur radius", mReblurSettings.diffusePrepassBlurRadius, 0.f, 256.f, 0.01f, false, "%.2f");

            group.tooltip("(pixels) - pre-accumulation spatial reuse pass blur radius (0 = disabled, must be used in case of badly defined signals and probabilistic sampling)");
            group.var("Prepass Specular Blur radius", mReblurSettings.specularPrepassBlurRadius, 0.f, 256.f, 0.01f, false, "%.2f");
            group.tooltip("(pixels) - pre-accumulation spatial reuse pass blur radius (0 = disabled, must be used in case of badly defined signals and probabilistic sampling)");

            group.var("Min Blur radius", mReblurSettings.minBlurRadius, 0.0f, 256.0f, 0.01f, false, "%.2f");
            group.tooltip("(pixels) - min denoising radius (for converged state)");
            group.var("Max Blur radius", mReblurSettings.maxBlurRadius, 0.0f, 256.0f, 0.01f, false, "%.2f");
            group.tooltip("(pixels) - base (max) denoising radius (gets reduced over time)");

            group.var("Normal weight (Lobe Angle Fraction)", mReblurSettings.lobeAngleFraction, 0.0f, 1.0f, 0.01f, false, "%.2f");
            group.tooltip("(normalized %) - base fraction of diffuse or specular lobe angle used to drive normal based rejection");
            group.var("Roughness Fraction", mReblurSettings.roughnessFraction, 0.0f, 1.0f, 0.01f, false, "%.2f");
            group.tooltip("(normalized %) - base fraction of center roughness used to drive roughness based rejection");

            group.var("Responsive Accumulation Roughness Threshold", mReblurSettings.responsiveAccumulationRoughnessThreshold, 0.0f, 1.0f, 0.01f, false, "%.2f");
            group.tooltip("[0; 1] - if roughness < this, temporal accumulation becomes responsive and driven by roughness (useful for animated water)");

            group.var("Stabilization Strength", mReblurSettings.stabilizationStrength, 0.0f, 1.0f, 0.01f, false, "%.2f");
            group.tooltip(
                "(normalized %) - stabilizes output, but adds temporal lag, at the same time more stabilization improves antilag (clean signals can use lower values)\n"
                "= N / (1 + N), where N is the number of accumulated frames \n"
                "0 - disables the stabilization pass"
            );

            group.var("hit Distance Stabilization Strength", mReblurSettings.hitDistanceStabilizationStrength, 0.0f, mReblurSettings.stabilizationStrength, 0.01f, false, "%.2f");
            group.tooltip("(normalized %) - same as \" stabilizationStrength \", but for hit distance (can't be > \" stabilizationStrength \", 0 - allows to reach parity with REBLUR_OCCLUSION) \n"
                "= N / (1 + N), where N is the number of accumulated frames "
            );

            group.var("Plane Distance Sensitivity", mReblurSettings.planeDistanceSensitivity, 0.0f, 1.0f, 0.01f, false, "%.2f");
            group.tooltip("(normalized %) - represents maximum allowed deviation from local tangent plane");

            float2 specularProbabilityThresholdsForMvModification = float2(mReblurSettings.specularProbabilityThresholdsForMvModification[0],mReblurSettings.specularProbabilityThresholdsForMvModification[1]);
            if (group.var("Specular Probability Thresholds For MV Modification", specularProbabilityThresholdsForMvModification, 0.0f, 1.f , 0.01f, false,"%.2f"))
            {
                mReblurSettings.specularProbabilityThresholdsForMvModification[0] = specularProbabilityThresholdsForMvModification.x;
                mReblurSettings.specularProbabilityThresholdsForMvModification[1] = specularProbabilityThresholdsForMvModification.y;
            }
            group.tooltip("IN_MV = lerp(IN_MV, specularMotion, smoothstep(this[0], this[1], specularProbability))");

            group.var("Firefly Suppressore Min Relative Scale", mReblurSettings.fireflySuppressorMinRelativeScale, 1.0f, 3.0f, 0.01f, false, "%.2f");
            group.tooltip("[1; 3] - undesired sporadic outliers suppression to keep output stable (smaller values maximize suppression in exchange of bias)");

            group.checkbox("Antifirefly", mReblurSettings.enableAntiFirefly);
            group.checkbox("Performance mode", mReblurSettings.enablePerformanceMode);
            group.dropdown("Hit Distance Reconstruction", mHitDistanceReconstructionMode);
            mReblurSettings.hitDistanceReconstructionMode = getNRDHitDistanceReconstructionMode(mHitDistanceReconstructionMode); //Set NRD setting
            group.checkbox("Material test for diffuse", mReblurSettings.enableMaterialTestForDiffuse);
            group.checkbox("Material test for specular", mReblurSettings.enableMaterialTestForSpecular);
            group.checkbox("Use Prepass Only For Specular Motion Estimation", mReblurSettings.usePrepassOnlyForSpecularMotionEstimation);
        }
    }
    else if (mDenoisingMethod == DenoisingMethod::Sigma)
    {
        if (auto group = widget.group("Sigma Shadow"))
        {
            group.var("Plane Distance Sensitivity", mSigmaSettings.planeDistanceSensitivity, 0.f, 1.f, 0.0001f, false, "%.4f");
            group.tooltip("(%normalized) represents maximum allowed deviation from local tangent plane");
            group.var("Stabilization Strength", mSigmaSettings.stabilizationStrength, 0.f, 1.f, 0.01f, false, "%.2f");
            group.tooltip(
                "(normalized %) - stabilizes output, more stabilization improves antilag (clean signals can use lower values)\n"
                "0 - disables the stabilization pass and makes denoising spatial only (no history)\n"
                "= N / (1 + N), where N is the number of accumulated frames"
            );
        }
    }
}

void NRDPassBase::setScene(RenderContext* pRenderContext, const ref<Scene>& pScene)
{
    mpScene = pScene;
}

static void* nrdAllocate(void* userArg, size_t size, size_t alignment)
{
    return malloc(size);
}

static void* nrdReallocate(void* userArg, void* memory, size_t size, size_t alignment)
{
    return realloc(memory, size);
}

static void nrdFree(void* userArg, void* memory)
{
    free(memory);
}

static ResourceFormat getFalcorFormat(nrd::Format format)
{
    switch (format)
    {
    case nrd::Format::R8_UNORM:             return ResourceFormat::R8Unorm;
    case nrd::Format::R8_SNORM:             return ResourceFormat::R8Snorm;
    case nrd::Format::R8_UINT:              return ResourceFormat::R8Uint;
    case nrd::Format::R8_SINT:              return ResourceFormat::R8Int;
    case nrd::Format::RG8_UNORM:            return ResourceFormat::RG8Unorm;
    case nrd::Format::RG8_SNORM:            return ResourceFormat::RG8Snorm;
    case nrd::Format::RG8_UINT:             return ResourceFormat::RG8Uint;
    case nrd::Format::RG8_SINT:             return ResourceFormat::RG8Int;
    case nrd::Format::RGBA8_UNORM:          return ResourceFormat::RGBA8Unorm;
    case nrd::Format::RGBA8_SNORM:          return ResourceFormat::RGBA8Snorm;
    case nrd::Format::RGBA8_UINT:           return ResourceFormat::RGBA8Uint;
    case nrd::Format::RGBA8_SINT:           return ResourceFormat::RGBA8Int;
    case nrd::Format::RGBA8_SRGB:           return ResourceFormat::RGBA8UnormSrgb;
    case nrd::Format::R16_UNORM:            return ResourceFormat::R16Unorm;
    case nrd::Format::R16_SNORM:            return ResourceFormat::R16Snorm;
    case nrd::Format::R16_UINT:             return ResourceFormat::R16Uint;
    case nrd::Format::R16_SINT:             return ResourceFormat::R16Int;
    case nrd::Format::R16_SFLOAT:           return ResourceFormat::R16Float;
    case nrd::Format::RG16_UNORM:           return ResourceFormat::RG16Unorm;
    case nrd::Format::RG16_SNORM:           return ResourceFormat::RG16Snorm;
    case nrd::Format::RG16_UINT:            return ResourceFormat::RG16Uint;
    case nrd::Format::RG16_SINT:            return ResourceFormat::RG16Int;
    case nrd::Format::RG16_SFLOAT:          return ResourceFormat::RG16Float;
    case nrd::Format::RGBA16_UNORM:         return ResourceFormat::RGBA16Unorm;
    case nrd::Format::RGBA16_SNORM:         return ResourceFormat::Unknown; // Not defined in Falcor
    case nrd::Format::RGBA16_UINT:          return ResourceFormat::RGBA16Uint;
    case nrd::Format::RGBA16_SINT:          return ResourceFormat::RGBA16Int;
    case nrd::Format::RGBA16_SFLOAT:        return ResourceFormat::RGBA16Float;
    case nrd::Format::R32_UINT:             return ResourceFormat::R32Uint;
    case nrd::Format::R32_SINT:             return ResourceFormat::R32Int;
    case nrd::Format::R32_SFLOAT:           return ResourceFormat::R32Float;
    case nrd::Format::RG32_UINT:            return ResourceFormat::RG32Uint;
    case nrd::Format::RG32_SINT:            return ResourceFormat::RG32Int;
    case nrd::Format::RG32_SFLOAT:          return ResourceFormat::RG32Float;
    case nrd::Format::RGB32_UINT:           return ResourceFormat::RGB32Uint;
    case nrd::Format::RGB32_SINT:           return ResourceFormat::RGB32Int;
    case nrd::Format::RGB32_SFLOAT:         return ResourceFormat::RGB32Float;
    case nrd::Format::RGBA32_UINT:          return ResourceFormat::RGBA32Uint;
    case nrd::Format::RGBA32_SINT:          return ResourceFormat::RGBA32Int;
    case nrd::Format::RGBA32_SFLOAT:        return ResourceFormat::RGBA32Float;
    case nrd::Format::R10_G10_B10_A2_UNORM: return ResourceFormat::RGB10A2Unorm;
    case nrd::Format::R10_G10_B10_A2_UINT:  return ResourceFormat::RGB10A2Uint;
    case nrd::Format::R11_G11_B10_UFLOAT:   return ResourceFormat::R11G11B10Float;
    case nrd::Format::R9_G9_B9_E5_UFLOAT:   return ResourceFormat::RGB9E5Float;
    default:
        throw RuntimeError("Unsupported NRD format.");
    }
}

static nrd::Denoiser getNrdDenoiser(NRDPassBase::DenoisingMethod denoisingMethod)
{
    switch (denoisingMethod)
    {
    case NRDPassBase::DenoisingMethod::RelaxDiffuseSpecular:
        return nrd::Denoiser::RELAX_DIFFUSE_SPECULAR;
    case NRDPassBase::DenoisingMethod::ReblurDiffuseSpecular:
        return nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR;
    case NRDPassBase::DenoisingMethod::RelaxDiffuse:
        return nrd::Denoiser::RELAX_DIFFUSE;
    case NRDPassBase::DenoisingMethod::RelaxSpecular:
        return nrd::Denoiser::RELAX_SPECULAR;
    case NRDPassBase::DenoisingMethod::ReblurDiffuse:
        return nrd::Denoiser::REBLUR_DIFFUSE;
    case NRDPassBase::DenoisingMethod::ReblurSpecular:
        return nrd::Denoiser::REBLUR_SPECULAR;
    case NRDPassBase::DenoisingMethod::Sigma:
        return nrd::Denoiser::SIGMA_SHADOW;
    case NRDPassBase::DenoisingMethod::ReblurOcclusionDiffuse:
        return nrd::Denoiser::REBLUR_DIFFUSE_OCCLUSION;
    case NRDPassBase::DenoisingMethod::ReblurOcclusionSpecular:
        return nrd::Denoiser::REBLUR_SPECULAR_OCCLUSION;
    case NRDPassBase::DenoisingMethod::ReblurOcclusionDiffuseSpecular:
        return nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR_OCCLUSION;
    default:
        FALCOR_UNREACHABLE();
        return nrd::Denoiser::RELAX_DIFFUSE_SPECULAR;
    }
}

/// Copies into col-major layout, as the NRD library works in column major layout,
/// while Falcor uses row-major layout
static void copyMatrix(float* dstMatrix, const float4x4& srcMatrix)
{
    float4x4 col_major = transpose(srcMatrix);
    memcpy(dstMatrix, static_cast<const float*>(col_major.data()), sizeof(float4x4));
}


void NRDPassBase::reinit()
{
    // Create a new denoiser instance.
    if (mpInstance)
        nrd::DestroyInstance(*mpInstance);

    //Create new Radiance Pass
    if (mpPackRadiancePass)
        mpPackRadiancePass.reset();

    const nrd::LibraryDesc& libraryDesc = nrd::GetLibraryDesc();

    const nrd::DenoiserDesc denoiserDescs[] = {
        {nrd::Identifier(getNrdDenoiser(mDenoisingMethod)), getNrdDenoiser(mDenoisingMethod)}
    };
    nrd::InstanceCreationDesc instanceCreationDesc = {};
    instanceCreationDesc.denoisers = denoiserDescs;
    instanceCreationDesc.denoisersNum = 1; //Only 1 denoiser is used at a time

    nrd::Result res = nrd::CreateInstance(instanceCreationDesc, mpInstance);
    if (res != nrd::Result::SUCCESS)
        throw RuntimeError("NRDPassBase: Failed to create NRD denoiser");

    createResources();
    createPipelines();
}

void NRDPassBase::createPipelines()
{
    mpPasses.clear();
    mpCachedProgramKernels.clear();
    mpCSOs.clear();
    mCBVSRVUAVdescriptorSetLayouts.clear();
    mpRootSignatures.clear();

    // Get denoiser desc for currently initialized denoiser implementation.
    const nrd::InstanceDesc& instanceDesc = nrd::GetInstanceDesc(*mpInstance);

    // Create samplers descriptor layout and set.
    D3D12DescriptorSetLayout SamplersDescriptorSetLayout;
    
    for (uint32_t j = 0; j < instanceDesc.samplersNum; j++)
    {
        SamplersDescriptorSetLayout.addRange(ShaderResourceType::Sampler, instanceDesc.samplersBaseRegisterIndex + j, 1);
    }
    mpSamplersDescriptorSet = D3D12DescriptorSet::create(mpDevice, SamplersDescriptorSetLayout, D3D12DescriptorSetBindingUsage::ExplicitBind);

    // Set sampler descriptors right away.
    for (uint32_t j = 0; j < instanceDesc.samplersNum; j++)
    {
        mpSamplersDescriptorSet->setSampler(0, j, mpSamplers[j].get());
    }

    // Go over NRD passes and creating descriptor sets, root signatures and PSOs for each.
    for (uint32_t i = 0; i < instanceDesc.pipelinesNum; i++)
    {
        const nrd::PipelineDesc& nrdPipelineDesc = instanceDesc.pipelines[i];
        const nrd::ComputeShaderDesc& nrdComputeShader = nrdPipelineDesc.computeShaderDXIL;

        // Initialize descriptor set.
        D3D12DescriptorSetLayout CBVSRVUAVdescriptorSetLayout;

        // Add constant buffer to descriptor set.
        CBVSRVUAVdescriptorSetLayout.addRange(ShaderResourceType::Cbv, instanceDesc.constantBufferRegisterIndex, 1);

        for (uint32_t j = 0; j < nrdPipelineDesc.resourceRangesNum; j++)
        {
            const nrd::ResourceRangeDesc& nrdResourceRange = nrdPipelineDesc.resourceRanges[j];

            ShaderResourceType descriptorType = nrdResourceRange.descriptorType == nrd::DescriptorType::TEXTURE
                                                    ?
                ShaderResourceType::TextureSrv :
                ShaderResourceType::TextureUav;

            CBVSRVUAVdescriptorSetLayout.addRange(descriptorType, nrdResourceRange.baseRegisterIndex, nrdResourceRange.descriptorsNum);
        }

        mCBVSRVUAVdescriptorSetLayouts.push_back(CBVSRVUAVdescriptorSetLayout);

        // Create root signature for the NRD pass.
        D3D12RootSignature::Desc rootSignatureDesc;
        rootSignatureDesc.addDescriptorSet(SamplersDescriptorSetLayout);
        rootSignatureDesc.addDescriptorSet(CBVSRVUAVdescriptorSetLayout);

        const D3D12RootSignature::Desc& desc = rootSignatureDesc;

        ref<D3D12RootSignature> pRootSig = D3D12RootSignature::create(mpDevice, desc);

        mpRootSignatures.push_back(pRootSig);

        // Create Compute PSO for the NRD pass.
        {
            std::string shaderFileName = "nrd/Shaders/Source/" + std::string(nrdPipelineDesc.shaderFileName) + ".hlsl";

            Program::Desc programDesc;
            programDesc.addShaderLibrary(shaderFileName).csEntry(nrdPipelineDesc.shaderEntryPointName);
            programDesc.setCompilerFlags(Program::CompilerFlags::MatrixLayoutColumnMajor);
            DefineList defines;
            defines.add("NRD_COMPILER_DXC");
            defines.add("NRD_NORMAL_ENCODING", kNormalEncoding);
            defines.add("NRD_ROUGHNESS_ENCODING", kRoughnessEncoding);
            defines.add("GROUP_X", "16");
            defines.add("GROUP_Y", "16");

            ref<ComputePass> pPass = ComputePass::create(mpDevice, programDesc, defines);

            ref<ComputeProgram> pProgram = pPass->getProgram();
            ref<const ProgramKernels> pProgramKernels = pProgram->getActiveVersion()->getKernels(mpDevice.get(), pPass->getVars().get());

            ComputeStateObject::Desc csoDesc;
            csoDesc.setProgramKernels(pProgramKernels);
            csoDesc.setD3D12RootSignatureOverride(pRootSig);

            ref<ComputeStateObject> pCSO = ComputeStateObject::create(mpDevice, csoDesc);

            mpPasses.push_back(pPass);
            mpCachedProgramKernels.push_back(pProgramKernels);
            mpCSOs.push_back(pCSO);
        }
    }
}

static inline uint16_t NRD_DivideUp(uint32_t x, uint16_t y)
{
    return uint16_t((x + y - 1) / y);
}

void NRDPassBase::createResources()
{
    // Destroy previously created resources.
    mpSamplers.clear();
    mpPermanentTextures.clear();
    mpTransientTextures.clear();
    mpConstantBuffer = nullptr;

    const nrd::InstanceDesc& instanceDesc = nrd::GetInstanceDesc(*mpInstance);
    const uint32_t poolSize = instanceDesc.permanentPoolSize + instanceDesc.transientPoolSize;

    // Create samplers.
    for (uint32_t i = 0; i < instanceDesc.samplersNum; i++)
    {
        const nrd::Sampler& nrdStaticsampler = instanceDesc.samplers[i];
        Sampler::Desc samplerDesc;
        samplerDesc.setFilterMode(Sampler::Filter::Linear, Sampler::Filter::Linear, Sampler::Filter::Point);

        if (nrdStaticsampler == nrd::Sampler::NEAREST_CLAMP || nrdStaticsampler == nrd::Sampler::LINEAR_CLAMP)
        {
            samplerDesc.setAddressingMode(Sampler::AddressMode::Clamp, Sampler::AddressMode::Clamp, Sampler::AddressMode::Clamp);
        }
        else
        {
            samplerDesc.setAddressingMode(Sampler::AddressMode::Mirror, Sampler::AddressMode::Mirror, Sampler::AddressMode::Mirror);
        }

        if (nrdStaticsampler == nrd::Sampler::NEAREST_CLAMP)
        {
            samplerDesc.setFilterMode(Sampler::Filter::Point, Sampler::Filter::Point, Sampler::Filter::Point);
        }
        else
        {
            samplerDesc.setFilterMode(Sampler::Filter::Linear, Sampler::Filter::Linear, Sampler::Filter::Point);
        }

        mpSamplers.push_back(Sampler::create(mpDevice, samplerDesc));
    }

    // Texture pool.
    for (uint32_t i = 0; i < poolSize; i++)
    {
        const bool isPermanent = (i < instanceDesc.permanentPoolSize);

        // Get texture desc.
        const nrd::TextureDesc& nrdTextureDesc =
            isPermanent ? instanceDesc.permanentPool[i] : instanceDesc.transientPool[i - instanceDesc.permanentPoolSize];

        // Create texture.
        ResourceFormat textureFormat = getFalcorFormat(nrdTextureDesc.format);
        uint w = NRD_DivideUp(mScreenSize.x, nrdTextureDesc.downsampleFactor);
        uint h = NRD_DivideUp(mScreenSize.y, nrdTextureDesc.downsampleFactor);
        ref<Texture> pTexture = Texture::create2D(
            mpDevice, w, h,
            textureFormat, 1u, 1,
            nullptr,
            ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess);

        if (isPermanent)
            mpPermanentTextures.push_back(pTexture);
        else
            mpTransientTextures.push_back(pTexture);
    }

    // Constant buffer.
    mpConstantBuffer = Buffer::create(
        mpDevice,
        instanceDesc.constantBufferMaxDataSize,
        ResourceBindFlags::Constant,
        Buffer::CpuAccess::Write,
        nullptr);
}

void NRDPassBase::packRadiancePass(RenderContext* pRenderContext, const RenderData& renderData) {
    //Init the pack Radiance pass
    if (!mpPackRadiancePass && (mDenoisingMethod != DenoisingMethod::Sigma))
    {
        //Get Method; 0=Relax, 1 = Reblur, 2= Occlusion
        uint nrdMethod = isRelax(mDenoisingMethod) ? 0 : isReblur(mDenoisingMethod) ? 1 : 2;
        DefineList defines;
        defines.add("NRD_NORMAL_ENCODING", kNormalEncoding);
        defines.add("NRD_ROUGHNESS_ENCODING", kRoughnessEncoding);
        defines.add("GROUP_X", "16");
        defines.add("GROUP_Y", "16");
        defines.add("NRD_DIFFUSE_VALID", denoiserIsDiffuse(mDenoisingMethod) ? "1" : "0");
        defines.add("NRD_SPECULAR_VALID", denoiserIsSpecular(mDenoisingMethod) ? "1" : "0");
        defines.add("NRD_METHOD", std::to_string(nrdMethod));
        mpPackRadiancePass = ComputePass::create(mpDevice, kShaderPackRadiance, "main", defines);
    }

    if (isRelax(mDenoisingMethod))
    {
        // Run classic Falcor compute pass to pack radiance.
        {
            FALCOR_PROFILE(pRenderContext, "PackRadiance");
            auto perImageCB = mpPackRadiancePass->getRootVar()["PerImageCB"];

            perImageCB["gMaxIntensity"] = mMaxIntensity;
            perImageCB["gDiffuseRadianceHitDist"] = renderData.getTexture(kInputDiffuseRadianceHitDist);
            perImageCB["gSpecularRadianceHitDist"] = renderData.getTexture(kInputSpecularRadianceHitDist);
            mpPackRadiancePass->execute(pRenderContext, uint3(mScreenSize.x, mScreenSize.y, 1u));
        }

        nrd::SetDenoiserSettings(*mpInstance, nrd::Identifier(getNrdDenoiser(mDenoisingMethod)), static_cast<void*>(&mRelaxSettings));
    }
    else if (isReblur(mDenoisingMethod))
    {
        // Run classic Falcor compute pass to pack radiance and hit distance.
        {
            FALCOR_PROFILE(pRenderContext, "PackRadianceHitDist");
            auto perImageCB = mpPackRadiancePass->getRootVar()["PerImageCB"];

            perImageCB["gHitDistParams"].setBlob(mReblurSettings.hitDistanceParameters);
            perImageCB["gMaxIntensity"] = mMaxIntensity;
            perImageCB["gDiffuseRadianceHitDist"] = renderData.getTexture(kInputDiffuseRadianceHitDist);
            perImageCB["gSpecularRadianceHitDist"] = renderData.getTexture(kInputSpecularRadianceHitDist);
            perImageCB["gNormalRoughness"] = renderData.getTexture(kInputNormalRoughnessMaterialID);
            perImageCB["gViewZ"] = renderData.getTexture(kInputViewZ);
            mpPackRadiancePass->execute(pRenderContext, uint3(mScreenSize.x, mScreenSize.y, 1u));
        }

        nrd::SetDenoiserSettings(*mpInstance, nrd::Identifier(getNrdDenoiser(mDenoisingMethod)), static_cast<void*>(&mReblurSettings)
        );
    }
    else if (isOcclusion(mDenoisingMethod))
    {
        FALCOR_PROFILE(pRenderContext, "PackHitDist");
        auto perImageCB = mpPackRadiancePass->getRootVar()["PerImageCB"];

        perImageCB["gHitDistParams"].setBlob(mReblurSettings.hitDistanceParameters);
        perImageCB["gDiffuseHitDist"] = renderData.getTexture(kInputDiffuseHitDist);
        perImageCB["gSpecularHitDist"] = renderData.getTexture(kInputSpecularHitDist);
        perImageCB["gNormalRoughness"] = renderData.getTexture(kInputNormalRoughnessMaterialID);
        perImageCB["gViewZ"] = renderData.getTexture(kInputViewZ);
        mpPackRadiancePass->execute(pRenderContext, uint3(mScreenSize.x, mScreenSize.y, 1u));

        nrd::SetDenoiserSettings(*mpInstance, nrd::Identifier(getNrdDenoiser(mDenoisingMethod)), static_cast<void*>(&mReblurSettings));
    }
    else if (mDenoisingMethod == DenoisingMethod::Sigma)
    {
        nrd::SetDenoiserSettings(*mpInstance, nrd::Identifier(nrd::Denoiser::SIGMA_SHADOW), static_cast<void*>(&mSigmaSettings));
    }
    else
    {
        FALCOR_UNREACHABLE();
        return;
    }
}

void NRDPassBase::checkMotionFormat(const RenderData& renderData) {
    //Get texture format
    auto format = renderData[kInputMotionVectors]->asTexture()->getFormat();
    bool has3Components = false;
    has3Components |= format == ResourceFormat::RGBA32Float;
    has3Components |= format == ResourceFormat::RGBA16Float;
    has3Components |= format == ResourceFormat::RGB32Float;

    mMotion2_5D = has3Components;
}

void NRDPassBase::executeInternal(RenderContext* pRenderContext, const RenderData& renderData)
{
    FALCOR_ASSERT(mpScene);

    if (mRecreateDenoiser)
    {
        reinit();
        checkMotionFormat(renderData);
        mRecreateDenoiser = false;
        mOptionsChanged = true;
    }

    packRadiancePass(pRenderContext, renderData);

    // Initialize common settings.
    float4x4 viewMatrix = mpScene->getCamera()->getViewMatrix();
    float4x4 projMatrix = mpScene->getCamera()->getData().projMatNoJitter;
    // NRD's convention for the jitter is: [-0.5; 0.5] sampleUv = pixelUv + cameraJitter. Falcors jitter is in subpixel size divided by screen res
    float2 cameraJitter = float2(-mpScene->getCamera()->getJitterX() * mScreenSize.x, mpScene->getCamera()->getJitterY() * mScreenSize.y);
    if (mFrameIndex == 0)
    {
        mPrevViewMatrix = viewMatrix;
        mPrevProjMatrix = projMatrix;
        mPrevCameraJitter = cameraJitter;
    }

    copyMatrix(mCommonSettings.viewToClipMatrix, projMatrix);
    copyMatrix(mCommonSettings.viewToClipMatrixPrev, mPrevProjMatrix);
    copyMatrix(mCommonSettings.worldToViewMatrix, viewMatrix);
    copyMatrix(mCommonSettings.worldToViewMatrixPrev, mPrevViewMatrix);
    
    mCommonSettings.cameraJitter[0] = cameraJitter.x;
    mCommonSettings.cameraJitter[1] = cameraJitter.y;
    mCommonSettings.cameraJitterPrev[0] = mPrevCameraJitter.x;
    mCommonSettings.cameraJitterPrev[1] = mPrevCameraJitter.y;
    mCommonSettings.denoisingRange = kNRDDepthRange;
    mCommonSettings.disocclusionThreshold = mDisocclusionThreshold * 0.01f;
    mCommonSettings.frameIndex = mFrameIndex;
    mCommonSettings.isMotionVectorInWorldSpace = mWorldSpaceMotion;
    if (!mWorldSpaceMotion)
        mCommonSettings.motionVectorScale[2] = mMotion2_5D ? 1.f : 0.f; // Enable 2.5D motion

    mCommonSettings.resourceSize[0] = mScreenSize.x;
    mCommonSettings.resourceSize[1] = mScreenSize.y;
    mCommonSettings.resourceSizePrev[0] = mScreenSize.x;
    mCommonSettings.resourceSizePrev[1] = mScreenSize.y;
    mCommonSettings.rectSize[0] = mScreenSize.x;
    mCommonSettings.rectSize[1] = mScreenSize.y;
    mCommonSettings.rectSizePrev[0] = mScreenSize.x;
    mCommonSettings.rectSizePrev[1] = mScreenSize.y;
    mCommonSettings.enableValidation = mEnableValidationLayer;
    mCommonSettings.splitScreen = mEnableSplitScreen ? mSplitScreenValue : 0.0f;

    mPrevViewMatrix = viewMatrix;
    mPrevProjMatrix = projMatrix;
    mPrevCameraJitter = cameraJitter;
    mFrameIndex++;

    nrd::Result result = nrd::SetCommonSettings(*mpInstance, mCommonSettings);
    FALCOR_ASSERT(result == nrd::Result::SUCCESS)

    // Run NRD dispatches.
    const nrd::DispatchDesc* dispatchDescs = nullptr;
    uint32_t dispatchDescNum = 0;
    nrd::Identifier denoiser = nrd::Identifier(getNrdDenoiser(mDenoisingMethod));   
    result = nrd::GetComputeDispatches(*mpInstance, &denoiser, 1, dispatchDescs, dispatchDescNum);
    FALCOR_ASSERT(result == nrd::Result::SUCCESS);

    for (uint32_t i = 0; i < dispatchDescNum; i++)
    {
        const nrd::DispatchDesc& dispatchDesc = dispatchDescs[i];
        FALCOR_PROFILE(pRenderContext, dispatchDesc.name);
        dispatch(pRenderContext, renderData, dispatchDesc);
    }

    // Submit the existing command list and start a new one.
    pRenderContext->flush();
}

void NRDPassBase::dispatch(RenderContext* pRenderContext, const RenderData& renderData, const nrd::DispatchDesc& dispatchDesc)
{
    const nrd::InstanceDesc& instanceDesc = nrd::GetInstanceDesc(*mpInstance);
    const nrd::PipelineDesc& pipelineDesc = instanceDesc.pipelines[dispatchDesc.pipelineIndex];

    // Set root signature.
    mpRootSignatures[dispatchDesc.pipelineIndex]->bindForCompute(pRenderContext);

    // Upload constants.
    mpConstantBuffer->setBlob(dispatchDesc.constantBufferData, 0, dispatchDesc.constantBufferDataSize);

    // Create descriptor set for the NRD pass.
    ref<D3D12DescriptorSet> CBVSRVUAVDescriptorSet = D3D12DescriptorSet::create(mpDevice, mCBVSRVUAVdescriptorSetLayouts[dispatchDesc.pipelineIndex], D3D12DescriptorSetBindingUsage::ExplicitBind);

    // Set CBV.
    mpCBV = D3D12ConstantBufferView::create(mpDevice, mpConstantBuffer);
    CBVSRVUAVDescriptorSet->setCbv(0 /* NB: range #0 is CBV range */, instanceDesc.constantBufferRegisterIndex, mpCBV.get());

    uint32_t resourceIndex = 0;
    for (uint32_t resourceRangeIndex = 0; resourceRangeIndex < pipelineDesc.resourceRangesNum; resourceRangeIndex++)
    {
        const nrd::ResourceRangeDesc& nrdResourceRange = pipelineDesc.resourceRanges[resourceRangeIndex];

        for (uint32_t resourceOffset = 0; resourceOffset < nrdResourceRange.descriptorsNum; resourceOffset++)
        {
            FALCOR_ASSERT(resourceIndex < dispatchDesc.resourcesNum);
            const nrd::ResourceDesc& resourceDesc = dispatchDesc.resources[resourceIndex];
            
            FALCOR_ASSERT(resourceDesc.descriptorType == nrdResourceRange.descriptorType);

            ref<Texture> texture;

            switch (resourceDesc.type)
            {
            case nrd::ResourceType::IN_MV:
                texture = renderData.getTexture(kInputMotionVectors);
                break;
            case nrd::ResourceType::IN_NORMAL_ROUGHNESS:
                texture = renderData.getTexture(kInputNormalRoughnessMaterialID);
                break;
            case nrd::ResourceType::IN_VIEWZ:
                texture = renderData.getTexture(kInputViewZ);
                break;
            case nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST:
                texture = renderData.getTexture(kInputDiffuseRadianceHitDist);
                break;
            case nrd::ResourceType::IN_SPEC_RADIANCE_HITDIST:
                texture = renderData.getTexture(kInputSpecularRadianceHitDist);
                break;
            /*
            case nrd::ResourceType::IN_DELTA_PRIMARY_POS:
                texture = renderData.getTexture(kInputDeltaPrimaryPosW);
                break;
            case nrd::ResourceType::IN_DELTA_SECONDARY_POS:
                texture = renderData.getTexture(kInputDeltaSecondaryPosW);
                break;
            */
            case nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST:
                texture = renderData.getTexture(kOutputFilteredDiffuseRadianceHitDist);
                break;
            case nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST:
                texture = renderData.getTexture(kOutputFilteredSpecularRadianceHitDist);
                break;
            case nrd::ResourceType::IN_PENUMBRA:
                texture = renderData.getTexture(kInputPenumbra);
                break;
            case nrd::ResourceType::OUT_SHADOW_TRANSLUCENCY:
                texture = renderData.getTexture(kOutputFilteredShadow);
                break;
            case nrd::ResourceType::IN_DIFF_HITDIST:
                texture = renderData.getTexture(kInputDiffuseHitDist);
                break;
            case nrd::ResourceType::OUT_DIFF_HITDIST:
                texture = renderData.getTexture(kOutputFilteredDiffuseOcclusion);
                break;
            case nrd::ResourceType::IN_SPEC_HITDIST:
                texture = renderData.getTexture(kInputSpecularHitDist);
                break;
            case nrd::ResourceType::OUT_SPEC_HITDIST:
                texture = renderData.getTexture(kOutputFilteredSpecularOcclusion);
                break;
            case nrd::ResourceType::OUT_VALIDATION:
                texture = renderData.getTexture(kOutputValidation);
                break;
            case nrd::ResourceType::TRANSIENT_POOL:
                texture = mpTransientTextures[resourceDesc.indexInPool];
                break;
            case nrd::ResourceType::PERMANENT_POOL:
                texture = mpPermanentTextures[resourceDesc.indexInPool];
                break;
            default:
                FALCOR_ASSERT(!"Unavailable resource type");
                break;
            }

            FALCOR_ASSERT(texture);

            // Set up resource barriers.
            Resource::State newState = resourceDesc.descriptorType == nrd::DescriptorType::TEXTURE ? Resource::State::ShaderResource
                                                                                                   : Resource::State::UnorderedAccess;
            {
                const ResourceViewInfo viewInfo = ResourceViewInfo(0, 1, 0, 1);
                pRenderContext->resourceBarrier(texture.get(), newState, &viewInfo);
            }

            // Set the SRV and UAV descriptors.
            if (nrdResourceRange.descriptorType == nrd::DescriptorType::TEXTURE)
            {
                ref<ShaderResourceView> pSRV = texture->getSRV(0, 1, 0, 1);
                CBVSRVUAVDescriptorSet->setSrv(
                    resourceRangeIndex + 1 /* NB: range #0 is CBV range */, nrdResourceRange.baseRegisterIndex + resourceOffset, pSRV.get()
                );
            }
            else
            {
                ref<UnorderedAccessView> pUAV = texture->getUAV(0, 0, 1);
                CBVSRVUAVDescriptorSet->setUav(
                    resourceRangeIndex + 1 /* NB: range #0 is CBV range */, nrdResourceRange.baseRegisterIndex + resourceOffset, pUAV.get()
                );
            }

            resourceIndex++;
        }
    }

    FALCOR_ASSERT(resourceIndex == dispatchDesc.resourcesNum);

    // Set descriptor sets.
    mpSamplersDescriptorSet->bindForCompute(pRenderContext, mpRootSignatures[dispatchDesc.pipelineIndex].get(), 0);
    CBVSRVUAVDescriptorSet->bindForCompute(pRenderContext, mpRootSignatures[dispatchDesc.pipelineIndex].get(), 1);

    // Set pipeline state.
    ref<ComputePass> pPass = mpPasses[dispatchDesc.pipelineIndex];
    ref<ComputeProgram> pProgram = pPass->getProgram();
    ref<const ProgramKernels> pProgramKernels = pProgram->getActiveVersion()->getKernels(mpDevice.get(), pPass->getVars().get());

    // Check if anything changed.
    bool newProgram = (pProgramKernels.get() != mpCachedProgramKernels[dispatchDesc.pipelineIndex].get());
    if (newProgram)
    {
        mpCachedProgramKernels[dispatchDesc.pipelineIndex] = pProgramKernels;

        ComputeStateObject::Desc desc;
        desc.setProgramKernels(pProgramKernels);
        desc.setD3D12RootSignatureOverride(mpRootSignatures[dispatchDesc.pipelineIndex]);

        ref<ComputeStateObject> pCSO = ComputeStateObject::create(mpDevice, desc);
        mpCSOs[dispatchDesc.pipelineIndex] = pCSO;
    }
    ID3D12GraphicsCommandList* pCommandList = pRenderContext->getLowLevelData()->getCommandBufferNativeHandle().as<ID3D12GraphicsCommandList*>();
    ID3D12PipelineState* pPipelineState = mpCSOs[dispatchDesc.pipelineIndex]->getNativeHandle().as<ID3D12PipelineState*>();

    pCommandList->SetPipelineState(pPipelineState);

    // Dispatch.
    pCommandList->Dispatch(dispatchDesc.gridWidth, dispatchDesc.gridHeight, 1);
}


