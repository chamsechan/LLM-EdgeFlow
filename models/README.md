# Model deployment directory

The C++ Operator `model_path` is the deployment bundle root. The `.conf` selects
Pipeline JSON through `pipe_path`; each model path is configured only in
`models[].model_path`. Relative model paths resolve beneath the same host-provided
root, and Integration passes resolved absolute paths to Core. With the repository
as the bundle root, model references include the `models/` prefix.

Asset manifest paths are relative to the asset directory. Tokenizer and runtime
configuration sidecars resolve relative to the actual model's directory. The
selection checker distinguishes these roots with `--model-root` (asset directory,
defaulting to this directory) and `--pipeline-root` (host deployment root,
defaulting to the repository root); choosing an asset directory does not override
the model paths saved in the Pipeline.

Prepare the default CPU real Profile artifacts from pinned upstream commits with:

```bash
./scripts/fetch_real_test_models.sh --all
```

For the real GGUF Operator/Profile gate only:

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
[asset_manifest.json](asset_manifest.json), which the fetch script reads.
`kite_vision_run.json` points to the projector relative to
this directory. These small models support functional regression; document
recognition accuracy must be evaluated on the intended data.

For Whisper ASR models (`ggml-base.bin`, `ggml-tiny-q5_1.bin`):

```bash
./scripts/fetch_real_test_models.sh --whisper
```

The authoritative artifact pins and selectable Model/Backend starting points live
in [asset_manifest.json](asset_manifest.json). The fetch script and selection
checker consume that same manifest. See [verifiable selection](../doc/VERIFIABLE_SELECTION.md)
for build presets, sidecar verification and dataset-based acceptance receipts.
