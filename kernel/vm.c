#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "spinlock.h"
#include "proc.h"

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[];  // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

// Make a direct-map page table for the kernel.
pagetable_t
kvmmake(void)
{
  pagetable_t kpgtbl;

  kpgtbl = (pagetable_t) kalloc();
  memset(kpgtbl, 0, PGSIZE);

  // uart registers
  kvmmap(kpgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(kpgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // PLIC
  kvmmap(kpgtbl, PLIC, PLIC, 0x400000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap(kpgtbl, (uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

  // map kernel stacks
  proc_mapstacks(kpgtbl);
  
  return kpgtbl;
}

// Initialize the one kernel_pagetable
void
kvminit(void)
{
  kernel_pagetable = kvmmake();
}

// Switch h/w page table register to the kernel's page table,
// and enable paging.
void
kvminithart()
{
  w_satp(MAKE_SATP(kernel_pagetable));
  sfence_vma();
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  if(va >= MAXVA)
    panic("walk");

  for(int level = 2; level > 0; level--) {
    pte_t *pte = &pagetable[PX(level, va)];
    if(*pte & PTE_V) {
      pagetable = (pagetable_t)PTE2PA(*pte);
    } 
    else {
      if(!alloc || (pagetable = (pde_t*)kalloc()) == 0)
        return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  return &pagetable[PX(0, va)];
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;

  if(va >= MAXVA)
    return 0;

  pte = walk(pagetable, va, 0);
  if(pte == 0)
    return 0;
  if((*pte & PTE_V) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  pa = PTE2PA(*pte);
  return pa;
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void
kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm)
{
  if(mappages(kpgtbl, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned. Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, last;
  pte_t *pte;

  a = PGROUNDDOWN(va);
  last = PGROUNDDOWN(va + size - 1);
  for(;;){
    if((pte = walk(pagetable, a, 1)) == 0)
      return -1;
    if(*pte & PTE_V)
    {
      panic("remap");
    }
    *pte = PA2PTE(pa) | perm | PTE_V;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. The mappings must exist.
// Optionally free the physical memory.
void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t *pte;
  struct proc * p =myproc();
  if((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  for(a = va; a < va + npages*PGSIZE; a += PGSIZE){
    if((pte = walk(pagetable, a, 0)) == 0)
      panic("uvmunmap: walk");
    if((*pte & (PTE_V|PTE_PG)) == 0)
      panic("uvmunmap: not mapped");
    if(PTE_FLAGS(*pte) == PTE_V)
      panic("uvmunmap: not a leaf");
    if((*pte & PTE_PG)>0)
      {
        for (int i = 0; i < MAX_TOTAL_PAGES; i++)
        {
          if(*pte == p->filePages[i].entry)
          {
            p->offsets_in_swap_file[p->filePages[i].offset_in_file]=i;
            p->filePages[i].entry=(uint64) 0;
            p->filePages[i].is_taken=0;
            p->filePages[i].va=0;
            p->filePages[i].offset_in_file=-1;
            p->filePages[i].on_phys=0;
            break;
          }
        }
/*         printf("UNMAP  p->num_of_total_pages--...  p->num_of_total_pages value : %d\n", p->num_of_total_pages); 
 */        p->num_of_total_pages--;
      }
    else if (do_free){
      uint64 pa = PTE2PA(*pte);
      kfree((void*)pa);
    
    for (int i = 0; i < MAX_TOTAL_PAGES; i++)
        {
          if(*pte == p->filePages[i].entry)
          {
            printf("****\n");
            p->offsets_in_swap_file[ p->filePages[i].offset_in_file]=i;
            p->filePages[i].entry=(uint64) 0;
            p->filePages[i].is_taken=0;
            p->filePages[i].va=0;
            p->filePages[i].offset_in_file=-1;
            p->filePages[i].on_phys=0;
            break;
          }
    }
                printf("       ****\n");

    p->num_of_total_pages--;
    p->num_of_physical_pages--;
    }
    *pte = 0;
  }
}

// create an empty user page table.
// returns 0 if out of memory.
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t) kalloc();
  if(pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Load the user initcode into address 0 of pagetable,
// for the very first process.
// sz must be less than a page.
void
uvminit(pagetable_t pagetable, uchar *src, uint sz)
{
  char *mem;

  if(sz >= PGSIZE)
    panic("inituvm: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pagetable, 0, PGSIZE, (uint64)mem, PTE_W|PTE_R|PTE_X|PTE_U);
  memmove(mem, src, sz);
}
int
get_page_to_swap(void)
{
struct proc * p = myproc();
for (int i = MAX_TOTAL_PAGES-1; i>=0; i--)
{ 
  if(p->filePages[i].on_phys)
    return i;
}
  printf("get_page_to_swap");

return -1;
}
int
get_page_index(uint64 pte)
{
  struct proc * p =myproc();
  for(int i =0; i<MAX_TOTAL_PAGES; i++)
  { 
    if((p->filePages[i].entry==pte)&&p->filePages[i].is_taken)
    {
      return i;
    }
  }
    printf("get_page_index");

  return -1;
}
int
get_free_offset(struct proc * p)
{ int index = -1;
  for(int i=0;i<MAX_DISC_PAGES;i++)
  {
    if(p->offsets_in_swap_file[i]!=-1)
    {
      index =p->offsets_in_swap_file[i];
      p->offsets_in_swap_file[i]=-1;
      break;
    }
  }
  panic("No free offsets in swap file");
  return index;
}
void
make_free_offset(struct proc * p,int offset)
{ 
  p->offsets_in_swap_file[offset]=offset;

}
void
put_in_file(pte_t* entry,int index_of_page_to_swap)
{
  struct proc * p = myproc();
 // int index = get_page_index((uint64)entry); should be the same as we got
  int index_offset_free_in_file=get_free_offset(p);
  int index = index_of_page_to_swap;
  p->filePages[index].offset_in_file=index_offset_free_in_file;
  p->filePages[index].on_phys=0;
  p->filePages[index].entry=(uint64)entry;
  pte_t* ent=(pte_t*)entry;
  char * pa = (char *) (PTE2PA(* ent)); 
 
  if(writeToSwapFile(p,pa , OFFSET_IND2OFFSET_FILE(index_offset_free_in_file),PGSIZE)<0){
    printf("Failed to write to swap\n");
  }
  else{
  }
    (*entry)&= ~PTE_V;
    (*entry)|= PTE_PG;
       p->num_of_physical_pages--;
       sfence_vma();//flush the tlb
  
  
}
void 
take_from_file(pte_t* pt_entry,int index_of_page_to_swap)
{
    struct proc * p = myproc();
     char * pa = (char *) (PTE2PA(* pt_entry));
    int offset= p->filePages[index_of_page_to_swap].offset_in_file;
    p->filePages[index_of_page_to_swap].offset_in_file=-1;//make it unused again
    readFromSwapFile(p,pa,OFFSET_IND2OFFSET_FILE(offset),PGSIZE);
    make_free_offset(p,offset);
    (*pt_entry) |= PTE_V;
    (*pt_entry) &= ~PTE_PG;
    p->num_of_physical_pages++;




}
void 
free_one_page_from_mem(struct proc* p)
{
 int index_of_page_to_swap = get_page_to_swap();
  if(index_of_page_to_swap==-1){ // should not happen
      panic("OMG");
  }
  pte_t* pte_entry_of_page_to_swap=(pte_t*)p->filePages[index_of_page_to_swap].entry;
  put_in_file(pte_entry_of_page_to_swap,index_of_page_to_swap);
  printf("FFF\n");
  uint64 pa= PTE2PA(*pte_entry_of_page_to_swap);
  if(pa==0)
    panic("kfree free_one_page_from_mem");
  kfree((void*)pa);
  
}

int
find_free_slot(struct proc* p ){
  
  for(int i=0;i<MAX_TOTAL_PAGES;i++){
    printf("**** p->pid %d,p->filePages[%d].is_taken= %d\n",p->pid,i,p->filePages[i].is_taken);
    if(p->filePages[i].is_taken==0)
        return i;
  }
  printf("findFreeSlot\n");
  
  return -1;
}

// Allocate PTEs and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  struct proc *p=myproc();
  char *mem;
  uint64 a;

  if(newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for(a = oldsz; a < newsz; a += PGSIZE){
    #ifndef NONE

    #endif

    mem = kalloc();
    if(mem == 0){
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    //check that there is not 16 pages inside RAM at the moment
    //if so , move one to swap file
    if(mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_W|PTE_X|PTE_R|PTE_U) != 0){
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    pte_t* pt_entry = walk(pagetable,a,0);
    //TODO: add the pte to the new struct
    if(p->num_of_total_pages==32)
    panic("f");
    int index=find_free_slot(p);
    if(index!=-1){
    p->filePages[index].is_taken=1;
    p->filePages[index].va=(uint64)a;
    p->filePages[index].entry=(uint64)pt_entry;
    p->filePages[index].on_phys=1;
    p->num_of_physical_pages++;
    p->num_of_total_pages++;
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if(newsz >= oldsz)
    return oldsz;

  if(PGROUNDUP(newsz) < PGROUNDUP(oldsz)){
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
void
freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0){
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if(pte & PTE_V){
      panic("freewalk: leaf");
    }
  }
  kfree((void*)pagetable);
}

// Free user memory pages,
// then free page-table pages.
void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  if(sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz)/PGSIZE, 1);
  freewalk(pagetable);
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;

  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walk(old, i, 0)) == 0)
      panic("uvmcopy: pte should exist");
    if((*pte & (PTE_V|PTE_PG)) == 0)
      panic("uvmcopy: page not present");


 

    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
mem=0;
   #ifndef NONE
   
    if(*pte & PTE_PG){ //original page on disk
      
    }
    else
    {
    if((mem = kalloc()) == 0)
      goto err;
    memmove(mem, (char*)pa, PGSIZE);
    }
    #endif

    if(mappages(new, i, PGSIZE, (uint64)mem, flags) != 0){
      kfree(mem);
      goto err;
    }
  }
  return 0;

 err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void
uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  
  pte = walk(pagetable, va, 0);
  if(pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int
copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;

  while(len > 0){
    va0 = PGROUNDDOWN(dstva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (dstva - va0);
    if(n > len)
      n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int
copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  uint64 n, va0, pa0;

  while(len > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if(n > len)
      n = len;
    memmove(dst, (void *)(pa0 + (srcva - va0)), n);

    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;
  }
  return 0;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int
copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  uint64 n, va0, pa0;
  int got_null = 0;

  while(got_null == 0 && max > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if(n > max)
      n = max;

    char *p = (char *) (pa0 + (srcva - va0));
    while(n > 0){
      if(*p == '\0'){
        *dst = '\0';
        got_null = 1;
        break;
      } else {
        *dst = *p;
      }
      --n;
      --max;
      p++;
      dst++;
    }

    srcva = va0 + PGSIZE;
  }
  if(got_null){
    return 0;
  } else {
    return -1;
  }
}
