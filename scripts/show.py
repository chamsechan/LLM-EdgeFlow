#!/usr/bin/env python3
"""
Alg-SDK Pipeline & DAG Visualizer CLI Tool
Usage:
    ./show configs/pipeline_doc_qa.json           # 默认：在终端打印精美彩色 DAG 拓扑图与数据流向
    ./show configs/pipeline_doc_qa.json --web     # 启动并在浏览器中打开交互式可视化工作台
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

# 算子类别与颜色映射
CATEGORY_COLOR = {
    "PreProcess": BLUE,
    "Inference": MAGENTA,
    "Search": CYAN,
    "Rule": YELLOW,
    "PostProcess": GREEN
}

# 常见节点的默认类别与输入输出元数据推导（如果 JSON 中未显式写）
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
        "cat": "Rule",
        "desc": "高危敏感词内存字典前置快筛过滤",
        "in": ["user_texts"],
        "out": ["hard_risk_flags", "hit_keywords"]
    },
    "DenseRetrievalNode": {
        "cat": "Search",
        "desc": "调用 Model 1 (Embedding) 初筛召回合规政策条款库",
        "in": ["user_texts"],
        "out": ["candidate_policies"]
    },
    "CrossRerankNode": {
        "cat": "Inference",
        "desc": "调用 Model 2 (Cross-Encoder Reranker) 深度语义精排打分",
        "in": ["user_texts", "candidate_policies"],
        "out": ["matched_policy_clauses", "rerank_scores"]
    },
    "RiskPromptNode": {
        "cat": "PreProcess",
        "desc": "融合规则标签与高分条款组装 LLM 风控判决 Prompt",
        "in": ["user_texts", "matched_policy_clauses", "hard_risk_flags"],
        "out": ["llm_audit_prompts"]
    },
    "LlmAuditNode": {
        "cat": "Inference",
        "desc": "调用 Model 3 (LLM) 进行深度合规判决与安全建议生成",
        "in": ["llm_audit_prompts"],
        "out": ["generated_verdicts"]
    },
    "AuditPostNode": {
        "cat": "PostProcess",
        "desc": "汇聚精排得分与 LLM 结论打包 CompanyAuditOutputStruct",
        "in": ["raw_request_ids", "matched_policy_clauses", "rerank_scores", "generated_verdicts"],
        "out": ["compliance_audit_outputs"]
    }
}

def render_terminal_dag(config_path, data):
    biz_name = data.get("business_name", "unnamed_business")
    models = data.get("models", [])
    pipeline = data.get("pipeline", [])

    print(f"\n{BOLD}{CYAN}╔══════════════════════════════════════════════════════════════════════════════════╗{RESET}")
    print(f"{BOLD}{CYAN}║  Alg-SDK Pipeline & DAG Visualizer                                               ║{RESET}")
    print(f"{BOLD}{CYAN}║  ConfigFile: {config_path:<30} BizName: {biz_name:<26} ║{RESET}")
    print(f"{BOLD}{CYAN}╚══════════════════════════════════════════════════════════════════════════════════╝{RESET}\n")

    # 1. 显示模型池 (Models Pool)
    print(f"{BOLD}[ 1. 模型资源池 (ModelManager) ]{RESET}")
    if not models:
        print(f"  {DIM}└── (纯规则业务，无需加载任何模型，零硬件显存开销){RESET}\n")
    else:
        for i, m in enumerate(models):
            prefix = "  └──" if i == len(models) - 1 else "  ├──"
            mid = m.get("model_id", "unknown")
            etype = m.get("engine_type", "unknown")
            mpath = os.path.basename(m.get("model_path", ""))
            cfg = m.get("config", {})
            max_b = cfg.get("max_batch_size", 1)
            print(f"{prefix} {MAGENTA}🧠 {mid}{RESET} ({CYAN}Engine: {etype}{RESET}, {YELLOW}FixedMaxBatch: {max_b}{RESET}, Path: {DIM}{mpath}{RESET})")
        print()

    # 2. 显示 DAG 拓扑结构与数据流向
    print(f"{BOLD}[ 2. 数据流向与 DAG 拓扑流图 (Data Flow & Nodes) ]{RESET}\n")
    print(f"   {GREEN}[外部请求输入: vector<void*> inputs]{RESET}")
    print(f"         │")
    print(f"         │  {DIM}📥 解包原始结构体注入 AlgContext{RESET}")
    print(f"         ▼")

    for i, node_item in enumerate(pipeline):
        ntype = node_item.get("node_type", "UnknownNode")
        meta = NODE_META.get(ntype, {"cat": "Operator", "desc": "算法处理节点", "in": [], "out": []})
        cat = node_item.get("category", meta["cat"])
        color = CATEGORY_COLOR.get(cat, BLUE)
        desc = node_item.get("desc", meta["desc"])
        bind_m = node_item.get("bind_model", node_item.get("config", {}).get("bind_model", ""))
        inputs = node_item.get("inputs", meta.get("in", []))
        outputs = node_item.get("outputs", meta.get("out", []))
        cfg = node_item.get("config", {})

        in_str = ", ".join(inputs) if inputs else "-"
        out_str = ", ".join(outputs) if outputs else "-"

        # 节点卡片外框
        card_w = 70
        title_line = f" #{i+1:02d} [{cat}] {ntype} "
        print(f"   {color}┌─{title_line}{'─' * max(0, card_w - len(title_line) - 2)}┐{RESET}")
        print(f"   {color}│{RESET}  {DIM}功能:{RESET} {desc:<58} {color}│{RESET}")
        
        if bind_m:
            model_info = f"🧠 绑定模型: {bind_m} (通过 SessionContext 依赖注入)"
            print(f"   {color}│{RESET}  {MAGENTA}{model_info:<60}{RESET} {color}│{RESET}")

        print(f"   {color}│{RESET}  {CYAN}📥 输入 (from AlgContext):{RESET} {in_str:<42} {color}│{RESET}")
        print(f"   {color}│{RESET}  {GREEN}📤 输出 (to AlgContext)  :{RESET} {out_str:<42} {color}│{RESET}")
        
        # 显示主要配置
        cfg_keys = [f"{k}={v}" for k, v in cfg.items() if k != "bind_model" and not isinstance(v, (dict, list))]
        if cfg_keys:
            cfg_str = f"⚙️ 配置: {', '.join(cfg_keys)}"
            print(f"   {color}│{RESET}  {YELLOW}{cfg_str:<62}{RESET} {color}│{RESET}")

        print(f"   {color}└{'─' * card_w}┘{RESET}")

        if i < len(pipeline) - 1:
            print(f"         │")
            print(f"         │  {DIM}📦 传递 TraceableItem 样本溯源数据{RESET}")
            print(f"         ▼")
        else:
            print(f"         │")
            print(f"         │  {DIM}📤 打包提取特征回写外部结构体{RESET}")
            print(f"         ▼")
            print(f"   {GREEN}[外部响应输出: vector<void*> outputs (状态: SUCCESS 200 OK)]{RESET}\n")

    print(f"{DIM}提示: 可添加 '--web' 参数以在浏览器中打开交互式 DAG 可视化界面，例如:{RESET}")
    print(f"{BOLD}      ./show {config_path} --web{RESET}\n")

def get_project_root():
    # 1. 优先通过符号链接真实路径解析 (scripts/show.py -> 项目根目录)
    real_file = os.path.realpath(__file__)
    dir_path = os.path.dirname(real_file)
    while dir_path and dir_path != "/":
        if os.path.exists(os.path.join(dir_path, "tools", "visualizer", "index.html")) or \
           os.path.exists(os.path.join(dir_path, "CMakeLists.txt")):
            return dir_path
        dir_path = os.path.dirname(dir_path)

    # 2. 从当前工作目录回退寻找
    cwd = os.getcwd()
    if os.path.exists(os.path.join(cwd, "tools", "visualizer", "index.html")):
        return cwd

    return os.path.dirname(os.path.dirname(real_file))

def launch_web_visualizer(config_path):
    root_dir = get_project_root()
    vis_html = os.path.join(root_dir, "tools", "visualizer", "index.html")
    
    if not os.path.exists(vis_html):
        print(f"{RED}[Error] Visualizer HTML not found at: {vis_html}{RESET}")
        return

    port = 8088
    vis_dir = os.path.dirname(vis_html)

    class CustomHandler(http.server.SimpleHTTPRequestHandler):
        def __init__(self, *args, **kwargs):
            super().__init__(*args, directory=vis_dir, **kwargs)
        def log_message(self, format, *args):
            pass

        def do_POST(self):
            if self.path.startswith("/api/run_cpp"):
                import subprocess
                start_t = time.time()
                demo_bin = os.path.join(root_dir, "build", "alg_demo")
                if not os.path.exists(demo_bin):
                    self.send_response(500)
                    self.send_header("Content-Type", "application/json")
                    self.end_headers()
                    self.wfile.write(json.dumps({"success": False, "error": "C++ 演示程序尚未编译，请先在 build 目录执行 make"}).encode())
                    return

                try:
                    res = subprocess.run([demo_bin], capture_output=True, text=True, cwd=root_dir, timeout=10)
                    elapsed = (time.time() - start_t) * 1000.0
                    resp_data = {
                        "success": res.returncode == 0,
                        "elapsed_ms": round(elapsed, 2),
                        "stdout": res.stdout,
                        "stderr": res.stderr
                    }
                    self.send_response(200)
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Access-Control-Allow-Origin", "*")
                    self.end_headers()
                    self.wfile.write(json.dumps(resp_data).encode("utf-8"))
                except Exception as ex:
                    self.send_response(500)
                    self.send_header("Content-Type", "application/json")
                    self.end_headers()
                    self.wfile.write(json.dumps({"success": False, "error": str(ex)}).encode())
    # 允许端口快速重用
    socketserver.TCPServer.allow_reuse_address = True
    try:
        server = socketserver.TCPServer(("", port), CustomHandler)
    except Exception:
        port = 8089
        server = socketserver.TCPServer(("", port), CustomHandler)

    t = threading.Thread(target=server.serve_forever, daemon=True)
    t.start()

    url = f"http://localhost:{port}/index.html"
    print(f"\n{BOLD}{GREEN}⚡ 已启动交互式 DAG 可视化 Web 服务: {url}{RESET}")
    print(f"{CYAN}浏览器访问地址: {url} (按 Ctrl+C 可随时退出){RESET}\n")
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
    if len(sys.argv) < 2 or sys.argv[1] in ("-h", "--help"):
        print("用法: ./show <path_to_config.json> [--web]")
        print("示例:")
        print("  ./show configs/pipeline_doc_qa.json")
        print("  ./show configs/pipeline_keyword_match.json")
        print("  ./show configs/pipeline_entity_extract.json")
        print("  ./show configs/pipeline_dialogue_audit.json")
        print("  ./show configs/pipeline_doc_qa.json --web")
        sys.exit(0)

    cfg_file = sys.argv[1]
    is_web = "--web" in sys.argv or "--ui" in sys.argv

    if not os.path.exists(cfg_file):
        # 兼容相对路径
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

    # 默认直接在终端输出精美 DAG 拓扑图
    render_terminal_dag(cfg_file, data)

    # 如果带了 --web 参数，则同时弹出网页
    if is_web:
        launch_web_visualizer(cfg_file)

if __name__ == "__main__":
    main()
