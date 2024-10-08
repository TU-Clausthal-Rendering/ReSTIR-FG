/***************************************************************************
 # Copyright (c) 2015-22, NVIDIA CORPORATION. All rights reserved.
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
#pragma once
#include "Utils/HostDeviceShared.slangh"

BEGIN_NAMESPACE_FALCOR

enum class ToneMapperOperator : uint32_t
{
    Linear,             ///< Linear mapping
    Reinhard,           ///< Reinhard operator
    ReinhardModified,   ///< Reinhard operator with maximum white intensity
    HejiHableAlu,       ///< John Hable's ALU approximation of Jim Heji's filmic operator
    HableUc2,           ///< John Hable's filmic tone-mapping used in Uncharted 2
    Aces,               ///< Aces Filmic Tone-Mapping
};

FALCOR_ENUM_INFO(ToneMapperOperator, {
    { ToneMapperOperator::Linear, "Linear" },
    { ToneMapperOperator::Reinhard, "Reinhard" },
    { ToneMapperOperator::ReinhardModified, "ReinhardModified" },
    { ToneMapperOperator::HejiHableAlu, "HejiHableAlu" },
    { ToneMapperOperator::HableUc2, "HableUc2" },
    { ToneMapperOperator::Aces, "Aces" },
});
FALCOR_ENUM_REGISTER(ToneMapperOperator);

/** Tone mapper parameters shared between host and device.
    Make sure struct layout follows the HLSL packing rules as it is uploaded as a memory blob.
    Do not use bool's as they are 1 byte in Visual Studio, 4 bytes in HLSL.
    https://msdn.microsoft.com/en-us/library/windows/desktop/bb509632(v=vs.85).aspx
*/
struct ToneMapperParams
{
    float whiteScale;
    float whiteMaxLuminance;
    float _pad0;
    float _pad1;
    float3x4 colorTransform;
};

END_NAMESPACE_FALCOR
