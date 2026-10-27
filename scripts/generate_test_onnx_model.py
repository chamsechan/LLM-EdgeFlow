#!/usr/bin/env python3
"""Generate tiny ONNX graphs for native Backend loading and storage checks.

The writer uses only stdlib and the public ONNX protobuf field definitions:
https://github.com/onnx/onnx/blob/main/onnx/onnx.proto
Fixtures use IR 8 / opset 13, with dynamic batch and six tokens per row.
These graphs exercise execution plumbing; they are not semantic model assets.
"""

import argparse
from pathlib import Path
import struct

FLOAT = 1
INT64 = 7


def varint(value):
    """不依赖 protobuf 库，编码一个非负 protobuf 整数。"""
    if value < 0:
        raise ValueError("Fixture protobuf integers must be nonnegative")
    encoded = bytearray()
    while value >= 128:
        encoded.append((value & 127) | 128)
        value >>= 7
    encoded.append(value)
    return bytes(encoded)


def integer(field, value):
    return varint(field << 3) + varint(value)


def message(field, value):
    return varint((field << 3) | 2) + varint(len(value)) + value


def text(field, value):
    return message(field, value.encode("utf-8"))


def tensor_type(element_type, dimensions):
    shape = bytearray()
    for dimension in dimensions:
        encoded = text(2, dimension) if isinstance(dimension, str) else integer(1, dimension)
        shape.extend(message(1, encoded))  # TensorShapeProto.dim
    tensor = integer(1, element_type) + message(2, bytes(shape))
    return message(1, tensor)  # TypeProto.tensor_type


def value_info(name, element_type, dimensions, sequence=False):
    declared_type = tensor_type(element_type, dimensions)
    if sequence:
        declared_type = message(4, message(1, declared_type))  # sequence_type.elem_type
    return text(1, name) + message(2, declared_type)


def tensor(name, element_type, dimensions, values):
    encoded = bytearray()
    for dimension in dimensions:
        encoded.extend(integer(1, dimension))
    encoded.extend(integer(2, element_type))
    encoded.extend(text(8, name))
    format_code = "f" if element_type == FLOAT else "q"
    encoded.extend(message(9, struct.pack("<" + format_code * len(values), *values)))
    return bytes(encoded)


def attribute(name, value):
    return text(1, name) + integer(3, value) + integer(20, 2)  # AttributeProto.INT


def node(operation, inputs, outputs, **attributes):
    encoded = bytearray(text(4, operation))
    for name in inputs:
        encoded.extend(text(1, name))
    for name in outputs:
        encoded.extend(text(2, name))
    for name, value in attributes.items():
        encoded.extend(message(5, attribute(name, value)))
    return bytes(encoded)


def model(name, nodes, inputs, outputs, initializers=()):
    graph = bytearray(text(2, name))
    for operation in nodes:
        graph.extend(message(1, operation))
    for initializer in initializers:
        graph.extend(message(5, initializer))
    for declared_input in inputs:
        graph.extend(message(11, declared_input))
    for declared_output in outputs:
        graph.extend(message(12, declared_output))
    return (integer(1, 8) + text(2, "edgeflow_native_fixture") +
            message(7, bytes(graph)) + message(8, integer(2, 13)))


def rerank_graph():
    """logits = .01 * sum(ids * mask * types) + .001 * sum(ids * mask)."""
    nodes = [
        node("Cast", ["input_ids"], ["ids"], to=FLOAT),
        node("Cast", ["attention_mask"], ["mask"], to=FLOAT),
        node("Cast", ["token_type_ids"], ["types"], to=FLOAT),
        node("Mul", ["ids", "mask"], ["visible"]),
        node("Mul", ["visible", "types"], ["candidate"]),
        node("ReduceSum", ["visible", "row_axis"], ["all_sum"], keepdims=1),
        node("ReduceSum", ["candidate", "row_axis"], ["candidate_sum"], keepdims=1),
        node("Mul", ["all_sum", "all_scale"], ["all_score"]),
        node("Mul", ["candidate_sum", "candidate_scale"], ["candidate_score"]),
        node("Add", ["all_score", "candidate_score"], ["logits"]),
    ]
    inputs = [value_info(name, INT64, ["batch", 6])
              for name in ("input_ids", "attention_mask", "token_type_ids")]
    constants = [tensor("row_axis", INT64, [1], [1]),
                 tensor("all_scale", FLOAT, [1], [0.001]),
                 tensor("candidate_scale", FLOAT, [1], [0.01])]
    return model("native_rerank", nodes, inputs,
                 [value_info("logits", FLOAT, ["batch", 1])], constants)


def non_tensor_graph():
    return model("native_sequence", [node("SequenceConstruct", ["input_ids"], ["sequence_output"])],
                 [value_info("input_ids", INT64, ["batch", 6])],
                 [value_info("sequence_output", INT64, ["batch", 6], sequence=True)])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, required=True)
    options = parser.parse_args()
    options.output_dir.mkdir(parents=True, exist_ok=True)
    graphs = {"rerank_fixture.onnx": rerank_graph(),
              "non_tensor_output_fixture.onnx": non_tensor_graph()}
    for name, graph in graphs.items():
        (options.output_dir / name).write_bytes(graph)
    vocabulary = ["[PAD]", "[UNK]", "[CLS]", "[SEP]", "hello", "world", "query", "candidate"]
    (options.output_dir / "vocab.txt").write_text("\n".join(vocabulary) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
