/*
 * Copyright (c) 2004 Topspin Communications.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdio.h>
#include <endian.h>

#include <infiniband/verbs.h>

#define	CLIENT_ARG	"client"
#define	CLIENT_ARG_LEN	strlen(CLIENT_ARG)
#define GID_1   0x05a923feff4b6b52
#define GID_2   0x25a823feff4b6b52

#define SEND_OPID	0x123
#define RECV_OPID	0xdead

int main(int argc, char *argv[])
{
	struct ibv_device **dev_list;
	int num_devices, i;

	int client = 0;
	// didn't use getopt.h to avoid Linux dependencies
	// If there is an argument, "client", then it's the client. Otherwise is server
	if (argc > 1) {
		if (!memcmp(argv[1], CLIENT_ARG, CLIENT_ARG_LEN)) {
			client = 1;
			printf("You are running the pingpong client\n");
		} else
			printf("You are running the pingpong server\n");
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
    
    struct ibv_context *ctx = ibv_open_device(dev_list[1]);	
    struct ibv_device_attr attr;

    if (ctx == NULL)
        printf("ibv_open_device failed?errno: %d\n", errno);

    if (ibv_query_device(ctx, &attr))
        printf("query device failed? errno: %d\n", errno);
    printf("max qp: %d\n", attr.max_qp);
    printf("max cq: %d\n", attr.max_cq);
    printf("max cqe: %d\n", attr.max_cqe);
    printf("max sge: %d\n", attr.max_sge);
    printf("num ports: %d\n", attr.phys_port_cnt);

    for (i = 0; i < attr.phys_port_cnt; i++) {
        struct ibv_port_attr port_attr;
        ibv_query_port(ctx, 1, &port_attr);
        printf("lid of port: %d\n", port_attr.lid);
    }

    /* Allocate a PD, in preparation for memory allocation */
    struct ibv_pd *pd;
    pd = ibv_alloc_pd(ctx);

    int buf_size = 1024;
    char rdma_buf[buf_size];
    char rdma_buf_read[buf_size];

    struct ibv_mr *mr;
    struct ibv_mr *mr_read;
    mr = ibv_reg_mr(pd, rdma_buf, buf_size, IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE);
    mr_read = ibv_reg_mr(pd, rdma_buf_read, buf_size, IBV_ACCESS_REMOTE_READ);

    if (mr == NULL || mr_read == NULL) 
        printf("unf, ibv_reg_mr failed\n");

    struct ibv_cq *cq;
    int cq_size = 1;
    cq = ibv_create_cq(ctx, cq_size, NULL, NULL, 0);

    struct ibv_qp_init_attr qp_init_attr;
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

    struct ibv_qp *qp;
    qp = ibv_create_qp(pd, &qp_init_attr);

    if (qp == NULL)
        printf("lol qp is null\n");

    fprintf(stderr, "qp_num: %d\n", qp->qp_num);

    // Try to query the attributes of the qp?
    struct ibv_qp_init_attr query_init_attr;
    struct ibv_qp_attr query_qp_attr;
    if (ibv_query_qp(qp, &query_qp_attr, IBV_QP_QKEY | IBV_QP_STATE | IBV_QP_PATH_MTU | IBV_QP_PORT , &query_init_attr)) {
        fprintf(stderr, "lol, query_qp failed, errno: %d\n", errno);
    } else {
        fprintf(stderr, "results of query: %d %d %d %d\n", query_qp_attr.qp_state, query_qp_attr.port_num, query_qp_attr.qp_access_flags, query_qp_attr.qkey);
    }

    /* AT THIS POINT, all resources are created. Now we need to connect the QPs */
    /* Now we need to move the QP through some state machine state, including connecting to remote QP */

    /* Move QP from RESET to INIT state */
    struct ibv_qp_attr qp_attr;
    memset(&qp_attr, 0, sizeof(qp_attr));

    qp_attr.qp_state = IBV_QPS_INIT;
    qp_attr.port_num = 1; // TODO: what's the port num??
    qp_attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
    //qp_attr.pkey_index = 0;
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
    memset(rdma_buf, 7, buf_size);
    rdma_buf[0] = 'l';
    rdma_buf[1] = 'o';
    rdma_buf[2] = 'l';
    struct ibv_sge list = {
		.addr	= (uintptr_t) rdma_buf,
		.length = buf_size,
		.lkey	= mr->lkey,
	};
	struct ibv_recv_wr wr = {
		.wr_id	    = RECV_OPID,
		.sg_list    = &list,
		.num_sge    = 1,
	};
	struct ibv_recv_wr *bad_wr;

    if (ibv_post_recv(qp, &wr, &bad_wr))
        fprintf(stderr, "lol, post_recv didn't work, errno: %d\n", errno);

    // Inspect these GIDs...
    union ibv_gid my_gid;
    if (ibv_query_gid(ctx, 1, 0, &my_gid)) {
        fprintf(stderr, "could not get gid for port %d, index %d\n", 1, 0);
    }
    fprintf(stderr, "gid: %016lx\n", my_gid.global.interface_id);

    /* Now we need to move the QP from INIT to RTR */
    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.qp_state = IBV_QPS_RTR;

    qp_attr.path_mtu = IBV_MTU_512; // This is the recommended value
    qp_attr.dest_qp_num = 240; // This is the remote qp_num
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

    /* OK, now the QP should be set up, and we are ready to send packets */
    /* Only send if you're the server */
    if (!client) {
            struct ibv_sge send_list = {
                    .addr	= (uintptr_t) rdma_buf,
                    .length = buf_size,
                    .lkey	= mr->lkey
            };
            struct ibv_send_wr send_wr = {
                    .wr_id	= SEND_OPID,
                    .sg_list    = &send_list,
                    .num_sge    = 1,
                    .opcode     = IBV_WR_SEND,
                    //.send_flags = ctx.send_flags,
            };
            struct ibv_send_wr *send_bad_wr;

	if (ibv_post_send(qp, &send_wr, &send_bad_wr))
        fprintf(stderr,  "oh god, post_send didn't work..\n");
    }

    struct ibv_wc wc;
    int ne;

    while (1) {
    do {
            ne = ibv_poll_cq(cq, 1, &wc);
            if (ne < 0) {
                    fprintf(stderr, "poll CQ failed %d\n", ne);
                    return 1;
            }

    } while (ne < 1);

    fprintf(stderr, "LOL ne: %d?\n", ne);
    if (wc.status != IBV_WC_SUCCESS) {
            fprintf(stderr, "Failed status %s (%d) for wr_id %d\n",
                            ibv_wc_status_str(wc.status),
                            wc.status, (int) wc.wr_id);
            return 1;
    } else {
        fprintf(stderr, "LOL completed a send or receive  packet... wr_id: %x\n", (int) wc.wr_id);
	// If it's a recv, post a new recv!
	if (wc.wr_id == RECV_OPID) {
		if (ibv_post_recv(qp, &wr, &bad_wr))
			fprintf(stderr, "lol, post_recv didn't work, errno: %d\n", errno);
		fprintf(stderr, "content of rdma_buf after recv: %s\n", rdma_buf);
	}
    }
    }


    ibv_destroy_qp(qp);
    ibv_destroy_cq(cq);

    ibv_dereg_mr(mr);
    ibv_dereg_mr(mr_read);
    ibv_dealloc_pd(pd);
    ibv_close_device(ctx);
	ibv_free_device_list(dev_list);

	return 0;
}
