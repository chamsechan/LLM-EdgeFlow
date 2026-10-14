#!/usr/bin/env python3
"""生成并验证 BPE 分词器文件（`.eftok`）。

文件格式见 doc/dev_guide/bpe_tokenizer_format.md，C++ 读取端是 `text::BpeTokenizer`。
工具只依赖输入文件，不含按模型名组织的配置；分词流程的差别由 HF `tokenizer.json`
中的组件类型决定，并写进文件头的五个参数：

  normalizer      none / replace_space_metaspace / nfc
  pre_tokenizer   none / byte_level_qwen2
  byte_fallback   0 / 1
  merge_priority  rank / score
  decoder         metaspace_byte_fallback / byte_level

子命令：
  compile  --tokenizer-json T --generation-config G [--sentencepiece-model S]
           [--unicode-data U] --output F
      为一个模型生成分词器文件，结果是确定的。提供 S 时使用 score，否则使用 rank。
  fixture {metaspace,byte_level} --output F
      生成合成小词表，只用标准库，供默认门禁使用：metaspace 用 score，
      byte_level 用 rank。
  golden   --tokenizer-json T --output J
      用 HF 生成“文本 -> ID”样例（需要 tokenizers==0.23.3）。
  verify   --tokenizer-json T --tokenizer-file F --driver EXE
      用 C++ 驱动（tests/support/bpe_tokenizer_driver.cpp）与 HF 比对编码和解码
      （需要 tokenizers==0.23.3）。

读取 tokenizer.model 时只解析 piece、score、type 三个 protobuf 字段，用标准库完成。
写 Unicode 区段（normalizer 为 nfc 或 pre_tokenizer 为 byte_level_qwen2）需要
tokenizers==0.23.3，以及 Unicode 9.0.0 的 UnicodeData.txt
(https://www.unicode.org/Public/9.0.0/ucd/UnicodeData.txt)：HF 的规范化器使用 Unicode 9，
字符类别则取自这个版本 HF 后端的正则实现。
"""
import argparse
import hashlib
import json
import math
import random
import re
import struct
import subprocess
import sys
import zlib
from pathlib import Path

MAGIC = b'EFBPE\r\n\0'
HEADER_WORDS = 40
SECTION_COUNT = 16
SECTIONS_WORD = HEADER_WORDS - SECTION_COUNT
CRC_START = 16

NORMALIZERS = {'none': 0, 'replace_space_metaspace': 1, 'nfc': 2}
PRE_TOKENIZERS = {'none': 0, 'byte_level_qwen2': 1}
PRIORITIES = {'rank': 0, 'score': 1}
DECODERS = {'metaspace_byte_fallback': 0, 'byte_level': 1}

KIND_NORMAL, KIND_BYTE_FALLBACK, KIND_SPECIAL = 0, 1, 2
MAX_RANK = (1 << 24) - 1
MAX_RANK_TOKEN_BYTES = 255

# 字符类别位，与 unicode_tables.h 一致。
FLAG_LETTER, FLAG_NUMBER, FLAG_SPACE = 1, 2, 4

SPACE_MARK = '▁'
REQUIRED_TOKENIZERS = '0.23.3'
UNICODE_SHA = '68dfc414d28257b9b5d6ddbb8b466c768c00ebdf6cbf7784364a9b6cad55ee8f'
# 由固定版本 HF 后端和上面的 Unicode 数据生成的表的哈希；换了后端会明确失败。
UNICODE_TABLES_SHA = 'd38d8eb4f89c5057e6f8290096d33313bb36625d5dca22778ded68537290032b'

BYTE_LEVEL_OPTIONS = {'add_prefix_space': False, 'trim_offsets': False, 'use_regex': False}
# 已支持的两种预分词/解码器组合，其余一律报错。
SUPPORTED_NORMALIZER_REPLACE = {'type': 'Replace', 'pattern': {'String': ' '}, 'content': SPACE_MARK}
SUPPORTED_NORMALIZER_NFC = {'type': 'NFC'}
SUPPORTED_DECODER_METASPACE = {'type': 'Sequence', 'decoders': [
    {'type': 'Replace', 'pattern': {'String': SPACE_MARK}, 'content': ' '},
    {'type': 'ByteFallback'}, {'type': 'Fuse'}]}
SUPPORTED_DECODER_BYTE_LEVEL = {'type': 'ByteLevel', **BYTE_LEVEL_OPTIONS}
BYTE_LEVEL_QWEN2_REGEX = (r"(?i:'s|'t|'re|'ve|'m|'ll|'d)|[^\r\n\p{L}\p{N}]?\p{L}+|\p{N}|"
                          r" ?[^\s\p{L}\p{N}]+[\r\n]*|\s*[\r\n]+|\s+(?!\S)|\s+")
SUPPORTED_PRE_BYTE_LEVEL = {'type': 'Sequence', 'pretokenizers': [
    {'type': 'Split', 'pattern': {'Regex': BYTE_LEVEL_QWEN2_REGEX},
     'behavior': 'Isolated', 'invert': False},
    {'type': 'ByteLevel', **BYTE_LEVEL_OPTIONS}]}


class ToolError(Exception):
    """输入不受支持或不一致；消息直接给使用者看。"""


def check(condition, message):
    if not condition:
        raise ToolError(message)


# ---------------------------------------------------------------- 数据模型

class UnicodeData:
    """文件中 Unicode 区段的内容。"""

    def __init__(self, properties=(), decompositions=(), pool=(), combining=(), compositions=()):
        self.properties = list(properties)        # (起, 止, 类别位)
        self.decompositions = list(decompositions)  # (码点, 池偏移, 长度)
        self.pool = list(pool)
        self.combining = list(combining)          # (码点, 组合类)
        self.compositions = list(compositions)    # (首, 次, 合成)


