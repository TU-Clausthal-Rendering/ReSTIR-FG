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
#include "CameraPath.h"
#include "Utils/Math/VectorMath.h"

#include <fstream>

extern "C" FALCOR_API_EXPORT void registerPlugin(Falcor::PluginRegistry& registry)
{
    registry.registerClass<RenderPass, CameraPath>();
}

CameraPath::CameraPath(ref<Device> pDevice, const Properties& props)
    : RenderPass(pDevice)
{
    mClock = Clock();
}

Properties CameraPath::getProperties() const
{
    return {};
}

RenderPassReflection CameraPath::reflect(const CompileData& compileData)
{
    //Nothing is happening here
    RenderPassReflection reflector;
    return reflector;
}

void CameraPath::setScene(RenderContext* pRenderContext, const ref<Scene>& pScene)
{
    mpScene = pScene;
    mpCamera = mpScene->getCamera();

    //Look in model path for camera path files
    std::string modelPath = mpScene->getPath().parent_path().string();     //Directory path
    for (const auto& entry : std::filesystem::directory_iterator(modelPath))
    {
        if (entry.path().extension() == ".fcp")
        {
            if (loadCameraPathFromFile(entry.path()))
                break;
        }
    }

    mClock.setTime(0); // Reset Time on scene load in
}

void CameraPath::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    if (!mpScene)
        return;
    mClock.tick();  //Update Clock

    if (mRecordCameraPath)
        recordFrame();

    //Follow the camera path
    if (mCameraPath.empty() || !mUseCameraPath)
        return;

    pathFrame();
}

void CameraPath::renderUI(Gui::Widgets& widget)
{
    //Info Block
    widget.text(mStatus);
    widget.text("Loaded Path Nodes: " + std::to_string(mCameraPath.size()));

    widget.dummy("", float2(1, 10));

    if (mRecordCameraPath)
    {
        if (widget.button("Stop Recording"))
        {
            mRecordCameraPath = false;
            mStatus = "Successfully recoreded path with " + std::to_string(mCameraPath.size()) + "nodes";
        }
           
        return;
    }

    if (mUseCameraPath)
    {
        pathUI(widget);
        return;
    }

    //Settings UI
    if(widget.button("Start Camera Path"))
        startPath();

    widget.tooltip("Starts the camera path. Number nodes need to be > 1");
    if (widget.button("Record Path", true))
        startRecording();

    if (widget.button("Store Camera Path"))
    {
        std::filesystem::path storePath;
        if (saveFileDialog(kCamPathFileFilters, storePath))
        {
            storeCameraPath(storePath);
        }
    }

    if (widget.button("Load Camera Path", true))
    {
        std::filesystem::path loadPath;
        if (openFileDialog(kCamPathFileFilters, loadPath))
        {
            loadCameraPathFromFile(loadPath);
        }
    }

    if (auto group = widget.group("Smooth Path"))
    {
        group.text("Smooth the Camera Path with a Gaussian Filter");
        group.checkbox("Smooth Target", mSmoothTarget);
        group.checkbox("Smooth Position", mSmoothPosition);
        group.var("Filter Size", mSmoothFilterSize, 1u, 1025u, 2u);
        group.slider("Sigma", mGaussSigma, 0.001f, mSmoothFilterSize / 2.f);
        if (group.button("Apply Smoothing"))
            smoothCameraPath();
        if (group.button("Load Backup"))
        {
            if (mCameraPathBackup.size() == mCameraPath.size())
            {
                mCameraPath = mCameraPathBackup;
                mStatus = "Backup loaded successfully";
            }
            else
                mStatus = "Backup loading failed";
                
        }
    }
}

void CameraPath::pathUI(Gui::Widgets& widget)
{
    widget.text("Current Node: " + std::to_string(mCurrentPathFrame));

    if (auto group = widget.group("Clock", true))
    {
        group.text("Current Time: " + std::to_string(mClock.getTime()));
        if (group.var("Speed Scale", mClockTimeScale, 0.001, 100.0, 0.001f))
        {
            mClock.setTimeScale(mClockTimeScale);
        }

        if (mClock.isPaused())
        {
            if (group.button("Resume Clock"))
            {
                mClock.play();
                mCurrentPathFrame = mCurrentPauseFrame;
                mStatus = "Following the path ...";
            }
                

            group.var("Set Node", mCurrentPathFrame, size_t(0), mCameraPath.size()-1, 1u);
        }
        else
        {
            if (group.button("Pause Clock"))
            {
                mClock.pause();
                mCurrentPauseFrame = mCurrentPathFrame;
                mStatus = "Clock paused!";
            }
                
        }
    }

    widget.dummy("", float2(1, 10));

    if (widget.button("Stop"))
        mUseCameraPath = false;
}

