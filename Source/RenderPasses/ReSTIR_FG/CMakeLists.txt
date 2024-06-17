add_plugin(ReSTIR_FG)

target_sources(ReSTIR_FG PRIVATE
	ReSTIR_FG.cpp
    	ReSTIR_FG.h
	Shader/CausticResamplingPass.cs.slang
	Shader/CollectPhotons.rt.slang
	Shader/DirectAnalytic.cs.slang
	Shader/FinalGatherData.slang
	Shader/FinalShading.cs.slang
	Shader/GenerateFinalGatherSamples.rt.slang
	Shader/GenerateGIPathSamples.rt.slang
    	Shader/GeneratePhotons.rt.slang
	Shader/Hash.slang
	Shader/ResamplingPass.cs.slang
	Shader/Reservoir.slang
	Shader/SurfaceDataFG.slang
	Shader/TraceTransmissionDelta.rt.slang	
)

target_copy_shaders(ReSTIR_FG RenderPasses/ReSTIR_FG)

target_source_group(ReSTIR_FG "RenderPasses")
