#!/usr/bin/env python3
"""Compile the pinned upstream UI shader math to Vulkan SPIR-V, without redesign."""
import argparse
from pathlib import Path
from prepare_build import write_text_if_changed
import re
import struct
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--kit', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
programs = [('batch2d', 'gl_batch.cpp', 'kVertex', 'kFragment'),
            ('mesh2d', 'gl_batch.cpp', 'kMeshVertex', 'kMeshFragment'),
            ('backdrop', 'backdrop.cpp', 'kVertex', 'kFragment'),
            ('blur', 'renderer.cpp', 'kBlurVertex', 'kBlurFragment')]
blocks = {
 'batch2d': 'vec4 viewport; vec4 surface;',
 'mesh2d': 'vec4 viewport; vec4 surface;',
 'backdrop': 'vec4 resolution_time_mode; vec4 colors[4]; vec4 params; vec4 misc;',
 'blur': 'vec4 step;'}
macros = {
 'batch2d': {'u_viewport': 'pc.viewport', 'u_surface': 'pc.surface.xy'},
 'mesh2d': {'u_viewport': 'pc.viewport', 'u_surface': 'pc.surface.xy'},
 'backdrop': {'u_resolution': 'pc.resolution_time_mode.xy', 'u_time': 'pc.resolution_time_mode.z',
              'u_mode': 'int(pc.resolution_time_mode.w)', 'u_colors': 'pc.colors',
              'u_params': 'pc.params', 'u_opacity': 'pc.misc.x'},
 'blur': {'u_step': 'pc.step.xy'}}
header = ['// Generated from pinned upstream UI shaders; do not edit.', '#pragma once', '#include <cstdint>']
for name, file, vertex, fragment in programs:
    source = (args.kit / 'gfx' / file).read_text()
    def extract(label):
        match = re.search(r'const char \*' + label + r' = R"\((.*?)\)";', source, re.S)
        if not match:
            raise SystemExit(f'Pinned shader {file}:{label} changed')
        return match.group(1)
    vs, fs = extract(vertex), extract(fragment)
    # Give the same location to every corresponding varying, including flat values.
    varyings = re.findall(r'^(?:flat )?out \w+ (\w+);', vs, re.M)
    for stage, shader in [('vert', vs), ('frag', fs)]:
        shader = re.sub(r'^layout\(location = \d+\) uniform (?!sampler)[^\n]*\n', '', shader, flags=re.M)
        shader = shader.replace('layout(location = 2) uniform sampler2D u_texture;',
                                'layout(set=0,binding=0) uniform sampler2D u_texture;')
        shader = shader.replace('layout(location = 3) uniform sampler2D u_fonts[6];',
                                'layout(set=0,binding=1) uniform sampler2D u_fonts[6];')
        shader = shader.replace('layout(location = 0) uniform sampler2D u_texture;',
                                'layout(set=0,binding=0) uniform sampler2D u_texture;')
        for location, varying in enumerate(varyings):
            shader = re.sub(r'^(flat )?(in|out) (\w+) ' + varying + ';',
                            lambda m: f'layout(location={location}) {m[1] or ""}{m[2]} {m[3]} {varying};', shader, flags=re.M)
        shader = shader.replace('out vec4 frag_color;', 'layout(location=0) out vec4 frag_color;')
        shader = shader.replace('gl_VertexID', 'gl_VertexIndex')
        if name == 'backdrop':
            shader = shader.replace('gl_FragCoord.xy', 'vec2(gl_FragCoord.x, pc.misc.y > 0.5 ? u_resolution.y - gl_FragCoord.y : gl_FragCoord.y)')
        prefix = '#version 450\nlayout(push_constant) uniform Push { ' + blocks[name] + ' } pc;\n'
        prefix += ''.join(f'#define {key} ({value})\n' for key, value in macros[name].items())
        glsl = args.output / f'{name}.{stage}'
        generated_source = prefix + shader
        source_unchanged = glsl.exists() and glsl.read_text() == generated_source
        write_text_if_changed(glsl, generated_source)
        spirv = glsl.with_suffix(glsl.suffix + '.spv')
        if not source_unchanged or not spirv.exists():
            subprocess.run(['glslangValidator', '-V', '--target-env', 'vulkan1.1', str(glsl), '-o', str(spirv)], check=True)
        data = spirv.read_bytes()
        words = struct.unpack('<' + 'I' * (len(data) // 4), data)
        header.append(f'inline constexpr std::uint32_t {name}_{stage}[] = {{')
        header.extend('    ' + ','.join(hex(w) for w in words[i:i+8]) + ',' for i in range(0,len(words),8))
        header.append('};')
write_text_if_changed(args.output / 'ui_shaders.hpp', '\n'.join(header) + '\n')
