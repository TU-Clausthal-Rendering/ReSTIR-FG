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
#pragma once

#include "Falcor.h"
#include "Core/Enum.h"
#include "Core/API/Shared/D3D12DescriptorSet.h"
#include "Core/API/Shared/D3D12RootSignature.h"
#include "Core/API/Shared/D3D12ConstantBufferView.h"
#include "RenderGraph/RenderPassHelpers.h"

#include <NRD.h>

using namespace Falcor;

class NRDPassBase : public RenderPass
{
public:
    //FALCOR_PLUGIN_CLASS(NRDPassBase, "NRD", "NRD denoiser.");

    enum class DenoisingMethod : uint32_t
    {
        RelaxDiffuseSpecular,
        ReblurDiffuseSpecular,
        RelaxDiffuse,
        RelaxSpecular,
        ReblurDiffuse,
        ReblurSpecular,
        Sigma,
        ReblurOcclusionDiffuse,
        ReblurOcclusionSpecular,
        ReblurOcclusionDiffuseSpecular,
    };

    FALCOR_ENUM_INFO(DenoisingMethod, {
        { DenoisingMethod::RelaxDiffuseSpecular, "RelaxDiffuseSpecular" },
        { DenoisingMethod::ReblurDiffuseSpecular, "ReblurDiffuseSpecular" },
        {DenoisingMethod::RelaxDiffuse, "RelaxDiffuse"},
        {DenoisingMethod::RelaxSpecular, "ReblurSpecular"},
        {DenoisingMethod::ReblurDiffuse, "ReblurDiffuse"},
        {DenoisingMethod::ReblurSpecular, "ReblurSpecular"},
        {DenoisingMethod::Sigma, "Sigma"},
        {DenoisingMethod::ReblurOcclusionDiffuse, "ReblurOcclusionDiffuse"},
        {DenoisingMethod::ReblurOcclusionSpecular, "ReblurOcclusionSpecular"},
        {DenoisingMethod::ReblurOcclusionDiffuseSpecular, "ReblurOcclusionDiffuseSpecular"},
    });

    //Copy of the NRD enum, as it uses uint8 and Falcor GUI uses uint32
    enum class HitDistanceReconstructionMode : uint32_t
    {
        OFF,
        AREA3X3,
        AREA5X5,
    };

    FALCOR_ENUM_INFO(HitDistanceReconstructionMode, {
            {HitDistanceReconstructionMode::OFF, "Off"},
            {HitDistanceReconstructionMode::AREA3X3, "Area3x3"},
            {HitDistanceReconstructionMode::AREA5X5, "Area5x5"},
    });

    virtual Properties getProperties() const override;
    virtual void compile(RenderContext* pRenderContext, const CompileData& compileData) override;
    virtual void execute(RenderContext* pRenderContext, const RenderData& renderData) override;
    virtual void renderUI(Gui::Widgets& widget) override;
    virtual void setScene(RenderContext* pRenderContext, const ref<Scene>& pScene) override;

protected:
    //
    // I/O Strings for RenderData
    //
    //  Input buffer names.
    static constexpr const char* kInputDiffuseRadianceHitDist = "diffuseRadianceHitDist";
    static constexpr const char* kInputSpecularRadianceHitDist = "specularRadianceHitDist";
    static constexpr const char* kInputPenumbra = "penumbra";
    static constexpr const char* kInputDiffuseHitDist = "diffuseHitDist";
    static constexpr const char* kInputSpecularHitDist = "specularHitDist";
    static constexpr const char* kInputMotionVectors = "mvec";
    static constexpr const char* kInputNormalRoughnessMaterialID = "normWRoughnessMaterialID";
    static constexpr const char* kInputViewZ = "viewZ";

    // Output buffer names.
    static constexpr const char* kOutputFilteredDiffuseRadianceHitDist = "filteredDiffuseRadianceHitDist";
    static constexpr const char* kOutputFilteredSpecularRadianceHitDist = "filteredSpecularRadianceHitDist";
    static constexpr const char* kOutputFilteredShadow = "outFilteredShadow";
    static constexpr const char* kOutputFilteredDiffuseOcclusion = "filteredDiffuseOcclusion";
    static constexpr const char* kOutputFilteredSpecularOcclusion = "filteredSpecularOcclusion";
    static constexpr const char* kOutputReflectionMotionVectors = "reflectionMvec";
    static constexpr const char* kOutputDeltaMotionVectors = "deltaMvec";
    static constexpr const char* kOutputValidation = "outValidation";


    NRDPassBase(ref<Device> pDevice, const Properties& props);
    const std::string kNormalEncoding = "2";
    const std::string kRoughnessEncoding = "1";

    ref<Scene> mpScene;
    uint2 mScreenSize{};
    uint32_t mFrameIndex = 0;
    RenderPassHelpers::IOSize  mOutputSizeSelection = RenderPassHelpers::IOSize::Default; ///< Selected output size.

    void reflectBase(const uint2 ioSize, RenderPassReflection& reflector);
    void reinit();
    void createPipelines();
    void createResources();
    void checkMotionFormat(const RenderData& renderData);
    void executeInternal(RenderContext* pRenderContext, const RenderData& renderData);
    void packRadiancePass(RenderContext* pRenderContext, const RenderData& renderData);
    void dispatch(RenderContext* pRenderContext, const RenderData& renderData, const nrd::DispatchDesc& dispatchDesc);

    nrd::Instance* mpInstance = nullptr;

    bool mEnabled = true;
    bool mOptionsChanged = true;       //Falcor specific options changed. Sets dict flags for other passes
    DenoisingMethod mDenoisingMethod = DenoisingMethod::RelaxDiffuseSpecular;
    bool mRecreateDenoiser = true;
    bool mWorldSpaceMotion = false;
    bool mMotion2_5D = false;
    bool mEnableValidationLayer = false;
    float mMaxIntensity = 250.f;
    float mDisocclusionThreshold = 2.f;
    bool mEnableSplitScreen = false;
    float mSplitScreenValue = 0.5f;
    nrd::CommonSettings mCommonSettings = {};

    nrd::RelaxSettings mRelaxSettings = {};
    nrd::ReblurSettings mReblurSettings = {};
    nrd::SigmaSettings mSigmaSettings = {};
    HitDistanceReconstructionMode mHitDistanceReconstructionMode = HitDistanceReconstructionMode::OFF;

    std::vector<ref<Sampler>> mpSamplers;
    std::vector<D3D12DescriptorSetLayout> mCBVSRVUAVdescriptorSetLayouts;
    ref<D3D12DescriptorSet> mpSamplersDescriptorSet;
    std::vector<ref<D3D12RootSignature>> mpRootSignatures;
    std::vector<ref<ComputePass>> mpPasses;
    std::vector<ref<const ProgramKernels>> mpCachedProgramKernels;
    std::vector<ref<ComputeStateObject>> mpCSOs;
    std::vector<ref<Texture>> mpPermanentTextures;
    std::vector<ref<Texture>> mpTransientTextures;
    ref<Buffer> mpConstantBuffer;
    ref<D3D12ConstantBufferView> mpCBV;

    float4x4 mPrevViewMatrix;
    float4x4 mPrevProjMatrix;
    float2 mPrevCameraJitter;

    // Additional classic Falcor compute pass and resources for packing radiance and hitT for NRD.
    ref<ComputePass> mpPackRadiancePass;
};

FALCOR_ENUM_REGISTER(NRDPassBase::DenoisingMethod);
FALCOR_ENUM_REGISTER(NRDPassBase::HitDistanceReconstructionMode);
