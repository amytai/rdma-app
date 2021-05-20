#include <stdio.h>
#include <stdlib.h>
#include <endian.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <elf.h>

#include "helper.h"

#define STACK_BOTTOM    0x700000000000
#define STACK_SIZE      (1024 * 1024 * 1024)
#define BUF_SIZE        1024

static int poll_cq(struct ibv_cq *cq, struct ibv_wc *wc) {
    int ne;
    do {
        ne = ibv_poll_cq(cq, 1, wc);
        if (ne < 0) {
            DEBUG_PRINT("poll CQ failed %d\n", ne);
            return 1;
        }

    } while (ne < 1);

    DEBUG_PRINT("num entries polled: %d\n", ne);
    if (wc->status != IBV_WC_SUCCESS) {
        fprintf(stderr, "Failed status %s (%d) for wr_id %d\n",
                ibv_wc_status_str(wc->status),
                wc->status, (int) wc->wr_id);
        return 1;
    }

    return 0;
}

static int load_segment(int fd, Elf64_Addr p_vaddr, Elf64_Addr p_filesz, Elf64_Off p_offset, 
        struct ibv_helper_context *helper_context, struct ibv_recv_wr *recv_wr) {
    struct ibv_wc wc;
    struct ibv_recv_wr *bad_wr;
    struct ibv_send_wr *send_bad_wr;
    size_t mmap_length;
    void *region;
	int err;

    struct ibv_sge send_list = {
        .addr    = (uintptr_t) helper_context->send_mr->addr,
        .length = helper_context->send_mr->length,
        .lkey    = helper_context->send_mr->lkey,
    };
    struct ibv_send_wr send_wr = {
        .wr_id = SEND_OPID,
        .sg_list    = &send_list,
        .num_sge    = 1,
        .opcode     = IBV_WR_SEND,
    };

	size_t offset = p_vaddr - (p_vaddr / 4096) * 4096;

	mmap_length = (((offset + p_filesz) / 4096) + 1) * 4096;

    // Now we make a region request:
    struct region_request req = {
        .start = p_vaddr - offset,
        .size = mmap_length,
    };
    
    // Now copy this region request into the send buffer
    marshall_region_request(&req, (void *)send_wr.sg_list->addr);

    if (ibv_post_send(helper_context->qp, &send_wr, &send_bad_wr))
        DEBUG_PRINT("oh god, post_send didn't work when sending region request..\n");

    DEBUG_PRINT("just sent load_segment request for addr %lx\n", req.start);

    int done = 0;
    
    struct ibv_mr *new_mr;
    void *mmapped_region;

    while (1) {
        ERR(poll_cq(helper_context->cq, &wc));

        // If it's a recv, post a new recv!
        if (wc.wr_id == RECV_OPID) {
            DEBUG_PRINT("completed a receive packet\n");

            if (ibv_post_recv(helper_context->qp, recv_wr, &bad_wr))
                fprintf(stderr, "lol, post_recv didn't work, errno: %d\n", errno);

            struct exokernel_rpc *rpc = (struct exokernel_rpc *) recv_wr->sg_list->addr;
            if (rpc->type == rpc_region_response) {
                if (rpc->payload.rresp.success) {
                    // OK now that we received the response for this RPC, we can continue
                    DEBUG_PRINT("received successful region_response \
                            remote_addr: %lx \
                            rkey: %x \n", rpc->payload.rresp.remote_addr, rpc->payload.rresp.rkey);
                }
                // Now we have to RDMA the segment into the mmap'd region.
                mmapped_region = mmap(0, mmap_length, PROT_WRITE | PROT_READ, MAP_PRIVATE, fd, p_offset - offset);
                if (mmapped_region == (void *) -1)
                    DEBUG_PRINT("shit, mmap failed on controller, errno: %d\n", errno);

                // Now register this as a new memory region
                new_mr = ibv_reg_mr(helper_context->pd, mmapped_region, mmap_length, IBV_ACCESS_LOCAL_WRITE);
                if (new_mr == NULL)
                    printf("unf, ibv_reg_mr failed\n");

                // Now write shit to that MR, and send another RPC, to check
                send_wr.wr_id = ONE_SIDED_WRITE_OPID;
                send_wr.opcode = IBV_WR_RDMA_WRITE;
                send_wr.wr.rdma.remote_addr = rpc->payload.rresp.remote_addr + offset;
                send_wr.wr.rdma.rkey = rpc->payload.rresp.rkey;

                struct ibv_sge one_sided_list = {
                    .addr = (uint64_t) mmapped_region + offset,
                    .lkey = new_mr->lkey,
                    .length = p_filesz,
                };
                send_wr.sg_list = &one_sided_list;

                if (ibv_post_send(helper_context->qp, &send_wr, &send_bad_wr))
                    DEBUG_PRINT("oh god, post_send didn't work.. while trying one-sided RDMA\n");
                else
                    DEBUG_PRINT("ok, just tried a one-sided RDMA write...\n");

            }
          } else if (wc.wr_id == ONE_SIDED_WRITE_OPID) {
              printf("allegedly finished one sided write..\n");

              ibv_dereg_mr(new_mr);
              munmap(mmapped_region, mmap_length);

              break;

          } else {
            DEBUG_PRINT("completed a send packet\n");
            if (done)
                break;
          }
    }


	return 0;
}

