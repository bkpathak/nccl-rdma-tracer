#!/bin/bash
set -e

echo "=== Installing vLLM ==="
pip install vllm --quiet

echo "=== Installing huggingface hub ==="
pip install huggingface_hub --quiet

echo "=== Login to Hugging Face ==="
huggingface-cli login --token $HF_TOKEN

echo "=== Downloading Llama 3 8B ==="
huggingface-cli download meta-llama/Meta-Llama-3-8B \
    --local-dir ~/models/llama3-8b \
    --local-dir-use-symlinks False

echo "=== Verifying model files ==="
ls ~/models/llama3-8b

echo "=== Done ==="