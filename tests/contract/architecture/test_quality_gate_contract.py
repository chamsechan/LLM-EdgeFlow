#!/usr/bin/env python3
"""Canonical gate failure propagation and the real GoogleTest delivery guard."""

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
HARNESS = None


class BackendSelectionContractTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='backend-selection-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        shutil.copy2(ROOT / 'CMakePresets.json', self.root)
        shutil.copy2(ROOT / 'cmake_ext/BackendOptions.cmake', self.root)
        shutil.copy2(ROOT / 'cmake_ext/PlatformOptions.cmake', self.root)
        (self.root / 'CMakeLists.txt').write_text(
            'cmake_minimum_required(VERSION 3.19)\nproject(BackendSelection NONE)\n'
            'set(CMAKE_SYSTEM_NAME ${TEST_SYSTEM})\n'
            'set(CMAKE_SYSTEM_PROCESSOR ${TEST_PROCESSOR})\n'
            'set(CMAKE_HOST_SYSTEM_NAME ${TEST_HOST_SYSTEM})\n'
            'set(CMAKE_HOST_SYSTEM_PROCESSOR ${TEST_HOST_PROCESSOR})\n'
            'if(TEST_CROSS)\nset(CMAKE_CROSSCOMPILING TRUE)\nendif()\n'
            'include(PlatformOptions.cmake)\ninclude(BackendOptions.cmake)\n')

    def configure(self, *arguments, expected=None):
        result = subprocess.run(
            ['cmake', '-S', str(self.root), '-B', str(self.root / 'build'),
             '-DTEST_SYSTEM=Linux', '-DTEST_PROCESSOR=x86_64',
             '-DTEST_HOST_SYSTEM=Linux', '-DTEST_HOST_PROCESSOR=x86_64', *arguments],
            cwd=self.root, capture_output=True, text=True, timeout=30)
        if expected is None:
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            enabled = {line.split(':')[0] for line in (self.root / 'build/CMakeCache.txt').read_text().splitlines()
                       if line.startswith('ENABLE_') and line.endswith(':BOOL=ON')}
            self.assertEqual(enabled, set(expected))
        return result.stdout + result.stderr

    def test_plain_defaults_and_explicit_opt_in(self):
        self.configure(expected={'ENABLE_INFERLET'})
        self.configure('-DENABLE_ONNXRUNTIME=ON', expected={'ENABLE_INFERLET', 'ENABLE_ONNXRUNTIME'})
        self.configure('-DENABLE_INFERLET=OFF', '-DENABLE_ONNXRUNTIME=OFF', expected=set())
        self.configure('-DENABLE_LLAMACPP=ON', expected={'ENABLE_LLAMACPP'})
        self.configure('-DENABLE_WHISPERCPP=ON', expected={'ENABLE_LLAMACPP', 'ENABLE_WHISPERCPP'})

    def test_presets_reset_cached_engines_and_match_selection_metadata(self):
        backends = {'inferlet': 'INFERLET', 'onnxruntime': 'ONNXRUNTIME',
                    'llama_cpp': 'LLAMACPP', 'whisper_cpp': 'WHISPERCPP', 'kite_llm': 'KITELLM'}
        presets = json.loads((self.root / 'CMakePresets.json').read_text())['configurePresets']
        # 先启用全部独立引擎，再复用目录，确保默认门禁不会继承旧 ONNX/llama/Whisper。
        self.configure('--preset=full-cpu', expected={'ENABLE_ONNXRUNTIME', 'ENABLE_LLAMACPP', 'ENABLE_WHISPERCPP'})
        self.configure('--preset=dev-gate', expected={'ENABLE_INFERLET'})
        for preset in presets:
            selection = preset.get('vendor', {}).get('llm-edgeflow/selection')
            if selection is not None:
                with self.subTest(preset=preset['name']):
                    shutil.rmtree(self.root / 'build', ignore_errors=True)
                    self.configure('--preset=' + preset['name'],
                                   *(['-DTEST_SYSTEM=Darwin', '-DTEST_HOST_SYSTEM=Darwin',
                                      '-DTEST_HOST_PROCESSOR=arm64']
                                     if preset['name'] == 'macos-arm64-cpu' else []),
                                   expected={'ENABLE_' + backends[name] for name in selection['backends']})

    def test_target_platform_validation_and_apple_architecture_override(self):
        self.configure('-DTEST_SYSTEM=Darwin', '-DCMAKE_OSX_ARCHITECTURES=arm64',
                       '-DLLM_EDGEFLOW_EXPECTED_PLATFORM=macos-arm64', expected={'ENABLE_LLAMACPP'})
        output = self.configure('-DTEST_SYSTEM=Darwin', '-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64')
        self.assertIn('one macOS architecture', output)
        output = self.configure('-DTEST_SYSTEM=Darwin', '-DCMAKE_OSX_ARCHITECTURES=arm64',
                                '-DENABLE_INFERLET=ON', '-DENABLE_LLAMACPP=OFF')
        self.assertIn('support Linux', output)

    def test_apple_host_architecture_mismatch_disables_native_tests(self):
        for host_arch, target_arch in (('x86_64', 'arm64'), ('arm64', 'x86_64')):
            with self.subTest(host=host_arch, target=target_arch):
                shutil.rmtree(self.root / 'build', ignore_errors=True)
                arguments = ['-DTEST_SYSTEM=Darwin', '-DTEST_HOST_SYSTEM=Darwin',
                             '-DTEST_HOST_PROCESSOR=' + host_arch,
                             '-DCMAKE_OSX_ARCHITECTURES=' + target_arch]
                self.configure(*arguments, expected={'ENABLE_LLAMACPP'})
                cache = (self.root / 'build/CMakeCache.txt').read_text()
                self.assertIn('BUILD_TESTING:BOOL=OFF', cache)
                output = self.configure(*arguments, '-DBUILD_TESTING=ON')
                self.assertIn('BUILD_TESTING=OFF', output)
        shutil.rmtree(self.root / 'build', ignore_errors=True)
        self.configure('-DTEST_SYSTEM=Darwin', '-DTEST_HOST_SYSTEM=Darwin',
                       '-DTEST_HOST_PROCESSOR=aarch64', '-DCMAKE_OSX_ARCHITECTURES=arm64',
                       '-DBUILD_TESTING=ON', expected={'ENABLE_LLAMACPP'})

    def test_unsupported_and_mismatched_platforms_fail_early(self):
        for arguments, diagnostic in (
                (['-DCMAKE_CONFIGURATION_TYPES=Debug;Release'], 'single-config'),
                (['-DTEST_SYSTEM=Windows'], 'Unsupported target system'),
                (['-DTEST_PROCESSOR=riscv64'], 'Unsupported target architecture'),
                (['-DLLM_EDGEFLOW_EXPECTED_PLATFORM=macos-arm64'], 'Preset requires'),
                (['-DTEST_CROSS=ON', '-DBUILD_TESTING=ON'], 'Cross builds compile target artifacts only')):
            with self.subTest(arguments=arguments):
                shutil.rmtree(self.root / 'build', ignore_errors=True)
                self.assertIn(diagnostic, self.configure(*arguments))

    def test_conflicts_fail_before_dependency_acquisition(self):
        for first, second in [('INFERLET', 'KITELLM'), ('INFERLET', 'LLAMACPP'),
                              ('INFERLET', 'WHISPERCPP'), ('KITELLM', 'LLAMACPP'),
                              ('KITELLM', 'WHISPERCPP')]:
            with self.subTest(first=first, second=second):
                output = self.configure('--preset=minimal', f'-DENABLE_{first}=ON', f'-DENABLE_{second}=ON')
                self.assertIn('conflicting runtimes', ' '.join(output.split()))
        output = self.configure('--preset=minimal', '-DENABLE_WHISPERCPP=ON')
        self.assertIn('requires ENABLE_LLAMACPP=ON', output)


class DeliveryGateConsumerTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='delivery-gate-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        (self.root / 'scripts').mkdir()
        (self.root / 'bin').mkdir()
        (self.root / 'build').mkdir()
        shutil.copy2(ROOT / 'scripts/run_all_tests.sh', self.root / 'scripts/run_all_tests.sh')
        self.trace = self.root / 'trace.txt'
        stub = '''#!/usr/bin/env bash
set -eu
name=$(basename "$0")
printf '%s %s\\n' "$name" "$*" >> "$CALL_TRACE"
case "$name:$*" in
  cmake:--build*) stage=build ;;
  configure_build.sh:*) stage=configure ;;
  format.sh:*) stage=format ;;
  *) stage=$name ;;
esac
if [[ "${FAIL_AT:-}" == "$stage" ]]; then exit 23; fi
'''
        for path in ('scripts/format.sh', 'scripts/configure_build.sh', 'bin/cmake', 'bin/ctest', 'bin/git'):
            target = self.root / path
            target.write_text(stub)
            target.chmod(0o755)

    def gate(self, failure='', preset='dev-gate'):
        return subprocess.run(['bash', str(self.root / 'scripts/run_all_tests.sh')], cwd=self.root,
                              env={**os.environ, 'PATH': str(self.root / 'bin') + os.pathsep + os.environ['PATH'],
                                   'CALL_TRACE': str(self.trace), 'FAIL_AT': failure, 'LLM_EDGEFLOW_BUILD_PRESET': preset, 'LLM_EDGEFLOW_JOBS': '2'},
                              capture_output=True, text=True, timeout=20)

    def test_canonical_gate_selects_one_build_and_all_ctest_cases(self):
        result = self.gate()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        calls = self.trace.read_text().splitlines()
        self.assertEqual([line.split()[0] for line in calls],
                         ['format.sh', 'git', 'configure_build.sh', 'cmake', 'ctest'])
        self.assertIn('dev-gate', calls[2])
        self.assertIn('--build', calls[3])
        self.assertIn('--no-tests=error', calls[4])
        self.assertNotIn(' -R ', calls[4])
        self.assertIn('All required development gates passed', result.stdout)

    def test_selected_preset_is_forwarded_without_disabling_gate_coverage(self):
        result = self.gate(preset='macos-arm64-cpu')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        configure = self.trace.read_text().splitlines()[2]
        self.assertIn('macos-arm64-cpu', configure)
        self.assertIn('-DBUILD_TESTING=ON', configure)

    def test_each_failed_stage_stops_later_commands_and_success_reporting(self):
        for stage, final_command in (('format', 'format.sh'), ('git', 'git'),
                                     ('configure', 'configure_build.sh'), ('build', 'cmake'), ('ctest', 'ctest')):
            with self.subTest(stage=stage):
                self.trace.unlink(missing_ok=True)
                result = self.gate(stage)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(self.trace.read_text().splitlines()[-1].split()[0], final_command)
                self.assertNotIn('All required development gates passed', result.stdout)

    def test_shell_syntax_failure_stops_before_any_build_transport(self):
        (self.root / 'scripts/broken.sh').write_text('if then\n')
        result = self.gate()
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.trace.exists())

    def test_sdk_export_guard_rejects_expanded_or_replaced_public_symbols(self):
        self.assertIsNotNone(HARNESS, 'CTest must supply the compiler')
        symbols = [
            'AlgBase_setLogLevelByName', 'AlgBase_getLogLevelByName',
            '_ZN12llm_edgeflow12operator_api30Get_LLM_EDGEFLOW_OperatorTableEv',
            '_ZN12llm_edgeflow12operator_api20GetOperatorLastErrorEv',
            '_ZN12llm_edgeflow12operator_api23ResolveOperatorConfigIoEPKcS2_PNS0_18OperatorIoContractEPcm',
        ]
        script = self.root / 'scripts/check_sdk_exports.sh'
        shutil.copy2(ROOT / 'scripts/check_sdk_exports.sh', script)
        manifest = self.root / 'cmake_ext/templates/edgeflow_sdk.exports'
        manifest.parent.mkdir(parents=True)
        binary_format = 'MachO' if sys.platform == 'darwin' else 'ELF'
        nm = shutil.which('nm')
        self.assertIsNotNone(nm)
        for name, exports, expected in (
                ('supported', symbols, 0),
                ('expanded', symbols + ['UnexpectedSdkExport'], 1),
                ('replaced', symbols[:-1] + ['UnexpectedSdkExport'], 1)):
            with self.subTest(case=name):
                # Also change the linker manifest: the independent public ABI
                # guard must still reject a coordinated manifest/library drift.
                manifest.write_text('\n'.join(exports) + '\n')
                source = self.root / (name + '.cpp')
                source.write_text('\n'.join('extern "C" void ' + symbol + '() {}' for symbol in exports))
                library = self.root / (name + ('.dylib' if sys.platform == 'darwin' else '.so'))
                compiled = subprocess.run(
                    [HARNESS.cxx, '-dynamiclib' if sys.platform == 'darwin' else '-shared',
                     '-fPIC', str(source), '-o', str(library)],
                    capture_output=True, text=True, timeout=60)
                self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
                checked = subprocess.run(['bash', str(script), str(library), binary_format, nm],
                                         capture_output=True, text=True, timeout=30)
                self.assertEqual(checked.returncode, expected, checked.stdout + checked.stderr)

    def test_real_gtest_main_rejects_failures_required_skips_and_zero_selected_cases(self):
        self.assertIsNotNone(HARNESS, 'CTest must supply the real compiler/GTest/main arguments')
        source = self.root / 'guard.cpp'
        source.write_text('''#include <gtest/gtest.h>
TEST(Guard, Pass) { EXPECT_EQ(1, 1); }
TEST(Guard, Fail) { EXPECT_EQ(1, 2); }
TEST(Guard, RequiredSkip) { GTEST_SKIP() << "required behavior unavailable"; }
TEST(Guard, EmptyAssetMarker) {
  RecordProperty("optional_runtime_asset", "");
  GTEST_SKIP() << "empty marker cannot excuse a required skip";
}
class OptionalTeardownFailure : public ::testing::Test {
 protected:
  static void TearDownTestSuite() { ADD_FAILURE() << "real suite cleanup failure"; }
};
TEST_F(OptionalTeardownFailure, SkippedAsset) {
  RecordProperty("optional_runtime_asset", "weight.gguf");
  GTEST_SKIP() << "asset missing";
}
TEST(Guard, SkipFirstIteration) {
  static int iteration = 0;
  if (++iteration == 1) GTEST_SKIP() << "required first iteration skipped";
}
TEST(Guard, OptionalAsset) {
  RecordProperty("optional_runtime_asset", "missing-weight.gguf");
  GTEST_SKIP() << "explicit optional physical weight";
}
''')
        executable = self.root / 'guard'
        command = [HARNESS.cxx, '-std=c++17', '-pthread']
        if HARNESS.sanitizers:
            command += [f'-fsanitize={HARNESS.sanitizers}', '-fno-omit-frame-pointer']
        for directory in HARNESS.gtest_include:
            for include in directory.split(';'):
                command += ['-I', include]
        command += [str(source), str(HARNESS.gtest_main), str(HARNESS.gtest_library), '-o', str(executable)]
        compiled = subprocess.run(command, capture_output=True, text=True, timeout=60)
        self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
        registrations = self.root / 'registrations'
        registrations.mkdir()
        for name, argument, property_, diagnostic in (
                ('enabled', '', '', ''),
                ('execution_zero', '--gtest_list_tests=0', '', ''),
                ('execution_false', '--gtest_list_tests=false', '', ''),
                ('disabled', '', 'DISABLED TRUE', 'Registered CTest entry is disabled'),
                ('list_only', '--gtest_list_tests', '', 'CTest execution command must not list GoogleTest cases'),
                ('list_valued', '--gtest_list_tests=1', '', 'CTest execution command must not list GoogleTest cases')):
            with self.subTest(registration=name):
                (registrations / 'CTestTestfile.cmake').write_text(
                    f'add_test(registered "{executable}" --gtest_filter=* {argument})\n'
                    f'set_tests_properties(registered PROPERTIES LABELS tier1 SKIP_RETURN_CODE 77 {property_})\n'
                    'add_test(other cmake -E true)\n'
                    'set_tests_properties(other PROPERTIES LABELS tier1)\n')
                inspected = subprocess.run(
                    [sys.executable, str(ROOT / 'tests/contract/architecture/test_test_labels_contract.py'),
                     '--build-dir', str(registrations)], capture_output=True, text=True, timeout=30)
                self.assertEqual(inspected.returncode, 1 if diagnostic else 0,
                                 inspected.stdout + inspected.stderr)
                if diagnostic:
                    self.assertIn(diagnostic, inspected.stdout + inspected.stderr)
        for name, expected in (('Pass', 0), ('Fail', 1), ('RequiredSkip', 1),
                               ('EmptyAssetMarker', 1), ('OptionalAsset', 77), ('Absent', 1)):
            with self.subTest(name=name):
                result = subprocess.run([str(executable), f'--gtest_filter=Guard.{name}'], capture_output=True, text=True)
                self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
        for arguments in (['--gtest_filter=OptionalTeardownFailure.*'],
                          ['--gtest_filter=Guard.Pass', '--gtest_repeat=0'],
                          ['--gtest_filter=Guard.SkipFirstIteration', '--gtest_repeat=2']):
            result = subprocess.run([str(executable), *arguments], capture_output=True, text=True)
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        driver = self.root / 'ctest'
        driver.mkdir()
        (driver / 'CTestTestfile.cmake').write_text(
            f'include("{HARNESS.ctest_policy}")\n'
            f'add_test(required "{executable}" --gtest_filter=Guard.RequiredSkip)\n'
            'set_tests_properties(required PROPERTIES SKIP_RETURN_CODE 77 '
            'SKIP_REGULAR_EXPRESSION "SKIPPED")\n'
            f'add_test(optional "{executable}" --gtest_filter=Guard.OptionalAsset)\n'
            'set_tests_properties(optional PROPERTIES SKIP_RETURN_CODE 77 '
            'SKIP_REGULAR_EXPRESSION "SKIPPED")\n')
        for name, code in (('required', 8), ('optional', 0)):
            checked = subprocess.run(['ctest', '-R', f'^{name}$', '--output-on-failure', '--no-tests=error'],
                                     cwd=driver, capture_output=True, text=True)
            self.assertEqual(checked.returncode, code, checked.stdout + checked.stderr)
        self.assertIn('Skipped', checked.stdout)
        listing = subprocess.run([str(executable), '--gtest_list_tests'],
                                 env={**os.environ, 'GTEST_FILTER': 'Guard.Pass'}, capture_output=True, text=True)
        self.assertEqual(listing.returncode, 0, listing.stderr)
        self.assertIn('RequiredSkip', listing.stdout)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--cxx', required=True)
    parser.add_argument('--sanitizers', default='')
    parser.add_argument('--gtest-include', action='append', required=True)
    parser.add_argument('--gtest-library', type=Path, required=True)
    parser.add_argument('--gtest-main', type=Path, required=True)
    parser.add_argument('--ctest-policy', type=Path, required=True)
    HARNESS, remaining = parser.parse_known_args()
    unittest.main(argv=[sys.argv[0], *remaining])