static int setup_stack(Elf64_Addr e_entry, struct ibv_helper_context *helper_context, struct ibv_recv_wr *recv_wr) {
    struct ibv_wc wc;
    struct ibv_recv_wr *bad_wr;
    struct ibv_send_wr *send_bad_wr;
	int err;
    
    struct ibv_sge send_list = {
        .addr    = (uintptr_t) helper_context->send_mr->addr,
        .length = helper_context->send_mr->length,
        .lkey    = helper_context->send_mr->lkey,
    };
    struct ibv_send_wr send_wr = {
        .wr_id = SEND_OPID,
        .sg_list    = &send_list,
        .num_sge    = 1,
        .opcode     = IBV_WR_SEND,
    };

    struct region_request req = {
        .start = STACK_BOTTOM,
        .size = STACK_SIZE,
    };
    marshall_region_request(&req, (void *)send_wr.sg_list->addr);

    if (ibv_post_send(helper_context->qp, &send_wr, &send_bad_wr))
        DEBUG_PRINT("oh god, post_send didn't work when sending region request..\n");

    DEBUG_PRINT("just asked exokernel to allocate stack memoryr addr\n");

    while (1) {
        ERR(poll_cq(helper_context->cq, &wc));

        // If it's a recv, post a new recv!
        if (wc.wr_id == RECV_OPID) {
            DEBUG_PRINT("completed a receive packet\n");

            if (ibv_post_recv(helper_context->qp, recv_wr, &bad_wr))
                fprintf(stderr, "lol, post_recv didn't work, errno: %d\n", errno);

            struct exokernel_rpc *rpc = (struct exokernel_rpc *) recv_wr->sg_list->addr;
            if (rpc->type == rpc_region_response) {
                if (rpc->payload.rresp.success) {
                    DEBUG_PRINT("received successful region_response \
                            remote_addr: %lx \
                            rkey: %x \n", rpc->payload.rresp.remote_addr, rpc->payload.rresp.rkey);
                }
                // OK, now the stack is allocated. So the controller needs to write
                // stuff onto the stack.

                uint64_t *scratch_buf = (uint64_t *)send_wr.sg_list->addr;
                scratch_buf[0] = 0; // argc
                scratch_buf[1] = 0; // NULL argv
                scratch_buf[2] = 0; // NULL envp
                scratch_buf[3] = 25; // aux AT_RANDOM
                scratch_buf[4] = rpc->payload.rresp.remote_addr + STACK_SIZE - 32; // address of RANDOM values
                scratch_buf[5] = 9; // some other AUX var
                scratch_buf[6] = e_entry;
                scratch_buf[7] = 0;
                scratch_buf[8] = 0;

                // AT_RANDOM values
                scratch_buf[9] = 0xdeadbeef;
                scratch_buf[10] = 0xabcd1234;
                scratch_buf[11] = 0xdeadbeef;
                scratch_buf[12] = 0xabcd1234;

                // Now write shit to that MR, and send another RPC, to check
                send_wr.wr_id = ONE_SIDED_WRITE_OPID;
                send_wr.opcode = IBV_WR_RDMA_WRITE;
                send_wr.wr.rdma.remote_addr = rpc->payload.rresp.remote_addr + STACK_SIZE - 13 * 4;
                send_wr.wr.rdma.rkey = rpc->payload.rresp.rkey;

                send_wr.sg_list->length = 13 * 4;

                if (ibv_post_send(helper_context->qp, &send_wr, &send_bad_wr))
                    DEBUG_PRINT("oh god, post_send didn't work.. while trying one-sided RDMA\n");
                else
                    DEBUG_PRINT("ok, just tried a one-sided RDMA write...\n");

            }
        } else if (wc.wr_id == ONE_SIDED_WRITE_OPID) {
            printf("allegedly finished one sided write..\n");
            break;

        } else {
            DEBUG_PRINT("completed a send packet\n");
        }
    }

    return 0;
}

