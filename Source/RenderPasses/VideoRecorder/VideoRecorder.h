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

struct PathPoint
{
    float3 pos;
    float3 dir;
    float3 up;
    float time;
};

class VideoRecorder : public RenderPass
{
    enum class State
    {
        Idle,
        Record, // record path
        Preview, // preview path in app
        Render, // render to video file
        Warmup // warmup prior to render that should fix temporal artifacts
    };
public:
    FALCOR_PLUGIN_CLASS(VideoRecorder, "VideoRecorder", "Camera Path and Video recorder(using FFMPEG)");

    static ref<VideoRecorder> create(ref<Device> pDevice, const Properties& props) { return make_ref<VideoRecorder>(pDevice, props); }

    VideoRecorder(ref<Device> pDevice, const Properties& props);

    virtual Properties getProperties() const override;
    virtual RenderPassReflection reflect(const CompileData& compileData) override;
    virtual void compile(RenderContext* pRenderContext, const CompileData& compileData) override {}
    virtual void execute(RenderContext* pRenderContext, const RenderData& renderData) override;
    virtual void setScene(RenderContext* pRenderContext, const ref<Scene>& pScene) override;
    virtual bool onMouseEvent(const MouseEvent& mouseEvent) override { return false; }
    virtual bool onKeyEvent(const KeyboardEvent& keyEvent) override { return false; }
    void renderUI(RenderContext* pRenderContext, Gui::Widgets& widget) override;

private:
    struct PathPointPre1_0
    {
        float3 pos;
        float3 dir;
        float time;
    };

    ref<Scene> mpScene;
    std::string mSceneDir = "."; // WD

    PathPoint createFromCamera();
    float getTime() const;

    void saveFrame(RenderContext* pRenderContext);
    void updateCamera();

    void startRecording(); 
    void stopRecording();
    void startPreview();
    void stopPreview();
    void startRender();
    void stopRender();
    void startWarmup();
    void stopWarmup();
    void smoothPath();

    double getStartTime();

    void savePath(const std::string& filename) const;
    void loadPath(const std::string& filename);

    void refreshFileList();

    // forces to return to idle state
    void forceIdle();

    std::vector<PathPoint> mPathPoints; // original recording
    std::vector<PathPoint> mSmoothPoints; // smoothed version
    Clock* mpGlobalClock = nullptr;

    State mState = State::Idle;
    size_t mRenderIndex = 0;
    RenderGraph* mpRenderGraph;
    std::string mActiveOutput = "";
    std::set<std::string> mOutputs;
    int mFps = 60;

    std::string mSaveName = "path";
    std::vector<Gui::DropdownValue> mFileList;
    uint32_t mLoadIndex = 0;
    float mTimeScale = 1.0;
    bool mLoop = false;
    std::string mOutputFilter;
    std::string mOutputPrefixFolder = "videos";
    std::string mOutputPrefix;
    bool mDeleteDublicatesAtStartAndEnd = true;

    PathPoint mLastFramePathPoint;
    bool mLastFramePathPointValid = false;

    int guardBand = 0;
    ref<Texture> mpBlitTexture;
};