class Model:
    """写出文件所需的全部内容。

    tokens 覆盖全部 ID；hashed[i] 表示第 i 个 token 属于 BPE 词表（可被合并产生），
    只出现在 added_tokens 中的 token 不在其内。
    """

    def __init__(self):
        self.normalizer = 'none'
        self.pre_tokenizer = 'none'
        self.byte_fallback = False
        self.merge_priority = 'rank'
        self.decoder = 'metaspace_byte_fallback'
        self.tokens = []       # list[bytes]
        self.kinds = []        # KIND_*，字节回退 token 的字节值由内容推出
        self.hashed = []       # list[bool]
        self.rank_entries = {}  # id -> [(左段字节长度, 排名)]，rank 模式
        self.scores = []       # list[float]，score 模式
        self.units = []        # [(键, ID)]，按键升序
        self.added = []        # [(bytes, ID)]，特殊 token 匹配表
        self.ends = []
        self.unicode = UnicodeData()


# ---------------------------------------------------------------- 文件写出

def fnv1a(data):
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return value


def slot_count_for(count):
    """不小于 1.5 * count 的最小 2 的幂。"""
    slots = 2
    while slots * 2 < count * 3:
        slots *= 2
    return slots


def pack_u32(values):
    values = list(values)
    return struct.pack(f'<{len(values)}I', *values)


def pack_rows(rows):
    return pack_u32(x for row in rows for x in row)


def build_hash(model):
    entries = [i for i, hashed in enumerate(model.hashed) if hashed]
    slot_count = slot_count_for(len(entries))
    slots = [0] * slot_count
    for token_id in entries:
        position = fnv1a(model.tokens[token_id]) & (slot_count - 1)
        while slots[position]:
            check(model.tokens[slots[position] - 1] != model.tokens[token_id],
                  f'duplicate token bytes in the vocabulary: {model.tokens[token_id]!r}')
            position = (position + 1) & (slot_count - 1)
        slots[position] = token_id + 1
    return slot_count, len(entries), slots


def build_trie(added):
    children, terminals = [{}], [0]
    for content, token_id in added:
        node = 0
        for byte in content:
            if byte not in children[node]:
                children[node][byte] = len(children)
                children.append({})
                terminals.append(0)
            node = children[node][byte]
        check(node != 0 and terminals[node] == 0, f'duplicate or empty added token: {content!r}')
        terminals[node] = token_id + 1
    nodes, edges = [], []
    for child, terminal in zip(children, terminals):
        nodes.append((len(edges), len(child), terminal))
        edges.extend(sorted(child.items()))
    return nodes, edges


def write_file(model):
    """按 doc/dev_guide/bpe_tokenizer_format.md 组装文件内容。"""
    count = len(model.tokens)
    check(count > 0 and len(model.kinds) == count and len(model.hashed) == count,
          'inconsistent token tables')
    rank = model.merge_priority == 'rank'
    offsets, pool = [0], bytearray()
    for token in model.tokens:
        check(token, 'empty token')
        pool.extend(token)
        offsets.append(len(pool))
    for token, kind in zip(model.tokens, model.kinds):
        check(kind != KIND_BYTE_FALLBACK or re.fullmatch(rb'<0x[0-9A-F]{2}>', token),
              f'byte-fallback kind on a token that is not <0xXX>: {token!r}')
    slot_count, hash_entries, slots = build_hash(model)

    rank_offsets, rank_entries = [], []
    if rank:
        for token_id in range(count):
            rank_offsets.append(len(rank_entries))
            entries = sorted(model.rank_entries.get(token_id, []))
            lefts = [left for left, _ in entries]
            check(len(set(lefts)) == len(lefts), 'a token has two merge rules with the same left length')
            for left, merge_rank in entries:
                check(0 < left < len(model.tokens[token_id]), 'merge left part is out of range')
                rank_entries.append((merge_rank << 8) | left)
        rank_offsets.append(len(rank_entries))
    check(rank or len(model.scores) == count, 'score priority needs one score per token')
    check(all(math.isfinite(score) for score in model.scores), 'token score is not finite')
    nodes, edges = build_trie(model.added)
    unicode_data = model.unicode

    sections = [
        pack_u32(offsets), bytes(pool), bytes(model.kinds), pack_u32(slots),
        pack_u32(rank_offsets), pack_u32(rank_entries),
        struct.pack(f'<{count}f', *model.scores) if not rank else b'',
        pack_rows(model.units), pack_rows(nodes), pack_rows((b, t) for b, t in edges),
        pack_u32(model.ends),
        pack_rows(unicode_data.properties), pack_rows(unicode_data.decompositions),
        pack_u32(unicode_data.pool), pack_rows(unicode_data.combining),
        pack_rows(unicode_data.compositions),
    ]
    header = [0] * HEADER_WORDS
    header[4:9] = [NORMALIZERS[model.normalizer], PRE_TOKENIZERS[model.pre_tokenizer],
                   int(model.byte_fallback), PRIORITIES[model.merge_priority],
                   DECODERS[model.decoder]]
    header[9:24] = [count, len(rank_entries), sum(1 for k in model.kinds if k == KIND_SPECIAL),
                    len(model.ends), len(pool), slot_count, hash_entries, len(model.units),
                    len(nodes), len(edges), len(unicode_data.properties),
                    len(unicode_data.decompositions), len(unicode_data.pool),
                    len(unicode_data.combining), len(unicode_data.compositions)]
    body = bytearray(HEADER_WORDS * 4)
    for index, section in enumerate(sections):
        body.extend(b'\0' * (-len(body) % 4))
        header[SECTIONS_WORD + index] = len(body)
        body.extend(section)
    header[2] = len(body)
    body[:HEADER_WORDS * 4] = struct.pack(f'<{HEADER_WORDS}I', *header)
    body[:8] = MAGIC
    header[3] = zlib.crc32(bytes(body[CRC_START:]))
    body[12:16] = struct.pack('<I', header[3])
    return bytes(body)


# ---------------------------------------------------------------- tokenizer.model

def read_varint(data, position):
    result, shift = 0, 0
    while True:
        check(position < len(data), 'truncated protobuf data')
        byte = data[position]
        position += 1
        result |= (byte & 0x7F) << shift
        if not byte & 0x80:
            return result, position
        shift += 7
        check(shift < 70, 'invalid protobuf varint')