static int send_binary(struct ibv_helper_context *helper_context, struct ibv_recv_wr *recv_wr) {
    struct ibv_wc wc;
    struct ibv_recv_wr *bad_wr;
    struct ibv_send_wr *send_bad_wr;
    int fd;
    int sent = 0;
    struct ibv_sge send_list = {
        .addr    = (uintptr_t) helper_context->send_mr->addr,
        .length = helper_context->send_mr->length,
        .lkey    = helper_context->send_mr->lkey,
    };
    struct ibv_send_wr send_wr = {
        .wr_id = SEND_OPID,
        .sg_list    = &send_list,
        .num_sge    = 1,
        .opcode     = IBV_WR_SEND,
    };

    memset((void *) send_wr.sg_list->addr, 0, send_wr.sg_list->length);

    /* First we parse the ELF file and find the loadable segments and BSS section.
       For each of these regions, we send a region_request to the exokernel, which
       tells the exokernel which memory regions to prepare.
     */
    Elf64_Ehdr header;
    Elf64_Shdr sh_header;
    Elf64_Phdr ph_header;
    uint32_t names_table_offset;
    char *str_table;
    int err;

    fd = open("helloworld", O_RDONLY);
    if (fd < 0) {
        printf("couldn't open binary\n");
        return fd;
    }

    // Read the ELF header
    ERR(read(fd, (void *)&header, sizeof(header)));

    // Now go to the program header sections
    ERR(lseek(fd, header.e_phoff, SEEK_SET));
    for (int i = 0; i < header.e_phnum; i++) {
        ERR(read(fd, (void *)&ph_header, sizeof(ph_header)));

        if (ph_header.p_type != PT_LOAD)
            continue;

        // Restore the memory buf for this op to the original scratch buffer.
        // This will be overwritten in load_segment with the mmapped buffer.
	    ERR(load_segment(fd, ph_header.p_vaddr, ph_header.p_filesz, ph_header.p_offset, helper_context, recv_wr));
    }
    //TODO: set up BSS section

    // Now we have to set up the stack
	ERR(setup_stack(header.e_entry, helper_context, recv_wr));
    
    // Now the exokernel is ready to run
    struct run_exokernel_request req = {
        .stack_ptr = STACK_BOTTOM + STACK_SIZE - 13 * 4,
        .entry_point = header.e_entry,
    };
    marshall_run_exokernel_request(&req, (void *)send_wr.sg_list->addr);

    if (ibv_post_send(helper_context->qp, &send_wr, &send_bad_wr))
        DEBUG_PRINT("oh god, post_send didn't work for run_exokernel..\n");
    else
        DEBUG_PRINT("ok, just posted a run_exokernel_request\n");

    ERR(poll_cq(helper_context->cq, &wc));

    if (wc.wr_id == SEND_OPID)
        DEBUG_PRINT("completed a send packet\n");
    else
        DEBUG_PRINT("completed unexpected op\n");

    close(fd);
}

