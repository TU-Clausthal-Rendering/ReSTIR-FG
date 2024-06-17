add_plugin(VideoRecorder)

target_sources(VideoRecorder PRIVATE
    VideoRecorder.cpp
    VideoRecorder.h
)

target_source_group(VideoRecorder "RenderPasses")
