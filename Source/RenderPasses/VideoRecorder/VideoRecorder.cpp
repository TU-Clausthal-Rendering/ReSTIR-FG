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
#include "VideoRecorder.h"

#include "RenderGraph/RenderGraph.h"
#include "RenderGraph/RenderPassStandardFlags.h"

#include <cstdio>
#include <fstream>

namespace{
    const std::string kVersionControlHeader = "VideoRecorderVersion1_0";
}

extern "C" FALCOR_API_EXPORT void registerPlugin(Falcor::PluginRegistry& registry)
{
    registry.registerClass<RenderPass, VideoRecorder>();
}

VideoRecorder::VideoRecorder(ref<Device> pDevice, const Properties& props)
    : RenderPass(pDevice)
{
    refreshFileList();
}

Properties VideoRecorder::getProperties() const
{
    return {};
}

RenderPassReflection VideoRecorder::reflect(const CompileData& compileData)
{
    // Define the required resources here
    RenderPassReflection reflector;
    //reflector.addOutput("dst");
    //reflector.addInput("src");
    return reflector;
}

void VideoRecorder::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    auto renderDict = renderData.getDictionary();
            
    // set render graph
    auto pRenderGraph = (RenderGraph*)renderDict[kRenderGraph];
    if(mpRenderGraph != pRenderGraph)
    {
        // clear old outputs and add the primary output as default target
        mOutputs.clear();
        if(pRenderGraph && pRenderGraph->getOutputCount() > 0) mOutputs.insert(pRenderGraph->getOutputName(0));
    }
    mpRenderGraph = pRenderGraph;

    //set GlobalClock
    mpGlobalClock = static_cast<Clock*>(renderDict[kRenderGlobalClock]);

    // set guard band
    guardBand = renderDict.getValue("guardBand", 0);
}

// helper for fuzzy string matching
int levenshteinDistance(const std::string& s1, const std::string& s2) {
    const int len1 = s1.length() + 1;
    const int len2 = s2.length() + 1;

    std::vector<std::vector<int>> dp(len1, std::vector<int>(len2, 0));

    for (int i = 0; i < len1; ++i) {
        for (int j = 0; j < len2; ++j) {
            if (i == 0) {
                dp[i][j] = j;
            }
            else if (j == 0) {
                dp[i][j] = i;
            }
            else {
                dp[i][j] = std::min({ dp[i - 1][j] + 1, dp[i][j - 1] + 1, dp[i - 1][j - 1] + (s1[i - 1] == s2[j - 1] ? 0 : 1) });
            }
        }
    }

    return dp[len1 - 1][len2 - 1];
}

int fuzzyRatio(const std::string& s1, const std::string& s2) {
    const int distance = levenshteinDistance(s1, s2);
    const int maxLength = std::max(s1.length(), s2.length());

    if (maxLength == 0) {
        return 100;  // Both strings are empty, consider them a perfect match
    }
    else {
        return static_cast<int>(100.0f * (1.0f - static_cast<float>(distance) / maxLength));
    }
}

std::vector<std::string> fuzzyFilter(const std::vector<std::string>& strings, std::string filter)
{
    // convert to lower
    std::transform(filter.begin(), filter.end(), filter.begin(), [](unsigned char c) { return std::tolower(c); });

    std::vector<std::pair<int, std::string>> tmp;
    for (const auto& s : strings)
    {
        auto slower = s;
        std::transform(slower.begin(), slower.end(), slower.begin(), [](unsigned char c) { return std::tolower(c); });
        tmp.push_back({ fuzzyRatio(slower, filter), s });
    }

    // sort result based on first element of pair (fuzzy ratio)
    std::sort(tmp.begin(), tmp.end(), [](const auto& a, const auto& b) { return a.first > b.first; });

    // remove fuzzy ratio from result
    std::vector<std::string> result;
    for(auto& e : tmp)
    {
        if (e.first <= 0) break; // no more matches
        result.push_back(move(e.second));
    }

    return result;
}

