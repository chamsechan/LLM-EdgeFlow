# Model deployment directory

`CompanyAlgParamCreate.model_root_dir` points to the directory that directly
contains model artifacts and their sidecars, so C ABI Pipeline JSON uses names
such as `bge_base_zh_v1.5.onnx` without repeating a `models/` prefix.

Operator `model_path` retains its existing bundle-root contract: `.conf`,
Pipeline JSON, and `data.model_paths` are resolved beneath that sandbox. The
Operator adapter and the C ABI adapter both submit absolute model paths to Core,
but their public root parameters intentionally describe different directory
layouts.

Prepare the default CPU real Profile artifacts from pinned upstream commits with:

```bash
./scripts/fetch_real_test_models.sh --all
```

For the real GGUF C ABI/Profile gate only:

```bash
./scripts/fetch_real_test_models.sh --gguf-only
./scripts/run_real_model_e2e.sh
```

The fetch script verifies SHA-256 before publishing a download into this
directory. Model weights and tokenizer sidecars remain Git-ignored; the deployment JSON sidecars and documentation are versioned.

For Kite text, mixed ONNX/text and image/document profiles:

```bash
./scripts/fetch_real_test_models.sh --kite
```

This additionally fetches SmolVLM-256M-Instruct Q8_0 and its matching projector
from `ggml-org/SmolVLM-256M-Instruct-GGUF` revision
`b9e4379657e1450d04d02eec8e345667265b0a00`. Both SHA-256 values are pinned in
the fetch script. `kite_vision_run.json` points to the projector relative to
this directory. These small models support functional regression; document
recognition accuracy must be evaluated on the intended data.