static void create_helper_context(struct ibv_context *ctx, struct ibv_helper_context *helper_context) {
    struct ibv_qp_init_attr qp_init_attr;
    struct ibv_qp_init_attr query_init_attr;
    struct ibv_qp_attr query_qp_attr;
    void *rdma_buf;
    void *rdma_buf_read;
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

    if (helper_context->qp == NULL)
        printf("lol qp is null\n");

    fprintf(stderr, "qp_num: %d\n", helper_context->qp->qp_num);

    // Try to query the attributes of the qp?
    if (ibv_query_qp(helper_context->qp, &query_qp_attr, IBV_QP_QKEY | IBV_QP_STATE | IBV_QP_PATH_MTU | IBV_QP_PORT , &query_init_attr))
        fprintf(stderr, "lol, query_qp failed, errno: %d\n", errno);
    else
        DEBUG_PRINT("results of query: %d %d %d %d\n", query_qp_attr.qp_state, query_qp_attr.port_num, query_qp_attr.qp_access_flags, query_qp_attr.qkey);
    
    helper_context->send_mr = ibv_reg_mr(helper_context->pd, rdma_buf, BUF_SIZE, IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE);
    helper_context->recv_mr = ibv_reg_mr(helper_context->pd, rdma_buf_read, BUF_SIZE, IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE);

    if (helper_context->send_mr || helper_context->recv_mr == NULL) 
        printf("unf, ibv_reg_mr failed\n");
}

static void destroy_helper_context(struct ibv_helper_context *helper_context) {
    ibv_destroy_qp(helper_context->qp);
    ibv_destroy_cq(helper_context->cq);

    ibv_dereg_mr(helper_context->send_mr);
    ibv_dereg_mr(helper_context->recv_mr);
    ibv_dealloc_pd(helper_context->pd);
    ibv_close_device(helper_context->ctx);
}

