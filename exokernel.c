#include <elf.h>
#include <endian.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "helper.h"

void *saved_region;

int main(int argc, char *argv[]) {
  struct ibv_device **dev_list;
  struct ibv_device_attr attr;
  struct ibv_context *ctx;

  struct ibv_helper_context helper_context;

  int num_devices, i, err;
  int remote_qp_num = 2300;

  // didn't use getopt.h to avoid Linux dependencies
  if (argc > 1) {
    remote_qp_num = atoi(argv[1]);
  }

  dev_list = ibv_get_device_list(&num_devices);
  if (!dev_list) {
    perror("Failed to get IB devices list");
    return 1;
  }

  printf("    %-16s\t   node GUID\n", "device");
  printf("    %-16s\t----------------\n", "------");

  for (i = 0; i < num_devices; ++i) {
    printf("    %-16s\t%016llx\n", ibv_get_device_name(dev_list[i]),
           (unsigned long long)be64toh(ibv_get_device_guid(dev_list[i])));
  }

  ctx = ibv_open_device(dev_list[1]);

  if (ctx == NULL)
    printf("ibv_open_device failed?errno: %d\n", errno);

  if (ibv_query_device(ctx, &attr))
    printf("query device failed? errno: %d\n", errno);

  DEBUG_PRINT("max qp: %d\n", attr.max_qp);
  DEBUG_PRINT("max cq: %d\n", attr.max_cq);
  DEBUG_PRINT("max cqe: %d\n", attr.max_cqe);
  DEBUG_PRINT("max sge: %d\n", attr.max_sge);
  DEBUG_PRINT("num ports: %d\n", attr.phys_port_cnt);

  for (i = 0; i < attr.phys_port_cnt; i++) {
    struct ibv_port_attr port_attr;
    ibv_query_port(ctx, 1, &port_attr);
    printf("lid of port: %d\n", port_attr.lid);
  }

  ERR(create_helper_context(ctx, &helper_context));
  /* AT THIS POINT, all resources are created. Now we need to connect the QPs */

  /* Now we need to move the QP through some state machine state, including
   * connecting to remote QP */
  ERR(init_qp(&helper_context, remote_qp_num));

  // Now we need to post a receive request (RR)?
  struct ibv_sge list = {
      .addr = (uintptr_t)helper_context.recv_mr->addr,
      .length = helper_context.recv_mr->length,
      .lkey = helper_context.recv_mr->lkey,
  };
  struct ibv_recv_wr wr = {
      .wr_id = RECV_OPID,
      .sg_list = &list,
      .num_sge = 1,
  };
  struct ibv_recv_wr *bad_wr;

  if (ibv_post_recv(helper_context.qp, &wr, &bad_wr))
    fprintf(stderr, "lol, post_recv didn't work, errno: %d\n", errno);

  /**** Now we have finished setting up RDMA ****/

  // Prepare to receive RPC requests

  struct ibv_sge send_list = {
      .addr = (uintptr_t)helper_context.send_mr->addr,
      .length = helper_context.send_mr->length,
      .lkey = helper_context.send_mr->lkey,
  };
  struct ibv_send_wr send_wr = {
      .wr_id = SEND_OPID,
      .sg_list = &send_list,
      .num_sge = 1,
      .opcode = IBV_WR_SEND,
      //.send_flags = ctx.send_flags,
  };
  int sent = 0;
  struct ibv_send_wr *send_bad_wr;

  struct ibv_wc wc;
  int ne;

  while (1) {
    ERR(poll_cq(&helper_context, &wc));
    if (wc.wr_id == RECV_OPID) {
      DEBUG_PRINT("got a recv completion\n");
      // If it's a recv, post a new recv!
      if (ibv_post_recv(helper_context.qp, &wr, &bad_wr))
        fprintf(stderr, "lol, post_recv didn't work, errno: %d\n", errno);
      else
        fprintf(stderr, "seemed to post recv..\n");

      struct exokernel_rpc *rpc = (struct exokernel_rpc *)wr.sg_list->addr;
      DEBUG_PRINT("rpc type: %d\n", rpc->type);
      switch (rpc->type) {
      case rpc_region_request: {
        struct region_request *req =
            (struct region_request *)&rpc->payload.rreq;
        fprintf(stderr, "rreq start: %lx, size: %lx\n", req->start, req->size);

        void *region = mmap((void *)req->start, req->size,
                            PROT_EXEC | PROT_WRITE | PROT_READ,
                            MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        memset(region, 0, req->size);
        saved_region = region;

        struct region_response resp;
        if (req->enable_mr) {
          // Register newly mapped region as mr with ibv
          struct ibv_mr *new_mr;
          new_mr = ibv_reg_mr(helper_context.pd, region, req->size,
                              IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE);

          if (new_mr == NULL)
            printf("unf, ibv_reg_mr failed, errno: %d\n", errno);

          // Now tell the other side we are ready for the next one
          resp.success = ((uint64_t)region == req->start);
          resp.remote_addr = (uint64_t)region;
          resp.rkey = new_mr->rkey;
        } else {
          resp.success = 1;
          resp.remote_addr = 0;
          resp.rkey = 0;
        }

        marshall_region_response(&resp, (void *)send_wr.sg_list->addr);

        if (ibv_post_send(helper_context.qp, &send_wr, &send_bad_wr))
          fprintf(stderr, "oh god, post_send didn't work..\n");
        break;
      }
      case rpc_run_exokernel_request:
        DEBUG_PRINT("got run_exokernel_request\n");

        struct run_exokernel_request *req =
            (struct run_exokernel_request *)&rpc->payload.rereq;
        printf("stack_ptr: %lx, entry_point: %lx\n", req->stack_ptr,
               req->entry_point);

        __asm__("mov %0,%%r9; "
                "mov %1,%%r8;"
                "mov %%r9,%%rsp;"
                //"lea 0x4(%%rip),%%r10;"
                //"push %%r10;"
                "jmp *%%r8;"
                :
                : "m"(req->stack_ptr), "m"(req->entry_point));

        break;
      default:
        printf("some other rpc type\n");
        break;
      }
    } else {
      DEBUG_PRINT("got a send completion\n");
      // if (ibv_post_recv(qp, &wr, &bad_wr))
      //  fprintf(stderr, "lol, post_recv didn't work, errno: %d\n", errno);
    }
  }

  destroy_helper_context(&helper_context);

  ibv_free_device_list(dev_list);

  return 0;
}
