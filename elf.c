#include <stdint.h>
#include <fcntl.h>
#include <sys/mman.h>

int main(int argc, char *argv[])
{
	// Load elf binary from file into memory and try to run it
    size_t mmap_length = 4096 * 2;
    int fd = open("received_bin", O_RDONLY);


    char *region = mmap(0, mmap_length, PROT_EXEC | PROT_WRITE | PROT_READ, MAP_PRIVATE, fd, 0);

    
    __asm__ ( "movl $10, %eax;"
            "movl $20, %ebx;"
            "addl %ebx, %eax;"
            );


    munmap(region, mmap_length);
	return 0;
}
