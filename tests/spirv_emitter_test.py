# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Check a direct-emitted module with system SPIRV-Tools."""

import argparse
import subprocess
import tempfile
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--generator', required=True)
    parser.add_argument('--validator', required=True)
    parser.add_argument('--disassembler', required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='snx-spirv-emitter-') as directory:
        expected_by_stage = {
            'compute': ('OpEntryPoint GLCompute', 'LocalSize 1 1 1', 'OpAccessChain', 'OpIAdd'),
            'vertex': ('OpEntryPoint Vertex', 'BuiltIn Position', 'OpConstantComposite'),
            'fragment': (
                'OpEntryPoint Fragment',
                'OriginUpperLeft',
                'Location 0',
                'Location 1',
                'DenormFlushToZero 32',
                'SignedZeroInfNanPreserve 32',
                'NoPerspective',
                'BuiltIn FragCoord',
                'DescriptorSet 1',
                'Binding 0',
                'OpImageSampleImplicitLod',
                'OpImageSampleDrefImplicitLod',
                'OpSelectionMerge',
                'OpBranchConditional',
                'OpIEqual',
                'OpFUnordLessThan',
                'OpLogicalNot',
                'GLSL.std.450',
                'FAbs',
                'Fma',
                'OpFNegate',
                'OpVariable',
                'OpLoad',
                'OpBitcast',
                'OpFAdd',
                'OpFMul',
                'OpFDiv',
                'NoContraction',
                'OpCompositeConstruct',
                'OpCompositeExtract',
            ),
        }
        for stage, expected in expected_by_stage.items():
            path = Path(directory) / f'{stage}.spv'
            subprocess.run([args.generator, stage, str(path)], check=True)
            subprocess.run([args.validator, '--target-env', 'vulkan1.3', str(path)], check=True)
            result = subprocess.run([args.disassembler, str(path)], check=True, capture_output=True, text=True)
            disassembly = result.stdout
            for instruction in (*expected, 'OpStore', 'OpReturn'):
                if instruction not in disassembly:
                    raise AssertionError(f'{stage}: missing {instruction} in direct-emitted SPIR-V')


if __name__ == '__main__':
    main()