void VideoRecorder::renderUI(RenderContext* pRenderContext, Gui::Widgets& widget)
{
    widget.text("Path Points: " + std::to_string(mPathPoints.size()));

    saveFrame(pRenderContext);

    widget.checkbox("Record: Delete dublicate Points at Start and End", mDeleteDublicatesAtStartAndEnd);
    if (mState == State::Record)
    {
        if (widget.button("Record Stop"))
        {
            stopRecording();
        }
    }
    else // (mState == State::Idle)
    {
        if (widget.button("Record Start") && mState == State::Idle)
        {
            startRecording();
        }
    }
    
    if(mState == State::Preview)
    {
        if(widget.button("Preview Stop"))
        {
            stopPreview();
        }
        //widget.slider("", mRenderIndex, size_t(0), mPathPoints.size() - 1, true);
    }
    else // (mState == State::Idle)
    {
        if(widget.button("Preview Start") && mPathPoints.size() && mState == State::Idle)
        {
            mLastFramePathPointValid = false;
            startPreview();
        }
    }

    if(widget.checkbox("Loop", mLoop, true))
    {
        if (mState == State::Idle && mPathPoints.size()) startPreview();
    }

    if(mState == State::Render || mState == State::Warmup)
    {
        if (widget.button("Render Stop"))
        {
            forceIdle();
        }
    }
    else
    {
        if(widget.button("Render Start") && mPathPoints.size() && mState == State::Idle && mOutputs.size())
        {
            //startRender();
            mLastFramePathPointValid = false;
            startWarmup();
        }
        if (mOutputs.empty())
            widget.tooltip("No outputs selected. Nothing will be saved to file!");
        else
            widget.tooltip("FFMPEG is needed. Put in \"build/[buildname]/Source/Mogwai\"");
    }
    widget.textbox("Folder Prefix", mOutputPrefixFolder);
    widget.tooltip("Leave empty if no folder is desired");
    widget.textbox("Filename Prefix", mOutputPrefix);

    widget.var("FPS", mFps, 1, 240);

    widget.var("Time Scale", mTimeScale, 0.01f, 100.0f, 0.1f);

    if(widget.button("Smooth Path") && mPathPoints.size() > 1 && mState != State::Record)
    {
        smoothPath();
    }
    if(widget.button("Reset", true))
    {
        mSmoothPoints.clear();
    }
    if (mSmoothPoints.size()) widget.text("Active!", true);
    else widget.text("Not used", true);

    // file IO
    widget.textbox("S:", mSaveName);
    if(widget.button("Save", true) && mPathPoints.size())
    {
        savePath(mSceneDir + "/" + mSaveName + ".campath");
        refreshFileList();
    }

    if(mFileList.size())
    {
        widget.dropdown("L:", mFileList, mLoadIndex);
        if (widget.button("Load", true) && mLoadIndex < mFileList.size())
        {
            loadPath(mSceneDir + "/" + mFileList[mLoadIndex].label + ".campath");
        }
    }

    if(widget.button("Output Directory"))
    {
        system("explorer .");
    }

    // list all outputs
    if(auto g = widget.group("Outputs"))
    {
        bool selectAll = false;

        if (g.button("All")) selectAll = true;
        if (g.button("None", true))
        {
            mOutputs.clear();
        }

        auto allOutputs = mpRenderGraph->getAvailableOutputs();
        g.textbox("Filter", mOutputFilter);
        if(mOutputFilter.size())
        {
            allOutputs = fuzzyFilter(allOutputs, mOutputFilter);
        }

        for(const auto& name : allOutputs)
        {
            bool selected = mOutputs.count(name) != 0;
            if(g.checkbox(name.c_str(), selected))
            {
                if (selected)
                {
                    mOutputs.insert(name);
                    mpRenderGraph->markOutput(name);
                }
                else mOutputs.erase(name);
            }
            if(selectAll) mOutputs.insert(name);
        }
    }

    

    // logic
    updateCamera();
}

