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
#include "CustomAccelerationStructure.h"
#include "Core/Assert.h"
#include "Core/API/RenderContext.h"
#include "Utils/Logger.h"
#include "Utils/Math/Common.h"
#include "Utils/Scripting/ScriptBindings.h"
#include <fstd/bit.h> // TODO C++20: Replace with <bit>

namespace Falcor
{
    CustomAccelerationStructure::CustomAccelerationStructure(ref<Device> pDevice, const uint64_t aabbCount, const uint64_t aabbGpuAddress)
    {
        mpDevice = pDevice;
        if (!mpDevice->isFeatureSupported(Device::SupportedFeatures::Raytracing))
        {
            throw std::exception("Raytracing is not supported by the current device");
        }

        const std::vector count = {aabbCount};
        const std::vector gpuAddress = {aabbGpuAddress};

        createAccelerationStructure(count, gpuAddress);
    }

    CustomAccelerationStructure::CustomAccelerationStructure(
        ref<Device> pDevice,
        const std::vector<uint64_t>& aabbCount,
        const std::vector<uint64_t>& aabbGpuAddress
    )
    {
        mpDevice = pDevice;
        if (!mpDevice->isFeatureSupported(Device::SupportedFeatures::Raytracing))
        {
            throw std::exception("Raytracing is not supported by the current device");
        }

        createAccelerationStructure(aabbCount, aabbGpuAddress);
    }

    CustomAccelerationStructure::~CustomAccelerationStructure()
    {
        clearData();
    }

    void CustomAccelerationStructure::update(RenderContext* pRenderContext)
    {
        const std::vector<uint64_t> count = {0u};
        buildAccelerationStructure(pRenderContext, count, false);
    }

    void CustomAccelerationStructure::update(RenderContext* pRenderContext, const uint64_t aabbCount)
    {
        const std::vector<uint64_t> count = {aabbCount};
        buildAccelerationStructure(pRenderContext, count, true);
    }

    void CustomAccelerationStructure::update(RenderContext* pRenderContext, const std::vector<uint64_t>& aabbCount)
    {
        buildAccelerationStructure(pRenderContext, aabbCount, true);
    }

    void CustomAccelerationStructure::bindTlas(ShaderVar& rootVar, std::string shaderName)
    {
        rootVar[shaderName].setAccelerationStructure(mTlas.pTlasObject);
    }

    void CustomAccelerationStructure::createAccelerationStructure(
        const std::vector<uint64_t>& aabbCount,
        const std::vector<uint64_t>& aabbGpuAddress
    )
    {
        clearData();

        FALCOR_ASSERT(aabbCount.size() == aabbGpuAddress.size());

        mNumberBlas = aabbCount.size();

        createBottomLevelAS(aabbCount, aabbGpuAddress);
        createTopLevelAS();
    }

    void CustomAccelerationStructure::clearData()
    {
        // clear all previous data
        for (uint i = 0; i < mBlas.size(); i++)
        {
            mBlas[i].reset();
            mBlasObjects[i].reset();
        }

        mBlas.clear();
        mBlasData.clear();
        mBlasObjects.clear();
        mInstanceDesc.clear();
        mTlasScratch.reset();
        mTlas.pInstanceDescs.reset();
        mTlas.pTlas.reset();
        mTlas.pTlasObject.reset();
    }