void CameraPath::startRecording() {
    mStatus = "Recording ...";
    mCameraPath.clear();
    mCameraPath.resize(0);
    mRecordedFrames = 0;
    // Restart Clock
    if (mClock.isPaused())
        mClock.play();
    mClock.setTime(0);      

    mRecordCameraPath = true;
}

void CameraPath::recordFrame() {
    double currentTime = mClock.getTime();

    //Create the node
    CamPathData node = {};
    node.position = mpCamera->getPosition();
    node.target = mpCamera->getTarget();
    node.deltaT = mRecordedFrames > 0 ? currentTime - mLastFrameTime : 0.0;

    mCameraPath.push_back(node);

    //Update time
    mRecordedFrames++;
    mLastFrameTime = currentTime;

}

void CameraPath::startPath() {
    if (mCameraPath.size() <= 1)
    {
        mStatus = "Camera Path empty, could not start!";
        reportError(mStatus);
        return;
    }
        
    mStatus = "Following the Path ...";
    mCurrentPathFrame = 0;
    mLastFrameTime = 0;
    mNextFrameTime = mCameraPath[0].deltaT;
    mUseCameraPath = true;
    mClock.setTime(0); // Restart Clock
}

void CameraPath::pathFrame() {
    double currentTime = mClock.getTime();

    //If the clock is paused, just return the current node
    if (mClock.isPaused())
    {
        const CamPathData& n = mCameraPath[mCurrentPathFrame];

        // Update Camera
        mpCamera->setPosition(n.position);
        mpCamera->setTarget(n.target);
        return;
    }

    size_t startFrame = mCurrentPathFrame;
    double startFrameTime = mLastFrameTime;

    //Find the current node
    for (uint i = 0; i < kMaxSearchedFrames; i++)
    {
        //Reset Path
        if (mCurrentPathFrame >= mCameraPath.size() - 1)
        {
            mCurrentPathFrame = 0;
            startFrame = 0;
            mClock.setTime(0);
            mLastFrameTime = 0.0;
            startFrameTime = 0.0;
            mNextFrameTime = mCameraPath[1].deltaT;
            break;
        }

        //Stop if time is within the node timeframe
        if (currentTime < mNextFrameTime)
            break;

        //Else update to next node pair
        mLastFrameTime = mNextFrameTime;
        mNextFrameTime += mCameraPath[mCurrentPathFrame].deltaT;
        mCurrentPathFrame++;
    }

    double dT = currentTime - startFrameTime;

    float lerpVal = dT / std::max(1e-15, mNextFrameTime - startFrameTime);
    lerpVal = std::clamp(lerpVal, 0.f, 1.f);

    //Set the Camera values to the lerp of the next two nodes
    const CamPathData& n1 = mCameraPath[startFrame];
    const CamPathData& n2 = mCameraPath[mCurrentPathFrame + 1];
    float3 position = math::lerp(n1.position, n2.position, lerpVal);
    float3 target = math::lerp(n1.target, n2.target, lerpVal);

    //Update Camera
    mpCamera->setPosition(position);
    mpCamera->setTarget(target);
}

std::string float3VecToString(float3 vec) {
    std::string out = std::to_string(vec.x) + "," + std::to_string(vec.y) + "," + std::to_string(vec.z);
    return out;
}

bool CameraPath::storeCameraPath(std::filesystem::path& path) {
    if (mCameraPath.size() <= 1)
    {
        mStatus = "Camera Path empty, could not store!";
        reportError(mStatus);
        return false;
    }
           
    std::ofstream file(path.string(), std::ios::trunc);

    if (!file)
    {
        mStatus = "Could not store file at " + path.string();
        reportError(mStatus);
        return false;
    }

    // Write into file
    file << std::fixed << std::setprecision(32);
    //Loop over all nodes
    for (size_t i = 0; i < mCameraPath.size(); i++)
    {
        auto& n = mCameraPath[i];
        std::string line = float3VecToString(n.position) + "," + float3VecToString(n.target) + "," + std::to_string(n.deltaT);
        file << line;
        file << std::endl;
    }
    file.close();

    mStatus = "File successfully stored at: " + path.string();
    return true;
}

