"""Vendor the SDK PBR extension points with per-map glTF UVs and samplers.

Run when upgrading the SDK, then review the generated shader changes.
O3DE source retains its Apache-2.0 OR MIT notices.
"""
import json
import argparse
from pathlib import Path

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--engine-root', type=Path, default=Path('D:/O3DE'))
args = parser.parse_args()
sdk = args.engine_root / 'Gems/Atom/Feature/Common/Assets'
destination = root / 'Assets/Materials/Types'
destination.mkdir(parents=True, exist_ok=True)
shader_root = 'Atom/Feature/Common/Assets/Shaders/Materials/'

material = json.loads((sdk / 'Materials/Types/StandardPBR.materialtype').read_text())
for group in material['propertyLayout']['propertyGroups']:
    source = group['$import']
    target = destination / source
    target.parent.mkdir(exist_ok=True)
    target.write_bytes((sdk / 'Materials/Types' / source).read_bytes())
properties = []
for name in ('Base', 'Normal', 'MetallicRoughness', 'Occlusion', 'Emissive'):
    for row, default in ((0, [1, 0, 0, 0]), (1, [0, 1, 0, 0])):
        properties.append(dict(name=f'{name}Row{row}', type='Vector4', defaultValue=default,
            connection=dict(type='ShaderInput', name=f'm_jester{name}Row{row}')))
    properties.append(dict(name=f'{name}Sampler', type='SamplerState', defaultValue={
        'm_filterMin': 'Linear', 'm_filterMag': 'Linear', 'm_filterMip': 'Linear',
        'm_addressU': 'Wrap', 'm_addressV': 'Wrap'},
        connection=dict(type='ShaderInput', name=f'm_jester{name}Sampler')))
properties.append(dict(name='alphaFactor', type='Float', defaultValue=1,
    connection=dict(type='ShaderInput', name='m_jesterAlphaFactor')))
properties.append(dict(name='unlit', type='Bool', defaultValue=False,
    connection=dict(type='ShaderInput', name='m_jesterUnlit')))
material['propertyLayout']['propertyGroups'].append(dict(name='jester', properties=properties))
material['description'] = 'Jester imported PBR: independent glTF texture transforms and samplers.'
material['materialShaderCode'] = 'JesterPBR.azsli'
material['materialShaderDefines'] = 'JesterPBR_Defines.azsli'
(destination / 'JesterPBR.materialtype').write_text(json.dumps(material, indent=4) + '\n')
(destination / 'JesterPBR_Defines.azsli').write_bytes((sdk / 'Materials/Types/StandardPBR_Defines.azsli').read_bytes())

def read_shader(path):
    text = (sdk / 'Shaders/Materials' / path).read_text()
    # The vendored files live together; unchanged dependencies remain in the SDK.
    parent = Path(path).parent
    import re
    def absolute_include(match):
        import posixpath
        return '#include <' + shader_root + posixpath.normpath(str(parent / match[1]).replace('\\', '/')) + '>'
    return re.sub(r'#include "([^"]+)"', absolute_include, text)

maps = dict(baseColor='Base', normal='Normal', metallic='MetallicRoughness',
            roughness='MetallicRoughness', diffuseOcclusion='Occlusion', emissive='Emissive')
def adapt_maps(text):
    for source, target in maps.items():
        uv = f'uvs[params.m_{source}MapUvIndex]'
        text = text.replace(uv, f'JesterUv({uv}, params.m_jester{target}Row0, params.m_jester{target}Row1)')
        text = text.replace(f'GetMaterialTexture(params.m_{source}Map), GetMaterialTextureSampler()',
            f'GetMaterialTexture(params.m_{source}Map), GetMaterialTextureSampler(params.m_jester{target}Sampler)')
    return text

base = adapt_maps(read_shader('BasePBR/BasePBR_SurfaceEval.azsli'))
start = base.index('    real3x3 uvMatrix =')
end = base.index('\n', start)
base = base[:start] + '    real3x3 uvMatrix = real3x3(params.m_jesterNormalRow0.xyz, params.m_jesterNormalRow1.xyz, float3(0,0,1));' + base[end:]
(destination / 'JesterBaseSurface.azsli').write_text(base)
surface = adapt_maps(read_shader('StandardPBR/StandardPBR_SurfaceEval.azsli'))
surface = surface.replace(f'<{shader_root}BasePBR/BasePBR_SurfaceEval.azsli>', '"JesterBaseSurface.azsli"')
surface = surface.replace(f'<{shader_root}MaterialFunctions/StandardGetAlphaAndClip.azsli>', '"JesterAlpha.azsli"')
surface = surface.replace('    return surface;', '''    if (params.m_jesterUnlit)
    {
        surface.emissiveLighting = surface.albedo;
        surface.albedo = real3(0,0,0);
        surface.specularF0 = real3(0,0,0);
    }
    return surface;''', 1)
(destination / 'JesterSurface.azsli').write_text(surface)
alpha = adapt_maps(read_shader('MaterialFunctions/StandardGetAlphaAndClip.azsli'))
alpha = alpha.replace('GetMaterialTextureSampler(), o_opacity_source', 'GetMaterialTextureSampler(params.m_jesterBaseSampler), o_opacity_source')
alpha = alpha.replace('    CheckClipping(alpha,', '    alpha *= real(params.m_jesterAlphaFactor);\n    CheckClipping(alpha,')
alpha = alpha.replace('return real(params.m_opacityFactor) * alpha;', 'return o_opacity_mode == OpacityMode::Cutout ? real(1) : real(params.m_opacityFactor) * alpha;')
(destination / 'JesterAlpha.azsli').write_text(alpha)
pbr = (sdk / 'Materials/Types/StandardPBR.azsli').read_text()
pbr = pbr.replace(f'<{shader_root}StandardPBR/StandardPBR_SurfaceEval.azsli>', '"JesterSurface.azsli"')
pbr = pbr.replace('"' + shader_root + 'MaterialFunctions/StandardGetAlphaAndClip.azsli"', '"JesterAlpha.azsli"')
pbr = pbr.replace('#if MATERIALPIPELINE_SHADER_HAS_PIXEL_STAGE', '''float2 JesterUv(float2 uv, float4 row0, float4 row1)
{
    return float2(dot(row0.xyz, float3(uv,1)), dot(row1.xyz, float3(uv,1)));
}

#if MATERIALPIPELINE_SHADER_HAS_PIXEL_STAGE''', 1)
(destination / 'JesterPBR.azsli').write_text(pbr)
asset = root / 'Assets/Materials/JesterDefault.material'
data = json.loads(asset.read_text())
data['materialType'] = 'Types/JesterPBR.materialtype'
asset.write_text(json.dumps(data, indent=4) + '\n')
