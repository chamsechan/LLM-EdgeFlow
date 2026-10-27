#!/usr/bin/env python3
"""检查当前 CTest 元数据中的层级、启用条件和跳过策略。"""

import argparse
import json
from pathlib import Path
import subprocess
import unittest

BUILD = None


class TestRegistrationConsumerTest(unittest.TestCase):
    def test_current_registrations_have_one_tier_and_executable_skip_policy(self):
        process = subprocess.run(['ctest', '--show-only=json-v1'], cwd=BUILD,
                                 capture_output=True, text=True, check=True)
        tests = json.loads(process.stdout)['tests']
        self.assertTrue(tests, 'CTest discovery must not be empty')
        names = [test['name'] for test in tests]
        self.assertEqual(len(names), len(set(names)))
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
                if any(argument.startswith('--gtest_filter=') for argument in command):
                    self.assertEqual(properties.get('SKIP_RETURN_CODE'), 77)
                    self.assertNotIn('SKIP_REGULAR_EXPRESSION', properties)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--build-dir', type=Path, required=True)
    arguments = parser.parse_args()
    BUILD = arguments.build_dir.resolve()
    unittest.main(argv=['test_test_labels_contract.py'])