void VideoRecorder::setScene(RenderContext* pRenderContext, const ref<Scene>& pScene)
{
    mpScene = pScene;
    if(mpScene) mSceneDir = mpScene->getPath().parent_path().string();
    else mSceneDir = ".";

    refreshFileList();
}

PathPoint VideoRecorder::createFromCamera()
{
    assert(mpScene);
    auto cam = mpScene->getCamera();
    PathPoint p;
    p.pos = cam->getPosition();
    p.dir = normalize(cam->getTarget() - p.pos);
    p.up = cam->getUpVector();
    p.time = (float)mpGlobalClock->getTime();

    return p;
}

float VideoRecorder::getTime() const
{
    return static_cast<float>(mpGlobalClock->getTime()) * mTimeScale;
}

namespace fs = std::filesystem;

// file helper functions
bool folderExists(const std::string& folderPath) {
    return fs::is_directory(folderPath);
}

bool createFolder(const std::string& folderPath) {
    try {
        fs::create_directories(folderPath);
        return true;
    }
    catch (const std::exception&) {
        return false;
    }
}

bool deleteFolder(const std::string& folderPath) {
    try {
        fs::remove_all(folderPath);
        return true;
    }
    catch (const std::exception&) {
        return false;
    }
}

void deleteFile(const std::string& filePath) {
    try {
        if (fs::exists(filePath) && fs::is_regular_file(filePath)) {
            fs::remove(filePath);
        }
    }
    catch (const std::exception&) { }
}

void VideoRecorder::saveFrame(RenderContext* pRenderContext)
{
    if (mState != State::Render) return;
    assert(mpRenderGraph);
    mRenderIndex++;

    for(const auto& target : mOutputs)
    {
        auto output = mpRenderGraph->getOutput(target);
        if(!output) continue;

        auto tex = output->asTexture();
        assert(tex);
        if(!tex) continue;

        const auto& outputName = output->getName();

        if(mRenderIndex <= 1) // replay index 1 == first frame, because this function is called after the camera update
        {
            // delete old content in the tmp output folder
            deleteFolder(outputName);
            createFolder(outputName);
        }

        auto filenameBase = outputName + "/frame" + outputName;
        std::stringstream filename;
        filename << filenameBase << std::setfill('0') << std::setw(4) << mRenderIndex << ".bmp";

        // blit texture
        uint4 srcRect = uint4(guardBand, guardBand, tex->getWidth() - guardBand, tex->getHeight() - guardBand);
        if(!mpBlitTexture ||
            mpBlitTexture->getWidth() != tex->getWidth() - 2 * guardBand ||
            mpBlitTexture->getHeight() != tex->getHeight() - 2 * guardBand)
        {
            mpBlitTexture = Texture::create2D(
                mpDevice, tex->getWidth() - 2 * guardBand, tex->getHeight() - 2 * guardBand, ResourceFormat::BGRA8UnormSrgb, 1, 1, nullptr, ResourceBindFlags::RenderTarget | ResourceBindFlags::ShaderResource);
        }

        pRenderContext->blit(tex->getSRV(), mpBlitTexture->getRTV(), srcRect);

        //tex->captureToFile(0, 0, filename.str(), Bitmap::FileFormat::BmpFile);
        mpBlitTexture->captureToFile(0, 0, filename.str(), Bitmap::FileFormat::BmpFile);
    }
}

