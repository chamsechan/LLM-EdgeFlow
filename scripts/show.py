#!/usr/bin/env python3
"""
LLM-EdgeFlow Pipeline & DAG Visualizer & Editor CLI Tool
Usage:
    ./show configs/pipeline_doc_qa.json           # 默认：在终端打印精美彩色 DAG 拓扑图与数据流向
    ./show configs/pipeline_doc_qa.json --web     # 启动并在浏览器中打开交互式可视化与节点编辑工坊
    ./show --web                                  # 直接打开可视化与节点编辑工坊
"""

import sys
import os
import json
import webbrowser
import http.server
import socketserver
import threading
import time

# 终端 ANSI 颜色常量
CYAN = "\033[96m"
BLUE = "\033[94m"
GREEN = "\033[92m"
YELLOW = "\033[93m"
MAGENTA = "\033[95m"
RED = "\033[91m"
BOLD = "\033[1m"
DIM = "\033[2m"
RESET = "\033[0m"

CATEGORY_COLOR = {
    "PreProcess": BLUE,
    "Inference": MAGENTA,
    "Search": CYAN,
    "Rule": YELLOW,
    "PostProcess": GREEN
}

NODE_META = {
    "DocChunkPreNode": {
        "cat": "PreProcess",
        "desc": "长文本分块切片与打标 (1对N裂变)",
        "in": ["raw_docs", "raw_queries"],
        "out": ["chunked_doc_items", "query_items"]
    },
    "DocEmbeddingNode": {
        "cat": "Inference",
        "desc": "调用 NPU Embedding 提取 128 维特征向量",
        "in": ["chunked_doc_items", "query_items"],
        "out": ["chunk_embeddings", "query_embeddings"]
    },
    "VectorSearchNode": {
        "cat": "Search",
        "desc": "向量余弦相似度计算与 Top-K 检索",
        "in": ["chunk_embeddings", "query_embeddings"],
        "out": ["matched_top_chunks"]
    },
    "PromptBuilderNode": {
        "cat": "PreProcess",
        "desc": "渲染 Prompt 模板并注入上下文与问题",
        "in": ["matched_top_chunks", "raw_queries"],
        "out": ["llm_input_prompts"]
    },
    "IntentRuleNode": {
        "cat": "Rule",
        "desc": "私有状态规则字典意图分类",
        "in": ["raw_queries"],
        "out": ["recognized_intents", "intent_confidences"]
    },
    "LlmGenerateNode": {
        "cat": "Inference",
        "desc": "调用 NPU LLM 模型推理生成回答文本",
        "in": ["llm_input_prompts"],
        "out": ["generated_llm_answers"]
    },
    "DocQaPostNode": {
        "cat": "PostProcess",
        "desc": "按 req_id 聚合结果并打包 CompanyDocOutputStruct",
        "in": ["raw_request_ids", "recognized_intents", "generated_llm_answers"],
        "out": ["final_doc_outputs"]
    },
    "KeywordMatcherNode": {
        "cat": "Rule",
        "desc": "纯规则关注词匹配与动态 Control 词表热更新",
        "in": ["input_sentences", "raw_request_ids"],
        "out": ["keyword_match_outputs"]
    },
    "EntityExtractPreNode": {
        "cat": "PreProcess",
        "desc": "0.6B LLM 实体/名词抽取 Prompt 渲染",
        "in": ["input_sentences", "raw_request_ids"],
        "out": ["llm_input_prompts"]
    },
    "EntityExtractPostNode": {
        "cat": "PostProcess",
        "desc": "解析 0.6B 输出并结构化为实体 JSON 列表",
        "in": ["raw_request_ids", "generated_llm_answers"],
        "out": ["entity_extract_outputs"]
    },
    "SafetyRulePreNode": {
        "cat": "PreProcess",
        "desc": "敏感词与前置黑名单规则过滤",
        "in": ["raw_dialogues"],
        "out": ["safety_flags"]
    },
    "DenseRetrievalNode": {
        "cat": "Search",
        "desc": "稠密向量召回最相关风控法条",
        "in": ["dialogue_embeddings"],
        "out": ["recalled_policies"]
    },
    "CrossRerankNode": {
        "cat": "Inference",
        "desc": "Cross-Encoder 精排模型打分矩阵",
        "in": ["recalled_policies"],
        "out": ["rerank_scores"]
    },
    "RiskPromptNode": {
        "cat": "PreProcess",
        "desc": "组装风控质检大模型 Prompt",
        "in": ["top_matched_policies"],
        "out": ["audit_prompts"]
    },
    "LlmAuditNode": {
        "cat": "Inference",
        "desc": "LLM 审核推理生成违规判定与建议",
        "in": ["audit_prompts"],
        "out": ["audit_verdicts"]
    },
    "AuditPostNode": {
        "cat": "PostProcess",
        "desc": "封装合规质检 CompanyAuditOutputStruct",
        "in": ["audit_verdicts"],
        "out": ["compliance_audit_outputs"]
    },
    "ImagePreNode": {
        "cat": "PreProcess",
        "desc": "图像解码、尺寸对齐与归一化处理",
        "in": ["raw_images"],
        "out": ["preprocessed_tensors"]
    },
    "OcrInferNode": {
        "cat": "Inference",
        "desc": "NPU OCR 文本检测与识别 (PP-OCRv4)",
        "in": ["preprocessed_tensors"],
        "out": ["ocr_boxes", "ocr_texts"]
    },
    "OcrDocPostNode": {
        "cat": "PostProcess",
        "desc": "发票票据结构化封装与 JSON 输出",
        "in": ["ocr_texts", "llm_raw_texts"],
        "out": ["ocr_doc_outputs"]
    },
    "AudioFeaturePreNode": {
        "cat": "PreProcess",
        "desc": "PCM 音频分帧与 Fbank 频谱特征提取",
        "in": ["raw_pcm"],
        "out": ["audio_features"]
    },
    "AsrInferNode": {
        "cat": "Inference",
        "desc": "NPU 语音识别引擎 (Paraformer ASR)",
        "in": ["audio_features"],
        "out": ["transcribed_texts"]
    },
    "SlotExtractNode": {
        "cat": "Rule",
        "desc": "槽位与意图结构化规则提取 (NLU)",
        "in": ["transcribed_texts"],
        "out": ["slot_entities"]
    },
    "AudioPostNode": {
        "cat": "PostProcess",
        "desc": "语音识别与意图槽位封装输出",
        "in": ["transcribed_texts", "slot_entities"],
        "out": ["audio_final_outputs"]
    },
    "RerankPairBuilderNode": {
        "cat": "PreProcess",
        "desc": "Query-Passage 对组合与 Tokenizer 打包",
        "in": ["raw_queries", "candidate_passages"],
        "out": ["pair_tensors"]
    },
    "CrossRerankBatchNode": {
        "cat": "Inference",
        "desc": "ONNX Cross-Encoder 批量相关度打分",
        "in": ["pair_tensors"],
        "out": ["rerank_scores"]
    },
    "RerankSortPostNode": {
        "cat": "PostProcess",
        "desc": "得分降序排序与原始索引映射输出",
        "in": ["rerank_scores"],
        "out": ["rerank_batch_final_outputs"]
    }
}