def skip_field(data, position, wire_type):
    if wire_type == 0:
        return read_varint(data, position)[1]
    if wire_type == 1:
        position += 8
    elif wire_type == 2:
        length, position = read_varint(data, position)
        position += length
    elif wire_type == 5:
        position += 4
    else:
        raise ToolError('unsupported protobuf wire type')
    check(position <= len(data), 'truncated protobuf data')
    return position


def parse_piece(data):
    piece, score, piece_type = None, 0.0, 1  # 缺省值：空、0、NORMAL
    position = 0
    while position < len(data):
        tag, position = read_varint(data, position)
        field, wire_type = tag >> 3, tag & 7
        if field == 1 and wire_type == 2:
            length, position = read_varint(data, position)
            check(position + length <= len(data), 'truncated protobuf data')
            piece = data[position:position + length].decode('utf-8')
            position += length
        elif field == 2 and wire_type == 5:
            check(position + 4 <= len(data), 'truncated protobuf data')
            score = struct.unpack('<f', data[position:position + 4])[0]
            position += 4
        elif field == 3 and wire_type == 0:
            piece_type, position = read_varint(data, position)
        else:
            position = skip_field(data, position, wire_type)
    check(piece is not None, 'SentencePiece entry has no piece text')
    return piece, score, piece_type


def parse_sentencepiece(path):
    """只解析 ModelProto 中每个 SentencePiece 的 piece、score、type。"""
    data = Path(path).read_bytes()
    pieces, position = [], 0
    while position < len(data):
        tag, position = read_varint(data, position)
        if tag >> 3 == 1 and tag & 7 == 2:
            length, position = read_varint(data, position)
            check(position + length <= len(data), 'truncated protobuf data')
            pieces.append(parse_piece(data[position:position + length]))
            position += length
        else:
            position = skip_field(data, position, tag & 7)
    return pieces


# ---------------------------------------------------------------- HF tokenizer.json

def byte_alphabet():
    """GPT-2 的字节到可见字符映射。"""
    selected = list(range(33, 127)) + list(range(161, 173)) + list(range(174, 256))
    mapped = selected[:]
    extra = 0
    for byte in range(256):
        if byte not in selected:
            selected.append(byte)
            mapped.append(256 + extra)
            extra += 1
    return {chr(c): b for b, c in zip(selected, mapped)}


def load_json(path):
    return json.loads(Path(path).read_text(encoding='utf-8'))


def describe(value):
    return json.dumps(value, ensure_ascii=False)[:200]


def classify_components(data):
    """识别规范化、预分词和解码器，返回各自的枚举名；不支持的组件直接报错。"""
    normalizer = data.get('normalizer')
    if normalizer == SUPPORTED_NORMALIZER_REPLACE:
        normalizer_name = 'replace_space_metaspace'
    elif normalizer == SUPPORTED_NORMALIZER_NFC:
        normalizer_name = 'nfc'
    else:
        raise ToolError(f'unsupported normalizer: {describe(normalizer)}')

    pre = data.get('pre_tokenizer')
    if pre is None:
        pre_name = 'none'
    elif pre == SUPPORTED_PRE_BYTE_LEVEL:
        pre_name = 'byte_level_qwen2'
    else:
        reason = 'unsupported pre_tokenizer'
        if isinstance(pre, dict) and pre.get('type') == 'Sequence':
            parts = pre.get('pretokenizers') or []
            if parts and isinstance(parts[0], dict) and parts[0].get('type') == 'Split':
                reason = 'pre_tokenizer Split regex or options differ from the supported byte_level_qwen2 pattern'
        raise ToolError(f'{reason}: {describe(pre)}')

    decoder = data.get('decoder')
    if decoder == SUPPORTED_DECODER_METASPACE:
        decoder_name = 'metaspace_byte_fallback'
    elif decoder == SUPPORTED_DECODER_BYTE_LEVEL:
        decoder_name = 'byte_level'
    else:
        raise ToolError(f'unsupported decoder: {describe(decoder)}')
    return normalizer_name, pre_name, decoder_name


def check_model_fields(model_json):
    check(model_json.get('type') == 'BPE', f"unsupported model type: {model_json.get('type')!r}")
    check(model_json.get('dropout') is None, 'BPE dropout is not supported')
    for field in ('continuing_subword_prefix', 'end_of_word_suffix'):
        check(not model_json.get(field), f'BPE {field} is not supported')
    check(not model_json.get('ignore_merges'), 'BPE ignore_merges is not supported')


def check_added_tokens(data):
    for token in data.get('added_tokens', []):
        for flag in ('lstrip', 'rstrip', 'single_word', 'normalized'):
            check(not token.get(flag),
                  f"added token {token.get('content')!r}: {flag}=true is not supported")
        check(token.get('content'), 'added token has empty content')


def read_end_tokens(path, total):
    value = load_json(path).get('eos_token_id')
    ends = sorted(set(value if isinstance(value, list) else [value]))
    check(ends and all(isinstance(i, int) and 0 <= i < total for i in ends),
          'generation_config.json eos_token_id is missing or outside the vocabulary')
    return ends


def merge_parts(rule):
    parts = rule.split(' ') if isinstance(rule, str) else rule
    check(isinstance(parts, list) and len(parts) == 2 and all(isinstance(p, str) for p in parts),
          f'malformed merge rule: {describe(rule)}')
    return parts[0], parts[1]


