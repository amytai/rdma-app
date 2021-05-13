#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/auxv.h>
#include <errno.h>
#include <elf.h>

#define MAX_MMAP    8
#define ERR(f)	do { \
		err = f; if (err < 0) {printf("some error\n"); return err;} \
		} while (0)

size_t mmap_length[MAX_MMAP];
void *regions[MAX_MMAP];
int region_num = 0;

int load_segment(int fd, Elf64_Addr p_vaddr, Elf64_Addr p_filesz, Elf64_Off p_offset) {
	int err;
	size_t cur_offset;
	size_t offset = p_vaddr - (p_vaddr / 4096) * 4096;

	mmap_length[region_num] = (((p_filesz) / 4096) + 1) * 4096;

	printf("about to mmap length: %lx\n", mmap_length[region_num]);

	regions[region_num] = mmap((void *) p_vaddr - offset, mmap_length[region_num], PROT_EXEC | PROT_WRITE | PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	if ((uint64_t) regions[region_num] != (p_vaddr - offset))
		printf("yikes, mmap returned bad addr: %lx\n", (uint64_t) regions[region_num]);

	// Now we have to read that segment into the mmap'd region.
	cur_offset = lseek(fd, 0, SEEK_CUR);
	if (cur_offset < 0) {
		printf("hmmm lseek had problem: %d\n", errno);
		return -1;
	}
	ERR(lseek(fd, p_offset, SEEK_SET));
	ERR(read(fd, (void *)regions[region_num] + offset, p_filesz));

	// Now return lseek to the program header table
	ERR(lseek(fd, cur_offset, SEEK_SET));

	region_num++;
	return 0;
}

void setup_stack_and_jump(Elf64_Addr e_entry) {
    void *stack = mmap((void *)(0x700000000000), 1024*1024*1024, PROT_WRITE | PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, - 1, 0);

    stack += (1024 * 1024 * 1024);

    __asm__ ( "mov %0, %%r8;" // Save header.e_entry before we change the stack
              "mov %1, %%r10;" // Save "stack" var
            :
            : "m" (e_entry), "r" (stack - 32)
            );

   /* __asm__ (
              "lea 0x28(%rip),%rdx;"
              "push %rdx;"
              "push %rbp;"
              "lea (%rsp),%rbp;"
              "lea (%rsp),%rax;"
            );
*/
    __asm__ (
              "mov $0xdeadbeefabcd1234,%rdx;" //dl_random, 16 bytes of random
              "push %rdx;"
              "push %rdx;"
            );
    __asm__ (
              "push $0;" //aux
              "push $0;" //aux
              "push %r8;" //aux
              "push $9;" //aux
              "push %r10;" //aux
              "push $25;" //aux AT_RANDOM
              "push $0;" //NULL envp
              "push $0;" //NULL argv
              "push $0;" //0 argc
              "mov $0, %rdx;"
            );
    

    // Now jmp to entry point, header.e_entry
    __asm__ ( 
              "jmp *%r8;"
            );
}


// Load elf binary from file into new address region of current process, then run it
int main(int argc, char *argv[])
{
    Elf64_Ehdr header;
    Elf64_Shdr sh_header;
    Elf64_Phdr ph_header;
    uint32_t names_table_offset;
    char *str_table;
    int err, fd;

    memset(regions, 0,sizeof(regions));

    fd = open("helloworld", O_RDONLY);
    if (fd < 0) {
	printf("couldn't open binary\n");
	return fd;
    }

    // Read the ELF header
    ERR(read(fd, (void *)&header, sizeof(header)));

    /*printf("printing elf header:%*s\n", EI_NIDENT, header.e_ident);
    printf("elf type: %hu\n", header.e_type);
    printf("elf machine: %hu\n", header.e_machine);
    printf("elf version: %u\n", header.e_version);
    printf("elf entry: %lx\n", header.e_entry);
    printf("elf phoff: %ld\n", header.e_phoff);
    printf("elf shoff: %ld\n", header.e_shoff);
    printf("elf ehsize: %hu\n", header.e_ehsize);
    printf("elf phentsize: %hu\n", header.e_phentsize);
    printf("elf phnum: %hu\n", header.e_phnum);
    printf("elf shentsize: %hu\n", header.e_shentsize);
    printf("elf shnum: %hu\n", header.e_shnum);*/

    // Now go to the program header sections
    ERR(lseek(fd, header.e_phoff, SEEK_SET));
    for (int i = 0; i < header.e_phnum; i++) {
        ERR(read(fd, (void *)&ph_header, sizeof(ph_header)));

        if (ph_header.p_type != PT_LOAD)
            continue;

        printf("Loadable PROGRAM HEADER #%d:\n", i);
        printf("program header type: %u\n", ph_header.p_type);
        printf("program header offset: %lu\n", ph_header.p_offset);
        printf("program header vaddr: %lx\n", ph_header.p_vaddr);
        printf("program header paddr: %lx\n", ph_header.p_paddr);
        printf("program header filesz: %lx\n", ph_header.p_filesz);
        printf("program header memsz: %lx\n", ph_header.p_memsz);
        printf("program header align: %lx\n", ph_header.p_align);

	ERR(load_segment(fd, ph_header.p_vaddr, ph_header.p_filesz, ph_header.p_offset));
    }


    // Now go to the section string table
    names_table_offset = header.e_shoff + sizeof(Elf64_Shdr) * header.e_shstrndx;
    ERR(lseek(fd, names_table_offset, SEEK_SET));
    // Read the str table section header
    ERR(read(fd, (void *)&sh_header, sizeof(sh_header)));

    // Now read the str table section contents into a buffer
    str_table = (char *) malloc(sh_header.sh_size);

    ERR(lseek(fd, sh_header.sh_offset, SEEK_SET));
    ERR(read(fd, (void *) str_table, sh_header.sh_size));

    ERR(lseek(fd, header.e_shoff, SEEK_SET));
    for (int i = 0; i < header.e_shnum; i++) {
        ERR(read(fd, (void *)&sh_header, sizeof(sh_header)));
        if (!strncmp(str_table + sh_header.sh_name, ".bss", 4)) {
	    size_t offset;
            printf("FOUND BSS SECTION!\n"); 

            offset = sh_header.sh_addr - (sh_header.sh_addr / 4096) * 4096;
            mmap_length[region_num] = ((sh_header.sh_size / 4096) + 1) * 4096;

            regions[region_num] = mmap((void *) ((sh_header.sh_addr / 4096 + 1) * 4096), mmap_length[region_num], PROT_WRITE | PROT_READ, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if ((uint64_t) regions[region_num] == (uint64_t) -1) {
                printf("mmap bss failed?? errno: %d\n", errno);
            }

            printf("bss section start: %lx, length: %lx\n", regions[region_num], mmap_length[region_num]);

            memset((void *) sh_header.sh_addr, 0, mmap_length[region_num]);

            region_num++;

            printf("section header name: %s\n", str_table + sh_header.sh_name);
            printf("section header type: %u\n", sh_header.sh_type);
            printf("section addr:  %lx\n", sh_header.sh_addr);
            printf("section offset: %lu\n", sh_header.sh_offset);
            printf("section size: %lu\n", sh_header.sh_size);
        } else
            continue;

    }

    free(str_table);
    
    // OK now we have loaded all the right segments. Need to set up the stack
    setup_stack_and_jump(header.e_entry);
    
    printf("we shouldn't get here\n");

    for (int i = 0; i < MAX_MMAP; i++) {
        if (regions[i] != NULL)
            munmap(regions[i], mmap_length[i]);
    }
    return 0;
}
