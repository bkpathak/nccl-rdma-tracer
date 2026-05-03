#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <arpa/inet.h>
#include <infiniband/verbs.h>

#define NUM_PROCS   3
#define DATA_SIZE   16
#define GID_INDEX   1
#define IB_PORT     1

struct conn_info {
    uint32_t qpn;
    uint32_t psn;
    union ibv_gid gid;
};

struct shared_info {
    struct conn_info send_info[NUM_PROCS];  // send_qp info
    struct conn_info recv_info[NUM_PROCS];  // recv_qp info
    int ready[NUM_PROCS];
    int step_sync[NUM_PROCS];
};

struct ibv_qp *create_qp(struct ibv_pd *pd, struct ibv_cq *cq) {
    struct ibv_qp_init_attr attr = {
        .send_cq = cq,
        .recv_cq = cq,
        .cap = {
            .max_send_wr  = 16,
            .max_recv_wr  = 16,
            .max_send_sge = 1,
            .max_recv_sge = 1,
        },
        .qp_type = IBV_QPT_RC,
    };
    return ibv_create_qp(pd, &attr);
}

int move_to_init(struct ibv_qp *qp) {
    struct ibv_qp_attr attr = {
        .qp_state        = IBV_QPS_INIT,
        .pkey_index      = 0,
        .port_num        = IB_PORT,
        .qp_access_flags = IBV_ACCESS_REMOTE_WRITE |
                           IBV_ACCESS_REMOTE_READ  |
                           IBV_ACCESS_LOCAL_WRITE,
    };
    return ibv_modify_qp(qp, &attr,
        IBV_QP_STATE | IBV_QP_PKEY_INDEX |
        IBV_QP_PORT  | IBV_QP_ACCESS_FLAGS);
}

int move_to_rtr(struct ibv_qp *qp, struct conn_info *remote) {
    struct ibv_qp_attr attr = {
        .qp_state           = IBV_QPS_RTR,
        .path_mtu           = IBV_MTU_1024,
        .dest_qp_num        = remote->qpn,
        .rq_psn             = remote->psn,
        .max_dest_rd_atomic = 1,
        .min_rnr_timer      = 12,
        .ah_attr = {
            .is_global  = 1,
            .grh = {
                .dgid       = remote->gid,
                .sgid_index = GID_INDEX,
                .hop_limit  = 1,
            },
            .port_num = IB_PORT,
        },
    };
    return ibv_modify_qp(qp, &attr,
        IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
        IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
        IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER);
}

int move_to_rts(struct ibv_qp *qp, uint32_t psn) {
    struct ibv_qp_attr attr = {
        .qp_state      = IBV_QPS_RTS,
        .timeout       = 14,
        .retry_cnt     = 7,
        .rnr_retry     = 7,
        .sq_psn        = psn,
        .max_rd_atomic = 1,
    };
    return ibv_modify_qp(qp, &attr,
        IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
        IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC);
}

void post_recv(struct ibv_qp *qp, struct ibv_mr *mr, float *buf) {
    struct ibv_sge sge = {
        .addr   = (uint64_t)buf,
        .length = DATA_SIZE * sizeof(float),
        .lkey   = mr->lkey,
    };
    struct ibv_recv_wr wr = {
        .wr_id   = 1,
        .sg_list = &sge,
        .num_sge = 1,
    };
    struct ibv_recv_wr *bad;
    ibv_post_recv(qp, &wr, &bad);
}

void post_send(struct ibv_qp *qp, struct ibv_mr *mr, float *buf) {
    struct ibv_sge sge = {
        .addr   = (uint64_t)buf,
        .length = DATA_SIZE * sizeof(float),
        .lkey   = mr->lkey,
    };
    struct ibv_send_wr wr = {
        .wr_id      = 1,
        .sg_list    = &sge,
        .num_sge    = 1,
        .opcode     = IBV_WR_SEND,
        .send_flags = IBV_SEND_SIGNALED,
    };
    struct ibv_send_wr *bad;
    ibv_post_send(qp, &wr, &bad);
}

void poll_cq(struct ibv_cq *cq) {
    struct ibv_wc wc;
    while (ibv_poll_cq(cq, 1, &wc) == 0);
    if (wc.status != IBV_WC_SUCCESS)
        fprintf(stderr, "CQ error: %d\n", wc.status);
}