def compile_model(tokenizer_json, generation_config, sentencepiece_model=None, unicode_data=None):
    data = load_json(tokenizer_json)
    check(data.get('truncation') is None and data.get('padding') is None,
          'truncation and padding are not supported')
    model_json = data.get('model') or {}
    check_model_fields(model_json)
    normalizer, pre_tokenizer, decoder = classify_components(data)
    byte_fallback = bool(model_json.get('byte_fallback'))
    supported = {('replace_space_metaspace', 'none', True, 'metaspace_byte_fallback'),
                 ('nfc', 'byte_level_qwen2', False, 'byte_level')}
    check((normalizer, pre_tokenizer, byte_fallback, decoder) in supported,
          'unsupported component combination: '
          f'normalizer={normalizer}, pre_tokenizer={pre_tokenizer}, '
          f'byte_fallback={byte_fallback}, decoder={decoder}')
    check_added_tokens(data)

    out = Model()
    out.normalizer, out.pre_tokenizer, out.byte_fallback, out.decoder = (
        normalizer, pre_tokenizer, byte_fallback, decoder)
    byte_level = pre_tokenizer == 'byte_level_qwen2'

    vocab = model_json['vocab']
    base = len(vocab)
    check(set(vocab.values()) == set(range(base)), 'vocabulary ids are not contiguous')
    alphabet = byte_alphabet()
    strings = [None] * base
    for text, token_id in vocab.items():
        strings[token_id] = text
    if byte_level:
        for text in strings:
            check(all(c in alphabet for c in text),
                  f'token {text!r} has a character outside the byte-level alphabet')
        out.tokens = [bytes(alphabet[c] for c in text) for text in strings]
    else:
        out.tokens = [text.encode('utf-8') for text in strings]
    out.hashed = [True] * base

    # 额外的 added token：落在词表内的要与词表一致，其余必须紧接着词表末尾。
    specials = set()
    added = []
    for token in sorted(data.get('added_tokens', []), key=lambda t: t['id']):
        token_id, content = token['id'], token['content']
        raw = content.encode('utf-8')
        if byte_level:
            # HF 的 ByteLevel 解码器按映射表还原字符；只有恒等映射的 ASCII 才与原始字节一致。
            check(all(c in alphabet and alphabet[c] == ord(c) for c in content),
                  f'added token {content!r} is not printable ASCII')
        if token_id < base:
            check(out.tokens[token_id] == raw, f'added token {content!r} disagrees with the vocabulary')
        else:
            check(token_id == len(out.tokens), f'added token ids are not contiguous at {token_id}')
            out.tokens.append(raw)
            out.hashed.append(False)
        added.append((raw, token_id))
        if token['special']:
            specials.add(token_id)
    out.added = added
    total = len(out.tokens)

    out.kinds = [KIND_NORMAL] * total
    for token_id in specials:
        out.kinds[token_id] = KIND_SPECIAL
    if decoder == 'metaspace_byte_fallback':
        pattern = re.compile(rb'<0x([0-9A-Fa-f]{2})>')
        for token_id, token in enumerate(out.tokens):
            match = pattern.fullmatch(token)
            if match and token_id not in specials:
                check(token == token.upper().replace(b'<0X', b'<0x'),
                      f'byte-fallback token {token!r} is not upper-case hexadecimal')
                out.kinds[token_id] = KIND_BYTE_FALLBACK
        # HF 的字节回退只查 BPE 词表（model.vocab），不查 added_tokens；
        # 同一个字节也不能有两个 token。
        values = [int(t[3:5], 16) for t, k in zip(out.tokens, out.kinds) if k == KIND_BYTE_FALLBACK]
        check(len(values) == len(set(values)), 'two tokens stand for the same byte-fallback byte')
        in_vocab = {int(t[3:5], 16) for t, k in zip(out.tokens[:base], out.kinds[:base])
                    if k == KIND_BYTE_FALLBACK}
        check(len(in_vocab) == 256,
              'byte fallback needs all 256 <0xXX> tokens in the BPE vocabulary (model.vocab), '
              f'only {len(in_vocab)} are there; added_tokens do not count')

    # 初始单位：字节级是 256 个单字节 token，字符级是“单字符 -> ID”。
    if byte_level:
        singles = {t[0]: i for i, t in enumerate(out.tokens[:base]) if len(t) == 1}
        check(len(singles) == 256, 'byte-level vocabulary needs all 256 single-byte tokens')
        out.units = sorted(singles.items())
    else:
        singles = {}
        for token_id, token in enumerate(out.tokens[:base]):
            text = token.decode('utf-8')
            if len(text) == 1:
                singles[ord(text)] = token_id
        out.units = sorted(singles.items())

    # 合并规则。
    merges = [merge_parts(rule) for rule in model_json['merges']]
    seen = set()
    for left, right in merges:
        check(left in vocab and right in vocab and left + right in vocab,
              f'merge {left!r} {right!r} refers to a token outside the vocabulary')
        check((left, right) not in seen, f'duplicate merge rule: {left!r} {right!r}')
        seen.add((left, right))
    if sentencepiece_model:
        build_score_priority(out, strings, vocab, merges, sentencepiece_model, total)
    else:
        build_rank_priority(out, vocab, merges)
    out.ends = read_end_tokens(generation_config, total)
    out.unicode = build_unicode_data(unicode_data, normalizer == 'nfc', byte_level)
    return out


def build_rank_priority(out, vocab, merges):
    out.merge_priority = 'rank'
    check(len(merges) <= MAX_RANK + 1, f'merge rank must stay below 2^24, got {len(merges)} rules')
    for rank, (left, right) in enumerate(merges):
        result = vocab[left + right]
        left_id = vocab[left]
        for token_id in (left_id, vocab[right], result):
            check(len(out.tokens[token_id]) <= MAX_RANK_TOKEN_BYTES,
                  f'token {out.tokens[token_id]!r} exceeds {MAX_RANK_TOKEN_BYTES} bytes, '
                  'which the rank priority cannot represent')
        out.rank_entries.setdefault(result, []).append((len(out.tokens[left_id]), rank))


