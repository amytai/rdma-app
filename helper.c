#include <stdlib.h>

#include "helper.h"

#define BUF_SIZE 1024

int poll_cq(struct ibv_helper_context *helper_context, struct ibv_wc *wc) {
  int ne;
  do {
    ne = ibv_poll_cq(helper_context->cq, 1, wc);
    if (ne < 0) {
      DEBUG_PRINT("poll CQ failed %d\n", ne);
      return 1;
    }

  } while (ne < 1);

  DEBUG_PRINT("num entries polled: %d\n", ne);
  if (wc->status != IBV_WC_SUCCESS) {
    fprintf(stderr, "Failed status %s (%d) for wr_id %d\n",
            ibv_wc_status_str(wc->status), wc->status, (int)wc->wr_id);
    return 1;
  }

  return 0;
}

int create_helper_context(struct ibv_context *ctx,
                          struct ibv_helper_context *helper_context) {
  struct ibv_qp_init_attr qp_init_attr;
  struct ibv_qp_init_attr query_init_attr;
  struct ibv_qp_attr query_qp_attr;
  void *rdma_buf;
  void *rdma_buf_read;
  int err;
  int cq_size = 1;

  rdma_buf = malloc(BUF_SIZE);
  rdma_buf_read = malloc(BUF_SIZE);

  helper_context->ctx = ctx;

  /* Allocate a PD, in preparation for memory allocation */
  helper_context->pd = ibv_alloc_pd(ctx);

  helper_context->cq = ibv_create_cq(ctx, cq_size, NULL, NULL, 0);

  memset(&qp_init_attr, 0, sizeof(qp_init_attr));

  qp_init_attr.qp_context = ctx;
  qp_init_attr.qp_type = IBV_QPT_UC;
  qp_init_attr.sq_sig_all = 1;
  qp_init_attr.send_cq = helper_context->cq;
  qp_init_attr.recv_cq = helper_context->cq;
  qp_init_attr.cap.max_send_wr = 1;
  qp_init_attr.cap.max_recv_wr = 1;
  qp_init_attr.cap.max_send_sge = 1;
  qp_init_attr.cap.max_recv_sge = 1;

  helper_context->qp = ibv_create_qp(helper_context->pd, &qp_init_attr);

  if (helper_context->qp == NULL) {
    DEBUG_PRINT("lol qp is null\n");
    return 1;
  }

  fprintf(stderr, "qp_num: %d\n", helper_context->qp->qp_num);

  // Try to query the attributes of the qp?
  ERR(ibv_query_qp(helper_context->qp, &query_qp_attr,
                   IBV_QP_QKEY | IBV_QP_STATE | IBV_QP_PATH_MTU | IBV_QP_PORT,
                   &query_init_attr));

  DEBUG_PRINT("results of query: %d %d %d %d\n", query_qp_attr.qp_state,
              query_qp_attr.port_num, query_qp_attr.qp_access_flags,
              query_qp_attr.qkey);

  helper_context->send_mr =
      ibv_reg_mr(helper_context->pd, rdma_buf, BUF_SIZE,
                 IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE);
  helper_context->recv_mr =
      ibv_reg_mr(helper_context->pd, rdma_buf_read, BUF_SIZE,
                 IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE);

  if (helper_context->send_mr || helper_context->recv_mr == NULL) {
    printf("unf, ibv_reg_mr failed\n");
    return 1;
  }

  return 0;
}

void destroy_helper_context(struct ibv_helper_context *helper_context) {
  ibv_destroy_qp(helper_context->qp);
  ibv_destroy_cq(helper_context->cq);

  ibv_dereg_mr(helper_context->send_mr);
  ibv_dereg_mr(helper_context->recv_mr);
  ibv_dealloc_pd(helper_context->pd);
  ibv_close_device(helper_context->ctx);
}