void VideoRecorder::updateCamera()
{
    if (!mpScene) return;

    float time = getTime();
    auto cam = mpScene->getCamera();

    // helper function to get the interpolated path point based on time
    auto getInterpolatedPathPoint = [&](float time)
    {
        assert(mPathPoints.size());
        const auto& path = mSmoothPoints.empty() ? mPathPoints : mSmoothPoints;

        auto step2 = std::find_if(path.begin(), path.end(), [time](const PathPoint& p) { return p.time >= time; });
        if (step2 == path.end())
        {
            return path.back();
        }

        auto step1 = step2;
        if (step1 != path.begin()) --step1; // move to previous step

        // interpolate position
        float t = (time - step1->time) / (step2->time - step1->time);
        if (step1->time >= step2->time) t = 1.0; // in case step1 == step2

        PathPoint res;
        res.time = time;
        res.pos = lerp(step1->pos, step2->pos, t);
        res.dir = lerp(step1->dir, step2->dir, t);
        res.up = normalize(lerp(step1->up, step2->up, t));
        return res;
    };

    auto updateCamera = [&](const PathPoint& curr) {
        bool updatePoint = false;
        if (!mLastFramePathPointValid)
        {
            mLastFramePathPoint = curr;
            mLastFramePathPointValid = true;
            updatePoint = true;
        }
            
        const float error = 0.00001f;

        updatePoint |= any(abs(mLastFramePathPoint.pos - curr.pos) > error);
        auto lastTarget = mLastFramePathPoint.pos + mLastFramePathPoint.dir;
        auto currentTarget = curr.pos + curr.dir;
        updatePoint |= any(abs(lastTarget - currentTarget) > error);
        updatePoint |= any(abs(mLastFramePathPoint.pos - curr.pos) > error);

        if (updatePoint)
            mLastFramePathPoint = curr;

        return updatePoint;
    };

    switch (mState)
    {
    case State::Record:
        mPathPoints.push_back(createFromCamera());
        break;
    case State::Preview:
    {
        auto p = getInterpolatedPathPoint(time);
        if (updateCamera(p))
        {
            cam->setPosition(p.pos);
            cam->setTarget(p.pos + p.dir);
            cam->setUpVector(p.up);
        }
        
        if(p.time >= mPathPoints.back().time)
        {
            if(mLoop)
            {
                mpGlobalClock->setTime(getStartTime());
            }
            else // stop animation
                stopPreview();
        }
    }  break;

    case State::Render:
    {
        auto p = getInterpolatedPathPoint(time);
        if (updateCamera(p))
        {
            cam->setPosition(p.pos);
            cam->setTarget(p.pos + p.dir);
            cam->setUpVector(p.up);
        }
        
        if (p.time >= mPathPoints.back().time)
        {
            // stop animation
            stopRender();
        }
    }  break;

    case State::Warmup:
    {
        size_t warmupFrames = 120;
        float t = 1.0f - (float)mRenderIndex / (float)warmupFrames;
        auto p = getInterpolatedPathPoint(t); // fix some issues with temporal passes 
        if (updateCamera(p))
        {
            cam->setPosition(p.pos);
            cam->setTarget(p.pos + p.dir);
            cam->setUpVector(p.up);
        }

        if(mRenderIndex++ > warmupFrames)
        {
            startRender(); // after 100 warmup frames, start rendering
        }
    }  break;

    }
}

void VideoRecorder::startRecording()
{
    assert(mState == State::Idle);
    if(mState != State::Idle) return;

    mState = State::Record;
    mPathPoints.clear();
    mSmoothPoints.clear();
    mpGlobalClock->play();
}

void VideoRecorder::startPreview()
{
    assert(mState == State::Idle);
    if(mState != State::Idle) return;

    mState = State::Preview;
    mpGlobalClock->play();
    mpGlobalClock->setTime(getStartTime());
    mpScene->getCamera()->setIsAnimated(false); //Disable camera animations
   
}

void VideoRecorder::startRender()
{
    assert(mState != State::Record);
    if(mState == State::Record) return;

    mState = State::Render;
    mpGlobalClock->play();
    mpGlobalClock->setFramerate(mFps);
    
    mRenderIndex = 0;
}

