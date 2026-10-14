# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Validate a SASS fragment program translated without GLSL."""

import argparse
import subprocess
import tempfile
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--generator', required=True)
    parser.add_argument('--validator', required=True)
    parser.add_argument('--disassembler', required=True)
    parser.add_argument('--regressions', required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='snx-direct-shader-') as directory:
        path = Path(directory) / 'fragment.spv'
        vertex = Path(directory) / 'vertex.spv'
        abs_fragment = Path(directory) / 'abs_fragment.spv'
        stack_fragment = Path(directory) / 'stack_fragment.spv'
        subgroup = Path(directory) / 'subgroup.spv'
        storage_write = Path(directory) / 'storage_write.spv'
        storage_atomic = Path(directory) / 'storage_atomic.spv'
        nro_raw = Path(directory) / 'nro_raw.spv'
        nro_ms = Path(directory) / 'nro_ms.spv'
        nro_ms_array = Path(directory) / 'nro_ms_array.spv'
        nro_exchange = Path(directory) / 'nro_exchange.spv'
        subprocess.run(
            [args.generator, str(path), str(vertex), str(abs_fragment), str(stack_fragment), str(subgroup)], check=True
        )
        subprocess.run([args.validator, '--target-env', 'vulkan1.3', str(path)], check=True)
        subprocess.run([args.validator, '--target-env', 'vulkan1.3', str(vertex)], check=True)
        subprocess.run([args.validator, '--target-env', 'vulkan1.3', str(abs_fragment)], check=True)
        subprocess.run([args.validator, '--target-env', 'vulkan1.3', str(stack_fragment)], check=True)
        subprocess.run([args.validator, '--target-env', 'vulkan1.3', str(subgroup)], check=True)
        fixtures = (storage_write, storage_atomic, nro_raw, nro_ms, nro_ms_array, nro_exchange)
        subprocess.run([args.regressions, *(str(path) for path in fixtures)], check=True)
        for fixture in fixtures:
            subprocess.run([args.validator, '--target-env', 'vulkan1.3', str(fixture)], check=True)
        raw_code = subprocess.run(
            [args.disassembler, str(nro_raw)], check=True, capture_output=True, text=True
        ).stdout
        if 'OpTypeImage %uint 2D 0 0 0 1 Unknown' not in raw_code or 'OpImageFetch %v4uint' not in raw_code:
            raise AssertionError('raw surface load must fetch an unsigned texel')
        for fixture, array in ((nro_ms, 0), (nro_ms_array, 1)):
            ms_code = subprocess.run(
                [args.disassembler, str(fixture)], check=True, capture_output=True, text=True
            ).stdout
            if f'OpTypeImage %float 2D 0 {array} 1 1 Unknown' not in ms_code or ' Sample %' not in ms_code:
                raise AssertionError('multisample texture load must fetch a specified sample')
        exchange_code = subprocess.run(
            [args.disassembler, str(nro_exchange)], check=True, capture_output=True, text=True
        ).stdout
        if exchange_code.count('OpAccessChain %_ptr_Workgroup_uint') < 4 or exchange_code.count('OpLoad %uint') < 4:
            raise AssertionError('64-bit shared exchange must access both words')
        result = subprocess.run([args.disassembler, str(path)], check=True, capture_output=True, text=True)
        for expected in (
            'OpEntryPoint Fragment',
            'OriginUpperLeft',
            'NoPerspective',
            'OpCompositeExtract',
            'Location 0',
            'DescriptorSet 1',
            'Binding 0',
            'OpAccessChain',
            'OpFMul',
            'Fma',
            'Exp2',
            'NoContraction',
            'OpImageSampleImplicitLod',
            'OpImageSampleDrefImplicitLod',
            'OpFUnordLessThan',
            'OpSelectionMerge',
            'OpBranchConditional',
        ):
            if expected not in result.stdout:
                raise AssertionError(f'missing {expected} in direct SASS shader')
        vs = subprocess.run([args.disassembler, str(vertex)], check=True, capture_output=True, text=True).stdout
        for expected in (
            'OpEntryPoint Vertex',
            'BuiltIn Position',
            'BuiltIn PointSize',
            'Invariant',
            'OpConstantComposite %v4float %float_0 %float_0 %float_0 %float_1',
        ):
            if expected not in vs:
                raise AssertionError(f'missing {expected} in direct vertex shader')
        abs_code = subprocess.run(
            [args.disassembler, str(abs_fragment)], check=True, capture_output=True, text=True
        ).stdout
        if 'FAbs' not in abs_code:
            raise AssertionError('missing right-operand absolute value in FADD32I shader')
        subgroup_code = subprocess.run(
            [args.disassembler, str(subgroup)], check=True, capture_output=True, text=True
        ).stdout
        for expected in (
            'GroupNonUniformBallot',
            'GroupNonUniformShuffle',
            'BuiltIn SubgroupLocalInvocationId',
            'OpGroupNonUniformBallot',
            'OpGroupNonUniformShuffle',
        ):
            if expected not in subgroup_code:
                raise AssertionError(f'missing {expected} in subgroup shader')


if __name__ == '__main__':
    main()
