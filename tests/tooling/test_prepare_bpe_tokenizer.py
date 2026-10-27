#!/usr/bin/env python3
"""tools/prepare_bpe_tokenizer.py：不支持的组件、score 前置条件与确定性。

只用标准库：不需要 tokenizers 包、Unicode 数据或网络。
"""

import copy
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools/prepare_bpe_tokenizer.py"
SPEC = importlib.util.spec_from_file_location("prepare_bpe_tokenizer_under_test", SCRIPT)
tool = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(tool)

SPACE = "▁"


def tiny_metaspace_json(merges=None, extra_vocab=()):
    """最小的受支持字符级 BPE：256 个字节回退 token 加 a、b、c 及其合并结果。"""
    tokens = ["<unk>"] + [f"<0x{b:02X}>" for b in range(256)] + [SPACE, "a", "b", "c"]
    tokens += ["ab", "bc", "abc"] + list(extra_vocab)
    if merges is None:
        # 全部“两段与结果都在词表中”的切分。
        # 同时满足 score 的排序条件：同分时左段码点数多的在前。
        merges = [["a", "b"], ["b", "c"], ["ab", "c"], ["a", "bc"]]
    return {
        "version": "1.0", "truncation": None, "padding": None,
        "added_tokens": [{"id": 0, "content": "<unk>", "single_word": False, "lstrip": False,
                          "rstrip": False, "normalized": False, "special": True}],
        "normalizer": {"type": "Replace", "pattern": {"String": " "}, "content": SPACE},
        "pre_tokenizer": None,
        "post_processor": None,
        "decoder": {"type": "Sequence", "decoders": [
            {"type": "Replace", "pattern": {"String": SPACE}, "content": " "},
            {"type": "ByteFallback"}, {"type": "Fuse"}]},
        "model": {"type": "BPE", "dropout": None, "unk_token": "<unk>",
                  "continuing_subword_prefix": None, "end_of_word_suffix": None,
                  "fuse_unk": True, "byte_fallback": True, "ignore_merges": False,
                  "vocab": {token: index for index, token in enumerate(tokens)},
                  "merges": merges},
    }


def varint(value):
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        out.append(byte | (0x80 if value else 0))
        if not value:
            return bytes(out)


def sentencepiece_model(pieces, scores, types=None):
    """手写的最小 ModelProto：每个 piece 只含 piece、score、type，外加一个要被跳过的字段。"""
    body = bytearray()
    for index, piece in enumerate(pieces):
        entry = b"\x0a" + varint(len(piece.encode())) + piece.encode()
        entry += b"\x15" + struct.pack("<f", scores[index])
        piece_type = 1 if types is None else types[index]
        if piece_type != 1:
            entry += b"\x18" + varint(piece_type)
        entry += b"\x20\x01"  # 未知的 varint 字段 4，必须被跳过
        body += b"\x0a" + varint(len(entry)) + entry
    body += b"\x12\x02\x08\x01"  # trainer_spec 等其它字段
    return bytes(body)