int init_qp(struct ibv_helper_context *helper_context, int remote_qp_num) {
  struct ibv_qp_init_attr query_init_attr;
  struct ibv_qp_attr query_qp_attr;
  struct ibv_qp_attr qp_attr;
  union ibv_gid my_gid;
  union ibv_gid remote_gid;
  int err;

  /* Move QP from RESET to INIT state */
  memset(&qp_attr, 0, sizeof(qp_attr));

  qp_attr.qp_state = IBV_QPS_INIT;
  qp_attr.port_num = 1; // TODO: what's the port num??
  qp_attr.qp_access_flags =
      IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;

  /* IMPORTANT: the flag masks that we pass to modify_qp depends on the qp_type
   * (IBV_QPT_UD vs IBV_QPT_UC, etc). For example, for IBV_QPT_UD, the flags
   * MUST be IBV_QP_STATE | IBV_QP_PORT | IBV_QP_QKEY | IBV_QP_PKEY_INDEX and
   * for IBV_QPT_UC, the flags MUST be IBV_QP_STATE | IBV_QP_PORT |
   * IBV_QP_PKEY_INDEX | IBV_QP_ACCESS_FLAGS.
   *
   * For this test, we are using IBV_QPT_UC (unreliable connected) type.
   */
  ERR(ibv_modify_qp(helper_context->qp, &qp_attr,
                    IBV_QP_STATE | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS |
                        IBV_QP_PKEY_INDEX));

  /* Now we need to move the QP from INIT to RTR */
  memset(&qp_attr, 0, sizeof(qp_attr));
  qp_attr.qp_state = IBV_QPS_RTR;

  qp_attr.path_mtu = IBV_MTU_512;      // This is the recommended value
  qp_attr.dest_qp_num = remote_qp_num; // This is the remote qp_num
  qp_attr.rq_psn = 0;

  qp_attr.ah_attr.dlid = 0; // this is likely 0 if the remote has only 1 port
  qp_attr.ah_attr.sl = 0;
  qp_attr.ah_attr.src_path_bits = 0;
  qp_attr.ah_attr.port_num = 1;

  // Inspect these GIDs...
  ERR(ibv_query_gid(helper_context->ctx, 1, 0, &my_gid));

  DEBUG_PRINT("gid: %016llx\n", my_gid.global.interface_id);

  remote_gid.global.subnet_prefix = 0x80fe;
  // Set the interface_id to the hard-coded id that is NOT the gid of this
  // machine
  if (my_gid.global.interface_id == GID_1)
    remote_gid.global.interface_id = GID_2;
  else
    remote_gid.global.interface_id = GID_1;

  qp_attr.ah_attr.is_global = 1; // Over ROCE means this has to be global
  qp_attr.ah_attr.grh.hop_limit = 1;
  qp_attr.ah_attr.grh.dgid = remote_gid;
  qp_attr.ah_attr.grh.sgid_index = 0;

  ERR(ibv_modify_qp(helper_context->qp, &qp_attr,
                    IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
                        IBV_QP_DEST_QPN | IBV_QP_RQ_PSN));

  // We also need to move qp to a RTS state....
  qp_attr.qp_state = IBV_QPS_RTS;
  qp_attr.sq_psn = helper_context->qp->qp_num;
  ERR(ibv_modify_qp(helper_context->qp, &qp_attr,
                    IBV_QP_STATE | IBV_QP_SQ_PSN));

  memset(&query_qp_attr, 0, sizeof(query_qp_attr));
  ERR(ibv_query_qp(helper_context->qp, &query_qp_attr,
                   IBV_QP_QKEY | IBV_QP_STATE | IBV_QP_PATH_MTU | IBV_QP_PORT,
                   &query_init_attr));
  fprintf(stderr, "state: %d\n", query_qp_attr.qp_state);
}

void marshall_region_request(struct region_request *req, void *buf) {
  struct exokernel_rpc rpc = {
      .type = rpc_region_request,
      .payload.rreq =
          {
              .start = req->start,
              .size = req->size,
          },
  };
  memcpy(buf, (void *)&rpc, sizeof(rpc));
};

void marshall_run_exokernel_request(struct run_exokernel_request *req,
                                    void *buf) {
  struct exokernel_rpc rpc = {
      .type = rpc_run_exokernel_request,
      .payload.rereq =
          {
              .stack_ptr = req->stack_ptr,
              .entry_point = req->entry_point,
          },
  };
  memcpy(buf, (void *)&rpc, sizeof(rpc));
};

void marshall_region_response(struct region_response *resp, void *buf) {
  struct exokernel_rpc rpc = {
      .type = rpc_region_response,
      .payload.rresp =
          {
              .success = resp->success,
              .remote_addr = resp->remote_addr,
              .rkey = resp->rkey,
          },
  };
  memcpy(buf, (void *)&rpc, sizeof(rpc));
}
