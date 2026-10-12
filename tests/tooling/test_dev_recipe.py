#!/usr/bin/env python3
"""Actual recipe CLI prepares isolated artifacts and invokes native validation."""

import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
TOOL = Path(os.environ.get('LLM_EDGEFLOW_PIPELINE_TOOL', ROOT / 'build/alg_pipeline_tool_test')).resolve()
DEMO = Path(os.environ.get('LLM_EDGEFLOW_DEMO_BINARY', TOOL.parent / 'alg_demo')).resolve()
SPEC = importlib.util.spec_from_file_location('recipe_under_test', ROOT / 'tools/dev_recipe.py')
recipe = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(recipe)


class RecipeConsumerTest(unittest.TestCase):
    def setUp(self):
        self.assertTrue(TOOL.is_file(), f'Set LLM_EDGEFLOW_PIPELINE_TOOL: {TOOL}')
        self.assertTrue(DEMO.is_file(), f'Set LLM_EDGEFLOW_DEMO_BINARY: {DEMO}')
        self.temporary = tempfile.TemporaryDirectory(prefix='recipe-consumer-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.build = self.root / 'build'
        self.build.mkdir()
        # Exact production scripts/templates plus actual built binaries; no inferred Catalog.
        files = [
            'tools/dev_recipe.py', 'tools/scaffold_custom_node.py', 'tools/verify_selection.py',
            'demo/common/demo_profile.py', 'demo/common/profile_fields.json', 'demo/profiles.json',
            'dev_support/node_authoring/starter_llm_node.cpp',
            'dev_support/node_authoring/starter_control_node.cpp',
            'configs/pipeline_keyword_match_rules.json', 'configs/pipeline_keyword_match_rules.conf',
            'demo/fixtures/mock/pipeline_entity_extract.json', 'demo/fixtures/mock/pipeline_entity_extract.conf',
            'demo/fixtures/mock/artifacts/neutral-llm.fixture',
            'tests/fixtures/asset_manifest_test.json', 'tests/fixtures/effects/keyword_exact.json',
            'tests/fixtures/effects/keyword_inputs.txt', 'tests/fixtures/effects/entity_mock_exact.json',
            'data/corpus_keyword_match.txt', 'data/corpus_entity_extract.txt',
        ]
        for relative in files:
            target = self.root / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / relative, target)
        artifact = self.root / 'configs/artifacts/neutral-llm.fixture'
        artifact.parent.mkdir(parents=True)
        shutil.copy2(ROOT / 'demo/fixtures/mock/artifacts/neutral-llm.fixture', artifact)
        for executable in (TOOL, DEMO):
            shutil.copy2(executable, self.build / executable.name)
        for library in TOOL.parent.glob('*.so*'):
            if library.is_file():
                shutil.copy2(library, self.build / library.name)
        self.tool = self.build / TOOL.name
        self.demo = self.build / DEMO.name
        # The copied source-root metadata exercises tool_context's matching-root contract.
        (self.build / 'CMakeCache.txt').write_text(f'CMAKE_HOME_DIRECTORY:INTERNAL={self.root}\n')
        self.script = self.root / 'tools/dev_recipe.py'
        self.pipeline = self.root / 'configs/pipeline_recipe.json'
        self.manifest = self.root / 'tests/fixtures/asset_manifest_test.json'

    def call(self, *arguments, expected=0):
        process = subprocess.run([sys.executable, str(self.script), *map(str, arguments)],
                                 cwd=self.root, capture_output=True, text=True, timeout=90)
        self.assertEqual(process.returncode, expected, process.stdout + process.stderr)
        return json.loads(process.stdout)

    def prepare(self, kind='prompt-config', name='consumer_prompt', profile='keyword_match_rules', expected=0):
        return self.call('prepare', kind, '--name', name, '--profile', profile,
                         '--tool', self.tool, '--build-dir', self.build,
                         '--pipeline', self.pipeline, '--manifest', self.manifest, '--json', expected=expected)

    def test_public_navigation_lists_recipes_and_prepare_returns_reviewable_artifacts(self):
        listed = self.call('list', '--json')
        self.assertTrue(listed['ok'])
        self.assertEqual(set(listed['recipes']), {'prompt-config', 'text-llm-node'})
        prepared = self.prepare()
        self.assertTrue(prepared['ok'])
        self.assertEqual(prepared['completed_steps'], ['preconditions', 'files_prepared'])
        self.assertEqual(prepared['pending_steps'], ['verify'])
        self.assertIsNone(prepared['failed_step'])
        self.assertEqual(set(map(Path, prepared['artifacts'])), {
            self.pipeline, self.pipeline.with_suffix('.conf'),
            self.pipeline.with_name('pipeline_recipe_effects.json')})
        self.assertEqual(json.loads(self.pipeline.with_suffix('.conf').read_text()), {'pipe_path': self.pipeline.name})
        native = subprocess.run([str(self.tool), 'validate', str(self.pipeline)], cwd=self.root,
                                capture_output=True, text=True)
        self.assertEqual(native.returncode, 0, native.stdout + native.stderr)
        self.assertTrue(json.loads(native.stdout)['ok'])
        next_command = prepared['next_commands'][0]['argv']
        self.assertIn(str(self.demo), next_command)
        self.assertIn(str(self.pipeline), next_command)
        self.assertIn(str(self.manifest), next_command)
        self.assertNotIn('build', prepared['completed_steps'])

    def test_prompt_verification_uses_actual_native_plan_and_demo_labelled_effects(self):
        prepared = self.prepare()
        command = prepared['next_commands'][0]['argv']
        # prompt-config's existing binaries require no nested compilation.
        process = subprocess.run([*command, '--json'], cwd=self.root,
                                 capture_output=True, text=True, timeout=90)
        self.assertEqual(process.returncode, 0, process.stdout + process.stderr)
        verified = json.loads(process.stdout)
        self.assertTrue(verified['ok'])
        self.assertEqual(verified['completed_steps'], ['preconditions', 'config_validation', 'evaluate'])
        self.assertEqual(verified['pending_steps'], [])
        self.assertEqual(verified['metrics']['status'], 'passed')
        self.assertEqual(verified['metrics']['total'], 3)

    def test_llm_prepare_generates_current_source_test_and_rewires_real_profile_output(self):
        prepared = self.prepare('text-llm-node', 'consumer_llm', 'entity_extract_mock')
        self.assertTrue(prepared['ok'], prepared)
        source = self.root / 'src/custom_nodes/consumer_llm_node.cpp'
        generated_test = self.root / 'tests/unit/nodes/test_consumer_llm_node.cpp'
        self.assertIn('REGISTER_FUNCTION_NODE(consumer_llm, Spec())', source.read_text())
        self.assertIn('tests/support/node_probe.h', generated_test.read_text())
        self.assertNotIn('node_harness.h', generated_test.read_text())
        pipeline = json.loads(self.pipeline.read_text())
        generated = next(node for node in pipeline['pipeline'] if node['type'] == 'consumer_llm')
        self.assertEqual(generated['params'], {'bind_model': 'entity_llm'})
        self.assertEqual(generated['inputs'], {'input': 'build_prompt.text'})
        parse = next(node for node in pipeline['pipeline'] if node['type'] == 'structured_json_parse')
        self.assertEqual(parse['inputs']['text'], 'consumer_llm.output')
        # Compilation/execution of the exact emitted test is owned by scaffold fixtures.
        self.assertEqual(prepared['pending_steps'], ['verify'])

    def test_prepare_conflict_preserves_all_existing_artifacts_and_source(self):
        self.pipeline.write_text('user file\n')
        failed = self.prepare(expected=1)
        self.assertFalse(failed['ok'])
        self.assertEqual(failed['failed_step'], 'preconditions')
        self.assertEqual(self.pipeline.read_text(), 'user file\n')
        self.assertFalse(self.pipeline.with_suffix('.conf').exists())
        self.assertFalse(self.pipeline.with_name('pipeline_recipe_effects.json').exists())

    def test_missing_independent_effect_labels_fail_without_publishing_files(self):
        labels = self.root / 'tests/fixtures/effects/keyword_exact.json'
        document = json.loads(labels.read_text())
        document['samples'] = [{'expected': {'/status': 0}}]
        labels.write_text(json.dumps(document))
        failed = self.prepare(expected=1)
        self.assertEqual(failed['failed_step'], 'preconditions')
        self.assertIn('independent /output/', failed['message'])
        self.assertFalse(self.pipeline.exists())

    def test_tool_context_rejects_an_unrelated_source_tree_before_native_execution(self):
        (self.build / 'CMakeCache.txt').write_text('CMAKE_HOME_DIRECTORY:INTERNAL=/other/source\n')
        failed = self.prepare(expected=1)
        self.assertIn('another source tree', failed['message'])
        self.assertFalse(self.pipeline.exists())


if __name__ == '__main__':
    unittest.main()
