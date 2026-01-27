#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "elf.h"

static int loadseg(pde_t *, uint64, struct inode *, uint, uint);

// map ELF permissions to PTE permission bits.
int flags2perm(int flags)
{
    int perm = 0;
    if(flags & 0x1)
      perm = PTE_X;
    if(flags & 0x2)
      perm |= PTE_W;
    return perm;
}

//
// the implementation of the exec() system call
//
int
kexec(char *path, char **argv)
{
  char *s, *last;
  int i, off;
  uint64 argc, sz = 0, sp, sp2, sp3, ustack[MAXARG], stackbase, stackbase2, stackbase3;
  struct elfhdr elf;
  struct inode *ip;
  struct proghdr ph;
  pagetable_t pagetable = 0, oldpagetable;
  struct proc *p = myproc();

  begin_op();

  // Open the executable file.
  if((ip = namei(path)) == 0){
    end_op();
    return -1;
  }
  ilock(ip);

  // Read the ELF header.
  if(readi(ip, 0, (uint64)&elf, 0, sizeof(elf)) != sizeof(elf))
    goto bad;

  // Is this really an ELF file?
  if(elf.magic != ELF_MAGIC)
    goto bad;

  if((pagetable = proc_pagetable(p)) == 0)
    goto bad;

  // Load program into memory.
  for(i=0, off=elf.phoff; i<elf.phnum; i++, off+=sizeof(ph)){
    if(readi(ip, 0, (uint64)&ph, off, sizeof(ph)) != sizeof(ph))
      goto bad;
    if(ph.type != ELF_PROG_LOAD)
      continue;
    if(ph.memsz < ph.filesz)
      goto bad;
    if(ph.vaddr + ph.memsz < ph.vaddr)
      goto bad;
    if(ph.vaddr % PGSIZE != 0)
      goto bad;
    uint64 sz1;
    if((sz1 = uvmalloc(pagetable, sz, ph.vaddr + ph.memsz, flags2perm(ph.flags))) == 0)
      goto bad;
    sz = sz1;
    if(loadseg(pagetable, ph.vaddr, ip, ph.off, ph.filesz) < 0)
      goto bad;
  }
  iunlockput(ip);
  end_op();
  ip = 0;

  p = myproc();
  uint64 oldsz = p->sz;

  // Allocate some pages at the next page boundary.
  // Make the first inaccessible as a stack guard.
  // Use the rest as the user stack.
  sz = PGROUNDUP(sz);
  uint64 sz11;
  if((sz11 = uvmalloc(pagetable, sz, sz + (USERSTACK+1)*PGSIZE, PTE_W)) == 0)
    goto bad;
  sz = sz11;
  uvmclear(pagetable, sz-(USERSTACK+1)*PGSIZE);
  sp = sz;
  stackbase = sp - USERSTACK*PGSIZE;

  //SECOND STACK for optional thread library
  // Allocate some pages at the next page boundary.
  // Make the first inaccessible as a stack guard.
  // Use the rest as the user stack.
  sz = PGROUNDUP(sz);
  uint64 sz2;
  if((sz2 = uvmalloc(pagetable, sz, sz + (USERSTACK+1)*PGSIZE, PTE_W)) == 0)
    goto bad;
  sz = sz2;
  uvmclear(pagetable, sz-(USERSTACK+1)*PGSIZE);
  sp2 = sz;
  stackbase2 = sp2 - USERSTACK*PGSIZE;


  //THIRD STACK for optional thread library
  // Allocate some pages at the next page boundary.
  // Make the first inaccessible as a stack guard.
  // Use the rest as the user stack.
  sz = PGROUNDUP(sz);
  uint64 sz3;
  if((sz3 = uvmalloc(pagetable, sz, sz + (USERSTACK+1)*PGSIZE, PTE_W)) == 0)
    goto bad;
  sz = sz3;
  uvmclear(pagetable, sz-(USERSTACK+1)*PGSIZE);
  sp3 = sz; 
  stackbase3 = sp3 - USERSTACK*PGSIZE;

  // Copy argument strings into new stack, remember their
  // addresses in ustack[].
  for(argc = 0; argv[argc]; argc++) {
    if(argc >= MAXARG)
      goto bad;
    sp -= strlen(argv[argc]) + 1;
    sp -= sp % 16; // riscv sp must be 16-byte aligned
    if(sp < stackbase)
      goto bad;
    if(copyout(pagetable, sp, argv[argc], strlen(argv[argc]) + 1) < 0)
      goto bad;
    ustack[argc] = sp;
  }
  ustack[argc] = 0;

  // push a copy of ustack[], the array of argv[] pointers.
  sp -= (argc+1) * sizeof(uint64);
  sp -= sp % 16;
  if(sp < stackbase)
    goto bad;
  if(copyout(pagetable, sp, (char *)ustack, (argc+1)*sizeof(uint64)) < 0)
    goto bad;

  // a0 and a1 contain arguments to user main(argc, argv)
  // argc is returned via the system call return
  // value, which goes in a0.
  p->trapframe->a1 = sp;

  // Save program name for debugging.
  for(last=s=path; *s; s++)
    if(*s == '/')
      last = s+1;
  safestrcpy(p->name, last, sizeof(p->name));
    
  // Commit to the user image.
  oldpagetable = p->pagetable;
  p->pagetable = pagetable;
  p->sz = sz;
  p->trapframe->epc = elf.entry;  // initial program counter = ulib.c:start()
  p->trapframe->sp = sp; // initial stack pointer
  
  // CRITICAL FIX: Initialize main thread (thread 0) trapframe with same values
  // The scheduler uses p->threads[0]->trapframe, not p->trapframe
  if(p->threads[0] && p->threads[0]->trapframe) {
    p->threads[0]->trapframe->epc = elf.entry;  // Same as p->trapframe->epc
    p->threads[0]->trapframe->sp = sp;          // Same as p->trapframe->sp
    // printf("DEBUG exec: Initialized thread 0 trapframe: epc=0x%lx sp=0x%lx\n", 
    //        p->threads[0]->trapframe->epc, p->threads[0]->trapframe->sp);
  }

  // Store stack bases for each thread (bottom boundaries)
  p->threads[0]->stack_base = stackbase;
  p->threads[1]->stack_base = stackbase2; 
  p->threads[2]->stack_base = stackbase3;
  // printf("DEBUG exec: stackbase=0x%lx stackbase2=0x%lx stackbase3=0x%lx\n", 
  //        stackbase, stackbase2, stackbase3);
  // printf("DEBUG exec: sp=0x%lx sp2=0x%lx sp3=0x%lx\n", sp, sp2, sp3);
  
  // printf("DEBUG exec: stackbase=0x%lx stackbase2=0x%lx stackbase3=0x%lx\n", 
  //        stackbase, stackbase2, stackbase3);

  // Check an address in the middle of each stack

  // stackbase  16384 the limit on the stack growth
  // sz1 should be 20480
  // sp  should be 20448

  // stackbase2 24576 the limit on the stack growth
  // sz2 and sp2 should be 28672

  // stackbase3 32768 the limit on the stack growth
  // sz3 and sp3 should be 36864

  int all_writable = 1;
  for (int j = 0; j < 3; j++) {
      uint64 stackbase_check, sp_check;
      if (j == 0) { stackbase_check = stackbase; sp_check = sp; }
      else if (j == 1) { stackbase_check = stackbase2; sp_check = sp2; }
      else { stackbase_check = stackbase3; sp_check = sp3; }
      
      uint64 test_va = stackbase_check + PGSIZE/2; // One page above base
      pte_t *pte = walk(p->pagetable, test_va, 0);
      int is_valid = (pte && (*pte & PTE_V));
      int is_user = (pte && (*pte & PTE_U));
      int is_writable = (pte && (*pte & PTE_W));
      
      // printf("Stack %d: va=0x%lx pte=%p flags=0x%lx (V=%d U=%d W=%d)\n", 
      //       j, test_va, pte, pte ? *pte : 0, is_valid, is_user, is_writable);
      
      if (!pte || (*pte & PTE_V) == 0 || (*pte & PTE_U) == 0) {
        all_writable = 0;
        printf("  Stack %d is NOT valid/user-accessible\n", j);
      } else if ((*pte & PTE_W) == 0) {
        all_writable = 0;
        printf("  Stack %d is NOT writable\n", j);
      }
  }
  
  // if (all_writable) {
  //   printf("All stacks are user-writable!\n");
  // } else {
  //   printf("WARNING: Not all stacks are user-writable\n");
  // }

  return argc; // this ends up in a0, the first argument to main(argc, argv)

 bad:
  if(pagetable)
    proc_freepagetable(pagetable, sz);
  if(ip){
    iunlockput(ip);
    end_op();
  }
  return -1;
}

// Load an ELF program segment into pagetable at virtual address va.
// va must be page-aligned
// and the pages from va to va+sz must already be mapped.
// Returns 0 on success, -1 on failure.
static int
loadseg(pagetable_t pagetable, uint64 va, struct inode *ip, uint offset, uint sz)
{
  uint i, n;
  uint64 pa;

  for(i = 0; i < sz; i += PGSIZE){
    pa = walkaddr(pagetable, va + i);
    if(pa == 0)
      panic("loadseg: address should exist");
    if(sz - i < PGSIZE)
      n = sz - i;
    else
      n = PGSIZE;
    if(readi(ip, 0, (uint64)pa, offset+i, n) != n)
      return -1;
  }
  
  return 0;
}