    void CustomAccelerationStructure::createBottomLevelAS(const std::vector<uint64_t> aabbCount, const std::vector<uint64_t> aabbGpuAddress)
    {
        // Create Number of desired blas and reset max size
        mBlasData.resize(mNumberBlas);
        mBlas.resize(mNumberBlas);
        mBlasObjects.resize(mNumberBlas);
        mBlasScratchMaxSize = 0;

        FALCOR_ASSERT(aabbCount.size() >= mNumberBlas);
        FALCOR_ASSERT(aabbGpuAddress.size() >= mNumberBlas);

        // Prebuild
        for (size_t i = 0; i < mNumberBlas; i++)
        {
            auto& blas = mBlasData[i];

            // Create geometry description
            auto& desc = blas.geomDescs;
            desc.type = RtGeometryType::ProcedurePrimitives;
            desc.flags = RtGeometryFlags::NoDuplicateAnyHitInvocation; //< Important! So that photons are not collected multiple times

            desc.content.proceduralAABBs.count = aabbCount[i];
            desc.content.proceduralAABBs.data = aabbGpuAddress[i];
            desc.content.proceduralAABBs.stride = sizeof(AABB);

            // Create input for blas
            auto& inputs = blas.buildInputs;
            inputs.kind = RtAccelerationStructureKind::BottomLevel;
            inputs.descCount = 1;
            inputs.geometryDescs = &blas.geomDescs;

            // Build option flags
            // TODO Check for performance if neither is activated
            inputs.flags = mFastBuild ? RtAccelerationStructureBuildFlags::PreferFastBuild : RtAccelerationStructureBuildFlags::PreferFastTrace;
            if (mUpdate)
                inputs.flags |= RtAccelerationStructureBuildFlags::AllowUpdate;

            // Get prebuild Info
            blas.prebuildInfo = RtAccelerationStructure::getPrebuildInfo(mpDevice.get(), inputs);

            // Figure out the padded allocation sizes to have proper alignment.
            FALCOR_ASSERT(blas.prebuildInfo.resultDataMaxSize > 0);
            blas.blasByteSize = align_to((uint64_t)kAccelerationStructureByteAlignment, blas.prebuildInfo.resultDataMaxSize);

            uint64_t scratchByteSize = std::max(blas.prebuildInfo.scratchDataSize, blas.prebuildInfo.updateScratchDataSize);
            blas.scratchByteSize = align_to((uint64_t)kAccelerationStructureByteAlignment, scratchByteSize);

            mBlasScratchMaxSize = std::max(blas.scratchByteSize, mBlasScratchMaxSize);
        }

        // Create the scratch and blas buffers
        mBlasScratch = Buffer::create(mpDevice, mBlasScratchMaxSize, Buffer::BindFlags::UnorderedAccess);
        mBlasScratch->setName("CustomAS::BlasScratch");

        for (uint i = 0; i < mNumberBlas; i++)
        {
            mBlas[i] = Buffer::create(mpDevice, mBlasData[i].blasByteSize, Buffer::BindFlags::AccelerationStructure);
            mBlas[i]->setName("CustomAS::BlasBuffer" + std::to_string(i));

            // Create API object
            RtAccelerationStructure::Desc blasDesc = {};
            blasDesc.setBuffer(mBlas[i], 0u, mBlasData[i].blasByteSize);
            blasDesc.setKind(RtAccelerationStructureKind::BottomLevel);
            mBlasObjects[i] = RtAccelerationStructure::create(mpDevice, blasDesc);
        }
    }

    void CustomAccelerationStructure::createTopLevelAS()
    {
        mInstanceDesc.clear(); // clear to be sure that it is empty

        // fill the instance description if empty
        for (int i = 0; i < mNumberBlas; i++)
        {
            RtInstanceDesc desc = {};
            desc.accelerationStructure = mBlas[i]->getGpuAddress();
            desc.flags = RtGeometryInstanceFlags::None;
            desc.instanceID = i;
            desc.instanceMask = mNumberBlas < 8 ? 1 << i : 0xFF; // For up to 8 they are instanced seperatly
            desc.instanceContributionToHitGroupIndex = 0;

            // Create a identity matrix for the transform and copy it to the instance desc
            float4x4 identityMat = float4x4::identity();
            std::memcpy(desc.transform, &identityMat, sizeof(desc.transform)); // Copy transform
            mInstanceDesc.push_back(desc);
        }

        RtAccelerationStructureBuildInputs inputs = {};
        inputs.kind = RtAccelerationStructureKind::TopLevel;
        inputs.descCount = (uint32_t)mNumberBlas;
        inputs.flags = RtAccelerationStructureBuildFlags::None; // TODO Check performance for Fast Trace or Fast Update

        // Prebuild
        mTlasPrebuildInfo = RtAccelerationStructure::getPrebuildInfo(mpDevice.get(), inputs);
        mTlasScratch = Buffer::create(mpDevice, mTlasPrebuildInfo.scratchDataSize, Buffer::BindFlags::UnorderedAccess, Buffer::CpuAccess::None);
        mTlasScratch->setName("CustomAS::TLAS_Scratch");

        // Create buffers for the TLAS
        mTlas.pTlas =
            Buffer::create(mpDevice, mTlasPrebuildInfo.resultDataMaxSize, Buffer::BindFlags::AccelerationStructure, Buffer::CpuAccess::None);
        mTlas.pTlas->setName("CustomAS::TLAS");
        mTlas.pInstanceDescs = Buffer::create(
            mpDevice, (uint32_t)mInstanceDesc.size() * sizeof(RtInstanceDesc), Buffer::BindFlags::None, Buffer::CpuAccess::Write,
            mInstanceDesc.data()
        );
        mTlas.pInstanceDescs->setName("CustomAS::TLAS_Instance_Description");

        // Create API object
        RtAccelerationStructure::Desc tlasDesc = {};
        tlasDesc.setBuffer(mTlas.pTlas, 0u, mTlasPrebuildInfo.resultDataMaxSize);
        tlasDesc.setKind(RtAccelerationStructureKind::TopLevel);
        mTlas.pTlasObject = RtAccelerationStructure::create(mpDevice, tlasDesc);
    }