def build_score_priority(out, strings, vocab, merges, sentencepiece_model, total):
    """score 的两个前置条件，缺一不可，不满足就报错（不会悄悄改用 rank）：

    1. merges 的集合恰好是“两段与拼接结果都在词表中”的全部切分；
    2. 把 merges 按（分数从高到低，左段码点数从多到少，右段码点数从多到少，合成 token 的 ID
       从小到大）排序，与 HF 的 merges 顺序逐条相同且没有并列。运行时按同一个组合键比较候选合并，
       所以候选合并的先后顺序与 HF 的排名完全相同。码点数不能换成字节数。
    """
    pieces = parse_sentencepiece(sentencepiece_model)
    check(len(pieces) == len(strings),
          f'tokenizer.model has {len(pieces)} pieces, tokenizer.json vocabulary has {len(strings)}')
    for index, (piece, score, piece_type) in enumerate(pieces):
        check(piece == strings[index], f'tokenizer.model piece {index} differs from the vocabulary')
        check(math.isfinite(score), f'tokenizer.model piece {index} has a non-finite score')
        check(piece_type in (1, 2, 3, 4, 6), f'tokenizer.model piece {index} has unsupported type {piece_type}')
    merge_set = set(merges)
    splits = set()
    for token in strings:
        for cut in range(1, len(token)):
            left, right = token[:cut], token[cut:]
            if left in vocab and right in vocab:
                splits.add((left, right))
    if splits != merge_set:
        missing = sorted(splits - merge_set)[:3]
        extra = sorted(merge_set - splits)[:3]
        raise ToolError(
            'score priority needs merges to equal the set of all in-vocabulary two-part splits, but '
            f'{len(splits - merge_set)} splits have no merge rule (e.g. {missing}) and '
            f'{len(merge_set - splits)} merge rules are not such splits (e.g. {extra}); '
            'omit --sentencepiece-model to use rank priority')

    def key(rule):
        left, right = rule
        return (-pieces[vocab[left + right]][1], -len(left), -len(right), vocab[left + right])

    keys = [key(rule) for rule in merges]
    bad = [i for i in range(1, len(keys)) if not keys[i - 1] < keys[i]]
    if bad:
        ties = sum(1 for i in bad if keys[i - 1] == keys[i])
        first = bad[0]
        raise ToolError(
            'score priority needs the merge order to equal sorting by (score descending, left code points '
            'descending, right code points descending, merged token id ascending), but '
            f'{len(bad)} adjacent merge rules are out of order or tied ({ties} ties); first at rule '
            f'{first}: {merges[first - 1]} then {merges[first]}; omit --sentencepiece-model to use rank priority')
    out.merge_priority = 'score'
    out.scores = [score for _, score, _ in pieces] + [0.0] * (total - len(pieces))


# ---------------------------------------------------------------- Unicode 区段

