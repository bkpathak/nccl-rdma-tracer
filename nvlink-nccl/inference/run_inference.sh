#!/bin/bash

# 1 GPU baseline
echo "=== Running 1 GPU inference ==="
python benchmark_inference.py \
    --model ~/models/llama3-8b \
    --tensor-parallel-size 1 \
    --num-prompts 50 \
    --output-json results/inference_1gpu.json

# 8 GPU tensor parallel
echo "=== Running 8 GPU inference ==="
python benchmark_inference.py \
    --model ~/models/llama3-8b \
    --tensor-parallel-size 8 \
    --num-prompts 50 \
    --output-json results/inference_8gpu.json