void VideoRecorder::startWarmup()
{
    assert(mState != State::Record);
    if (mState == State::Record) return;

    mState = State::Warmup;
    mpScene->getCamera()->setIsAnimated(false); // Disable camera animations
    mpGlobalClock->setTime(getStartTime());
    mpGlobalClock->pause();
    mRenderIndex = 0;
}



void VideoRecorder::stopRecording()
{
    assert(mState == State::Record);
    if(mState != State::Record) return;

    mState = State::Idle;
    // remove duplicate points from start and end
    if (mDeleteDublicatesAtStartAndEnd)
    {
        auto newStart = std::find_if(
            mPathPoints.begin(), mPathPoints.end(), [&](const PathPoint& p) { return any(p.pos != mPathPoints.begin()->pos); }
        );
        if (newStart != mPathPoints.end())
        {
            if (newStart != mPathPoints.begin())
                --newStart;
            auto newEnd =
                std::find_if(
                    mPathPoints.rbegin(), mPathPoints.rend(), [&](const PathPoint& p) { return any(p.pos != mPathPoints.rbegin()->pos); }
                ).base();
            if (newEnd != mPathPoints.end())
                ++newEnd;
            mPathPoints = std::vector<PathPoint>(newStart, newEnd);
        }
    }
    
}

void VideoRecorder::stopPreview()
{
    assert(mState == State::Preview);
    if (mState != State::Preview) return;

    mState = State::Idle;
}

void VideoRecorder::stopRender()
{
    assert(mState == State::Render);
    if(mState != State::Render) return;

    mState = State::Idle;

    mpGlobalClock->setFramerate(0); //Reset framerate simulation

    // create video files for each output
    for (const auto& target : mOutputs)
    {
        auto output = mpRenderGraph->getOutput(target);
        if (!output) continue;

        auto tex = output->asTexture();
        assert(tex);
        if (!tex) continue;

        const auto& outputName = output->getName();

        auto filenameBase = outputName + "/frame" + outputName;
        char buffer[2048];

        std::string outputFilename;
        if (!mOutputPrefixFolder.empty())
        {
            if (!folderExists(mOutputPrefixFolder))
                createFolder(mOutputPrefixFolder);
            outputFilename = mOutputPrefixFolder + "/" + mOutputPrefix + outputName + ".mp4";
        }
        else
            outputFilename = mOutputPrefix + outputName + ".mp4";

        deleteFile(outputFilename); // delete old file (otherwise ffmpeg will not write anything)
        sprintf_s(buffer, "ffmpeg -r %d -i %s%%04d.bmp -c:v libx264 -preset medium -crf 12 -vf \"fps=%d,format=yuv420p\" \"%s\" 2>&1", mFps, filenameBase.c_str(), mFps, outputFilename.c_str());

        // last frame, convert to video
        FILE* ffmpeg = _popen(buffer, "w");
        if (!ffmpeg)
        {
            logError("Cannot use popen to execute ffmpeg!. Put ffmpeg in \"build/[buildname]/Source/Mogwai\"");
            continue;
        }

        auto err = _pclose(ffmpeg);
        deleteFolder(outputName); // delete the temporary files
        if (err)
        {
            logError("Error while executing ffmpeg:\n");
        }
    }
}

void VideoRecorder::stopWarmup()
{
    // stop warmup and transition to render
    assert(mState == State::Warmup);
    startRender();
}


