#!/usr/bin/env python3
"""Inspect current CTest metadata and discover actual selected GoogleTest cases."""

import argparse
import fnmatch
import json
from pathlib import Path
import subprocess
import unittest

BUILD = None


class TestRegistrationConsumerTest(unittest.TestCase):
    def test_current_registrations_have_one_tier_and_nonempty_actual_gtest_filters(self):
        process = subprocess.run(['ctest', '--show-only=json-v1'], cwd=BUILD,
                                 capture_output=True, text=True, check=True)
        tests = json.loads(process.stdout)['tests']
        self.assertTrue(tests, 'CTest discovery must not be empty')
        names = [test['name'] for test in tests]
        self.assertEqual(len(names), len(set(names)))
        discovery = {}
        scheduled = {}
        for test in tests:
            with self.subTest(test=test['name']):
                properties = {item['name']: item['value'] for item in test.get('properties', [])}
                self.assertFalse(properties.get('DISABLED', False), 'Registered CTest entry is disabled')
                labels = set(properties.get('LABELS', []))
                self.assertEqual(len(labels & {'tier1', 'tier2', 'tier3', 'tier4'}), 1)
                command = test.get('command', [])
                self.assertTrue(command, 'registered test needs an executable command')
                listing_flags = [argument.partition('=') for argument in command
                                 if argument.split('=', 1)[0] == '--gtest_list_tests']
                self.assertFalse(any(not separator or value[:1] not in ('0', 'f', 'F')
                                     for _, separator, value in listing_flags),
                                 'CTest execution command must not list GoogleTest cases')
                filters = [argument.split('=', 1)[1] for argument in command if argument.startswith('--gtest_filter=')]
                if not filters:
                    continue
                self.assertEqual(properties.get('SKIP_RETURN_CODE'), 77)
                self.assertNotIn('SKIP_REGULAR_EXPRESSION', properties)
                executable = command[0]
                if executable not in discovery:
                    listed = subprocess.run([executable, '--gtest_list_tests', '--gtest_filter=*'], cwd=properties.get('WORKING_DIRECTORY', BUILD),
                                            capture_output=True, text=True, timeout=30)
                    self.assertEqual(listed.returncode, 0, listed.stdout + listed.stderr)
                    cases, suite = [], ''
                    for line in listed.stdout.splitlines():
                        text = line.split('#', 1)[0].strip()
                        if not text:
                            continue
                        if not line.startswith(' ') and text.endswith('.'):
                            suite = text
                        elif line.startswith('  ') and suite:
                            cases.append(suite + text)
                    discovery[executable] = cases
                    scheduled[executable] = set()
                for expression in filters:
                    positive, _, negative = expression.partition('-')
                    include = positive.split(':') if positive else ['*']
                    exclude = negative.split(':') if negative else []
                    selected = [case for case in discovery[executable]
                                if any(fnmatch.fnmatchcase(case, pattern) for pattern in include)
                                and not any(fnmatch.fnmatchcase(case, pattern) for pattern in exclude)]
                    self.assertTrue(selected, f'Current filter selects no cases: {expression}')
                    duplicate = scheduled[executable].intersection(selected)
                    self.assertFalse(duplicate, f'Cases scheduled twice: {sorted(duplicate)}')
                    scheduled[executable].update(selected)
        for executable, cases in discovery.items():
            with self.subTest(executable=executable):
                self.assertEqual(set(cases), scheduled[executable],
                                 f'Compiled cases without a CTest owner: {sorted(set(cases) - scheduled[executable])}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--build-dir', type=Path, required=True)
    arguments = parser.parse_args()
    BUILD = arguments.build_dir.resolve()
    unittest.main(argv=['test_test_labels_contract.py'])
