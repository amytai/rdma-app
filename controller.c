#include <stdio.h>
#include <stdlib.h>
#include <endian.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <elf.h>

#include "helper.h"

int load_segment(int fd, Elf64_Addr p_vaddr, Elf64_Addr p_filesz, Elf64_Off p_offset, 
        struct ibv_send_wr *send_wr, struct ibv_qp *qp, struct ibv_cq *cq, struct ibv_recv_wr *wr) {
    struct ibv_wc wc;
    struct ibv_recv_wr *bad_wr;
    struct ibv_send_wr *send_bad_wr;
    size_t mmap_length;
    void *region;
	int err, ne;
	size_t cur_offset;
	size_t offset = p_vaddr - (p_vaddr / 4096) * 4096;

	mmap_length = (((p_filesz) / 4096) + 1) * 4096;

    // Now we make a region request:
    struct region_request req = {
        .start = p_vaddr - offset,
        .size = mmap_length,
    };
    
    // Now copy this region request into the send buffer
    marshall_region_request(&req, (void *)send_wr->sg_list->addr);

    if (ibv_post_send(qp, send_wr, &send_bad_wr))
        DEBUG_PRINT("oh god, post_send didn't work..\n");

    int done = 0;
    while (!done) {
        do {
            ne = ibv_poll_cq(cq, 1, &wc);
            if (ne < 0) {
                DEBUG_PRINT("poll CQ failed %d\n", ne);
                return 1;
            }

        } while (ne < 1);

        DEBUG_PRINT("num entries polled: %d\n", ne);
        if (wc.status != IBV_WC_SUCCESS) {
            fprintf(stderr, "Failed status %s (%d) for wr_id %d\n",
                    ibv_wc_status_str(wc.status),
                    wc.status, (int) wc.wr_id);
            return 1;
        } else {
          // If it's a recv, post a new recv!
          if (wc.wr_id == RECV_OPID) {
            DEBUG_PRINT("completed a receive packet\n");

            if (ibv_post_recv(qp, wr, &bad_wr))
              fprintf(stderr, "lol, post_recv didn't work, errno: %d\n", errno);

            struct exokernel_rpc *rpc = (struct exokernel_rpc *) wr->sg_list->addr;
            if (rpc->type == rpc_region_response) {
                if (rpc->payload.rresp.success) {
                // OK now that we received the response for this RPC, we can continue
                    DEBUG_PRINT("received successful region_response\n");
                    done = 1;
                }
            }
          } else
            DEBUG_PRINT("completed a send packet\n");
        }
    }

    // Now we have to RDMA the segment into the mmap'd region.

	/*cur_offset = lseek(fd, 0, SEEK_CUR);
	if (cur_offset < 0) {
		printf("hmmm lseek had problem: %d\n", errno);
		return -1;
	}
	ERR(lseek(fd, p_offset, SEEK_SET));
	ERR(read(fd, (void *)region + offset, p_filesz));

	// Now return lseek to the program header table
	ERR(lseek(fd, cur_offset, SEEK_SET));*/

	return 0;
}

int send_binary(struct ibv_qp *qp, struct ibv_cq *cq,
        struct ibv_send_wr *send_wr, struct ibv_recv_wr *wr) {
    struct ibv_wc wc;
    struct ibv_recv_wr *bad_wr;
    struct ibv_send_wr *send_bad_wr;
    int ne;
    int fd;
    int sent = 0;

    memset((void *) send_wr->sg_list->addr, 0, send_wr->sg_list->length);

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

	    ERR(load_segment(fd, ph_header.p_vaddr, ph_header.p_filesz, ph_header.p_offset, send_wr, qp, cq, wr));
    }


    while (1) {
        do {
            ne = ibv_poll_cq(cq, 1, &wc);
            if (ne < 0) {
                DEBUG_PRINT("poll CQ failed %d\n", ne);
                return 1;
            }

        } while (ne < 1);

        DEBUG_PRINT("num entries polled: %d\n", ne);
        if (wc.status != IBV_WC_SUCCESS) {
            fprintf(stderr, "Failed status %s (%d) for wr_id %d\n",
                    ibv_wc_status_str(wc.status),
                    wc.status, (int) wc.wr_id);
            return 1;
        } else {
          // If it's a recv, post a new recv!
          if (wc.wr_id == RECV_OPID) {
            DEBUG_PRINT("completed a receive packet\n");

            

            /*if (ibv_post_recv(qp, wr, &bad_wr))
              fprintf(stderr, "lol, post_recv didn't work, errno: %d\n", errno);
            fprintf(stderr, "content of rdma_buf after recv: %s\n", (char *) wr->sg_list->addr);

            // This means we can send more stuff
            sent++;
            if (sent > 1000)
              continue;

            int res = read(fd, rdma_buf, buf_size);
            if (res < 0)
              fprintf(stderr, "lol fd read didn't work\n");
            if (res == 0) {
              fprintf(stderr, "nothing more in binary\n");
              continue;
            }
            if (res < buf_size) {
              rdma_buf[res+1]='\0';
            }
            if (ibv_post_send(qp, send_wr, &send_bad_wr))
              DEBUG_PRINT("oh god, post_send didn't work..\n");
            else
              DEBUG_PRINT("ok, just sent another buf\n");*/
          } else
            DEBUG_PRINT("completed a send packet\n");
        }
    }

    close(fd);

}

