#!/usr/bin/env python3
"""Exercise layer guards through their real command-line entrypoints."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]


class SourceTreeTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='edgeflow-layer-contract-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        for directory in ('include', 'src', 'demo'):
            (self.root / directory).mkdir()

    def write(self, relative, contents=''):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(contents, encoding='utf-8')
        return path


class IncludeOwnershipContractTest(SourceTreeTest):
    def check_includes(self):
        return subprocess.run(
            [sys.executable, str(ROOT / 'scripts/check_layer_dependencies.py'),
             '--root', str(self.root)],
            capture_output=True, text=True, timeout=10)

    def test_allowed_contracts_and_public_consumers(self):
        cases = (
            ('include/edgeflow/log.h', 'platform_mock/log.h', 'include/platform_mock/log.h'),
            ('include/platform_mock/log.h', 'edgeflow/export.h', 'include/edgeflow/export.h'),
            ('src/engine/models/probe.cpp', 'edgeflow/log.h', 'include/edgeflow/log.h'),
            ('src/adapter/operator/mock/operator_adapter.cpp', 'platform_mock/error_codes.h', 'include/platform_mock/error_codes.h'),
            ('src/adapter/operator/mock/operator_error_mapping.cpp', 'platform_mock/error_codes.h', 'include/platform_mock/error_codes.h'),
            ('src/custom_nodes/normalize.cpp', 'core/alg_context.h', 'include/core/alg_context.h'),
            ('include/nodes/node_base.h', 'core/validated_node_plan.h', 'include/core/validated_node_plan.h'),
            ('src/common_nodes/infer.cpp', 'engine/model_interface.h', 'include/engine/model_interface.h'),
            ('src/core/schedule.cpp', 'contracts/diagnostic.h', 'include/contracts/diagnostic.h'),
            ('src/engine/models/probe.cpp', 'contracts/diagnostic.h', 'include/contracts/diagnostic.h'),
            ('src/custom_nodes/normalize.cpp', 'custom_nodes/helper.h', 'src/custom_nodes/helper.h'),
            ('demo/request.cpp', 'edgeflow/operator/interface.h', 'include/edgeflow/operator/interface.h'),
            ('demo/request.cpp', 'platform_mock/operator_types.h', 'include/platform_mock/operator_types.h'),
        )
        for source, include, target in cases:
            with self.subTest(source=source, include=include):
                self.write(target)
                path = self.write(source, f'#include "{include}"\n')
                self.addCleanup(path.unlink, missing_ok=True)
                result = self.check_includes()
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn('Resolved include ownership and vendor boundaries passed.', result.stdout)
                self.assertEqual(result.stderr, '')
                path.unlink()

    def test_converters_can_use_adapter_values_and_neutral_contracts(self):
        self.write('include/adapter/io_values.h')
        self.write('include/core/common_contracts.h')
        for direction in ('input', 'output'):
            with self.subTest(direction=direction):
                source = self.write(f'src/adapter/{direction}/convert.cpp',
                                    '#include "adapter/io_values.h"\n'
                                    '#include "core/common_contracts.h"\n')
                result = self.check_includes()
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                source.unlink()

    def test_upward_and_private_dependencies_report_exact_source_and_target(self):
        cases = (
            ('src/engine/models/probe.cpp', 'platform_mock/log.h',
             'include/platform_mock/log.h', 'Model Execution -> Contracts'),
            ('src/adapter/shared_algorithm_runtime.cpp', 'platform_mock/error_codes.h',
             'include/platform_mock/error_codes.h', 'Integration -> Integration'),
            ('include/adapter/adapter_status.h', 'platform_mock/error_codes.h',
             'include/platform_mock/error_codes.h', 'Integration -> Integration'),
            ('src/engine/models/embedding/probe.cpp', 'nodes/node_interface.h',
             'include/nodes/node_interface.h', 'Model Execution -> Capability Nodes'),
            ('src/engine/models/embedding/probe.cpp', 'core/alg_context.h',
             'include/core/alg_context.h', 'Model Execution -> Orchestration'),
            ('src/core/schedule.cpp', 'adapter/input_limits.h',
             'include/adapter/input_limits.h', 'Orchestration -> Integration'),
            ('src/custom_nodes/extract.cpp', 'core/pipeline_validator.h',
             'include/core/pipeline_validator.h', 'Capability Nodes -> Orchestration'),
            ('include/nodes/node_base.h', 'core/pipeline_validator.h',
             'include/core/pipeline_validator.h', 'Capability Nodes -> Orchestration'),
            ('src/common_nodes/filter.cpp', 'custom_nodes/domain.h',
             'src/custom_nodes/domain.h', 'Capability Nodes -> Capability Nodes'),
            ('src/adapter/input/parse.cpp', 'platform_mock/operator_data_types.h',
             'include/platform_mock/operator_data_types.h', 'Integration -> Integration'),
            ('src/adapter/input/parse.cpp', 'engine/model_interface.h',
             'include/engine/model_interface.h', 'Integration -> Model Execution'),
            ('src/adapter/output/write.cpp', 'engine/model_interface.h',
             'include/engine/model_interface.h', 'Integration -> Model Execution'),
            ('src/adapter/input/parse.cpp', 'nodes/node_base.h',
             'include/nodes/node_base.h', 'Integration -> Capability Nodes'),
            ('src/adapter/output/write.cpp', 'nodes/node_base.h',
             'include/nodes/node_base.h', 'Integration -> Capability Nodes'),
            ('demo/request.cpp', 'engine/model_interface.h',
             'include/engine/model_interface.h', 'Demo -> Model Execution'),
        )
        for source, include, target, direction in cases:
            with self.subTest(source=source, include=include):
                self.write(target)
                path = self.write(source, f'// boundary probe\n#include "{include}"\n')
                self.addCleanup(path.unlink, missing_ok=True)
                result = self.check_includes()
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertEqual(result.stdout.splitlines(), [
                    f'{source}:2: {include}: {direction} forbidden ({target})'])
                self.assertEqual(result.stderr, '')
                path.unlink()

    def test_framework_and_demo_boundaries_cover_headers_and_sources(self):
        lower_layers = (
            ('src/common_nodes/probe.cpp', 'Capability Nodes'),
            ('src/custom_nodes/probe.cpp', 'Capability Nodes'),
            ('include/nodes/probe.h', 'Capability Nodes'),
            ('src/core/probe.cpp', 'Orchestration'),
            ('include/core/probe.h', 'Orchestration'),
            ('src/engine/models/probe.cpp', 'Model Execution'),
            ('include/engine/probe.h', 'Model Execution'),
        )
        cases = []
        for source, owner in lower_layers:
            for include in ('adapter/io_values.h', 'edgeflow/operator/interface.h',
                            'platform_mock/operator_types.h'):
                cases.append((source, include, 'include/' + include, owner, 'Integration'))
            if not source.startswith('src/custom_nodes/'):
                cases.append((source, 'custom_nodes/helper.h', 'src/custom_nodes/helper.h',
                              owner, 'Capability Nodes'))
        for target, owner in (
                ('include/adapter/io_values.h', 'Integration'),
                ('include/core/pipeline.h', 'Orchestration'),
                ('include/nodes/node_base.h', 'Capability Nodes'),
                ('include/engine/model_interface.h', 'Model Execution'),
                ('include/contracts/diagnostic.h', 'Contracts'),
                ('src/adapter/private.h', 'Integration')):
            cases.append(('demo/request.cpp', target, target, 'Demo', owner))
        for source, include, target, source_owner, target_owner in cases:
            with self.subTest(source=source, include=include):
                self.write(target)
                path = self.write(source, f'#include "{include}"\n')
                self.addCleanup(path.unlink, missing_ok=True)
                result = self.check_includes()
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertEqual(result.stdout.splitlines(), [
                    f'{source}:1: {include}: {source_owner} -> {target_owner} forbidden ({target})'])
                self.assertEqual(result.stderr, '')
                path.unlink()

    def test_include_path_and_directive_spellings_cannot_hide_dependency(self):
        target = self.write('include/adapter/probe.h')
        directives = (
            '#include "../../include/adapter/probe.h"\n',
            f'#include "{target}"\n',
            '#include \\\n"adapter/probe.h"\n',
            '  # include <adapter/probe.h>\n',
        )
        for directive in directives:
            with self.subTest(directive=directive):
                self.write('src/custom_nodes/probe.cpp', directive)
                result = self.check_includes()
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn('Capability Nodes -> Integration forbidden (include/adapter/probe.h)',
                              result.stdout)
                self.assertEqual(result.stderr, '')

    def test_commented_includes_do_not_create_dependencies(self):
        self.write('include/adapter/probe.h')
        self.write('include/core/alg_context.h')
        self.write('src/custom_nodes/probe.cpp',
                   '/*\n#include "adapter/probe.h"\n*/\n'
                   '// #include "adapter/probe.h"\n'
                   '#include "core/alg_context.h"\n')
        result = self.check_includes()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stderr, '')

    def test_vendor_headers_belong_to_their_concrete_backend(self):
        for backend, header in (('onnxruntime', 'onnxruntime_cxx_api.h'),
                                ('llama_cpp', 'llama.h'),
                                ('whisper_cpp', 'whisper.h'),
                                ('kite_llm', 'kiteLLM.h'),
                                ('inferlet', 'inferlet.h')):
            with self.subTest(backend=backend):
                # Empty resolved headers prove ownership without using any SDK.
                target = f'src/engine/backends/{backend}/vendor/{header}'
                self.write(target)
                include = f'vendor/{header}'
                allowed = self.write(f'src/engine/backends/{backend}/resource.cpp',
                                     f'#include "{include}"\n')
                result = self.check_includes()
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                allowed.unlink()
                other_backend = 'llama_cpp' if backend == 'onnxruntime' else 'onnxruntime'
                for source in ('src/engine/models/qwen_causal_lm/resource.cpp',
                               'include/engine/probe.h', 'src/core/probe.cpp',
                               'src/common_nodes/probe.cpp', 'src/custom_nodes/probe.cpp',
                               'src/adapter/probe.cpp', 'demo/probe.cpp',
                               f'src/engine/backends/{other_backend}/resource.cpp'):
                    with self.subTest(source=source):
                        path = self.write(source, f'#include "{target}"\n')
                        self.addCleanup(path.unlink, missing_ok=True)
                        result = self.check_includes()
                        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                        self.assertEqual(result.stdout.splitlines(), [
                            f'{source}:1: {target}: vendor header outside its concrete Backend'])
                        self.assertEqual(result.stderr, '')
                        path.unlink()


class LayerIsolationTransportTest(SourceTreeTest):
    def setUp(self):
        super().setUp()
        for directory in ('src/common_nodes', 'src/custom_nodes',
                          'src/adapter/input', 'src/adapter/output',
                          'src/core', 'src/engine', 'include/nodes',
                          'include/core', 'include/engine'):
            (self.root / directory).mkdir(parents=True, exist_ok=True)

    def check_transport(self):
        environment = dict(os.environ, REPO_ROOT=str(self.root))
        environment.pop('LLM_EDGEFLOW_LAYER_COMPILE_MANIFEST', None)
        return subprocess.run(
            ['bash', str(ROOT / 'scripts/check_layer_isolation.sh')],
            env=environment, capture_output=True, text=True, timeout=10)

    def test_missing_required_source_directory_fails(self):
        for directory in ('src/common_nodes', 'src/custom_nodes',
                          'src/adapter/input', 'src/adapter/output'):
            with self.subTest(directory=directory):
                path = self.root / directory
                path.rmdir()
                result = self.check_transport()
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn(f'{directory} directory not found!', result.stdout)
                path.mkdir()

    def test_node_integration_include_stops_the_shell_guard(self):
        self.write('include/edgeflow/operator/interface.h')
        self.write('src/custom_nodes/request.cpp',
                   '#include "edgeflow/operator/interface.h"\n')
        result = self.check_transport()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn('src/custom_nodes/request.cpp:1: edgeflow/operator/interface.h: '
                      'Capability Nodes -> Integration forbidden (include/edgeflow/operator/interface.h)',
                      result.stdout)
        self.assertEqual(result.stderr, '')
        self.assertNotIn('does not assign sources', result.stdout)
        self.assertNotIn('pure C11', result.stdout)

    def test_custom_source_target_ownership_fails_before_compiler_checks(self):
        for layer, target in (
                ('engine', 'edgeflow_model_execution_objects'),
                ('common_nodes', 'edgeflow_capability_nodes_objects'),
                ('core', 'edgeflow_orchestration_objects'),
                ('adapter', 'edgeflow_integration_objects')):
            self.write(f'src/{layer}/CMakeLists.txt', f'target_sources({target} PRIVATE fixture.cpp)\n')
        self.write('src/CMakeLists.txt',
                   'target_sources(edgeflow_composition_objects PRIVATE fixture.cpp)\n')
        self.write('src/adapter/CMakeLists.txt',
                   'target_sources(edgeflow_integration_objects PRIVATE fixture.cpp)\n'
                   'target_sources(edgeflow_composition_objects PRIVATE shared_algorithm_runtime.cpp)\n')
        self.write('src/custom_nodes/CMakeLists.txt',
                   'target_sources(edgeflow_orchestration_objects PRIVATE fixture.cpp)\n')
        result = self.check_transport()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn('Resolved include ownership and vendor boundaries passed.', result.stdout)
        self.assertIn('src/custom_nodes/CMakeLists.txt does not assign sources to '
                      'edgeflow_capability_nodes_objects.', result.stdout)
        self.assertEqual(result.stdout.count('[LayerGuard ERROR]'), 1)
        self.assertEqual(result.stderr, '')
        self.assertNotIn('pure C11', result.stdout)


if __name__ == '__main__':
    unittest.main()
