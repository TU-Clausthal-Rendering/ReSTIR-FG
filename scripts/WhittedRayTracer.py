from pathlib import WindowsPath, PosixPath
from falcor import *

def render_graph_DefaultRenderGraph():
    g = RenderGraph('DefaultRenderGraph')
    g.create_pass('WhittedRayTracer', 'WhittedRayTracer', {'maxBounces': 3, 'texLODMode': 'Mip0', 'rayConeMode': 'Combo', 'rayConeFilterMode': 'Isotropic', 'rayDiffFilterMode': 'Isotropic', 'useRoughnessToVariance': False})
    g.create_pass('GBufferRT', 'GBufferRT', {'outputSize': 'Default', 'samplePattern': 'Center', 'sampleCount': 16, 'useAlphaTest': True, 'adjustShadingNormals': True, 'forceCullMode': False, 'cull': 'Back', 'texLOD': 'Mip0', 'useTraceRayInline': False, 'useDOF': True})
    g.create_pass('AccumulatePass', 'AccumulatePass', {'enabled': True, 'outputSize': 'Default', 'autoReset': True, 'precisionMode': 'Single', 'maxFrameCount': 0, 'overflowMode': 'Stop'})
    g.create_pass('ToneMapper', 'ToneMapper', {'outputSize': 'Default', 'useSceneMetadata': True, 'exposureCompensation': 0.0, 'autoExposure': False, 'filmSpeed': 100.0, 'whiteBalance': False, 'whitePoint': 6500.0, 'operator': 'Aces', 'clamp': True, 'whiteMaxLuminance': 1.0, 'whiteScale': 11.199999809265137, 'fNumber': 1.0, 'shutter': 1.0, 'exposureMode': 'AperturePriority'})
    g.add_edge('GBufferRT.posW', 'WhittedRayTracer.posW')
    g.add_edge('GBufferRT.normW', 'WhittedRayTracer.normalW')
    g.add_edge('GBufferRT.tangentW', 'WhittedRayTracer.tangentW')
    g.add_edge('GBufferRT.faceNormalW', 'WhittedRayTracer.faceNormalW')
    g.add_edge('GBufferRT.texC', 'WhittedRayTracer.texC')
    g.add_edge('GBufferRT.texGrads', 'WhittedRayTracer.texGrads')
    g.add_edge('GBufferRT.mtlData', 'WhittedRayTracer.mtlData')
    g.add_edge('WhittedRayTracer.color', 'AccumulatePass.input')
    g.add_edge('AccumulatePass.output', 'ToneMapper.src')
    g.add_edge('GBufferRT.vbuffer', 'WhittedRayTracer.vbuffer')
    g.mark_output('ToneMapper.dst')
    return g

DefaultRenderGraph = render_graph_DefaultRenderGraph()
try: m.addGraph(DefaultRenderGraph)
except NameError: None