def sha256_file(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def build_unicode_data(unicode_path, need_nfc, need_properties):
    if not need_nfc and not need_properties:
        return UnicodeData()
    check(unicode_path, 'this tokenizer needs Unicode tables: pass --unicode-data '
                        '(Unicode 9.0.0 UnicodeData.txt)')
    try:
        import tokenizers
        from tokenizers import Regex, normalizers, pre_tokenizers
    except ImportError as error:
        raise ToolError(f'Unicode tables need the tokenizers=={REQUIRED_TOKENIZERS} package: {error}')
    check(tokenizers.__version__ == REQUIRED_TOKENIZERS,
          f'Unicode tables need tokenizers=={REQUIRED_TOKENIZERS}, found {tokenizers.__version__}')
    path = Path(unicode_path)
    check(path.is_file() and sha256_file(path) == UNICODE_SHA,
          f'{path.name} is not the pinned Unicode 9.0.0 UnicodeData.txt')

    result = UnicodeData()
    if need_nfc:
        canonical, combining, compositions = {}, [], []
        nfd, nfc = normalizers.NFD(), normalizers.NFC()
        for line in path.read_text().splitlines():
            fields = line.split(';')
            code_point = int(fields[0], 16)
            if int(fields[3]):
                combining.append((code_point, int(fields[3])))
            if fields[5] and not fields[5].startswith('<'):
                pair = [int(x, 16) for x in fields[5].split()]
                canonical[code_point] = pair
                if len(pair) == 2 and nfc.normalize_str(''.join(map(chr, pair))) == chr(code_point):
                    compositions.append((*pair, code_point))

        def flatten(code_point):
            sequence = []
            for part in canonical.get(code_point, [code_point]):
                sequence.extend(flatten(part) if part != code_point else [part])
            return sequence

        for code_point in sorted(canonical):
            sequence = flatten(code_point)
            check(''.join(map(chr, sequence)) == nfd.normalize_str(chr(code_point)),
                  'HF normalization tables differ from Unicode 9')
            result.decompositions.append((code_point, len(result.pool), len(sequence)))
            result.pool.extend(sequence)
        result.combining = combining
        result.compositions = sorted(compositions)
    if need_properties:
        # 下标等于码点，代理区不在字符串里，所以高于 0xD7FF 的下标整体偏移 0x800。
        universe = ''.join(chr(c) for c in range(0x110000) if not 0xD800 <= c <= 0xDFFF)
        flags = bytearray(0x110000)
        for bit, pattern in [(FLAG_LETTER, r'\p{L}+'), (FLAG_NUMBER, r'\p{N}+'), (FLAG_SPACE, r'\s+')]:
            splitter = pre_tokenizers.Split(Regex(pattern), behavior='removed')
            start = 0
            # 返回的区间是所求类别的补集。
            for _, (low, high) in splitter.pre_tokenize_str(universe) + [('', (len(universe), len(universe)))]:
                for index in range(start, low):
                    flags[index if index < 0xD800 else index + 0x800] |= bit
                start = high
        low = 0
        for code_point in range(1, len(flags) + 1):
            if code_point == len(flags) or flags[code_point] != flags[low]:
                if flags[low]:
                    result.properties.append((low, code_point - 1, flags[low]))
                low = code_point
    digest = hashlib.sha256(b''.join(
        [pack_rows(result.properties), pack_rows(result.decompositions), pack_u32(result.pool),
         pack_rows(result.combining), pack_rows(result.compositions)])).hexdigest()
    check(digest == UNICODE_TABLES_SHA,
          'unsupported tokenizers Unicode backend: the generated tables differ from the pinned reference')
    return result


# ---------------------------------------------------------------- 合成 fixture

def tiny_unicode_data():
    """小型 Unicode 区段，只够 fixture 使用；不是真实的 Unicode 数据。"""
    letters = [(0x41, 0x5A), (0x61, 0x7A), (0xC0, 0xD6), (0xD8, 0xF6), (0xF8, 0x17F),
               (0x1100, 0x11FF), (0x4E00, 0x9FFF), (0xAC00, 0xD7A3)]
    properties = [(low, high, FLAG_LETTER) for low, high in letters]
    properties += [(0x30, 0x39, FLAG_NUMBER), (0x9, 0xD, FLAG_SPACE), (0x20, 0x20, FLAG_SPACE),
                   (0x85, 0x85, FLAG_SPACE), (0xA0, 0xA0, FLAG_SPACE), (0x2000, 0x200A, FLAG_SPACE),
                   (0x3000, 0x3000, FLAG_SPACE)]
    return UnicodeData(
        properties=sorted(properties),
        decompositions=[(0xE8, 0, 2), (0xE9, 2, 2)],
        pool=[0x65, 0x300, 0x65, 0x301],
        combining=[(0x300, 230), (0x301, 230), (0x323, 220)],
        compositions=[(0x65, 0x300, 0xE8), (0x65, 0x301, 0xE9)])


def fixture_metaspace():
    """score 优先级、字符级、字节回退的合成词表。

    ID 布局：0 到 5 是特殊 token，6 到 261 是 <0xXX>，之后是字符和合并结果，
    最后是只出现在 added_tokens 中的 token。合并结果的 ID 顺序故意与分数顺序相反，
    使得测试能够区分“按分数合并”和“按 ID 合并”；另有两处同分，用来区分“只看分数、同分取最左”
    和 score 的组合键（分数、左段码点数、右段码点数、合成 token 的 ID）。
    """
    model = Model()
    model.normalizer, model.pre_tokenizer = 'replace_space_metaspace', 'none'
    model.byte_fallback, model.merge_priority, model.decoder = True, 'score', 'metaspace_byte_fallback'
    scores = []

    def add(text, score=0.0, hashed=True):
        model.tokens.append(text.encode('utf-8'))
        model.hashed.append(hashed)
        scores.append(score)
        return len(model.tokens) - 1

    specials = ['<pad>', '<eos>', '<bos>', '<unk>', '<start_of_turn>', '<end_of_turn>']
    for text in specials:
        add(text)
    for byte in range(256):
        add(f'<0x{byte:02X}>')
    for char in [SPACE_MARK] + list('abcdefghijklmnopqrstuvwxyz') + ['你', '好', 'é']:
        add(char)
    # 合并结果：ID 递增，分数递减的反方向排列。
    merged = [
        # 同一段文字里中间的合并先发生：abcd 先合 bc，再比较 abc 与 bcd。两者分数相同，
        # 组合键按左段码点数决定：bcd（左段 bc 有 2 个）先于 abc（左段 a 只有 1 个），
        # 尽管 abc 更靠左、ID 也更小。
        ('abc', -13.0), ('bcd', -13.0), ('cd', -12.0), ('ab', -11.0), ('bc', -10.0),
        # pqr：分数、左右段码点数都相同，组合键最后按合成 token 的 ID 决定，qr 先于 pq。
        ('qr', -60.0), ('pq', -60.0),
        ('aa', -20.0),                                  # 同分时取最左
        ('hello', -33.0), ('hell', -32.0), ('ll', -31.0), ('he', -30.0),
        (SPACE_MARK + 'hello', -34.0), (SPACE_MARK * 2, -40.0),
        ('你好', -50.0),
        ('<unused0>', 0.0),                             # 词表内的非特殊 added token
    ]
    for text, score in merged:
        add(text, score)
    model.added = [(b'<start_of_turn>', 4), (b'<end_of_turn>', 5),
                   (b'<pad>', 0), (b'<eos>', 1), (b'<bos>', 2), (b'<unk>', 3),
                   (b'<unused0>', len(model.tokens) - 1)]
    # 只出现在 added_tokens 中的 token：不进哈希，也不参与合并。
    extra = add('<ex>', hashed=False)
    extra_long = add('<ex><ey>', hashed=False)
    model.added += [(b'<ex>', extra), (b'<ex><ey>', extra_long)]
    model.kinds = [KIND_NORMAL] * len(model.tokens)
    for token_id in range(6):
        model.kinds[token_id] = KIND_SPECIAL
    for byte in range(256):
        model.kinds[6 + byte] = KIND_BYTE_FALLBACK
    model.scores = scores
    model.units = sorted((ord(model.tokens[i].decode()), i)
                         for i in range(262, 262 + 1 + 26 + 3))
    model.ends = [1, 5]
    return model


def fixture_byte_level():
    """rank 优先级、字节级的合成词表。

    ID 0 到 255 是单字节 token，之后的合并结果按规则顺序编号（ID = 256 + 排名），
    最后是特殊 token 和只出现在 added_tokens 中的 token。
    """
    model = Model()
    model.normalizer, model.pre_tokenizer = 'nfc', 'byte_level_qwen2'
    model.byte_fallback, model.merge_priority, model.decoder = False, 'rank', 'byte_level'
    model.tokens = [bytes([b]) for b in range(256)]
    index = {t: i for i, t in enumerate(model.tokens)}
    model.rank_entries = {}

    def rule(left, right):
        left, right = left.encode('latin-1') if isinstance(left, str) else left, (
            right.encode('latin-1') if isinstance(right, str) else right)
        result = left + right
        if result not in index:
            model.tokens.append(result)
            index[result] = len(model.tokens) - 1
        rank = sum(len(v) for v in model.rank_entries.values())
        model.rank_entries.setdefault(index[result], []).append((len(left), rank))

    # 缩写前的 n' 先合并：若预分词没有把 't 切出来，don't 会被错误合并。
    rule("n", "'")
    rule("s", "x")                       # 's 缩写：'sx 必须切成 's 和 x
    rule("'", "s")
    rule("o", "1")                       # hello1：预分词必须把数字切开
    rule("h", "e"); rule("l", "l"); rule("he", "ll"); rule("hell", "o")
    rule(" ", "hello"); rule(" ", "b"); rule("\n", "\n"); rule("'", "t")
    rule("a", "a")                       # 同分取最左：aaa -> aa a
    # abcd：bc 最先，随后 bcd 先于 abc。abc 有两条规则，分别在左段为 1 和 2 个字节。
    rule("b", "c"); rule("a", "b"); rule("c", "d"); rule("bc", "d"); rule("a", "bc"); rule("ab", "c")
    # pqr：pq 先合并；(pq, r) 拼出的 pqr 不在 pqr 自己的规则里，所以不能合并。
    rule("p", "q"); rule("q", "r"); rule("p", "qr")
    # mno：mn 先合并，之后用 pqr 以外的第二条规则 (mn, o) 得到 mno。
    rule("m", "n"); rule("n", "o"); rule("m", "no"); rule("mn", "o")
    rule(b"\xc3", b"\xa9")               # é 的 UTF-8
    rule(b"\xe4", b"\xbd"); rule(b"\xe4\xbd", b"\xa0")   # 你
    count_ranks = sum(len(v) for v in model.rank_entries.values())
    check(count_ranks < MAX_RANK, 'fixture rank overflow')
    specials = ['<|endoftext|>', '<|im_start|>', '<|im_end|>']
    first_special = len(model.tokens)
    for text in specials:
        model.tokens.append(text.encode())
    model.tokens.append(b'<tool_call>')
    model.hashed = [True] * first_special + [False] * 4
    model.added = [(model.tokens[i], i) for i in range(first_special, first_special + 4)]
    model.kinds = [KIND_NORMAL] * len(model.tokens)
    for token_id in range(first_special, first_special + 3):
        model.kinds[token_id] = KIND_SPECIAL
    model.units = [(b, b) for b in range(256)]
    model.ends = [first_special, first_special + 2]
    model.unicode = tiny_unicode_data()
    return model


# ---------------------------------------------------------------- 语料

def basic_texts():
    """通用语料，同时包含 ChatML 和 Gemma 模板；与具体模型无关。"""
    return ['', 'Hello, world!', 'hello world', ' hello  world ', '你好，世界！', '\0a\0\n',
            'a' * 300, ' ' * 300, '\r\n' * 100, '你' * 90 + 'a', '🙂' * 70,
            "I'm I'LL 'ſ 'S 'RE don't", 'a▁b', 'é', 'à̕', '각',
            '각', '̈́', 'Å', 'ཱི', ' \n \r\n  hi', ' hi  \t  .x', 'à́b',
            '́̂', 'x‍y', '١٢٣४५६中文', '\u0085   　',
            '👍🏽👩‍🚀', '12345 6789 0', 'a_b-c.d/e\\f', '\t\tindent\n\tmore', '<think>x</think>',
            'ﬁ ﬂ ǆ ㌀', '﻿bom', 'tab\there', 'multi\n\n\nline\n', 'a  b   c', '\n\n\n', '\t\t',
            '<table><tr>', '▁▁▁', 'x<end_of_turn', '<bos>', '<eos>', '<pad><mask>',
            '<start_of_image>', '<unused0>', '<unused99>x',
            '<|im_start|>system\nYou are a helpful assistant.<|im_end|>\n<|im_start|>user\nhi<|im_end|>\n'
            '<|im_start|>assistant\n',
            '<bos><start_of_turn>user\nhi<end_of_turn>\n<start_of_turn>model\n',
            '<|endoftext|>', '<tool_call>', 'a<|im_end|>b<|im_start|>', '<|im_start|', '<|',
            'x<|fim_prefix|>y']


def random_unicode(rng, length):
    pools = [(0x20, 0x7E, 40), (0xA0, 0x24F, 10), (0x300, 0x36F, 5), (0x370, 0x6FF, 6),
             (0x900, 0xDFF, 4), (0x1100, 0x11FF, 2), (0x2000, 0x206F, 5), (0x3000, 0x303F, 3),
             (0x3040, 0x30FF, 5), (0x4E00, 0x9FFF, 12), (0xAC00, 0xD7A3, 4), (0xFB00, 0xFB4F, 1),
             (0x1F300, 0x1FAFF, 6), (0x0, 0x1F, 2), (0xE000, 0xF8FF, 1), (0x10000, 0x10FFF, 1)]
    weights = [p[2] for p in pools]
    return ''.join(chr(rng.randint(low, high))
                   for low, high, _ in rng.choices(pools, weights, k=length))


def verify_corpus():
    rng = random.Random(20261014)
    texts = basic_texts()
    texts += [random_unicode(rng, rng.randint(1, 40)) for _ in range(1500)]
    for _ in range(300):
        words = [''.join(rng.choice("abcdefghijklmnopqrstuvwxyzABCDEFGHIJ0123456789'.,;:!?-_()")
                         for _ in range(rng.randint(1, 9))) for _ in range(rng.randint(1, 30))]
        separators = [rng.choice([' ', ' ', ' ', '  ', '\n', '\r\n', '\t', '']) for _ in words]
        texts.append(''.join(w + s for w, s in zip(words, separators)))
    # NFC 的规范排序：长串组合记号，顺序与组合类都打乱；曾经是平方复杂度。
    marks = ['\u0301', '\u0300', '\u0323', '\u0315', '\u0316', '\u0334', '\u0345', '\u035c']
    texts += ['a' + '\u0301' * 3000 + '\u0323' * 3000, 'e' + '\u0323' * 3000 + '\u0301' * 3000,
              ''.join(rng.choice(['a', 'e', 'o'] + marks * 3) for _ in range(30000)),
              'a' + ''.join(rng.choice(marks) for _ in range(20000)),
              'a' + '\u0301' * 16000 + '\u0323' * 16000]
    texts += ['x' * 70000, ' ' * 70000,
              ('The quick brown fox, jumps over 13 lazy dogs.\n' * 1600)[:65536],
              random_unicode(rng, 20000), ('你好 hello\n' * 8000)[:65536]]
    return texts


def golden_texts():
    return basic_texts() + ['The quick brown fox, jumps over 13 lazy dogs.\n你好 hello\n' * 60]


# ---------------------------------------------------------------- golden / verify

def reference_tokenizer(tokenizer_json):
    try:
        import tokenizers
        from tokenizers import Tokenizer
    except ImportError as error:
        raise ToolError(f'this command needs the tokenizers=={REQUIRED_TOKENIZERS} package: {error}')
    check(tokenizers.__version__ == REQUIRED_TOKENIZERS,
          f'this command needs tokenizers=={REQUIRED_TOKENIZERS}, found {tokenizers.__version__}')
    return Tokenizer.from_file(str(tokenizer_json))


def write_golden(tokenizer_json, output):
    reference = reference_tokenizer(tokenizer_json)
    cases = [{'text': text, 'ids': reference.encode(text, add_special_tokens=False).ids}
             for text in golden_texts()]
    # 每个样例一行，便于审阅差异。
    lines = [json.dumps(case, ensure_ascii=True) for case in cases]
    atomic_write(output, ('{"cases": [\n' + ',\n'.join(lines) + '\n]}\n').encode())


def run_driver(driver, tokenizer_file, lines):
    process = subprocess.run([str(driver), str(tokenizer_file)], input='\n'.join(lines) + '\n',
                             capture_output=True, text=True, check=False)
    check(process.returncode == 0, f'driver failed: {process.stderr.strip()}')
    # 每个请求恰好对应一行输出；缺行、多行或 ERR 都不能当作“零差异”。语料中的请求都是合法的，
    # 所以驱动对任何一个请求输出 ERR 都说明有问题。
    output = process.stdout.split('\n')
    check(output and output[-1] == '', 'driver output does not end with a newline')
    output = output[:-1]
    check(len(output) == len(lines),
          f'driver returned {len(output)} lines for {len(lines)} requests')
    for index, line in enumerate(output):
        check(line != 'ERR', f'driver reported an error for request {index}: {lines[index][:80]}')
    return output


def verify(tokenizer_json, tokenizer_file, driver):
    reference = reference_tokenizer(tokenizer_json)
    data = load_json(tokenizer_json)
    texts = verify_corpus()
    expected = [reference.encode(text, add_special_tokens=False).ids for text in texts]
    actual = run_driver(driver, tokenizer_file, ['E ' + text.encode().hex() for text in texts])
    mismatches = 0
    for text, want, got in zip(texts, expected, actual):
        if got != ' '.join(map(str, want)):
            mismatches += 1
            if mismatches <= 10:
                print('ENCODE MISMATCH', repr(text[:80]), want[:20], got[:120], file=sys.stderr)
    encode_mismatches = mismatches

    # 解码：编码结果、随机 ID，以及字节回退 token（含非法 UTF-8）和特殊 token 的混排。
    rng = random.Random(7)
    vocab = reference.get_vocab(with_added_tokens=True)
    total = max(vocab.values()) + 1
    special_ids = [t['id'] for t in data.get('added_tokens', []) if t['special']]
    byte_pattern = re.compile(r'<0x[0-9A-F]{2}>')
    byte_ids = [i for t, i in vocab.items() if byte_pattern.fullmatch(t)]
    short_ids = [i for t, i in vocab.items() if len(t) == 1]
    id_lists = [ids for ids in expected if ids][:800]
    id_lists += [[rng.randrange(total) for _ in range(rng.randint(1, 12))] for _ in range(1500)]
    for pool in (byte_ids, short_ids):
        if pool:
            id_lists += [[rng.choice(pool) for _ in range(rng.randint(1, 8))] for _ in range(1500)]
    if byte_ids and special_ids:
        id_lists += [[rng.choice(byte_ids + special_ids) for _ in range(rng.randint(1, 8))]
                     for _ in range(1500)]
    decoded = run_driver(driver, tokenizer_file, ['D ' + ' '.join(map(str, ids)) for ids in id_lists])
    for ids, got in zip(id_lists, decoded):
        want = reference.decode(ids, skip_special_tokens=True).encode().hex()
        if got != want:
            mismatches += 1
            if mismatches - encode_mismatches <= 10:
                print('DECODE MISMATCH', ids[:20], got[:80], want[:80], file=sys.stderr)
    print(f'{len(texts)} encode cases, {len(id_lists)} decode cases: '
          f'{encode_mismatches} encode mismatches, {mismatches - encode_mismatches} decode mismatches')
    return mismatches == 0


# ---------------------------------------------------------------- 入口

def atomic_write(path, content):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + '.tmp')
    temporary.write_bytes(content)
    temporary.replace(path)


