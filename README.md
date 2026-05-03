# nccl-rdma-tracer

eBPF-based tracer for RDMA operations, built to understand what happens
at the kernel level during AllReduce collective communication.

## Background

AI training frameworks like PyTorch use NCCL for gradient synchronization
across GPUs. NCCL uses RDMA (via InfiniBand or RoCE) for fast inter-node
transfers, but the actual kernel-level operations are opaque to most
engineers. This project builds an RDMA ring allreduce from scratch and
traces it with eBPF to make those operations visible.

## What's Inside

```
nccl-rdma-tracer/
├── setup/
│   └── install.sh            # environment setup script
├── rdma-primer/
│   ├── rdma_pingpong.c       # QP lifecycle: RESET -> INIT -> RTR -> RTS
│   └── ring_allreduce.c      # ring allreduce over RDMA, 3 processes
├── ebpf/
│   └── nccl_tracer.bt        # bpftrace script: traces QP transitions and sends
└── analysis/
    ├── parse_events.py        # parse tracer output
    └── plot_allreduce.py      # visualize per-step latency
```

## Environment

Tested on Ubuntu 22.04 with SoftRoCE (no InfiniBand hardware required).

```bash
# Install RDMA stack
sudo apt update && sudo apt install -y \
    rdma-core libibverbs-dev ibverbs-utils \
    infiniband-diags iproute2 gcc bpftrace

# Load SoftRoCE kernel module
sudo modprobe rdma_rxe

# Create virtual RDMA device (replace ens5 with your NIC name)
sudo rdma link add rxe0 type rxe netdev ens5

# Verify
ibv_devices
rdma link show
```

## Running the RDMA Pingpong

Basic QP lifecycle demo: opens a device, allocates PD/MR/CQ/QP,
walks through state transitions, and sends a message.

```bash
gcc -o rdma-primer/rdma_pingpong rdma-primer/rdma_pingpong.c -libverbs

# Terminal 1: server
./rdma-primer/rdma_pingpong

# Terminal 2: client
./rdma-primer/rdma_pingpong 127.0.0.1
```

Expected output:
```
[server] QP state -> INIT
[server] QP state -> RTR
[server] QP state -> RTS, ready!
[server] success, message: "Hello from RDMA client!"
```

## Running the Ring AllReduce

Three processes form a ring and reduce their values over RDMA.
Expected result: all ranks converge to 6.0 (1+2+3).

```bash
gcc -o rdma-primer/ring_allreduce rdma-primer/ring_allreduce.c -libverbs
./rdma-primer/ring_allreduce
```

Expected output:
```
[rank 0] step 0, sum=4.0 latency=51 us
[rank 1] step 0, sum=3.0 latency=1664 us
[rank 2] step 0, sum=5.0 latency=33 us
[rank 0] step 1, sum=6.0 latency=31 us
[rank 1] step 1, sum=6.0 latency=859 us
[rank 2] step 1, sum=6.0 latency=34 us
[rank 0] allreduce complete, final value: 6.0 (expected: 6.0)
```

## Running the eBPF Tracer

Traces QP state transitions and send events using fentry hooks on
the rdma_rxe kernel module.

```bash
# Terminal 1: start tracer
sudo bpftrace ebpf/nccl_tracer.bt

# Terminal 2: run allreduce
./rdma-primer/ring_allreduce
```

Sample tracer output:
```
TIME(us)   EVENT       PID    COMM             DETAIL
2060272    modify_qp   7096   ring_allreduce   RESET->INIT
2060289    modify_qp   7096   ring_allreduce   RESET->INIT
2063676    modify_qp   7096   ring_allreduce   INIT->RTR
2063688    modify_qp   7096   ring_allreduce   RTR->RTS
2063791    post_send   7096   ring_allreduce   step=1
2063985    post_send   7096   ring_allreduce   step=2
```

## Latency Results (SoftRoCE on EC2)

```
Step 0: rank0=51us  rank1=1664us  rank2=33us
Step 1: rank0=31us  rank1=859us   rank2=34us
```

rank0 and rank2 consistently complete in 30-50 us. rank1 shows jitter,
likely OS scheduling on EC2. Numbers include barrier synchronization
overhead. Real InfiniBand raw message latency is ~1-2 us.

## Limitations

SoftRoCE emulates RDMA in software on top of regular Ethernet. Some
kernel hooks that fire on real InfiniBand hardware (poll_cq, post_recv
via rdma_core tracepoints) do not fire on SoftRoCE because these
functions are dispatched via function pointers in ib_device_ops rather
than direct calls. The tracer is most useful for QP lifecycle events
and send-side visibility.

## Blog Post

[Tracing RDMA AllReduce with eBPF](https://bkpathak.github.io)

## References

- [RDMA Aware Networks Programming User Manual](https://docs.nvidia.com/networking/display/RDMAAwareProgrammingv17)
- [Linux kernel rdma_rxe driver](https://github.com/torvalds/linux/tree/master/drivers/infiniband/sw/rxe)
- [NCCL source code](https://github.com/NVIDIA/nccl)
- [bpftrace reference guide](https://github.com/bpftrace/bpftrace/blob/master/docs/reference_guide.md)