def get_project_root():
    return os.path.dirname(os.path.abspath(__file__))

def render_terminal_dag(cfg_path, data):
    print(f"\n{BOLD}{CYAN}=================================================================={RESET}")
    print(f"  {BOLD}LLM-EdgeFlow Pipeline 拓扑与算子图解析{RESET}")
    print(f"  {DIM}配置文件: {cfg_path}{RESET}")
    print(f"{BOLD}{CYAN}=================================================================={RESET}\n")

    biz_name = data.get("business_name", "unknown")
    exec_mode = data.get("execution_mode", "sequential")
    workers = data.get("max_parallel_workers", 1)
    models = data.get("models", [])
    pipeline = data.get("pipeline", [])

    print(f"  {BOLD}业务标识 (Business){RESET} : {GREEN}{biz_name}{RESET}")
    print(f"  {BOLD}调度模式 (Execution){RESET}: {CYAN}{exec_mode.upper()} (Max Workers: {workers}){RESET}")
    print(f"  {BOLD}算子节点 (Total Nodes){RESET}: {YELLOW}{len(pipeline)}{RESET}\n")

    if models:
        print(f"  {BOLD}📦 已挂载异构推理引擎与模型 (Models Pool):{RESET}")
        for m in models:
            name = m.get("name", "")
            engine = m.get("engine_type", "")
            path = m.get("model_path", "")
            batch = m.get("max_batch_size", 1)
            print(f"    • {MAGENTA}{name:<20}{RESET} [{engine:<18}] (Batch: {batch}) -> {DIM}{path}{RESET}")
        print()

    print(f"  {BOLD}🚀 DAG 拓扑执行流向与波前层级 (Wavefront DAG Order):{RESET}")
    print(f"  {DIM}{'─'*62}{RESET}")

    for idx, node in enumerate(pipeline):
        node_id = node.get("node_id", f"node_{idx}")
        node_type = node.get("node_type", "UnknownNode")
        deps = node.get("depends_on", [])
        meta = NODE_META.get(node_type, {"cat": "Operator", "desc": "算法处理算子", "in": [], "out": []})
        cat = meta.get("cat", "Operator")
        color = CATEGORY_COLOR.get(cat, CYAN)
        desc = meta.get("desc", "")

        deps_str = f" <- [{', '.join(deps)}]" if deps else " [Root Node]"
        print(f"  [{idx}] {BOLD}{node_id:<12}{RESET} : {color}{node_type:<24}{RESET}{DIM}{deps_str}{RESET}")
        print(f"      {DIM}功能: {desc}{RESET}")

        in_keys = meta.get("in", [])
        out_keys = meta.get("out", [])
        if in_keys:
            print(f"      {BLUE}📥 读黑板: {', '.join(in_keys)}{RESET}")
        if out_keys:
            print(f"      {GREEN}📤 写黑板: {', '.join(out_keys)}{RESET}")

        if idx < len(pipeline) - 1:
            print(f"      {DIM}│{RESET}")
            print(f"      {DIM}▼{RESET}")

    print(f"  {DIM}{'─'*62}{RESET}\n")

