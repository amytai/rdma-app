#ifndef HELPER_H
#define HELPER_H

#include <infiniband/verbs.h>

#define DEBUG	0
#define DEBUG_PRINT(...)	do { \
				if (DEBUG) printf(__VA_ARGS__); \
				} while (0)

#define GID_1   0x25a823feff4b6b52
#define GID_2   0x2da823feff4b6b52 //25a823feff4b6b52
//#define GID_2   0xa9c24dfefff6ceba //0x2da823feff4b6b52 //0xa9c24dfefff6ceba //25a823feff4b6b52

#define SEND_OPID	0x123
#define RECV_OPID	0xdead

struct region_request {
 Elf64_Addr start;
 uint64_t size;
};

struct region_response {
  int success;
};

struct run_exokernel_request {
  uint64_t stack_ptr;
  uint64_t entry_point;
};



#endif
