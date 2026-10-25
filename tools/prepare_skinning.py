"""Adapt the installed O3DE skinning shader without modifying the SDK.

Preserves its SRG ABI and native compute/morph passes. The byte-address stride
and per-influence inverse-transpose normals match Jester's CPU reference.
"""
import argparse
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--engine-root', type=Path, default=Path('D:/O3DE'))
args = parser.parse_args()
source = args.engine_root / 'Gems/Atom/Feature/Common/Assets/Shaders/SkinnedMesh'
destination = Path(__file__).resolve().parents[1] / 'Assets/Shaders/SkinnedMesh'
destination.mkdir(parents=True, exist_ok=True)
text = (source / 'LinearSkinningCS.azsl').read_text()
text = text.replace('#include "LinearSkinningPassSRG.azsli"',
    '#include <Atom/Feature/Common/Assets/Shaders/SkinnedMesh/LinearSkinningPassSRG.azsli>')
assert 'jointIdStartOffsetForVertex + influenceIndex)' in text
text = text.replace('jointIdStartOffsetForVertex + influenceIndex)',
                    'jointIdStartOffsetForVertex + influenceIndex * 2)')
start = text.index('void SkinVertexLinear(')
end = text.index('\nvoid AddWeightedDualQuaternion', start)
text = text[:start] + '''// Jester: eight influences, CPU-reference normal and orthonormal TBN.
float3x3 JesterNormalMatrix(float3x3 skinTransform)
{
    // Global bone transforms can contain shear when a rotated child inherits
    // non-uniform scale. The SDK fast helper assumes orthogonal columns.
    float3 a=skinTransform[0], b=skinTransform[1], c=skinTransform[2];
    float3x3 cofactors=float3x3(cross(b,c),cross(c,a),cross(a,b));
    return cofactors / dot(a,cross(b,c));
}
void SkinVertexLinear(uint vertexIndex, inout float3 position, inout float3 normal, inout float4 tangent, inout float3 bitangent)
{
    float3 p = 0, n = 0, t = 0, b = 0;
    float totalWeight = 0;
    uint weightOffset = vertexIndex * InstanceSrg::m_numInfluencesPerVertex;
    uint jointOffset = weightOffset * 2;
    [loop] for(uint i = 0; i < InstanceSrg::m_numInfluencesPerVertex; i+=2)
    {
        float2 weights;
        uint2 joints;
        GetInfluences(weightOffset, jointOffset, i, weights, joints);
        [unroll] for(uint k=0; k<2; ++k)
        {
            if(weights[k] > 0)
            {
                float3x4 skinMatrix = InstanceSrg::m_boneTransformsLinear[joints[k]];
                p += mul(skinMatrix, float4(position, 1)) * weights[k];
                n += mul(JesterNormalMatrix((float3x3)skinMatrix), normal) * weights[k];
                t += mul((float3x3)skinMatrix, tangent.xyz) * weights[k];
                b += mul((float3x3)skinMatrix, bitangent) * weights[k];
                totalWeight += weights[k];
            }
        }
    }
    position = totalWeight > 0 ? p / totalWeight : position;
    normal = dot(n,n) > 1e-12 ? normalize(n) : float3(0,1,0);
    t -= normal * dot(normal,t);
    if(dot(t,t) < 1e-12) t = cross(normal,abs(normal.y)<.9 ? float3(0,1,0) : float3(1,0,0));
    tangent.xyz = normalize(t);
    tangent.w = dot(cross(normal,tangent.xyz),b)<0 ? -1 : 1;
    bitangent = cross(normal,tangent.xyz)*tangent.w;
}
''' + text[end:]
(destination / 'LinearSkinningCS.azsl').write_text(text, newline='\n')
(destination / 'LinearSkinningCS.shader').write_bytes((source / 'LinearSkinningCS.shader').read_bytes())
morph_source = source.parent / 'MorphTargets'
morph_destination = destination.parent / 'MorphTargets'
morph_destination.mkdir(exist_ok=True)
morph = (morph_source / 'MorphTargetCS.azsl').read_text()
morph = morph.replace('#include "MorphTargetSRG.azsli"',
    '#include <Atom/Feature/Common/Assets/Shaders/MorphTargets/MorphTargetSRG.azsli>')
helper = '''
// Jester tagged half-float TBN payload; untagged SDK inputs retain their format.
float JesterHalf(uint value)
{
    uint mantissa=value & 1023;
    uint exponent=(value >> 10) & 31;
    uint sign=(value & 32768) << 16;
    if(exponent==0) return (sign!=0 ? -1.0 : 1.0)*float(mantissa)*(1.0/16777216.0);
    return asfloat(sign | ((exponent+112)<<23) | (mantissa<<13));
}
float3 JesterNormalDelta(MorphTargetDelta delta)
{
    return float3(JesterHalf(delta.m_compressedPositionDeltaZNormalDeltaXY & 65535),
                  JesterHalf(delta.m_compressedNormalDeltaZTangentDelta >> 16),
                  JesterHalf(delta.m_compressedNormalDeltaZTangentDelta & 65535));
}
float3 JesterTangentDelta(MorphTargetDelta delta)
{
    return float3(JesterHalf(delta.m_compressedPadBitangentDeltaXYZ >> 16),
                  JesterHalf(delta.m_compressedPadBitangentDeltaXYZ & 65535),
                  JesterHalf(delta.m_pad.x >> 16));
}
float3 JesterBitangentDelta(MorphTargetDelta delta)
{
    return float3(JesterHalf(delta.m_pad.x & 65535),JesterHalf(delta.m_pad.y >> 16),JesterHalf(delta.m_pad.y & 65535));
}
'''
morph = morph.replace('rootconstant float s_min;', helper+'\nrootconstant float s_min;')
for name in ['Normal','Tangent','Bitangent']:
    morph = morph.replace(f'DecodeTBNDelta(compressed{name}Delta)',
        f'(delta.m_pad.z==0x4a535448 ? Jester{name}Delta(delta) : DecodeTBNDelta(compressed{name}Delta))')
(morph_destination / 'MorphTargetCS.azsl').write_text(morph, newline='\n')
(morph_destination / 'MorphTargetCS.shader').write_bytes((morph_source / 'MorphTargetCS.shader').read_bytes())
print('Prepared project skinning and tagged morph shaders (SDK unchanged).')