def report(path, content):
    print(f'{path}: {len(content)} bytes, sha256 {hashlib.sha256(content).hexdigest()}')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest='command', required=True)
    compile_parser = commands.add_parser('compile', help='compile one model into a tokenizer file')
    compile_parser.add_argument('--tokenizer-json', required=True)
    compile_parser.add_argument('--generation-config', required=True)
    compile_parser.add_argument('--sentencepiece-model')
    compile_parser.add_argument('--unicode-data')
    compile_parser.add_argument('--output', required=True)
    fixture_parser = commands.add_parser('fixture', help='write a synthetic tokenizer file')
    fixture_parser.add_argument('kind', choices=['metaspace', 'byte_level'])
    fixture_parser.add_argument('--output', required=True)
    golden_parser = commands.add_parser('golden', help='write text -> ids samples from HF')
    golden_parser.add_argument('--tokenizer-json', required=True)
    golden_parser.add_argument('--output', required=True)
    verify_parser = commands.add_parser('verify', help='compare the C++ tokenizer with HF')
    verify_parser.add_argument('--tokenizer-json', required=True)
    verify_parser.add_argument('--tokenizer-file', required=True)
    verify_parser.add_argument('--driver', required=True)
    args = parser.parse_args(argv)

    try:
        if args.command == 'compile':
            content = write_file(compile_model(args.tokenizer_json, args.generation_config,
                                               args.sentencepiece_model, args.unicode_data))
            atomic_write(args.output, content)
            report(args.output, content)
        elif args.command == 'fixture':
            model = fixture_metaspace() if args.kind == 'metaspace' else fixture_byte_level()
            content = write_file(model)
            atomic_write(args.output, content)
        elif args.command == 'golden':
            write_golden(args.tokenizer_json, args.output)
        else:
            return 0 if verify(args.tokenizer_json, args.tokenizer_file, args.driver) else 1
    except ToolError as error:
        print(f'error: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