void run_process(int rank, struct shared_info *shared) {
    int left  = (rank - 1 + NUM_PROCS) % NUM_PROCS;
    int right = (rank + 1) % NUM_PROCS;

    printf("[rank %d] starting — left=%d right=%d\n", rank, left, right);
    sleep(3);

    // Step 1 — open device
    int num_devices;
    struct ibv_device **dev_list = ibv_get_device_list(&num_devices);
    struct ibv_context *ctx = ibv_open_device(dev_list[0]);
    ibv_free_device_list(dev_list);

    // Step 2 — alloc PD, register MR
    struct ibv_pd *pd = ibv_alloc_pd(ctx);

    float *send_buf = calloc(DATA_SIZE, sizeof(float));
    float *recv_buf = calloc(DATA_SIZE, sizeof(float));

    for (int i = 0; i < DATA_SIZE; i++)
        send_buf[i] = rank + 1.0f;

    struct ibv_mr *send_mr = ibv_reg_mr(pd, send_buf,
        DATA_SIZE * sizeof(float),
        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ);

    struct ibv_mr *recv_mr = ibv_reg_mr(pd, recv_buf,
        DATA_SIZE * sizeof(float),
        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);

    // Step 3 — create CQ and two QPs
    struct ibv_cq *cq = ibv_create_cq(ctx, 32, NULL, NULL, 0);
    struct ibv_qp *send_qp = create_qp(pd, cq);
    struct ibv_qp *recv_qp = create_qp(pd, cq);

    move_to_init(send_qp);
    move_to_init(recv_qp);

    // Step 4 — publish both QP infos in shared memory
    union ibv_gid gid;
    ibv_query_gid(ctx, IB_PORT, GID_INDEX, &gid);

    shared->send_info[rank].qpn = send_qp->qp_num;
    shared->send_info[rank].psn = lrand48() & 0xffffff;
    shared->send_info[rank].gid = gid;

    shared->recv_info[rank].qpn = recv_qp->qp_num;
    shared->recv_info[rank].psn = lrand48() & 0xffffff;
    shared->recv_info[rank].gid = gid;

    __sync_synchronize();
    shared->ready[rank] = 1;

    // Step 5 — wait for all processes to publish their info
    for (int i = 0; i < NUM_PROCS; i++)
        while (!shared->ready[i]) usleep(1000);

    printf("[rank %d] all processes ready — connecting QPs\n", rank);

   // Step 6 — connect QPs
    // send_qp connects to right neighbor's recv_qp
    move_to_rtr(send_qp, &shared->recv_info[right]);
    move_to_rts(send_qp, shared->send_info[rank].psn);

    // recv_qp connects to left neighbor's send_qp
    move_to_rtr(recv_qp, &shared->send_info[left]);
    move_to_rts(recv_qp, shared->recv_info[rank].psn);

    // Step 7 — ring allreduce
    float *local_sum = calloc(DATA_SIZE, sizeof(float));
    float *fwd_buf   = calloc(DATA_SIZE, sizeof(float));

    for (int i = 0; i < DATA_SIZE; i++)
        local_sum[i] = send_buf[i];

    for (int step = 0; step < NUM_PROCS - 1; step++) {
        if (step == 0)
            memcpy(send_buf, local_sum, DATA_SIZE * sizeof(float));
        else
            memcpy(send_buf, fwd_buf, DATA_SIZE * sizeof(float));

        post_recv(recv_qp, recv_mr, recv_buf);

        __sync_fetch_and_add(&shared->step_sync[step], 1);
        while (shared->step_sync[step] < NUM_PROCS) usleep(100);

        struct timespec t_start, t_end;
        clock_gettime(CLOCK_MONOTONIC, &t_start);

        post_send(send_qp, send_mr, send_buf);
        poll_cq(cq);  // send completion
        poll_cq(cq);  // recv completion

        clock_gettime(CLOCK_MONOTONIC, &t_end);

        long latency_us = (t_end.tv_sec - t_start.tv_sec) * 1000000 +
                        (t_end.tv_nsec - t_start.tv_nsec) / 1000;

        for (int i = 0; i < DATA_SIZE; i++)
            local_sum[i] += recv_buf[i];

        memcpy(fwd_buf, recv_buf, DATA_SIZE * sizeof(float));

        printf("[rank %d] step %d — sum=%.1f latency=%ld us\n",
           rank, step, local_sum[0], latency_us);
    }
    printf("[rank %d] allreduce complete — final value: %.1f (expected: %.1f)\n",
           rank, local_sum[0], (float)(NUM_PROCS * (NUM_PROCS + 1) / 2));

    free(local_sum);
    free(fwd_buf);

    // Cleanup
    ibv_destroy_qp(send_qp);
    ibv_destroy_qp(recv_qp);
    ibv_destroy_cq(cq);
    ibv_dereg_mr(send_mr);
    ibv_dereg_mr(recv_mr);
    ibv_dealloc_pd(pd);
    ibv_close_device(ctx);
    free(send_buf);
    free(recv_buf);
}

int main() {
    struct shared_info *shared = mmap(NULL, sizeof(struct shared_info),
        PROT_READ | PROT_WRITE,
        MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    memset(shared, 0, sizeof(struct shared_info));

    for (int rank = 0; rank < NUM_PROCS; rank++) {
        pid_t pid = fork();
        if (pid == 0) {
            run_process(rank, shared);
            exit(0);
        }
    }

    for (int i = 0; i < NUM_PROCS; i++)
        wait(NULL);

    printf("[parent] all processes done\n");
    munmap(shared, sizeof(struct shared_info));
    return 0;
}