void VideoRecorder::smoothPath()
{
    if(mPathPoints.size() < 2) return;

    mSmoothPoints.clear();
    // apply gaussian blur to path
    mSmoothPoints.resize(mPathPoints.size());

    const float timeRadius = 0.5f * mTimeScale; // 0.5 seconds

    for(size_t i = 0; i < mPathPoints.size(); ++i)
    {
        const auto& p = mPathPoints[i];
        auto& sp = mSmoothPoints[i];
        sp = p; // initialize with p
        float wsum = 1.0; // weight sum

        auto addPoint = [&](const PathPoint& p)
        {
            float w = expf(-powf(p.time - sp.time, 2) / (2 * powf(timeRadius, 2)));
            sp.pos += w * p.pos;
            sp.dir += w * p.dir;
            wsum += w;
        };

        for(int j = int(i - 1); j >= 0 && mPathPoints[j].time > p.time - timeRadius; --j)
        {
            addPoint(mPathPoints[j]);
        }
        for(int j = int(i + 1); j < int(mPathPoints.size()) && mPathPoints[j].time < p.time + timeRadius; ++j)
        {
            addPoint(mPathPoints[j]);
        }

        // normalize
        sp.pos /= wsum;
        sp.dir /= wsum;
    }
}

void VideoRecorder::savePath(const std::string& filename) const
{
    const auto& path = mSmoothPoints.empty() ? mPathPoints : mSmoothPoints;

    // open binary file with fstream
    std::ofstream outFile(filename, std::ios::binary);
    if (outFile.is_open())
    {
        //Write header
        outFile.write(kVersionControlHeader.c_str(), kVersionControlHeader.size());
        for (const auto& point : path) {
            outFile.write(reinterpret_cast<const char*>(&point), sizeof(PathPoint));
        }
        outFile.close();
    }
}

void VideoRecorder::loadPath(const std::string& filename)
{
    forceIdle();

    //Convert old format to the new one that includes the up vector
    auto convertPathPointFormat = [](PathPointPre1_0& oldPoint) {
        PathPoint p;
        p.dir = oldPoint.dir;
        p.pos = oldPoint.pos;
        p.time = oldPoint.time;
        p.up = float3(0, 1, 0);
        return p;
    };

    mPathPoints.clear();
    std::ifstream inFile(filename, std::ios::binary);
    if (inFile.is_open())
    {
        bool isPre1_0 = false;  //If pre 1_0, it has no up vector
        //Read header
        std::vector<char> headerChecker(kVersionControlHeader.size());
        if (inFile.read(headerChecker.data(), kVersionControlHeader.size()))
        {
            for (uint32_t i = 0; i < headerChecker.size(); i++)
            {
                if (headerChecker[i] != kVersionControlHeader[i])
                {
                    isPre1_0 = true;
                    //Return to start of file
                    inFile.clear();
                    inFile.seekg(0);
                    logInfo("Old path file format detected. Will be converted!");
                    break;
                }
            }
        }

        if (isPre1_0)
        {
            PathPointPre1_0 point;
            while (inFile.read(reinterpret_cast<char*>(&point), sizeof(PathPointPre1_0)))
            {
                mPathPoints.push_back(convertPathPointFormat(point));
            }
        }
        else
        {
            PathPoint point;
            while (inFile.read(reinterpret_cast<char*>(&point), sizeof(PathPoint)))
            {
                mPathPoints.push_back(point);
            }
        }

        inFile.close();
    }
    else logError("Cannot open camera path file!");
}

void VideoRecorder::refreshFileList()
{
    mFileList.clear();
    Gui::DropdownValue v;
    v.value = 0;
    for (const auto& entry : std::filesystem::directory_iterator(mSceneDir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".campath") {
            v.label = entry.path().filename().replace_extension().string();
            mFileList.push_back(v);
            ++v.value;
        }
    }
}

void VideoRecorder::forceIdle()
{
    switch(mState)
    {
    case State::Record:
        stopRecording();
        break;
    case State::Preview:
        stopPreview();
        break;
    case State::Render:
        //stopRender();
        //break;
    case State::Warmup:
        mState = State::Idle;
        break;
    }

    assert(mState == State::Idle);
}

double VideoRecorder::getStartTime()
{
    if (mPathPoints.size() > 0)
        return static_cast<double>(mPathPoints[0].time);
    else
        return 0.0;
}
