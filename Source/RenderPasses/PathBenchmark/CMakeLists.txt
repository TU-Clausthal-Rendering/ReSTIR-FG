add_plugin(PathBenchmark)

target_sources(PathBenchmark PRIVATE
    PathBenchmark.cpp
    PathBenchmark.h
)

target_source_group(PathBenchmark "RenderPasses")
