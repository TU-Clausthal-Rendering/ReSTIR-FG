add_plugin(ReSTIR_FG_Lite)

target_sources(ReSTIR_FG_Lite PRIVATE
    ReSTIR_FG_Lite.cpp
    ReSTIR_FG_Lite.h
    
    EvaluateReservoirs.cs.slang
    ResampleReservoirFG.cs.slang
    ResampleReservoirCaustic.cs.slang
    StructsAndHelpers.slang
    GenerateInitialSamples.rt.slang
    TracePhotons.rt.slang
)

target_copy_shaders(ReSTIR_FG_Lite RenderPasses/ReSTIR_FG_Lite)
target_source_group(ReSTIR_FG_Lite "RenderPasses")

