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
#include "RenderGraph/RenderPass.h"
#include "Utils/Timing/Clock.h"

using namespace Falcor;

class CameraPath : public RenderPass
{
public:
    FALCOR_PLUGIN_CLASS(CameraPath, "CameraPath", "Create, Load and use Camera Paths");

    static ref<CameraPath> create(ref<Device> pDevice, const Properties& props) { return make_ref<CameraPath>(pDevice, props); }

    CameraPath(ref<Device> pDevice, const Properties& props);

    virtual Properties getProperties() const override;
    virtual RenderPassReflection reflect(const CompileData& compileData) override;
    virtual void compile(RenderContext* pRenderContext, const CompileData& compileData) override {}
    virtual void execute(RenderContext* pRenderContext, const RenderData& renderData) override;
    virtual void renderUI(Gui::Widgets& widget) override;
    virtual void setScene(RenderContext* pRenderContext, const ref<Scene>& pScene) override;
    virtual bool onMouseEvent(const MouseEvent& mouseEvent) override { return false; }
    virtual bool onKeyEvent(const KeyboardEvent& keyEvent) override { return false; }

private:
    struct CamPathData
    {
        float3 position;
        float3 target;
        double deltaT;
    };

    void startRecording();
    void recordFrame();
    void startPath();
    void pathFrame();
    bool storeCameraPath(std::filesystem::path& path);
    bool loadCameraPathFromFile(const std::filesystem::path& path);
    void smoothCameraPath();

    void pathUI(Gui::Widgets & widget);

    const size_t kMaxSearchedFrames = 512;
    const size_t kMinSmoothSize = 30;
    FileDialogFilterVec kCamPathFileFilters = {FileDialogFilter("fcp", "Faclor Camera Path File")};

    ref<Scene> mpScene;     ///< Scene Reference Pointer
    ref<Camera> mpCamera;   ///< Camera Pointer

    Clock mClock;           ///< Local Time Clock for the Camera animation

    std::vector<CamPathData> mCameraPath; ///< The currently loaded in camera path
    std::vector<CamPathData> mCameraPathBackup; ///< Backup of Camera Path that is used for smoothing

    std::string mStatus = "Camera Path Pass";       ///< Current Status for user feedback
    bool mUseCameraPath = false;    ///<Go along the camera path
    bool mRecordCameraPath = false; ///<Record path mode
    size_t mCurrentPathFrame = 0;   ///< Currently read frame
    size_t mRecordedFrames = 0;     ///< Used for recording frames to memorise how many frames have been created
    size_t mCurrentPauseFrame = 0;  ///< When clock is paused, stores the current position
    double mLastFrameTime = 0.0;           ///< Time for the last frame
    double mNextFrameTime = 0.0;           ///<Time for the next node

    double mClockTimeScale = 1.0;       ///< Time scale for the clock

    //Smoothing
    bool mSmoothTarget = true;
    bool mSmoothPosition = false;
    uint mSmoothFilterSize = 5;
    float mGaussSigma = 1.0f;
};
