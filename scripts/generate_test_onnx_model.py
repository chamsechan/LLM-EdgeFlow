#!/usr/bin/env python3
"""Generate a deterministic, dependency-free ONNX Runtime test fixture.

The graph is intentionally small and is not a pretrained BGE model. It proves
the real ONNX Runtime Load/Run boundary used by ``BgeEmbeddingModel``:

  input_ids[int64, batch, sequence]
    -> Cast -> Unsqueeze -> Mul(scale) -> Add(bias)
    -> last_hidden_state[float32, batch, sequence, 128]

Only Python's standard library is used. The minimal ONNX protobuf is encoded
directly so a clean checkout does not require the ``onnx`` or ``numpy`` wheels.
"""

from __future__ import annotations

import argparse
import hashlib
import math
import os
import struct
from pathlib import Path
from typing import List, Optional, Union


def _varint(value: int) -> bytes:
    if value < 0:
        value &= (1 << 64) - 1
    encoded = bytearray()
    while value > 0x7F:
        encoded.append((value & 0x7F) | 0x80)
        value >>= 7
    encoded.append(value)
    return bytes(encoded)


def _key(field_number: int, wire_type: int) -> bytes:
    return _varint((field_number << 3) | wire_type)


def _int_field(field_number: int, value: int) -> bytes:
    return _key(field_number, 0) + _varint(value)


def _bytes_field(field_number: int, value: bytes) -> bytes:
    return _key(field_number, 2) + _varint(len(value)) + value


def _string_field(field_number: int, value: str) -> bytes:
    return _bytes_field(field_number, value.encode("utf-8"))


def _message_field(field_number: int, value: bytes) -> bytes:
    return _bytes_field(field_number, value)


def _tensor_shape(dimensions: List[Union[int, str]]) -> bytes:
    shape = bytearray()
    for dimension in dimensions:
        if isinstance(dimension, str):
            dim = _string_field(2, dimension)
        else:
            dim = _int_field(1, dimension)
        shape += _message_field(1, dim)
    return bytes(shape)


def _value_info(name: str, element_type: int,
                dimensions: List[Union[int, str]]) -> bytes:
    tensor_type = _int_field(1, element_type)
    tensor_type += _message_field(2, _tensor_shape(dimensions))
    type_proto = _message_field(1, tensor_type)
    return _string_field(1, name) + _message_field(2, type_proto)


def _tensor(name: str, element_type: int, dimensions: List[int],
            raw_data: bytes) -> bytes:
    tensor = bytearray()
    for dimension in dimensions:
        tensor += _int_field(1, dimension)
    tensor += _int_field(2, element_type)
    tensor += _string_field(8, name)
    tensor += _bytes_field(9, raw_data)
    return bytes(tensor)


def _attribute_int(name: str, value: int) -> bytes:
    # AttributeProto.INT = 2.
    return (_string_field(1, name) + _int_field(3, value) +
            _int_field(20, 2))


def _node(op_type: str, inputs: List[str], outputs: List[str],
          attributes: Optional[List[bytes]] = None) -> bytes:
    node = bytearray()
    for input_name in inputs:
        node += _string_field(1, input_name)
    for output_name in outputs:
        node += _string_field(2, output_name)
    node += _string_field(4, op_type)
    for attribute in attributes or []:
        node += _message_field(5, attribute)
    return bytes(node)


def generate_vocab(vocab_path: Path) -> None:
    tokens = [
        "[PAD]", "[UNK]", "[CLS]", "[SEP]", "hello", "world", "bge",
        "embedding", "test", "edgeflow", "北", "京", "大", "学", "智",
        "能", "问", "答", "系", "统",
    ]
    tokens.extend(f"token_{index}" for index in range(len(tokens), 1000))
    vocab_path.parent.mkdir(parents=True, exist_ok=True)
    vocab_path.write_text("\n".join(tokens) + "\n", encoding="utf-8")


