#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <infiniband/verbs.h>

#define MSG_SIZE    64
#define GID_INDEX   1
#define IB_PORT     1
#define TCP_PORT    18515

struct conn_info {
    uint32_t qpn;
    uint32_t psn;
    union ibv_gid gid;
};

void exchange_info(int is_server, const char *peer_ip,
                   struct conn_info *local, struct conn_info *remote) {
    int sockfd;
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(TCP_PORT),
    };

    if (is_server) {
        int listenfd = socket(AF_INET, SOCK_STREAM, 0);
        addr.sin_addr.s_addr = INADDR_ANY;
        int opt = 1;
        setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        bind(listenfd, (struct sockaddr *)&addr, sizeof(addr));
        listen(listenfd, 1);
        printf("[server] waiting for client...\n");
        sockfd = accept(listenfd, NULL, NULL);
        close(listenfd);
    } else {
        sockfd = socket(AF_INET, SOCK_STREAM, 0);
        inet_pton(AF_INET, peer_ip, &addr.sin_addr);
        while (connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)) != 0)
            sleep(1);
    }

    send(sockfd, local, sizeof(*local), 0);
    recv(sockfd, remote, sizeof(*remote), MSG_WAITALL);
    close(sockfd);
}

int main(int argc, char *argv[]) {
    int is_server = (argc == 1);
    char *peer_ip = is_server ? NULL : argv[1];

    // Step 1 — open device
    int num_devices;
    struct ibv_device **dev_list = ibv_get_device_list(&num_devices);
    struct ibv_context *ctx = ibv_open_device(dev_list[0]);
    ibv_free_device_list(dev_list);
    printf("[%s] opened device\n", is_server ? "server" : "client");

    // Step 2 — allocate PD
    struct ibv_pd *pd = ibv_alloc_pd(ctx);

    // Step 3 — register MR
    char *buf = calloc(1, MSG_SIZE);
    if (is_server) strcpy(buf, "Hello from RDMA server!");

    struct ibv_mr *mr = ibv_reg_mr(pd, buf, MSG_SIZE,
        IBV_ACCESS_LOCAL_WRITE  |
        IBV_ACCESS_REMOTE_READ  |
        IBV_ACCESS_REMOTE_WRITE);
    printf("[%s] MR registered — lkey=%u rkey=%u\n",
           is_server ? "server" : "client", mr->lkey, mr->rkey);

    // Step 4 — create CQ
    struct ibv_cq *cq = ibv_create_cq(ctx, 10, NULL, NULL, 0);

    // Step 5 — create QP
    struct ibv_qp_init_attr qp_init_attr = {
        .send_cq = cq,
        .recv_cq = cq,
        .cap = {
            .max_send_wr  = 10,
            .max_recv_wr  = 10,
            .max_send_sge = 1,
            .max_recv_sge = 1,
        },
        .qp_type = IBV_QPT_RC,
    };
    struct ibv_qp *qp = ibv_create_qp(pd, &qp_init_attr);
    printf("[%s] QP created — QPN=%u (RESET)\n",
           is_server ? "server" : "client", qp->qp_num);

    // Step 6 — RESET → INIT
    struct ibv_qp_attr init_attr = {
        .qp_state        = IBV_QPS_INIT,
        .pkey_index      = 0,
        .port_num        = IB_PORT,
        .qp_access_flags = IBV_ACCESS_REMOTE_WRITE |
                           IBV_ACCESS_REMOTE_READ  |
                           IBV_ACCESS_LOCAL_WRITE,
    };
    ibv_modify_qp(qp, &init_attr,
        IBV_QP_STATE | IBV_QP_PKEY_INDEX |
        IBV_QP_PORT  | IBV_QP_ACCESS_FLAGS);
    printf("[%s] QP state → INIT\n", is_server ? "server" : "client");

    // Step 7 — exchange connection info over TCP
    union ibv_gid local_gid;
    ibv_query_gid(ctx, IB_PORT, GID_INDEX, &local_gid);

    struct conn_info local_info = {
        .qpn = qp->qp_num,
        .psn = lrand48() & 0xffffff,
        .gid = local_gid,
    };
    struct conn_info remote_info;
    exchange_info(is_server, peer_ip, &local_info, &remote_info);
    printf("[%s] exchanged info — remote QPN=%u\n",
           is_server ? "server" : "client", remote_info.qpn);

    // Step 8 — INIT → RTR
    struct ibv_qp_attr rtr_attr = {
        .qp_state           = IBV_QPS_RTR,
        .path_mtu           = IBV_MTU_1024,
        .dest_qp_num        = remote_info.qpn,
        .rq_psn             = remote_info.psn,
        .max_dest_rd_atomic = 1,
        .min_rnr_timer      = 12,
        .ah_attr = {
            .is_global  = 1,
            .grh = {
                .dgid       = remote_info.gid,
                .sgid_index = GID_INDEX,
                .hop_limit  = 1,
            },
            .port_num   = IB_PORT,
        },
    };
    ibv_modify_qp(qp, &rtr_attr,
        IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
        IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
        IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER);
    printf("[%s] QP state → RTR\n", is_server ? "server" : "client");

    // Step 9 — RTR → RTS
    struct ibv_qp_attr rts_attr = {
        .qp_state      = IBV_QPS_RTS,
        .timeout       = 14,
        .retry_cnt     = 7,
        .rnr_retry     = 7,
        .sq_psn        = local_info.psn,
        .max_rd_atomic = 1,
    };
    ibv_modify_qp(qp, &rts_attr,
        IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
        IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC);
    printf("[%s] QP state → RTS — ready!\n", is_server ? "server" : "client");

    // Step 10 — post recv (server) or send (client)
    struct ibv_sge sge = {
        .addr   = (uint64_t)buf,
        .length = MSG_SIZE,
        .lkey   = mr->lkey,
    };

    if (is_server) {
        struct ibv_recv_wr recv_wr = {
            .wr_id   = 1,
            .sg_list = &sge,
            .num_sge = 1,
        };
        struct ibv_recv_wr *bad_wr;
        ibv_post_recv(qp, &recv_wr, &bad_wr);
        printf("[server] recv WR posted — waiting for message...\n");
    } else {
        strcpy(buf, "Hello from RDMA client!");
        struct ibv_send_wr send_wr = {
            .wr_id      = 1,
            .sg_list    = &sge,
            .num_sge    = 1,
            .opcode     = IBV_WR_SEND,
            .send_flags = IBV_SEND_SIGNALED,
        };
        struct ibv_send_wr *bad_wr;
        ibv_post_send(qp, &send_wr, &bad_wr);
        printf("[client] send WR posted\n");
    }

    // Step 11 — poll CQ for completion
    struct ibv_wc wc;
    printf("[%s] polling CQ...\n", is_server ? "server" : "client");
    while (ibv_poll_cq(cq, 1, &wc) == 0);

    if (wc.status == IBV_WC_SUCCESS) {
        printf("[%s] success — message: \"%s\"\n",
               is_server ? "server" : "client", buf);
    } else {
        printf("[%s] FAILED — status: %d\n",
               is_server ? "server" : "client", wc.status);
    }

    // Cleanup
    ibv_destroy_qp(qp);
    ibv_destroy_cq(cq);
    ibv_dereg_mr(mr);
    ibv_dealloc_pd(pd);
    ibv_close_device(ctx);
    free(buf);

    return 0;
}