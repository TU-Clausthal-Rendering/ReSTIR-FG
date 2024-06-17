add_plugin(CameraPath)

target_sources(CameraPath PRIVATE
    CameraPath.cpp
    CameraPath.h
)

target_source_group(CameraPath "RenderPasses")
