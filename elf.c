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

int main(int argc, char *argv[])
{
    Elf64_Ehdr header;
    Elf64_Shdr sh_header;
    Elf64_Phdr ph_header;
    int err;
    size_t mmap_length[MAX_MMAP];;
    void *regions[MAX_MMAP];
    memset(regions, 0,sizeof(regions));
    int region_num = 0;

	// Load elf binary from file into memory and try to run it
    int fd = open("helloworld", O_RDONLY);


    // Read the ELF header
    err = read(fd, (void *)&header, sizeof(header));
    if (err < 0) {
        printf("hmmm reading bin had a problem: %d\n", errno);
        return -1;
    }
    printf("printing elf header:%*s\n", EI_NIDENT, header.e_ident);
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
    printf("elf shnum: %hu\n", header.e_shnum);

    // Now go to the program header sections
    err = lseek(fd, header.e_phoff, SEEK_SET);
    if (err < 0) {
        printf("hmmm lseek had problem: %d\n", errno);
        return -1;
    }
    for (int i = 0; i < header.e_phnum; i++) {
        err = read(fd, (void *)&ph_header, sizeof(ph_header));
        if (err < 0) {
            printf("hmmm reading header had problem: %d\n", errno);
            return -1;
        }
        if (ph_header.p_type != PT_LOAD)
            continue;

        printf("PROGRAM HEADER #%d:\n", i);
        printf("program header type: %u\n", ph_header.p_type);
        printf("program header offset: %lu\n", ph_header.p_offset);
        printf("program header vaddr: %lx\n", ph_header.p_vaddr);
        printf("program header paddr: %lx\n", ph_header.p_paddr);
        printf("program header filesz: %lx\n", ph_header.p_filesz);
        printf("program header memsz: %lx\n", ph_header.p_memsz);
        printf("program header align: %lx\n", ph_header.p_align);

        size_t offset = ph_header.p_vaddr - (ph_header.p_vaddr / 4096) * 4096;
        mmap_length[region_num] = (((ph_header.p_filesz) / 4096) + 1) * 4096;
        printf("about to mmap length: %lx\n", mmap_length[region_num]);
        regions[region_num] = mmap((void *) ph_header.p_vaddr - offset, mmap_length[region_num], PROT_EXEC | PROT_WRITE | PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (regions[region_num] != (ph_header.p_vaddr - offset))
            printf("yikes, mmap returned bad addr: %lx\n", (uint64_t) regions[region_num]);


        // Now we have to read that segment into the mmap'd region.
        size_t cur_offset = lseek(fd, 0, SEEK_CUR);
        lseek(fd, ph_header.p_offset, SEEK_SET);
        err = read(fd, (void *)regions[region_num] + offset, ph_header.p_filesz);
        if (err < 0) {
            printf("hmmm read had problem: %d\n", errno);
            return -1;
        }

        // Now return lseek to the program header table
        lseek(fd, cur_offset, SEEK_SET);

        region_num++;
    }


    // Now go to the section string table
    uint32_t names_table_offset = header.e_shoff + sizeof(Elf64_Shdr) * header.e_shstrndx;
    err = lseek(fd, names_table_offset, SEEK_SET);
    if (err < 0) {
        printf("hmmm lseek had problem: %d\n", errno);
        return -1;
    }
    // Read the str table section header
    err = read(fd, (void *)&sh_header, sizeof(sh_header));
    if (err < 0) {
        printf("hmmm reading header had problem: %d\n", errno);
        return -1;
    }
    // Now read the str table section contents into a buffer
    char *str_table = (char *) malloc(sh_header.sh_size);

    err = lseek(fd, sh_header.sh_offset, SEEK_SET);
    if (err < 0) {
        printf("hmmm lseek had problem: %d\n", errno);
        return -1;
    }
    err = read(fd, (void *) str_table, sh_header.sh_size);
    if (err < 0) {
        printf("hmmm reading had problem: %d\n", errno);
        return -1;
    }

    err = lseek(fd, header.e_shoff, SEEK_SET);
    if (err < 0) {
        printf("hmmm lseek had problem: %d\n", errno);
        return -1;
    }
    for (int i = 0; i < header.e_shnum; i++) {
        printf("READING SECTION %d\n", i);
        err = read(fd, (void *)&sh_header, sizeof(sh_header));
        if (err < 0) {
            printf("hmmm reading header had problem: %d\n", errno);
            return -1;
        }
        if (!strncmp(str_table + sh_header.sh_name, ".bss", 4)) {
            printf("FOUND BSS SECTION!\n"); 
            size_t offset = sh_header.sh_addr - (sh_header.sh_addr / 4096) * 4096;
            mmap_length[region_num] = ((sh_header.sh_size / 4096) + 1) * 4096;

            regions[region_num] = mmap((void *) ((sh_header.sh_addr / 4096 + 1) * 4096), mmap_length[region_num], PROT_WRITE | PROT_READ, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if ((uint64_t) regions[region_num] == (uint64_t) -1) {
                printf("mmap bss failed?? errno: %d\n", errno);
            }

            printf("bss section start: %lx, length: %lx\n", regions[region_num], mmap_length[region_num]);

            memset(sh_header.sh_addr, 0, mmap_length[region_num]);

            region_num++;

            printf("section header name: %s\n", str_table + sh_header.sh_name);
            printf("section header type: %u\n", sh_header.sh_type);
            printf("section addr:  %lx\n", sh_header.sh_addr);
            printf("section offset: %lu\n", sh_header.sh_offset);
            printf("section size: %lu\n", sh_header.sh_size);
        } else
            continue;

    }
    uint64_t rand_addr = getauxval(AT_RANDOM);
    printf("normal AT_RANDOM: %lx\n", rand_addr);
    uint64_t rand_val;
    __asm__ ( "mov %1, %%rax;"
              "mov (%%rax),%%rax;"
              "mov %%rax, %0"
            : "=m" (rand_val)
            : "m" (rand_addr)
            );

    printf("rand val: %lx\n", rand_val);

    // OK now we have loaded all the right segments. Need to set up the stack

    void *stack = mmap((void *)(0x700000000000), 1024*1024*1024, PROT_WRITE | PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, - 1, 0);

    stack += (1024 * 1024 * 1024);

    printf("stack: %lx\n", (uint64_t) stack);
    
    uint64_t esp;
    uint64_t ebp;
    uint64_t eip;
    __asm__ ( 
              "mov %%r8, %0;"
              "mov %%rbp,%1;"
            : "=m" (esp), "=m" (ebp)
            );
    
    printf("mylabel? %lx\n", esp);
    printf("ebp? %lx\n", ebp);
    uint64_t at_ran = AT_RANDOM;
    uint64_t at_entry = AT_ENTRY;
    uint64_t at_pagesz = AT_PAGESZ;
    printf("AT_random: %d\n", at_ran);
    
    __asm__ ( "mov %0, %%r8;" // Save header.e_entry before we change the stack
              "mov %1, %%r9;" // Save "stack" var
              "mov %2, %%r10;" // Save "stack" var
            :
            : "m" (header.e_entry), "m" (stack), "r" (stack - 32)
            );
    __asm__ (
              "mov %r9,%rsp;"
            );

    __asm__ (
              "lea 0x28(%rip),%rdx;"
              "push %rdx;"
              "push %rbp;"
              "lea (%rsp),%rbp;"
              "lea (%rsp),%rax;"
            );

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
              "push $0;" //envp
              "push $0;" //argv
              "push $0;" //argc
              "mov $0, %rdx;"
            );
    
   /* __asm__ (
              //"push %rbp;"
              //"lea -0x8(%rsp),%rbp;"
            );
*/
    // esp and ebp should be the same before we jmp to entry point

    // Now jmp to entry point, header.e_entry
    __asm__ ( 
              "jmp %r8;"
            );

    
   /* __asm__ ( "movl $10, %eax;"
            "movl $20, %ebx;"
            "addl %ebx, %eax;"
            );*/
    printf("hi hello???\n");

    for (int i = 0; i < MAX_MMAP; i++) {
        if (regions[i] != NULL)
            munmap(regions[i], mmap_length[i]);
    }
    free(str_table);
	return 0;
}