class ToolTestCase(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="bpe-tool-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def write_json(self, name, value):
        path = self.root / name
        path.write_text(json.dumps(value, ensure_ascii=False), encoding="utf-8")
        return path

    def generation_config(self):
        return self.write_json("generation_config.json", {"eos_token_id": 0})

    def run_tool(self, *arguments, expected=0):
        result = subprocess.run([sys.executable, str(SCRIPT), *map(str, arguments)],
                                capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
        return result

    def compile(self, data, *extra, expected=0, name="tokenizer.json"):
        source = self.write_json(name, data)
        output = self.root / "out.eftok"
        output.unlink(missing_ok=True)
        result = self.run_tool("compile", "--tokenizer-json", source, "--generation-config",
                               self.generation_config(), "--output", output, *extra,
                               expected=expected)
        return result, output


def header_words(content):
    return struct.unpack("<40I", content[:160])


class FixtureTest(ToolTestCase):
    def test_fixtures_are_deterministic_and_well_formed(self):
        # （merge_priority：0 rank、1 score；pre_tokenizer：0 none、1 byte_level_qwen2）
        expected_parameters = {"metaspace": (1, 0), "byte_level": (0, 1)}
        for kind, (priority, pre_tokenizer) in expected_parameters.items():
            with self.subTest(kind):
                first, second = self.root / f"{kind}-1.eftok", self.root / f"{kind}-2.eftok"
                self.run_tool("fixture", kind, "--output", first)
                self.run_tool("fixture", kind, "--output", second)
                content = first.read_bytes()
                self.assertEqual(content, second.read_bytes())
                self.assertEqual(content[:8], b"EFBPE\r\n\0")
                words = header_words(content)
                self.assertEqual(words[2], len(content))
                self.assertEqual(words[3], zlib.crc32(content[16:]))
                self.assertEqual((words[7], words[5]), (priority, pre_tokenizer))
                # 区段 4 字节对齐并首尾相接。
                self.assertTrue(all(offset % 4 == 0 for offset in words[24:]))
                self.assertEqual(sorted(words[24:]), list(words[24:]))

    def test_fixture_in_place_without_other_output(self):
        self.run_tool("fixture", "metaspace", "--output", self.root / "nested/dir/m.eftok")
        self.assertTrue((self.root / "nested/dir/m.eftok").is_file())


class CompileTest(ToolTestCase):
    def test_baseline_compiles_with_rank_priority(self):
        result, output = self.compile(tiny_metaspace_json())
        self.assertIn("sha256", result.stdout)
        words = header_words(output.read_bytes())
        self.assertEqual((words[4], words[5], words[6], words[7], words[8]), (1, 0, 1, 0, 0))
        again = self.root / "again.eftok"
        self.run_tool("compile", "--tokenizer-json", self.root / "tokenizer.json",
                      "--generation-config", self.root / "generation_config.json",
                      "--output", again)
        self.assertEqual(output.read_bytes(), again.read_bytes())

    def assert_rejected(self, data, *needles, **kwargs):
        result, output = self.compile(data, **kwargs, expected=1)
        for needle in needles:
            self.assertIn(needle, result.stderr)
        self.assertTrue(result.stderr.startswith("error:"), result.stderr)
        self.assertFalse(output.exists(), "a rejected input must not leave an output file")

    def test_rejects_unsupported_model_fields(self):
        def mutated(path, value):
            data = tiny_metaspace_json()
            data["model"][path] = value
            return data
        self.assert_rejected(mutated("dropout", 0.1), "dropout")
        self.assert_rejected(mutated("continuing_subword_prefix", "##"), "continuing_subword_prefix")
        self.assert_rejected(mutated("end_of_word_suffix", "</w>"), "end_of_word_suffix")
        self.assert_rejected(mutated("ignore_merges", True), "ignore_merges")
        self.assert_rejected(mutated("type", "Unigram"), "model type")
        self.assert_rejected(mutated("byte_fallback", False), "combination")
        # 空字符串与 null 等价，是被接受的。
        for field in ("continuing_subword_prefix", "end_of_word_suffix"):
            self.compile(mutated(field, ""))

    def test_rejects_unsupported_added_token_flags(self):
        for flag in ("lstrip", "rstrip", "single_word", "normalized"):
            with self.subTest(flag):
                data = tiny_metaspace_json()
                data["added_tokens"][0][flag] = True
                self.assert_rejected(data, flag)

    def test_rejects_unsupported_components_and_combinations(self):
        data = tiny_metaspace_json()
        data["normalizer"] = {"type": "NFKC"}
        self.assert_rejected(data, "normalizer")

        data = tiny_metaspace_json()
        data["normalizer"] = None
        self.assert_rejected(data, "normalizer")

        data = tiny_metaspace_json()
        data["pre_tokenizer"] = {"type": "Whitespace"}
        self.assert_rejected(data, "pre_tokenizer")

        data = tiny_metaspace_json()
        data["decoder"] = {"type": "WordPiece", "prefix": "##", "cleanup": True}
        self.assert_rejected(data, "decoder")

        data = tiny_metaspace_json()
        data["truncation"] = {"max_length": 8}
        self.assert_rejected(data, "truncation")

        # 每个组件单独看都受支持，但组合不是受支持的两种之一。
        data = tiny_metaspace_json()
        data["decoder"] = {"type": "ByteLevel", **tool.BYTE_LEVEL_OPTIONS}
        self.assert_rejected(data, "combination", "decoder=byte_level")

    def test_rejects_a_regex_that_differs_from_the_known_pattern(self):
        data = tiny_metaspace_json()
        data["normalizer"] = {"type": "NFC"}
        data["model"]["byte_fallback"] = False
        data["decoder"] = {"type": "ByteLevel", **tool.BYTE_LEVEL_OPTIONS}
        pre = copy.deepcopy(tool.SUPPORTED_PRE_BYTE_LEVEL)
        data["pre_tokenizer"] = pre
        # 正则被改动一个字符。
        pre["pretokenizers"][0]["pattern"]["Regex"] = pre["pretokenizers"][0]["pattern"]["Regex"].replace(
            "{L}+", "{L}*")
        self.assert_rejected(data, "pre_tokenizer", "regex")
        # 行为不同同样不被接受。
        pre["pretokenizers"][0]["pattern"]["Regex"] = tool.BYTE_LEVEL_QWEN2_REGEX
        pre["pretokenizers"][0]["behavior"] = "Removed"
        self.assert_rejected(data, "pre_tokenizer")

    def test_unicode_tables_must_be_supplied_when_needed(self):
        alphabet = tool.byte_alphabet()
        data = tiny_metaspace_json()
        data["normalizer"] = {"type": "NFC"}
        data["pre_tokenizer"] = tool.SUPPORTED_PRE_BYTE_LEVEL
        data["decoder"] = {"type": "ByteLevel", **tool.BYTE_LEVEL_OPTIONS}
        data["added_tokens"] = []
        data["model"].update(byte_fallback=False, unk_token=None, fuse_unk=False, merges=[],
                             vocab={char: index for index, char in enumerate(sorted(alphabet))})
        result, output = self.compile(data, expected=1)
        self.assertIn("--unicode-data", result.stderr)
        self.assertFalse(output.exists())

    def test_rank_priority_limits(self):
        # 超过 255 字节的 token 在 rank 模式下无法表示左段长度。
        left, right = "a" * 150, "b" * 150
        data = tiny_metaspace_json(
            merges=[["a", "b"], ["b", "c"], ["ab", "c"], ["a", "bc"], [left, right]],
            extra_vocab=[left, right, left + right])
        self.assert_rejected(data, "255 bytes")

        # 排名上限由 MAX_RANK 给出；在副本里压低它，不必构造 2^24 条规则。
        original = tool.MAX_RANK
        tool.MAX_RANK = 2
        try:
            with self.assertRaises(tool.ToolError) as raised:
                tool.build_rank_priority(
                    tool.Model(), {"a": 0, "b": 1, "ab": 2},
                    [("a", "b")] * 4)
            self.assertIn("2^24", str(raised.exception))
        finally:
            tool.MAX_RANK = original

    def test_rejects_malformed_merges(self):
        for merges, needle in [
                ([["a", "b"], ["a", "b"]], "duplicate merge"),
                ([["a", "zz"]], "outside the vocabulary"),
                ([["b", "a"]], "outside the vocabulary"),   # ba 不在词表中
                ([["a b c"]], "malformed merge")]:
            with self.subTest(merges=merges):
                self.assert_rejected(tiny_metaspace_json(merges=merges), needle)


class VerifyDriverTest(ToolTestCase):
    """verify 用 run_driver 取回驱动输出：缺行、多行和 ERR 都必须失败。"""

    def driver(self, body):
        path = self.root / "driver.sh"
        path.write_text("#!/bin/sh\n" + body)
        path.chmod(0o755)
        return path

    def test_complete_output_is_returned(self):
        driver = self.driver("while read line; do echo 5; done\n")
        self.assertEqual(tool.run_driver(driver, "unused.eftok", ["E 61", "E 62", "D 1"]), ["5", "5", "5"])
        self.assertEqual(tool.run_driver(driver, "unused.eftok", ["E "]), ["5"])

    def test_missing_extra_and_error_lines_fail(self):
        requests = ["E 61", "E 62", "D 1"]
        cases = {
            "missing": ("read line; echo 5\n", "1 lines for 3 requests"),
            "extra": ("while read line; do echo 5; done; echo 9\n", "4 lines for 3 requests"),
            "empty": ("exit 0\n", "0 lines for 3 requests"),
            "error": ("while read line; do echo ERR; done\n", "reported an error for request 0"),
            "late error": ("n=0; while read line; do n=$((n+1)); "
                           "if [ $n = 3 ]; then echo ERR; else echo 5; fi; done\n",
                           "reported an error for request 2"),
            "no trailing newline": ("printf '5\\n5\\n5'\n", "newline"),
            "driver failure": ("echo broken >&2; exit 3\n", "driver failed: broken"),
        }
        for name, (body, message) in cases.items():
            with self.subTest(name):
                with self.assertRaises(tool.ToolError) as raised:
                    tool.run_driver(self.driver(body), "unused.eftok", requests)
                self.assertIn(message, str(raised.exception))
        with self.assertRaises(tool.ToolError):
            tool.run_driver(self.driver("exit 0\n"), "/nonexistent.eftok", requests)


class ByteFallbackVocabularyTest(ToolTestCase):
    def test_byte_tokens_only_in_added_tokens_are_rejected(self):
        # HF 的字节回退只查 model.vocab：把 <0x41> 挪到 added_tokens，"A" 在 HF 里会变成 <unk>。
        data = tiny_metaspace_json(merges=[])
        vocab = data["model"]["vocab"]
        tokens = [token for token in sorted(vocab, key=vocab.get) if token != "<0x41>"]
        data["model"]["vocab"] = {token: index for index, token in enumerate(tokens)}
        data["added_tokens"][0]["id"] = 0
        data["added_tokens"].append({"id": len(tokens), "content": "<0x41>", "single_word": False,
                                     "lstrip": False, "rstrip": False, "normalized": False,
                                     "special": False})
        result, output = self.compile(data, expected=1)
        self.assertIn("256 <0xXX> tokens in the BPE vocabulary", result.stderr)
        self.assertIn("added_tokens do not count", result.stderr)
        self.assertFalse(output.exists())
        # 对照：<0x41> 在 model.vocab 里时可以编译。
        self.compile(tiny_metaspace_json(merges=[]))


class ScorePriorityTest(ToolTestCase):
    def pieces_and_scores(self, data):
        pieces = [token for token, _ in sorted(data["model"]["vocab"].items(), key=lambda kv: kv[1])]
        scores = [0.0 if len(token) == 1 or token.startswith("<") else -float(len(token)) for token in pieces]
        return pieces, scores

    def sentencepiece(self, data, **overrides):
        pieces, scores = self.pieces_and_scores(data)
        for index, value in overrides.get("scores", {}).items():
            scores[index] = value
        path = self.root / "tokenizer.model"
        types = [2 if token == "<unk>" else 6 if token.startswith("<0x") else 1 for token in pieces]
        path.write_bytes(sentencepiece_model(overrides.get("pieces", pieces), scores, types))
        return path

    def test_merges_equal_to_all_splits_use_score(self):
        data = tiny_metaspace_json()
        model = self.sentencepiece(data)
        _, output = self.compile(data, "--sentencepiece-model", model)
        words = header_words(output.read_bytes())
        self.assertEqual(words[7], 1)   # score
        self.assertEqual(words[10], 0)  # score 不携带合并规则
        # 不提供 tokenizer.model 时一律使用 rank。
        _, ranked = self.compile(data)
        self.assertEqual(header_words(ranked.read_bytes())[7], 0)

    def test_missing_split_is_an_error_and_never_falls_back_to_rank(self):
        # 缺少 (ab, c)：abc 还可以切成 ab + c，两段都在词表中。
        data = tiny_metaspace_json(merges=[["a", "b"], ["b", "c"], ["a", "bc"]])
        model = self.sentencepiece(data)
        result, output = self.compile(data, "--sentencepiece-model", model, expected=1)
        self.assertIn("score priority needs merges to equal", result.stderr)
        self.assertIn("1 splits have no merge rule", result.stderr)
        self.assertFalse(output.exists())
        # 同一份输入去掉 tokenizer.model，则用 rank 正常生成。
        _, ranked = self.compile(data)
        self.assertEqual(header_words(ranked.read_bytes())[7], 0)

    def test_abc_counterexample_with_a_different_merge_order_is_rejected(self):
        # 同一份 merges 与全部切分一致，但分数使 bc 先于 ab，而 HF 的顺序是 ab 先于 bc：
        # 只按分数合并会把 "abc" 编码为 a,bc，HF 则是 ab,c。
        data = tiny_metaspace_json()
        vocab = data["model"]["vocab"]
        model = self.sentencepiece(data, scores={vocab["ab"]: -2.0, vocab["bc"]: -1.0, vocab["abc"]: -3.0})
        result, output = self.compile(data, "--sentencepiece-model", model, expected=1)
        self.assertIn("merge order", result.stderr)
        self.assertIn("out of order", result.stderr)
        self.assertFalse(output.exists())
        # 去掉 tokenizer.model 就用 rank，rank 保留 HF 的顺序。
        self.compile(data)

    def test_equal_scores_must_follow_code_point_counts(self):
        # abc 的两条规则分数相同：左段码点数多的 (ab, c) 必须在 (a, bc) 之前。
        data = tiny_metaspace_json(merges=[["a", "b"], ["b", "c"], ["a", "bc"], ["ab", "c"]])
        model = self.sentencepiece(data)
        result, _ = self.compile(data, "--sentencepiece-model", model, expected=1)
        self.assertIn("left code points", result.stderr)
        data = tiny_metaspace_json(merges=[["a", "b"], ["b", "c"], ["ab", "c"], ["a", "bc"]])
        self.compile(data, "--sentencepiece-model", self.sentencepiece(data))

    def test_sentencepiece_must_match_the_vocabulary(self):
        data = tiny_metaspace_json()
        pieces, _ = self.pieces_and_scores(data)
        swapped = list(pieces)
        swapped[-1], swapped[-2] = swapped[-2], swapped[-1]
        model = self.sentencepiece(data, pieces=swapped)
        result, _ = self.compile(data, "--sentencepiece-model", model, expected=1)
        self.assertIn("differs from the vocabulary", result.stderr)

        model = self.sentencepiece(data, pieces=pieces[:-1])
        result, _ = self.compile(data, "--sentencepiece-model", model, expected=1)
        self.assertIn("pieces", result.stderr)

        model = self.sentencepiece(data, scores={len(pieces) - 1: float("inf")})
        result, _ = self.compile(data, "--sentencepiece-model", model, expected=1)
        self.assertIn("non-finite", result.stderr)

    def test_parser_reads_only_piece_score_and_type(self):
        content = sentencepiece_model(["<unk>", "a", "<0x41>"], [0.0, -1.5, 0.0], [2, 1, 6])
        path = self.root / "m.model"
        path.write_bytes(content)
        self.assertEqual(tool.parse_sentencepiece(path),
                         [("<unk>", 0.0, 2), ("a", -1.5, 1), ("<0x41>", 0.0, 6)])
        path.write_bytes(content[:-3] + b"\x12")  # 截断
        with self.assertRaises(tool.ToolError):
            tool.parse_sentencepiece(path)


if __name__ == "__main__":
    unittest.main()
