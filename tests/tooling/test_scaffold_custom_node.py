#!/usr/bin/env python3
"""Public scaffold artifacts and atomic source/test publication."""

import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools/scaffold_custom_node.py"
SPEC = importlib.util.spec_from_file_location("scaffold_under_test", SCRIPT)
scaffold = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(scaffold)


class ScaffoldConsumerTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="scaffold-consumer-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source_dir = self.root / "src/custom_nodes"
        self.test_dir = self.root / "tests/unit/nodes"

    def call(self, *arguments, expected=0):
        result = subprocess.run([sys.executable, str(SCRIPT), *arguments],
                                env={**os.environ, "LLM_EDGEFLOW_REPO_ROOT": str(self.root)},
                                capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
        return result

    def test_default_source_is_current_typed_author_node_and_dry_run_has_no_writes(self):
        preview = self.call("consumer_identity", "--dry-run")
        self.assertIn('REGISTER_FUNCTION_NODE(consumer_identity, Spec())', preview.stdout)
        self.assertIn('Required("input", &Inputs::input)', preview.stdout)
        self.assertIn('PreservedOutput<TextBatch>("output", "input")', preview.stdout)
        self.assertFalse(self.source_dir.exists())
        created = self.call("consumer_identity")
        source = self.source_dir / "consumer_identity_node.cpp"
        self.assertIn(str(source), created.stdout)
        self.assertEqual(source.read_text() + "\n", preview.stdout)
        self.assertFalse(self.test_dir.exists())
        self.assertEqual(source.stat().st_mode & 0o777, 0o644)

    def test_custom_ports_description_and_generated_test_are_a_single_requested_artifact_pair(self):
        description = 'line one "quote" \\ slash\n中文 line two'
        self.call("consumer_named", "--write-test", "--description", description,
                  "--in-port", "source:TextBatch", "--out-port", "answer:TextBatch")
        source = self.source_dir / "consumer_named_node.cpp"
        generated_test = self.test_dir / "test_consumer_named_node.cpp"
        content = source.read_text()
        self.assertIn(f'.Description({json.dumps(description, ensure_ascii=False)})', content)
        self.assertIn('Required("source",', content)
        self.assertIn('PreservedOutput<TextBatch>("answer", "source")', content)
        test_content = generated_test.read_text()
        self.assertIn('tests/support/node_probe.h', test_content)
        self.assertIn('consumer_named', test_content)
        self.assertNotIn('node_harness.h', test_content)
        self.assertNotIn('node_test_utils.h', test_content)
        self.assertTrue(source.exists() and generated_test.exists())

    def test_existing_test_conflict_publishes_neither_new_source_nor_replacement_test(self):
        self.test_dir.mkdir(parents=True)
        target = self.test_dir / "test_consumer_conflict_node.cpp"
        target.write_text('user-owned test\n')
        result = self.call("consumer_conflict", "--write-test", expected=2)
        self.assertIn('already exists', result.stderr)
        self.assertFalse((self.source_dir / "consumer_conflict_node.cpp").exists())
        self.assertEqual(target.read_text(), 'user-owned test\n')

    def test_force_changes_one_source_but_write_test_refuses_multi_file_overwrite(self):
        self.call("consumer_force", "--description", "original")
        target = self.source_dir / "consumer_force_node.cpp"
        target.chmod(0o640)
        self.call("consumer_force", "--force", "--description", "updated")
        self.assertIn('.Description("updated")', target.read_text())
        self.assertEqual(target.stat().st_mode & 0o777, 0o640)
        snapshot = target.read_bytes()
        result = self.call("consumer_force", "--force", "--write-test", expected=2)
        self.assertIn('rejects --force', result.stderr)
        self.assertEqual(target.read_bytes(), snapshot)
        self.assertFalse(self.test_dir.exists())

    def test_invalid_public_arguments_are_rejected_before_creating_artifacts(self):
        cases = [
            (['BadName'], 'snake_case'),
            (['consumer_bad', '--model-capability', 'llm', '--control-id', '1001'], '--control-id'),
            (['consumer_bad', '--kind', 'model'], 'unrecognized arguments'),
            (['consumer_bad', '--in-port', 'x:TextBatch:1'], 'expected'),
            (['consumer_bad', '--in-port', 'x:TextBatch:2:N'], 'Cardinality'),
            (['consumer_bad', '--model-capability', 'embedding',
              '--out-port', 'output:TextBatch'], 'EmbeddingBatch'),
            (['consumer_bad', '--model-capability', 'llm', '--in-port', 'input:TextBatch:1:N:split'], '1:1'),
            (['consumer_bad', '--control-id', '999'], '1000'),
            (['consumer_bad', '--control-id', '190001', '--in-port', 'input:AudioPcmBatch'], 'TextBatch'),
            (['consumer_bad', '--write-test', '--output-dir', 'elsewhere'], 'standard source'),
        ]
        for arguments, message in cases:
            with self.subTest(arguments=arguments):
                result = self.call(*arguments, expected=2)
                self.assertIn(message, result.stderr)
                self.assertFalse(self.source_dir.exists())
                self.assertFalse(self.test_dir.exists())

    def test_models_and_controls_extend_the_same_node_structure(self):
        plain = self.call('consumer_plain', '--dry-run').stdout
        llm = self.call('consumer_plain', '--model-capability', 'llm', '--dry-run').stdout
        for shared in ('struct Inputs {\n  const TextBatch* input = nullptr;\n};',
                       'MapPayloads(*inputs.input, &', 'InputsOf<Inputs>{Required("input", &Inputs::input)}',
                       'PreservedOutput<TextBatch>("output", "input")',
                       'REGISTER_FUNCTION_NODE(consumer_plain, Spec());'):
            self.assertIn(shared, plain)
            self.assertIn(shared, llm)
        self.assertNotIn('Models', plain)
        self.assertIn('struct Models {\n  LlmCall model;\n};', llm)
        self.assertIn('ModelsOf<Models>{Model("model", "bind_model", &Models::model)}', llm)
        self.call('consumer_encoder', '--model-capability', 'embedding',
                  '--in-port', 'sentences:TextBatch', '--out-port', 'vectors:EmbeddingBatch')
        source = (self.source_dir / 'consumer_encoder_node.cpp').read_text()
        self.assertIn('EmbeddingCall model;', source)
        self.assertIn('models.model.Embed(*inputs.input)', source)
        self.assertIn('Required("sentences", &Inputs::input)', source)
        self.assertIn('PreservedOutput<EmbeddingBatch>("vectors", "sentences")', source)
        self.call('consumer_prefix', '--control-id', '190002')
        control = (self.source_dir / 'consumer_prefix_node.cpp').read_text()
        self.assertIn('kUpdatePrefix = 190002;', control)
        self.assertIn('REGISTER_FUNCTION_NODE(consumer_prefix, Spec())', control)

    def test_nonstandard_output_is_explicitly_not_collected_and_symlink_is_never_overwritten(self):
        result = self.call('consumer_external', '--output-dir', 'external')
        self.assertIn('build does not collect it', result.stdout)
        self.assertTrue((self.root / 'external/consumer_external_node.cpp').exists())
        self.source_dir.mkdir(parents=True)
        outside = self.root / 'user.cpp'
        outside.write_text('user source')
        (self.source_dir / 'consumer_link_node.cpp').symlink_to(outside)
        self.call('consumer_link', '--force', expected=2)
        self.assertEqual(outside.read_text(), 'user source')
        self.assertTrue((self.source_dir / 'consumer_link_node.cpp').is_symlink())

    def test_actual_link_failure_rolls_back_created_files_and_staging(self):
        self.source_dir.mkdir(parents=True)
        first = self.source_dir / 'first.cpp'
        second = self.source_dir / 'second.cpp'
        plan = scaffold.ChangePlan()
        plan.add_new_file(first, 'first generated')
        plan.add_new_file(second, 'second generated')
        link = scaffold.os.link

        def fail_second(source, target, **options):
            if Path(target) == second:
                raise OSError('publication failed')
            return link(source, target, **options)

        with mock.patch.object(scaffold.os, 'link', side_effect=fail_second):
            with self.assertRaisesRegex(OSError, 'publication failed'):
                plan.commit()
        self.assertFalse(first.exists())
        self.assertFalse(second.exists())
        self.assertFalse(list(self.source_dir.glob('*.edgeflow-tmp')))

    def test_concurrent_modification_is_preserved_and_new_file_is_rolled_back(self):
        self.source_dir.mkdir(parents=True)
        existing = self.source_dir / 'registration.cpp'
        created = self.source_dir / 'new.cpp'
        existing.write_text('before')
        plan = scaffold.ChangePlan()
        plan.add_new_file(created, 'generated')
        plan.add_modification(existing, 'before', 'after')
        existing.write_text('user edit after planning')
        with self.assertRaisesRegex(RuntimeError, 'Concurrent edit'):
            plan.commit()
        self.assertEqual(existing.read_text(), 'user edit after planning')
        self.assertFalse(created.exists())
        self.assertFalse(list(self.source_dir.glob('*.edgeflow-tmp')))

    def test_new_file_race_preserves_competing_file_and_never_replaces_it(self):
        self.source_dir.mkdir(parents=True)
        target = self.source_dir / 'new.cpp'
        plan = scaffold.ChangePlan()
        plan.add_new_file(target, 'generated')
        target.write_text('competing user creation')
        with self.assertRaises(FileExistsError):
            plan.commit()
        self.assertEqual(target.read_text(), 'competing user creation')
        self.assertFalse(list(self.source_dir.glob('*.edgeflow-tmp')))


if __name__ == '__main__':
    unittest.main()