int main(int argc, char *argv[])
{
    struct ibv_device **dev_list;
    struct ibv_context *ctx;
    struct ibv_device_attr attr;
    struct ibv_pd *pd;
    struct ibv_mr *mr;
    struct ibv_mr *mr_read;
    struct ibv_cq *cq;
    struct ibv_qp_init_attr query_init_attr;
    struct ibv_qp_attr query_qp_attr;
    struct ibv_qp_init_attr qp_init_attr;
    struct ibv_qp *qp;
    struct ibv_qp_attr qp_attr;
    struct ibv_recv_wr *bad_wr;

    int buf_size = 1024;
    char rdma_buf[buf_size];
    char rdma_buf_read[buf_size];
    
    int num_devices, i;


    int cq_size = 1;
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

    /* Allocate a PD, in preparation for memory allocation */
    pd = ibv_alloc_pd(ctx);

    mr = ibv_reg_mr(pd, rdma_buf, buf_size, IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE);
    mr_read = ibv_reg_mr(pd, rdma_buf_read, buf_size, IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE);

    if (mr == NULL || mr_read == NULL) 
        printf("unf, ibv_reg_mr failed\n");

    cq = ibv_create_cq(ctx, cq_size, NULL, NULL, 0);

    memset(&qp_init_attr, 0, sizeof(qp_init_attr));

    qp_init_attr.qp_context = ctx;
    qp_init_attr.qp_type = IBV_QPT_UC;
    qp_init_attr.sq_sig_all = 1;
    qp_init_attr.send_cq = cq;
    qp_init_attr.recv_cq = cq;
    qp_init_attr.cap.max_send_wr = 1;
    qp_init_attr.cap.max_recv_wr = 1;
    qp_init_attr.cap.max_send_sge = 1;
    qp_init_attr.cap.max_recv_sge = 1;

    qp = ibv_create_qp(pd, &qp_init_attr);

    if (qp == NULL)
        printf("lol qp is null\n");

    fprintf(stderr, "qp_num: %d\n", qp->qp_num);

    // Try to query the attributes of the qp?
    if (ibv_query_qp(qp, &query_qp_attr, IBV_QP_QKEY | IBV_QP_STATE | IBV_QP_PATH_MTU | IBV_QP_PORT , &query_init_attr))
        fprintf(stderr, "lol, query_qp failed, errno: %d\n", errno);
    else
        DEBUG_PRINT("results of query: %d %d %d %d\n", query_qp_attr.qp_state, query_qp_attr.port_num, query_qp_attr.qp_access_flags, query_qp_attr.qkey);

    /* AT THIS POINT, all resources are created. Now we need to connect the QPs */
    /* Now we need to move the QP through some state machine state, including connecting to remote QP */

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
    if (ibv_modify_qp(qp, &qp_attr, IBV_QP_STATE | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS | IBV_QP_PKEY_INDEX))
        fprintf(stderr, "unf, modify_qp to init failed, errno: %d\n", errno);

    // Now we need to post a receive request (RR)?
    struct ibv_sge list = {
        .addr	= (uintptr_t) rdma_buf_read,
        .length = buf_size,
        .lkey	= mr_read->lkey,
    };
    struct ibv_recv_wr wr = {
        .wr_id	    = RECV_OPID,
        .sg_list    = &list,
        .num_sge    = 1,
    };

    if (ibv_post_recv(qp, &wr, &bad_wr))
        fprintf(stderr, "lol, post_recv didn't work, errno: %d\n", errno);

    // Inspect these GIDs...
    union ibv_gid my_gid;
    if (ibv_query_gid(ctx, 1, 0, &my_gid)) {
        fprintf(stderr, "could not get gid for port %d, index %d\n", 1, 0);
    }
    fprintf(stderr, "gid: %016llx\n", my_gid.global.interface_id);

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

    union ibv_gid remote_gid;
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

    if (ibv_modify_qp(qp, &qp_attr, IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
                IBV_QP_DEST_QPN | IBV_QP_RQ_PSN))
        fprintf(stderr, "unf, modify_qp to rtr failed, errno: %d\n", errno);

    //TODO: we also need to move qp to a RTS state....
    qp_attr.qp_state = IBV_QPS_RTS;
    qp_attr.sq_psn = qp->qp_num;
    if (ibv_modify_qp(qp, &qp_attr, IBV_QP_STATE | IBV_QP_SQ_PSN))
        fprintf(stderr, "unf, modify_qp to rts failed, errno: %d\n", errno);

    memset(&query_qp_attr, 0, sizeof(query_qp_attr));
    if (ibv_query_qp(qp, &query_qp_attr, IBV_QP_QKEY | IBV_QP_STATE | IBV_QP_PATH_MTU | IBV_QP_PORT , &query_init_attr)) {
        fprintf(stderr, "lol, query_qp failed, errno: %d\n", errno);
    } else {
        fprintf(stderr, "state: %d\n", query_qp_attr.qp_state);
    }

    /**** Now we have finished setting up RDMA ****/

    struct ibv_sge send_list = {
        .addr    = (uintptr_t) rdma_buf,
        .length = buf_size,
        .lkey    = mr->lkey
    };
    struct ibv_send_wr send_wr = {
        .wr_id = SEND_OPID,
        .sg_list    = &send_list,
        .num_sge    = 1,
        .opcode     = IBV_WR_SEND,
        //.send_flags = ctx.send_flags,
    };

    if (send_binary(qp, cq, &send_wr, &wr))
        DEBUG_PRINT("something failed with send_binary\n");


    ibv_destroy_qp(qp);
    ibv_destroy_cq(cq);

    ibv_dereg_mr(mr);
    ibv_dereg_mr(mr_read);
    ibv_dealloc_pd(pd);
    ibv_close_device(ctx);
    ibv_free_device_list(dev_list);

    return 0;
}
