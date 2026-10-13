# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Prove forbidden includes and indirect graphics linkage fail architecture checks."""

import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location('architecture', Path(__file__).with_name('architecture.py'))
architecture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(architecture)


class ArchitectureTests(unittest.TestCase):
    def test_compiling_nx_source_into_common_is_rejected(self):
        with tempfile.TemporaryDirectory(prefix='snx-arch-test-') as temporary:
            repo = Path(temporary)
            build = repo / 'build'
            reply = build / '.cmake/api/v1/reply'
            reply.mkdir(parents=True)
            documents = {
                'index-test.json': {'reply': {'codemodel-v2': {'jsonFile': 'model.json'}}},
                'model.json': {
                    'configurations': [{'targets': [{'name': 'scarletnx-common', 'jsonFile': 'common.json'}]}]
                },
                'common.json': {'paths': {'source': 'src/common'}, 'sources': [{'path': 'src/nx/horizon.c'}]},
            }
            for name, content in documents.items():
                (reply / name).write_text(json.dumps(content), encoding='utf-8')
            errors = architecture.check_target_sources(repo, build)
            self.assertEqual(len(errors), 1)
            self.assertIn('scarletnx-common compiles a source owned by another runtime', errors[0])

    def test_common_cannot_include_nx(self):
        with tempfile.TemporaryDirectory(prefix='snx-arch-test-') as temporary:
            root = Path(temporary)
            for owner in architecture.BOUNDARIES:
                (root / 'src' / owner).mkdir(parents=True)
            (root / 'src/nx/horizon.h').write_text('/* private */\n', encoding='ascii')
            (root / 'src/common/core.c').write_text('#include "../nx/horizon.h"\n', encoding='ascii')
            errors = architecture.check_sources(root)
            self.assertEqual(len(errors), 1)
            self.assertIn('common includes nx', errors[0])

    def test_transitive_graphics_link_is_rejected(self):
        with tempfile.TemporaryDirectory(prefix='snx-arch-test-') as temporary:
            graph = Path(temporary) / 'targets.dot'
            graph.write_text(
                'digraph {\n"a" [label = "scarletnx-common", shape = octagon];\n'
                '"b" [label = "scarletnx-gnuabi", shape = octagon];\n'
                '"c" [label = "adapter", shape = octagon];\n'
                '"d" [label = "snx_gpu", shape = octagon];\n'
                '"b" -> "a";\n"b" -> "c";\n"c" -> "d";\n}\n',
                encoding='ascii',
            )
            errors = architecture.check_graph(graph)
            self.assertEqual(errors, ['scarletnx-gnuabi transitively links graphics/media target snx_gpu'])


if __name__ == '__main__':
    unittest.main()
