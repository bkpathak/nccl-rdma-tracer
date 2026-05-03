#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <infiniband/verbs.h>

#define MSG_SIZE 64
#define GID_INDEX 1

int main() {
    // Step 1 — open the RDMA device
    int num_devices;
    struct ibv_device **dev_list = ibv_get_device_list(&num_devices);
    if (!dev_list || num_devices == 0) {
        fprintf(stderr, "No RDMA devices found\n");
        return 1;
    }

    printf("Found device: %s\n", ibv_get_device_name(dev_list[0]));

    struct ibv_context *ctx = ibv_open_device(dev_list[0]);
    if (!ctx) { fprintf(stderr, "Failed to open device\n"); return 1; }

    // Step 2 — query device info
    struct ibv_device_attr dev_attr;
    ibv_query_device(ctx, &dev_attr);
    printf("Max QPs: %d, Max CQEs: %d\n", dev_attr.max_qp, dev_attr.max_cqe);

    // Step 3 — allocate Protection Domain
    struct ibv_pd *pd = ibv_alloc_pd(ctx);
    if (!pd) { fprintf(stderr, "Failed to alloc PD\n"); return 1; }
    printf("Protection Domain allocated\n");

    // Step 4 — register a memory region
    char *buf = malloc(MSG_SIZE);
    strcpy(buf, "Hello RDMA");

    struct ibv_mr *mr = ibv_reg_mr(pd, buf, MSG_SIZE,
        IBV_ACCESS_LOCAL_WRITE |
        IBV_ACCESS_REMOTE_READ |
        IBV_ACCESS_REMOTE_WRITE);
    if (!mr) { fprintf(stderr, "Failed to register MR\n"); return 1; }
    printf("Memory Region registered — lkey: %u, rkey: %u\n", mr->lkey, mr->rkey);

    // Step 5 — create Completion Queue
    struct ibv_cq *cq = ibv_create_cq(ctx, 10, NULL, NULL, 0);
    if (!cq) { fprintf(stderr, "Failed to create CQ\n"); return 1; }
    printf("Completion Queue created\n");

    // Step 6 — create Queue Pair
    struct ibv_qp_init_attr qp_attr = {
        .send_cq = cq,
        .recv_cq = cq,
        .cap = {
            .max_send_wr = 10,
            .max_recv_wr = 10,
            .max_send_sge = 1,
            .max_recv_sge = 1,
        },
        .qp_type = IBV_QPT_RC,  // Reliable Connected
    };

    struct ibv_qp *qp = ibv_create_qp(pd, &qp_attr);
    if (!qp) { fprintf(stderr, "Failed to create QP\n"); return 1; }
    printf("Queue Pair created — QPN: %u\n", qp->qp_num);

    // Step 7 — print QP state (starts at RESET)
    struct ibv_qp_attr attr;
    struct ibv_qp_init_attr init_attr;
    ibv_query_qp(qp, &attr, IBV_QP_STATE, &init_attr);
    printf("QP state: %d (0=RESET, 1=INIT, 2=RTR, 3=RTS)\n", attr.qp_state);

    // Cleanup
    ibv_destroy_qp(qp);
    ibv_destroy_cq(cq);
    ibv_dereg_mr(mr);
    ibv_dealloc_pd(pd);
    ibv_close_device(ctx);
    ibv_free_device_list(dev_list);
    free(buf);

    printf("Done — QP lifecycle: RESET → (ready for INIT)\n");
    return 0;
}