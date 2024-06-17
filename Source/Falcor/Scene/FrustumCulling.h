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
#include "Utils/Math/Vector.h"
#include "Utils/Math/Matrix.h"
#include "Utils/Math/AABB.h"
#include "Camera/Camera.h"
#include "Camera/CameraController.h"
#include "Core/API/Buffer.h"
#include "Core/API/IndirectCommands.h"
#include "Core/API/Device.h"
#include "Core/API/GpuFence.h"

namespace Falcor
{
    /** Frustum Culling Class that helps with Frustum Culling for Rasterizer passes
    *   Based on https://learnopengl.com/Guest-Articles/2021/Scene/Frustum-Culling
    */
    class FALCOR_API FrustumCulling : public Object
    {
        FALCOR_OBJECT(FrustumCulling)
    public:
        FrustumCulling() = default;
        //Constructor based on perspective camera
        FrustumCulling(const ref<Camera>& camera);
        //Constructor based on lookAt and perspective
        FrustumCulling(float3 eye, float3 center, float3 up, float aspect, float fovY, float near, float far);
        //Constructor based on lookAt and ortho
        FrustumCulling(float3 eye, float3 center, float3 up, float left, float right, float bottom, float top, float near, float far);

        //Updates the frustum based on the camera
        void updateFrustum(const ref<Camera>& camera);

        //Updates the frustum based on lookAt and perspective
        void updateFrustum(float3 eye, float3 center, float3 up, float aspect, float fovY, float near, float far);

        // Updates the frustum based on lookAt and ortho
        void updateFrustum(float3 eye, float3 center, float3 up, float left, float right, float bottom, float top, float near, float far);

        // Frustum Culling Test. Assumes AABB is transformed to world coordinates
        bool isInFrustum(const AABB& aabb) const;

        //Returns the number of draw buffers
        size_t getDrawBufferSize() { return mDraw.size(); }

        //Creates the draw buffer from the existing drawBuffer of the scene
        void createDrawBuffer(
            ref<Device> pDevice,
            ref<GpuFence> pSceneFence,
            RenderContext* pRenderContext,
            const std::vector<ref<Buffer>>& drawBuffer,
            const std::vector<bool>& isDynamic
        );

        //Update of the draw buffer with (culled) vector of draw arguments. Overload for DrawIndexedArguments
        void updateDrawBuffer(RenderContext* pRenderContext, uint index, const std::vector<DrawIndexedArguments> drawArguments);

        // Update of the draw buffer with (culled) vector of draw arguments. Overload for DrawArguments
        void updateDrawBuffer(RenderContext* pRenderContext, uint index, const std::vector<DrawArguments> drawArguments);

        // Call at the start of the draw call with the sync value from the scene for proper CPU/GPU sync
        void startUpdate(const uint lastFrameSyncValue);

        bool hasDynamic() const {return mHasDynamic; }

        //Checks if the dynamic instances have changed
        bool checkDynamicInstances(uint index, const std::vector<uint> passedInstanceIDs);

        std::vector<ref<Buffer>>& getDrawBuffers() { return mDraw; }
        std::vector<uint>& getDrawCounts() { return mDrawCount; }

        bool isBufferValid(uint index) { return mValidDrawBuffer[index]; }
        void invalidateAllDrawBuffers();

    private:
        static const uint kStagingFramesInFlight = 6u;

        struct Plane
        {
            float3 normal = float3(0.f, 1.f, 0.f);
            float distance = 0.f;

            Plane() = default;
            Plane(const float3 p, const float3 N);

            float getSignedDistanceToPlane(const float3 point) const;
        };

        struct Frustum
        {
            Plane near;
            Plane far;

            Plane top;
            Plane bottom;
            
            Plane left;
            Plane right;
        };

        struct StagingInfo
        {
            uint count;
            uint maxElementsBytes;
            ref<Buffer> buffer;
        };

        //Creates the camera frustum based on an perspective camera
        void createFrustum(float3 camPos, float3 camU, float3 camV, float3 camW, float aspect, float fovY, float near, float far);

        // Creates the camera frustum based on an orthographic
        void createFrustum(float3 camPos, float3 camU, float3 camV, float3 camW, float left, float right, float bottom, float top, float near, float far);

        //Test if a AABB is in front of the plane based on https://gdbooks.gitbooks.io/3dcollisions/content/Chapter2/static_aabb_plane.html. Assumes that the AABB already transformed to world coordinates
        bool isInFrontOfPlane(const Plane& plane, const AABB& aabb) const;

        Frustum mFrustum;
        ref<GpuFence> mpStagingFence;   //Copy of the scenes fence

        bool mDrawValid = false;
        bool mHasDynamic = false;
        uint mStagingCount = 0; 
        std::array<uint64_t, kStagingFramesInFlight> mFenceWaitValues;
        std::vector<StagingInfo> mStagingBuffer; // Current Staging index for each buff
        std::vector<ref<Buffer>> mDraw;      //Draw buffer that can be reused if there was no change in frustum. One per mDrawArgs from scene
        std::vector<uint> mDrawCount;         //The number of elements in the draw buffer. One per mDrawArgs from scene
        std::vector<bool> mValidDrawBuffer;

        std::vector<std::vector<uint>> mDynamicInstanceID;  //The dynamic instance id is stored to check if the culling buffer does not need to be copied again
        std::vector<uint> mDynamicDrawArgsToInstanceID;     //Mapping buffer to map between drawArgs and the above vector
    };
}