def generate_onnx_model(model_path: Path, hidden_dim: int = 128) -> None:
    # TensorProto.FLOAT = 1, TensorProto.INT64 = 7.
    scale = [0.25 + (index + 1) / hidden_dim for index in range(hidden_dim)]
    bias = [math.sin(index + 1) * 0.5 for index in range(hidden_dim)]
    scale_raw = struct.pack(f"<{hidden_dim}f", *scale)
    bias_raw = struct.pack(f"<{hidden_dim}f", *bias)
    axes_raw = struct.pack("<q", -1)

    graph = bytearray()
    graph += _message_field(
        1, _node("Cast", ["input_ids"], ["ids_float"],
                 [_attribute_int("to", 1)]))
    graph += _message_field(
        1, _node("Unsqueeze", ["ids_float", "unsqueeze_axes"],
                 ["ids_expanded"]))
    graph += _message_field(
        1, _node("Mul", ["ids_expanded", "scale"], ["scaled_ids"]))
    graph += _message_field(
        1, _node("Add", ["scaled_ids", "bias"], ["last_hidden_state"]))
    graph += _string_field(2, "edgeflow_embedding_fixture")
    graph += _message_field(5, _tensor("unsqueeze_axes", 7, [1], axes_raw))
    graph += _message_field(
        5, _tensor("scale", 1, [1, 1, hidden_dim], scale_raw))
    graph += _message_field(
        5, _tensor("bias", 1, [1, 1, hidden_dim], bias_raw))
    graph += _message_field(
        11, _value_info("input_ids", 7, ["batch", "sequence"]))
    graph += _message_field(
        11, _value_info("attention_mask", 7, ["batch", "sequence"]))
    graph += _message_field(
        12, _value_info("last_hidden_state", 1,
                        ["batch", "sequence", hidden_dim]))

    model = bytearray()
    model += _int_field(1, 8)  # ModelProto.ir_version
    model += _string_field(2, "edgeflow_test_generator")
    model += _message_field(7, bytes(graph))
    model += _message_field(8, _int_field(2, 13))  # default opset 13

    model_path.parent.mkdir(parents=True, exist_ok=True)
    model_path.write_bytes(model)


def generate_rerank_onnx_model(model_path: Path) -> None:
    # TensorProto.FLOAT = 1, TensorProto.INT64 = 7.
    axes_raw = struct.pack("<q", 1)
    scale_cand_raw = struct.pack("<f", 0.01)
    scale_all_raw = struct.pack("<f", 0.001)

    graph = bytearray()
    graph += _message_field(
        1, _node("Cast", ["input_ids"], ["ids_float"],
                 [_attribute_int("to", 1)]))
    graph += _message_field(
        1, _node("Cast", ["attention_mask"], ["mask_float"],
                 [_attribute_int("to", 1)]))
    graph += _message_field(
        1, _node("Cast", ["token_type_ids"], ["type_float"],
                 [_attribute_int("to", 1)]))
    graph += _message_field(
        1, _node("Mul", ["ids_float", "mask_float"], ["masked_ids"]))
    graph += _message_field(
        1, _node("Mul", ["masked_ids", "type_float"], ["cand_ids"]))
    graph += _message_field(
        1, _node("ReduceSum", ["cand_ids", "reduce_axes"], ["cand_sum"],
                 [_attribute_int("keepdims", 1)]))
    graph += _message_field(
        1, _node("ReduceSum", ["masked_ids", "reduce_axes"], ["all_sum"],
                 [_attribute_int("keepdims", 1)]))
    graph += _message_field(
        1, _node("Mul", ["cand_sum", "scale_cand"], ["cand_scaled"]))
    graph += _message_field(
        1, _node("Mul", ["all_sum", "scale_all"], ["all_scaled"]))
    graph += _message_field(
        1, _node("Add", ["cand_scaled", "all_scaled"], ["logits"]))

    graph += _string_field(2, "edgeflow_rerank_fixture")
    graph += _message_field(5, _tensor("reduce_axes", 7, [1], axes_raw))
    graph += _message_field(5, _tensor("scale_cand", 1, [1, 1], scale_cand_raw))
    graph += _message_field(5, _tensor("scale_all", 1, [1, 1], scale_all_raw))

    graph += _message_field(
        11, _value_info("input_ids", 7, ["batch", "sequence"]))
    graph += _message_field(
        11, _value_info("attention_mask", 7, ["batch", "sequence"]))
    graph += _message_field(
        11, _value_info("token_type_ids", 7, ["batch", "sequence"]))
    graph += _message_field(
        12, _value_info("logits", 1, ["batch", 1]))

    model = bytearray()
    model += _int_field(1, 8)  # ModelProto.ir_version
    model += _string_field(2, "edgeflow_test_generator")
    model += _message_field(7, bytes(graph))
    model += _message_field(8, _int_field(2, 13))  # default opset 13

    model_path.parent.mkdir(parents=True, exist_ok=True)
    model_path.write_bytes(model)


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--output-dir",
        default=os.path.join(os.path.dirname(__file__), "..", "models"),
        help="Directory receiving fixtures and vocab.txt",
    )
    args = parser.parse_args()

    output_dir = Path(args.output_dir).resolve()
    model_path = output_dir / "embedding_fixture.onnx"
    rerank_path = output_dir / "rerank_fixture.onnx"
    vocab_path = output_dir / "vocab.txt"
    generate_vocab(vocab_path)
    generate_onnx_model(model_path)
    generate_rerank_onnx_model(rerank_path)

    print(f"ONNX_FIXTURE={model_path}")
    print(f"ONNX_SHA256={_sha256(model_path)}")
    print(f"RERANK_ONNX_FIXTURE={rerank_path}")
    print(f"RERANK_ONNX_SHA256={_sha256(rerank_path)}")
    print(f"VOCAB_FIXTURE={vocab_path}")
    print(f"VOCAB_SHA256={_sha256(vocab_path)}")


if __name__ == "__main__":
    main()
