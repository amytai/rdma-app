#ifndef HELPER_H
#define HELPER_H

#include <infiniband/verbs.h>

#define DEBUG	1
#define DEBUG_PRINT(...)	do { \
				if (DEBUG) printf(__VA_ARGS__); \
				} while (0)
#define ERR(f)	do { \
		err = f; if (err < 0) {printf("some error\n"); return err;} \
		} while (0)

#define GID_1   0x25a823feff4b6b52
#define GID_2   0x2da823feff4b6b52 //25a823feff4b6b52
//#define GID_2   0xa9c24dfefff6ceba //0x2da823feff4b6b52 //0xa9c24dfefff6ceba //25a823feff4b6b52

#define SEND_OPID	0x123
#define RECV_OPID	0xdead
#define ONE_SIDED_WRITE_OPID    0x456

struct ibv_helper_context {
    struct ibv_context *ctx;
    struct ibv_qp *qp;
    struct ibv_cq *cq;
    struct ibv_pd *pd;
    struct ibv_mr *send_mr;
    struct ibv_mr *recv_mr;
};

enum rpc_type {
    rpc_region_request = 0,
    rpc_region_response = 1,
    rpc_run_exokernel_request = 2,
    rpc_run_exokernel_response = 3,
};

struct region_request {
 Elf64_Addr start;
 uint64_t size;
};

struct region_response {
  int success;
  uint64_t remote_addr;
  uint32_t rkey;
};

struct run_exokernel_request {
  uint64_t stack_ptr;
  uint64_t entry_point;
};

struct exokernel_rpc {
    enum rpc_type type;
    union payload {
        struct region_request rreq;
        struct region_response rresp;
        struct run_exokernel_request rereq;
    } payload;
};

void marshall_region_request(struct region_request *req, void *buf) {
    struct exokernel_rpc rpc = {
        .type = rpc_region_request,
        .payload.rreq = {
            .start = req->start,
            .size = req->size,
        },
    };
    memcpy(buf, (void *) &rpc, sizeof(rpc));
};

void marshall_run_exokernel_request(struct run_exokernel_request *req, void *buf) {
    struct exokernel_rpc rpc = {
        .type = rpc_run_exokernel_request,
        .payload.rereq = {
            .stack_ptr = req->stack_ptr,
            .entry_point = req->entry_point,
        },
    };
    memcpy(buf, (void *) &rpc, sizeof(rpc));
};

void marshall_region_response(struct region_response *resp, void *buf) {
    struct exokernel_rpc rpc = {
        .type = rpc_region_response,
        .payload.rresp = {
            .success = resp->success,
            .remote_addr = resp->remote_addr,
            .rkey = resp->rkey,
        },
    };
    memcpy(buf, (void *) &rpc, sizeof(rpc));
}

int parse_rpc_type(enum rpc_type type, void *buf) {
    struct exokernel_rpc *rpc = (struct exokernel_rpc *) buf;
    if (rpc->type != type) {
        DEBUG_PRINT("Expected rpc_type %d but got rpc_type %d\n", type, rpc->type);
        return -1;
    }
    // TODO: return paylod
    return 0;
}



#endif
