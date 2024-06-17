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
#include "PathBenchmark.h"

#include <fstream>

#include "RenderGraph/RenderPassStandardFlags.h"

extern "C" FALCOR_API_EXPORT void registerPlugin(Falcor::PluginRegistry& registry)
{
    registry.registerClass<RenderPass, PathBenchmark>();
}

PathBenchmark::PathBenchmark(ref<Device> pDevice, const Properties& props) : RenderPass(pDevice)
{
    mpProfiler = mpDevice->getProfiler();
}

Properties PathBenchmark::getProperties() const
{
    return {};
}

RenderPassReflection PathBenchmark::reflect(const CompileData& compileData)
{
    // Define the required resources here
    RenderPassReflection reflector;
    // reflector.addOutput("dst");
    // reflector.addInput("src");
    return reflector;
}

void PathBenchmark::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    if (!mpProfiler->isEnabled())
        return;

    auto d = renderData.getDictionary();
    float time = (float)(double)d[kRenderPassTime];
    mLastTime = time;

    bool overwrite = false;
    if (!mTimestamps.empty() && mTimestamps.back() == time)
    {
        // overwrite entry if time did not change (stopped)
        overwrite = true;
    }
    else
    {
        if (!mTimestamps.empty() && mTimestamps.back() > time)
            reset(); // reset if times were reset

        mTimestamps.push_back(time);
    }

    const auto& events = mpProfiler->getEvents();
    for (const auto& e : events)
    {
        if (!mEnabled[e->getName()])
            continue;
        auto& vec = mTimes[e->getName()];
        if (overwrite && !vec.empty())
            vec.back() = e->getGpuTimeAverage();
        else
            vec.push_back(e->getGpuTimeAverage());

        assert(vec.size() == mTimestamps.size());
    }
}

std::vector<std::string> splitString(const std::string& input, char delimiter)
{
    std::vector<std::string> result;
    std::string token;
    size_t start = 0, end = 0;

    // Iterate through the input string
    while ((end = input.find(delimiter, start)) != std::string::npos)
    {
        // Extract substring between start and end
        token = input.substr(start, end - start);
        // Add substring to the result vector
        result.push_back(token);
        // Update start position for the next iteration
        start = end + 1;
    }

    // Add the last substring after the last delimiter
    token = input.substr(start);
    result.push_back(token);

    return result;
}

std::string getFilename(const std::string& input)
{
    size_t lastSlashPos = input.find_last_of('/');
    if (lastSlashPos == std::string::npos)
        return input;
    return input.substr(lastSlashPos + 1);
}

void PathBenchmark::renderUI(Gui::Widgets& widget)
{
    if (!mpProfiler->isEnabled())
    {
        widget.text("Profiler is disabled. Press P to enable");
        return;
    }

    widget.text("Time: " + std::to_string(mLastTime));

    const auto& events = mpProfiler->getEvents();

    auto g = widget.group("Events");
    bool selectAll = false;
    bool selectNone = false;
    bool reset = false;

    if (g.button("All"))
        selectAll = true;
    if (g.button("None", true))
        selectNone = true;
    if (g.button("Reset", true))
        reset = true;

    for (const auto& e : events)
    {
        const auto& name = e->getName();
        bool& enabled = mEnabled[name];
        if (selectAll)
            enabled = true;
        if (selectNone)
            enabled = false;
        auto shortName = getFilename(name);

        if (g.checkbox(shortName.c_str(), enabled) && enabled)
        {
            reset = true;
        }

        if (enabled && !reset)
        {
            auto& times = mTimes[name];
            auto max_time = *std::max_element(times.begin(), times.end());

            g.graph(
                "",
                [](void* user, int index)
                {
                    auto& times = *(std::vector<float>*)user;
                    return times[index];
                },
                &times, uint32_t(times.size()), 0, 0.0f, max_time, 0, 100
            );
        }
    }
    g.release();

    if (reset)
        this->reset();

    if (widget.button("Export"))
    {
        FileDialogFilterVec filters = {{"csv"}};
        std::filesystem::path path;
        if (saveFileDialog(filters, path))
            writeCsv(path.string());
    }
}

void PathBenchmark::setScene(RenderContext* pRenderContext, const ref<Scene>& pScene)
{
    if (pScene)
    {
        auto anim = pScene->getAnimationController();
        /*
        if (anim)
        {
            anim->isLooped();
            anim->getGlobalAnimationLength();
        }
        */
    }
}

void PathBenchmark::reset()
{
    mTimestamps.resize(0);
    mTimes.clear();
}

void PathBenchmark::writeCsv(const std::string& filename) const
{
    std::ofstream file(filename);
    if (!file.is_open())
    {
        logError("could not open csv");
        return;
    }

    // header
    // file << "sep=,\n";

    std::vector<const std::vector<float>*> enabledTimes;

    // titles
    file << "time";
    for (const auto& v : mTimes)
    {
        auto enabled = mEnabled.at(v.first);
        if (!enabled)
            continue;

        file << "," << getFilename(v.first);
        enabledTimes.push_back(&v.second);
    }
    file << "\n";

    // write each row
    for (size_t i = 0; i < mTimestamps.size(); ++i)
    {
        file << mTimestamps[i];
        for (const auto& pVec : enabledTimes)
        {
            file << "," << (*pVec)[i];
        }
        file << "\n";
    }

    file.close();
}