    void CustomAccelerationStructure::buildAccelerationStructure(
        RenderContext* pRenderContext,
        const std::vector<uint64_t>& aabbCount,
        bool updateAABBCount
    )
    {
        // Check if buffers exists
        FALCOR_ASSERT(mBlas[0]);
        FALCOR_ASSERT(mTlas.pTlas);

        FALCOR_ASSERT(aabbCount.size() >= mNumberBlas)

        buildBottomLevelAS(pRenderContext, aabbCount, updateAABBCount);
        buildTopLevelAS(pRenderContext);
    }

    void CustomAccelerationStructure::buildBottomLevelAS(
        RenderContext* pRenderContext,
        const std::vector<uint64_t>& aabbCount,
        bool updateAABBCount
    )
    {
        FALCOR_PROFILE(pRenderContext, "buildCustomBlas");

        for (size_t i = 0; i < mNumberBlas; i++)
        {
            auto& blas = mBlasData[i];

            // barriers for the scratch and blas buffer
            pRenderContext->uavBarrier(mBlasScratch.get());
            pRenderContext->uavBarrier(mBlas[i].get());

            if (updateAABBCount)
                blas.geomDescs.content.proceduralAABBs.count = aabbCount[i];

            // Fill the build desc struct
            RtAccelerationStructure::BuildDesc asDesc = {};
            asDesc.inputs = blas.buildInputs;
            asDesc.scratchData = mBlasScratch->getGpuAddress();
            asDesc.dest = mBlasObjects[i].get();

            // Build the acceleration structure
            pRenderContext->buildAccelerationStructure(asDesc, 0, nullptr);

            // Barrier for the blas
            pRenderContext->uavBarrier(mBlas[i].get());
        }
    }

    void CustomAccelerationStructure::buildTopLevelAS(RenderContext* pRenderContext)
    {
        FALCOR_PROFILE(pRenderContext, "buildCustomTlas");

        RtAccelerationStructureBuildInputs inputs = {};
        inputs.kind = RtAccelerationStructureKind::TopLevel;
        inputs.descCount = (uint32_t)mInstanceDesc.size();
        // Update Flag could be set for TLAS. This made no real time difference in our test so it is left out. Updating could reduce the memory
        // of the TLAS scratch buffer a bit
        inputs.flags = mFastBuild ? RtAccelerationStructureBuildFlags::PreferFastBuild : RtAccelerationStructureBuildFlags::PreferFastTrace;
        if (mUpdate)
            inputs.flags |= RtAccelerationStructureBuildFlags::AllowUpdate;

        RtAccelerationStructure::BuildDesc asDesc = {};
        asDesc.inputs = inputs;
        asDesc.inputs.instanceDescs = mTlas.pInstanceDescs->getGpuAddress();
        asDesc.scratchData = mTlasScratch->getGpuAddress();
        asDesc.dest = mTlas.pTlasObject.get();

        // Create TLAS
        if (mTlas.pInstanceDescs)
        {
            pRenderContext->resourceBarrier(mTlas.pInstanceDescs.get(), Resource::State::NonPixelShader);
        }

        // Build the acceleration structure
        pRenderContext->buildAccelerationStructure(asDesc, 0, nullptr);

        // Barrier for the blas
        pRenderContext->uavBarrier(mTlas.pTlas.get());
    }

}