static void init_qp(struct ibv_helper_context *helper_context, int remote_qp_num) {
    struct ibv_qp_init_attr query_init_attr;
    struct ibv_qp_attr query_qp_attr;
    struct ibv_qp_attr qp_attr;
    union ibv_gid my_gid;
    union ibv_gid remote_gid;

    /* Move QP from RESET to INIT state */
    memset(&qp_attr, 0, sizeof(qp_attr));

    qp_attr.qp_state = IBV_QPS_INIT;
    qp_attr.port_num = 1; // TODO: what's the port num??
    qp_attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;

    /* IMPORTANT: the flag masks that we pass to modify_qp depends on the qp_type
     * (IBV_QPT_UD vs IBV_QPT_UC, etc). For example, for IBV_QPT_UD, the flags MUST be
     * IBV_QP_STATE | IBV_QP_PORT | IBV_QP_QKEY | IBV_QP_PKEY_INDEX and for IBV_QPT_UC,
     * the flags MUST be IBV_QP_STATE | IBV_QP_PORT | IBV_QP_PKEY_INDEX | IBV_QP_ACCESS_FLAGS.
     *
     * For this test, we are using IBV_QPT_UC (unreliable connected) type.
     */
    if (ibv_modify_qp(helper_context->qp, &qp_attr, IBV_QP_STATE | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS | IBV_QP_PKEY_INDEX))
        fprintf(stderr, "unf, modify_qp to init failed, errno: %d\n", errno);

    /* Now we need to move the QP from INIT to RTR */
    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.qp_state = IBV_QPS_RTR;

    qp_attr.path_mtu = IBV_MTU_512; // This is the recommended value
    qp_attr.dest_qp_num = remote_qp_num; // This is the remote qp_num
    qp_attr.rq_psn = 0;

    qp_attr.ah_attr.dlid = 0; // this is likely 0 if the remote has only 1 port
    qp_attr.ah_attr.sl = 0;
    qp_attr.ah_attr.src_path_bits = 0;
    qp_attr.ah_attr.port_num = 1;

    // Inspect these GIDs...
    if (ibv_query_gid(helper_context->ctx, 1, 0, &my_gid)) {
        fprintf(stderr, "could not get gid for port %d, index %d\n", 1, 0);
    }
    DEBUG_PRINT("gid: %016llx\n", my_gid.global.interface_id);

    remote_gid.global.subnet_prefix = 0x80fe;
    // Set the interface_id to the hard-coded id that is NOT the gid of this machine
    if (my_gid.global.interface_id == GID_1)
        remote_gid.global.interface_id = GID_2;
    else
        remote_gid.global.interface_id = GID_1;

    qp_attr.ah_attr.is_global = 1; // Over ROCE means this has to be global
    qp_attr.ah_attr.grh.hop_limit = 1;
    qp_attr.ah_attr.grh.dgid = remote_gid;
    qp_attr.ah_attr.grh.sgid_index = 0;

    if (ibv_modify_qp(helper_context->qp, &qp_attr, IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
                IBV_QP_DEST_QPN | IBV_QP_RQ_PSN))
        fprintf(stderr, "unf, modify_qp to rtr failed, errno: %d\n", errno);

    //We also need to move qp to a RTS state....
    qp_attr.qp_state = IBV_QPS_RTS;
    qp_attr.sq_psn = helper_context->qp->qp_num;
    if (ibv_modify_qp(helper_context->qp, &qp_attr, IBV_QP_STATE | IBV_QP_SQ_PSN))
        fprintf(stderr, "unf, modify_qp to rts failed, errno: %d\n", errno);

    memset(&query_qp_attr, 0, sizeof(query_qp_attr));
    if (ibv_query_qp(helper_context->qp, &query_qp_attr, IBV_QP_QKEY | IBV_QP_STATE | IBV_QP_PATH_MTU | IBV_QP_PORT , &query_init_attr)) {
        fprintf(stderr, "lol, query_qp failed, errno: %d\n", errno);
    } else {
        fprintf(stderr, "state: %d\n", query_qp_attr.qp_state);
    }
}

int main(int argc, char *argv[])
{
    struct ibv_device **dev_list;
    struct ibv_device_attr attr;
    struct ibv_context *ctx;
    struct ibv_recv_wr *bad_wr;

    struct ibv_helper_context helper_context;
    
    int num_devices, i;
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
        printf("    %-16s\t%016llx\n",
                ibv_get_device_name(dev_list[i]),
                (unsigned long long) be64toh(ibv_get_device_guid(dev_list[i])));
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

    create_helper_context(ctx, &helper_context);
    /* AT THIS POINT, all resources are created. Now we need to connect the QPs */

    /* Now we need to move the QP through some state machine state, including connecting to remote QP */
    init_qp(&helper_context, remote_qp_num);

    // Now we need to post a receive request (RR)
    // We happen to use the same wr for all recvs
    struct ibv_sge list = {
        .addr	= (uintptr_t) helper_context.recv_mr->addr,
        .length = helper_context.recv_mr->length,
        .lkey	= helper_context.recv_mr->lkey,
    };
    struct ibv_recv_wr recv_wr = {
        .wr_id	    = RECV_OPID,
        .sg_list    = &list,
        .num_sge    = 1,
    };

    if (ibv_post_recv(helper_context.qp, &recv_wr, &bad_wr))
        fprintf(stderr, "lol, post_recv didn't work, errno: %d\n", errno);

    /**** Now we have finished setting up RDMA ****/

    if (send_binary(&helper_context, &recv_wr))
        DEBUG_PRINT("something failed with send_binary\n");

    destroy_helper_context(&helper_context);

    ibv_free_device_list(dev_list);

    return 0;
}
