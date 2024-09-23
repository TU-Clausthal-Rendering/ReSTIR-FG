if(NOT (FALCOR_HAS_NRD AND FALCOR_HAS_D3D12))
    return()
endif()

add_plugin(NRDPass)

target_sources(NRDPass PRIVATE
    NRDPassBase.cpp
    NRDPassBase.h
    NRDPasses/NRDPassNormal.cpp
    NRDPasses/NRDPassNormal.h
	NRDPasses/NRDPassOcclusion.cpp
    NRDPasses/NRDPassOcclusion.h
	NRDPasses/NRDPassShadow.cpp
    NRDPasses/NRDPassShadow.h
    PackRadiance.cs.slang
)

target_link_libraries(NRDPass PRIVATE nrd)

target_copy_shaders(NRDPass RenderPasses/NRDPass)

target_source_group(NRDPass "RenderPasses")
