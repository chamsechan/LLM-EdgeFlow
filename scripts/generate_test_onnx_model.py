#!/usr/bin/env python3
"""
Generate a minimal valid BGE Embedding ONNX model fixture and vocab.txt.
Graph:
  input_ids: int64[batch, seq]
  attention_mask: int64[batch, seq]
  -> Gather from Embedding table (vocab_size=1000, hidden_dim=128)
  -> last_hidden_state: float32[batch, seq, 128]
"""

import os
import hashlib
import numpy as np
import onnx
from onnx import helper, TensorProto

def generate_vocab(vocab_path):
    tokens = [
        "[PAD]",
        "[UNK]",
        "[CLS]",
        "[SEP]",
        "hello",
        "world",
        "bge",
        "embedding",
        "test",
        "edgeflow",
        "北",
        "京",
        "大",
        "学",
        "智",
        "能",
        "问",
        "答",
        "系",
        "统",
    ]
    # Pad to 1000 tokens
    for i in range(len(tokens), 1000):
        tokens.append(f"token_{i}")
    
    os.makedirs(os.path.dirname(vocab_path), exist_ok=True)
    with open(vocab_path, "w", encoding="utf-8") as f:
        for t in tokens:
            f.write(t + "\n")
    print(f"Generated vocab at {vocab_path} (size={len(tokens)})")

def generate_onnx_model(model_path, vocab_size=1000, hidden_dim=128):
    os.makedirs(os.path.dirname(model_path), exist_ok=True)
    
    # 1. Inputs
    input_ids = helper.make_tensor_value_info(
        "input_ids", TensorProto.INT64, ["batch", "sequence"]
    )
    attention_mask = helper.make_tensor_value_info(
        "attention_mask", TensorProto.INT64, ["batch", "sequence"]
    )
    
    # 2. Output
    last_hidden_state = helper.make_tensor_value_info(
        "last_hidden_state", TensorProto.FLOAT, ["batch", "sequence", hidden_dim]
    )
    
    # 3. Initializer: Embedding table weights
    np.random.seed(42)
    embedding_table = np.random.randn(vocab_size, hidden_dim).astype(np.float32)
    # Ensure non-zero deterministic vectors
    for i in range(vocab_size):
        embedding_table[i] = embedding_table[i] / np.linalg.norm(embedding_table[i])
        
    embed_weights = helper.make_tensor(
        name="embed_weights",
        data_type=TensorProto.FLOAT,
        dims=[vocab_size, hidden_dim],
        vals=embedding_table.flatten().tolist()
    )
    
    # 4. Node: Gather(embed_weights, input_ids, axis=0) -> last_hidden_state
    gather_node = helper.make_node(
        "Gather",
        inputs=["embed_weights", "input_ids"],
        outputs=["last_hidden_state"],
        axis=0
    )
    
    # 5. Graph & Model
    graph = helper.make_graph(
        [gather_node],
        "bge_embedding_graph",
        [input_ids, attention_mask],
        [last_hidden_state],
        initializer=[embed_weights]
    )
    
    model = helper.make_model(
        graph,
        producer_name="edgeflow_test_generator",
        opset_imports=[helper.make_opsetid("", 14)],
        ir_version=8
    )
    onnx.checker.check_model(model)
    onnx.save(model, model_path)
    
    with open(model_path, "rb") as f:
        sha256 = hashlib.sha256(f.read()).hexdigest()
    print(f"Generated valid ONNX model at {model_path} (SHA-256={sha256})")

if __name__ == "__main__":
    base_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    model_path = os.path.join(base_dir, "models", "bge_base_zh_v1.5.onnx")
    vocab_path = os.path.join(base_dir, "models", "vocab.txt")
    generate_vocab(vocab_path)
    generate_onnx_model(model_path, vocab_size=1000, hidden_dim=128)