bool CameraPath::loadCameraPathFromFile(const std::filesystem::path& path) {

    std::ifstream file(path.string());

    if (!file.is_open())
    {
        mStatus = "Could not open file";
        reportError(mStatus);
        return false;
    }

    std::vector<CamPathData> readData;

    std::string line;
    while (std::getline(file, line))
    {
        std::vector<std::string> valuesStr;
        std::stringstream ss(line);
        std::string lineValue;
        while (std::getline(ss, lineValue, ','))
        {
            valuesStr.push_back(lineValue);
        }

        if (valuesStr.size() != 7)
        {
            mStatus = "CameraPath file format error!";
            reportError(mStatus);
            return false;
        }

        //Create and fill node
        CamPathData n;
        n.position.x = std::stof(valuesStr[0]);
        n.position.y = std::stof(valuesStr[1]);
        n.position.z = std::stof(valuesStr[2]);
        n.target.x = std::stof(valuesStr[3]);
        n.target.y = std::stof(valuesStr[4]);
        n.target.z = std::stof(valuesStr[5]);
        n.deltaT = std::stod(valuesStr[6]);

        readData.push_back(n);
    }

    if (readData.size() <= 1)
    {
        mStatus = "Read Camera Path too small, ignored";
        reportError(mStatus);
        return false;
    }

    mCameraPath = readData;
    mRecordedFrames = mCameraPath.size() - 1;

    mStatus = "File with " + std::to_string(mRecordedFrames) + " nodes successfully loaded";
    return true;
}

float getCoefficient(float sigma, float x)
{
    float sigmaSquared = sigma * sigma;
    float p = -(x * x) / (2 * sigmaSquared);
    float e = std::exp(p);

    float a = std::sqrt(2 * (float)M_PI) * sigma;
    return e / a;
}

void CameraPath::smoothCameraPath() {
    if (mCameraPath.size() < kMinSmoothSize)
    {
        mStatus = "Smooth error !Path to short, needs at least " + std::to_string(kMinSmoothSize) + " nodes."; 
        reportError(mStatus);
        return;
    }

    if (!mSmoothTarget && !mSmoothPosition)
    {
        mStatus = "Smooth error! At least one smooth variable needs to be choosen!";
        reportError(mStatus);
        return;
    }

    //Create backup
    mCameraPathBackup = mCameraPath;

    //Get the gaussian weights
    uint32_t center = mSmoothFilterSize / 2;
    float sum = 0;
    std::vector<float> weights(center + 1);
    for (uint32_t i = 0; i <= center; i++)
    {
        weights[i] = getCoefficient(mGaussSigma, (float)i);
        sum += (i == 0) ? weights[i] : 2 * weights[i];
    }

    //Normalize weights
    for (uint32_t i = 0; i <= center; i++)
        weights[i] = weights[i] / sum;

    //Loop over the camera path
    const int offset = -int(center);
    std::vector<CamPathData> smoothedData;
    smoothedData.reserve(mCameraPath.size());

    for (int i = 0; i < mCameraPath.size(); i++)
    {
        CamPathData filtered;
        filtered.position = mSmoothPosition ? float3(0) : mCameraPath[i].position;
        filtered.target = mSmoothTarget ? float3(0) : mCameraPath[i].target;
        filtered.deltaT = mCameraPath[i].deltaT;
        for (int j = 0; j < int(mSmoothFilterSize); j++)
        {
            int idx = std::clamp(i + offset + j, 0, int(mCameraPath.size() - 1));
            int weightIdx = std::abs(offset + j);
            if (mSmoothPosition)
                filtered.position += mCameraPath[idx].position * weights[weightIdx];
            if (mSmoothTarget)
                filtered.target += mCameraPath[idx].target * weights[weightIdx];
        }
        smoothedData.push_back(filtered);
    }

    //Copy Vector
    mCameraPath = smoothedData;

    mStatus = "Gaussian Smoothing successfully applied";
}
