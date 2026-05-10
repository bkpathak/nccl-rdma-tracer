import argparse
import json
import time
from vllm import LLM, SamplingParams

PROMPTS = [
    "Explain how neural networks work in simple terms.",
    "What is the difference between supervised and unsupervised learning?",
    "Describe the transformer architecture.",
    "How does attention mechanism work in deep learning?",
    "What are the key challenges in training large language models?",
] * 10  # 50 prompts total

def benchmark(model_path, tensor_parallel_size, num_prompts):
    print(f"Loading model with tensor_parallel_size={tensor_parallel_size}")
    
    llm = LLM(
        model=model_path,
        tensor_parallel_size=tensor_parallel_size,
        dtype="float16",
    )
    
    sampling_params = SamplingParams(
        temperature=0.0,
        max_tokens=100,
    )
    
    prompts = PROMPTS[:num_prompts]
    
    # Warmup
    print("Warming up...")
    llm.generate(prompts[:5], sampling_params)
    
    # Benchmark
    print(f"Running benchmark with {num_prompts} prompts...")
    start = time.perf_counter()
    outputs = llm.generate(prompts, sampling_params)
    end = time.perf_counter()
    
    total_time = end - start
    total_tokens = sum(len(o.outputs[0].token_ids) for o in outputs)
    throughput = total_tokens / total_time
    ttft = total_time / num_prompts * 1000  # ms per prompt
    
    results = {
        "tensor_parallel_size": tensor_parallel_size,
        "num_prompts": num_prompts,
        "total_time_s": round(total_time, 3),
        "total_tokens": total_tokens,
        "throughput_tokens_per_sec": round(throughput, 2),
        "avg_ttft_ms": round(ttft, 2),
    }
    
    print(json.dumps(results, indent=2))
    return results

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", required=True)
    parser.add_argument("--tensor-parallel-size", type=int, default=1)
    parser.add_argument("--num-prompts", type=int, default=50)
    parser.add_argument("--output-json", required=True)
    args = parser.parse_args()

    import os
    os.makedirs(os.path.dirname(args.output_json), exist_ok=True)

    results = benchmark(args.model, args.tensor_parallel_size, args.num_prompts)

    with open(args.output_json, "w") as f:
        json.dump(results, f, indent=2)
    
    print(f"Results saved to {args.output_json}")