def launch_web_visualizer(cfg_file=None, port=8080):
    root_dir = get_project_root()
    web_dir = os.path.join(root_dir, "tools", "visualizer")

    if not os.path.exists(web_dir):
        print(f"{RED}[Error] 找不到可视化前端目录: {web_dir}{RESET}")
        return

    os.chdir(web_dir)

    class CustomHandler(http.server.SimpleHTTPRequestHandler):
        def log_message(self, format, *args):
            pass

    server = socketserver.TCPServer(("", port), CustomHandler)
    url = f"http://localhost:{port}/index.html"

    print(f"\n{BOLD}{GREEN}=================================================================={RESET}")
    print(f"  🎉 {BOLD}LLM-EdgeFlow 可视化与节点工坊 Web 服务已启动！{RESET}")
    print(f"  🌐 访问地址: {CYAN}{url}{RESET}")
    print(f"  💡 支持：DAG 节点图拖拽、➕ 新增算子、属性动态编辑、成环检测、导出 JSON")
    print(f"  {DIM}按 Ctrl+C 可停止 Web 服务。{RESET}")
    print(f"{BOLD}{GREEN}=================================================================={RESET}\n")

    t = threading.Thread(target=server.serve_forever)
    t.daemon = True
    t.start()

    try:
        webbrowser.open(url)
    except Exception:
        pass

    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        print("\nWeb 可视化服务已停止。")

def main():
    if len(sys.argv) == 1 or sys.argv[1] in ("-h", "--help"):
        print("用法: ./show <path_to_config.json> [--web]")
        print("      ./show --web")
        print("\n示例:")
        print("  ./show configs/pipeline_doc_qa.json           # 在终端打印 DAG 拓扑图")
        print("  ./show configs/pipeline_doc_qa.json --web     # 在浏览器中打开可视化与节点工坊")
        print("  ./show --web                                  # 直接启动可视化编辑工坊")
        sys.exit(0)

    if sys.argv[1] in ("--web", "-w", "--ui"):
        launch_web_visualizer()
        sys.exit(0)

    cfg_file = sys.argv[1]
    is_web = "--web" in sys.argv or "--ui" in sys.argv

    if not os.path.exists(cfg_file):
        root_dir = get_project_root()
        alt_path = os.path.join(root_dir, cfg_file)
        if os.path.exists(alt_path):
            cfg_file = alt_path
        else:
            print(f"{RED}[Error] 找不到配置文件: {cfg_file}{RESET}")
            sys.exit(1)

    try:
        with open(cfg_file, "r", encoding="utf-8") as f:
            data = json.load(f)
    except Exception as e:
        print(f"{RED}[Error] 读取 JSON 配置异常: {e}{RESET}")
        sys.exit(1)

    render_terminal_dag(cfg_file, data)

    if is_web:
        launch_web_visualizer(cfg_file)

if __name__ == "__main__":
    main()
