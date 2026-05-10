#!/bin/bash
set -e

echo "=== Installing bpftrace ==="
sudo apt-get update -q
sudo apt-get install -y bpftrace linux-headers-$(uname -r)

echo "=== Verifying NCCL ==="
ldconfig -p | grep nccl

echo "=== Building nccl-tests ==="
git clone https://github.com/NVIDIA/nccl-tests.git
cd nccl-tests
make -j$(nproc) CUDA_HOME=/usr/local/cuda MPI=0
cd ..

echo "=== GPU Topology ==="
nvidia-smi topo -m

echo "=== Smoke Test ==="
./nccl-tests/build/all_reduce_perf -b 8M -e 8M -f 2 -g 4 -n 5

echo "=== Done ==="