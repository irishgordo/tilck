
#include <exos/common/basic_defs.h>
#include <exos/common/string_util.h>

#include <exos/kernel/paging.h>
#include <exos/kernel/irq.h>
#include <exos/kernel/kmalloc.h>
#include <exos/kernel/debug_utils.h>
#include <exos/kernel/process.h>
#include <exos/kernel/hal.h>
#include <exos/kernel/user.h>
#include <exos/kernel/elf_utils.h>
#include <exos/kernel/system_mmap.h>
#include <exos/kernel/fault_resumable.h>
#include <exos/kernel/errno.h>

#include "paging_int.h"

/*
 * When this flag is set in the 'avail' bits in page_t, in means that the page
 * is writeable even if it marked as read-only and that, on a write attempt
 * the page has to be copied (copy-on-write).
 */
#define PAGE_COW_ORIG_RW 1


/* ---------------------------------------------- */

extern page_directory_t *kernel_page_dir;
extern page_directory_t *curr_page_dir;
extern u8 page_size_buf[PAGE_SIZE];
extern char vsdo_like_page[PAGE_SIZE];

static u16 *pageframes_refcount;
static uptr phys_mem_lim;

static ALWAYS_INLINE u32 pf_ref_count_inc(u32 paddr)
{
   if (paddr >= phys_mem_lim)
      return 0;

   return ++pageframes_refcount[paddr >> PAGE_SHIFT];
}

static ALWAYS_INLINE u32 pf_ref_count_dec(u32 paddr)
{
   if (paddr >= phys_mem_lim)
      return 0;

   ASSERT(pageframes_refcount[paddr >> PAGE_SHIFT] > 0);
   return --pageframes_refcount[paddr >> PAGE_SHIFT];
}

static ALWAYS_INLINE u32 pf_ref_count_get(u32 paddr)
{
   if (paddr >= phys_mem_lim)
      return 0;

   return pageframes_refcount[paddr >> PAGE_SHIFT];
}


bool handle_potential_cow(u32 vaddr)
{
   page_table_t *ptable;
   const u32 page_table_index = (vaddr >> PAGE_SHIFT) & 1023;
   const u32 page_dir_index = (vaddr >> (PAGE_SHIFT + 10));
   void *const page_vaddr = (void *)(vaddr & PAGE_MASK);

   ptable =
   KERNEL_PA_TO_VA(curr_page_dir->entries[page_dir_index].ptaddr << PAGE_SHIFT);

   if (!(ptable->pages[page_table_index].avail & PAGE_COW_ORIG_RW))
      return false; /* Not a COW page */

   const u32 orig_page_paddr =
      ptable->pages[page_table_index].pageAddr << PAGE_SHIFT;

   if (pf_ref_count_get(orig_page_paddr) == 1) {

      /* This page is not shared anymore. No need for copying it. */
      ptable->pages[page_table_index].rw = true;
      ptable->pages[page_table_index].avail = 0;
      invalidate_page(vaddr);
      return true;
   }

   // Decrease the ref-count of the original pageframe.
   pf_ref_count_dec(orig_page_paddr);

   // Copy the whole page to our temporary buffer.
   memcpy(page_size_buf, page_vaddr, PAGE_SIZE);

   // Allocate and set a new page.
   void *new_page_vaddr = kmalloc(PAGE_SIZE);
   VERIFY(new_page_vaddr != NULL); // TODO: handle this OOM condition.
   ASSERT(IS_PAGE_ALIGNED(new_page_vaddr));

   const uptr paddr = KERNEL_VA_TO_PA(new_page_vaddr);

   /* Sanity-check: a newly allocated pageframe MUST have ref-count == 0 */
   ASSERT(pf_ref_count_get(paddr) == 0);
   pf_ref_count_inc(paddr);

   ptable->pages[page_table_index].pageAddr = paddr >> PAGE_SHIFT;
   ptable->pages[page_table_index].rw = true;
   ptable->pages[page_table_index].avail = 0;

   invalidate_page(vaddr);

   // Copy back the page.
   memcpy(page_vaddr, page_size_buf, PAGE_SIZE);
   return true;
}

static void
find_sym_at_addr_no_ret(uptr vaddr,
                        ptrdiff_t *offset,
                        u32 *sym_size,
                        const char **sym_name_ref)
{
  *sym_name_ref = find_sym_at_addr(vaddr, offset, sym_size);
}

static const char *
find_sym_at_addr_safe(uptr vaddr, ptrdiff_t *offset, u32 *sym_size)
{
   const char *sym_name = NULL;
   fault_resumable_call(~0, &find_sym_at_addr_no_ret, 4,
                        vaddr, offset, sym_size, &sym_name);

   return sym_name;
}


void handle_page_fault_int(regs *r)
{
   u32 vaddr;
   asmVolatile("movl %%cr2, %0" : "=r"(vaddr));

   bool us = (r->err_code & (1 << 2)) != 0;
   bool rw = (r->err_code & (1 << 1)) != 0;
   bool p = (r->err_code & (1 << 0)) != 0;

   if (us && rw && p && handle_potential_cow(vaddr)) {
      return;
   }

   if (!us) {
      ptrdiff_t off = 0;
      const char *sym_name = find_sym_at_addr_safe(r->eip, &off, NULL);
      panic("PAGE FAULT in attempt to %s %p from %s%s\nEIP: %p [%s + 0x%x]\n",
            rw ? "WRITE" : "READ",
            vaddr,
            "kernel",
            !p ? " (NON present)." : ".",
            r->eip, sym_name ? sym_name : "???", off);
   }

   panic("PAGE FAULT in attempt to %s %p from %s%s\nEIP: %p\n",
         rw ? "WRITE" : "READ",
         vaddr,
         "userland",
         !p ? " (NON present)." : ".", r->eip);

   // We are not really handling yet user page-faults.
}


void handle_page_fault(regs *r)
{
   if (in_panic()) {

      printk("Page fault while already in panic state.\n");

      while (true) {
         halt();
      }
   }

   ASSERT(!is_preemption_enabled());
   ASSERT(!are_interrupts_enabled());

   enable_interrupts_forced();
   {
      /* Page fault are processed with IF = 1 */
      handle_page_fault_int(r);
   }
   disable_interrupts_forced(); /* restore IF = 0 */
}


void handle_general_protection_fault(regs *r)
{
   /*
    * For the moment, we don't properly handle GPF yet.
    *
    * TODO: handle GPF caused by user applications with by sending SIGSEGV.
    * Example: user code attempts to execute privileged instructions.
    */
   panic("General protection fault. Error: %p\n", r->err_code);
}

void set_page_directory(page_directory_t *pdir)
{
   curr_page_dir = pdir;
   asmVolatile("mov %0, %%cr3" :: "r"(KERNEL_VA_TO_PA(pdir)));
}

bool is_mapped(page_directory_t *pdir, void *vaddrp)
{
   page_table_t *ptable;
   const uptr vaddr = (uptr) vaddrp;
   const u32 page_table_index = (vaddr >> PAGE_SHIFT) & 1023;
   const u32 page_dir_index = (vaddr >> (PAGE_SHIFT + 10));

   page_dir_entry_t *e = &pdir->entries[page_dir_index];

   if (!e->present)
      return false;

   if (e->psize) /* 4-MB page */
      return e->present;

   ptable = KERNEL_PA_TO_VA(pdir->entries[page_dir_index].ptaddr << PAGE_SHIFT);
   return ptable->pages[page_table_index].present;
}

void set_page_rw(page_directory_t *pdir, void *vaddrp, bool rw)
{
   page_table_t *ptable;
   const uptr vaddr = (uptr) vaddrp;
   const u32 page_table_index = (vaddr >> PAGE_SHIFT) & 1023;
   const u32 page_dir_index = (vaddr >> (PAGE_SHIFT + 10));

   ptable = KERNEL_PA_TO_VA(pdir->entries[page_dir_index].ptaddr << PAGE_SHIFT);
   ASSERT(KERNEL_VA_TO_PA(ptable) != 0);
   ptable->pages[page_table_index].rw = rw;
   invalidate_page(vaddr);
}

void unmap_page(page_directory_t *pdir, void *vaddrp)
{
   page_table_t *ptable;
   const uptr vaddr = (uptr) vaddrp;
   const u32 page_table_index = (vaddr >> PAGE_SHIFT) & 1023;
   const u32 page_dir_index = (vaddr >> (PAGE_SHIFT + 10));

   ptable = KERNEL_PA_TO_VA(pdir->entries[page_dir_index].ptaddr << PAGE_SHIFT);
   ASSERT(KERNEL_VA_TO_PA(ptable) != 0);
   ASSERT(ptable->pages[page_table_index].present);

   const uptr paddr = ptable->pages[page_table_index].pageAddr << PAGE_SHIFT;
   ptable->pages[page_table_index].raw = 0;

   pf_ref_count_dec(paddr);
   invalidate_page(vaddr);
}

uptr get_mapping(page_directory_t *pdir, void *vaddrp)
{
   page_table_t *ptable;
   const uptr vaddr = (uptr)vaddrp;
   const u32 page_table_index = (vaddr >> PAGE_SHIFT) & 0x3FF;
   const u32 page_dir_index = (vaddr >> 22) & 0x3FF;

   /*
    * This function shall be never called for the linear-mapped zone of the
    * the kernel virtual memory.
    */
   ASSERT(vaddr < KERNEL_BASE_VA || vaddr >= LINEAR_MAPPING_OVER_END);

   ptable = KERNEL_PA_TO_VA(pdir->entries[page_dir_index].ptaddr << PAGE_SHIFT);
   ASSERT(KERNEL_VA_TO_PA(ptable) != 0);

   ASSERT(ptable->pages[page_table_index].present);
   return ptable->pages[page_table_index].pageAddr << PAGE_SHIFT;
}

NODISCARD int
map_page_int(page_directory_t *pdir, void *vaddrp, uptr paddr, u32 flags)
{
   page_table_t *ptable;
   const u32 vaddr = (u32) vaddrp;
   const u32 page_table_index = (vaddr >> PAGE_SHIFT) & 1023;
   const u32 page_dir_index = (vaddr >> (PAGE_SHIFT + 10));

   ASSERT(!(vaddr & OFFSET_IN_PAGE_MASK)); // the vaddr must be page-aligned
   ASSERT(!(paddr & OFFSET_IN_PAGE_MASK)); // the paddr must be page-aligned

   ptable = KERNEL_PA_TO_VA(pdir->entries[page_dir_index].ptaddr << PAGE_SHIFT);
   ASSERT(IS_PAGE_ALIGNED(ptable));

   if (UNLIKELY(KERNEL_VA_TO_PA(ptable) == 0)) {

      // we have to create a page table for mapping 'vaddr'.
      ptable = kzmalloc(sizeof(page_table_t));

      if (!ptable)
         return -ENOMEM;

      ASSERT(IS_PAGE_ALIGNED(ptable));

      pdir->entries[page_dir_index].raw =
         PG_PRESENT_BIT |
         PG_RW_BIT |
         (flags & PG_US_BIT) |
         KERNEL_VA_TO_PA(ptable);
   }

   ASSERT(ptable->pages[page_table_index].present == 0);

   ptable->pages[page_table_index].raw = PG_PRESENT_BIT | flags | paddr;
   pf_ref_count_inc(paddr);
   invalidate_page(vaddr);
   return 0;
}


NODISCARD int
map_page(page_directory_t *pdir,
         void *vaddrp,
         uptr paddr,
         bool us,
         bool rw)
{
   return
      map_page_int(pdir,
                   vaddrp,
                   paddr,
                   (us << PG_US_BIT_POS) |
                   (rw << PG_RW_BIT_POS) |
                   ((!us) << PG_GLOBAL_BIT_POS)); /* Kernel pages are global */
}

page_directory_t *pdir_clone(page_directory_t *pdir)
{
   page_directory_t *new_pdir = kmalloc(sizeof(page_directory_t));
   ASSERT(IS_PAGE_ALIGNED(new_pdir));

   if (!new_pdir)
      return NULL;

   memcpy(new_pdir, pdir, sizeof(page_directory_t));

   for (u32 i = 0; i < (KERNEL_BASE_VA >> 22); i++) {

      /* User-space cannot use 4-MB pages */
      ASSERT(!pdir->entries[i].psize);

      if (!pdir->entries[i].present)
         continue;

      page_table_t *orig_pt =
         KERNEL_PA_TO_VA(pdir->entries[i].ptaddr << PAGE_SHIFT);

      /* Mark all the pages in that page-table as COW. */
      for (u32 j = 0; j < 1024; j++) {

         if (!orig_pt->pages[j].present)
            continue;

         const uptr orig_paddr = orig_pt->pages[j].pageAddr << PAGE_SHIFT;

         /* Sanity-check: a mapped page MUST have ref-count > 0 */
         ASSERT(pf_ref_count_get(orig_paddr) > 0);

         if (orig_pt->pages[j].rw) {
            orig_pt->pages[j].avail |= PAGE_COW_ORIG_RW;
         }

         orig_pt->pages[j].rw = false;

         // We're making for the first time this page to be COW.
         pf_ref_count_inc(orig_paddr);
      }

      // alloc memory for the page table

      page_table_t *pt = kmalloc(sizeof(*pt));
      VERIFY(pt != NULL); // TODO: handle this OOM condition!
      ASSERT(IS_PAGE_ALIGNED(pt));

      // copy the page table
      memcpy(pt, orig_pt, sizeof(*pt));

      /* We've already copied the other members of new_pdir->entries[i] */
      new_pdir->entries[i].ptaddr = KERNEL_VA_TO_PA(pt) >> PAGE_SHIFT;
   }

   return new_pdir;
}


void pdir_destroy(page_directory_t *pdir)
{
   // Kernel's pdir cannot be destroyed!
   ASSERT(pdir != kernel_page_dir);

   for (u32 i = 0; i < (KERNEL_BASE_VA >> 22); i++) {

      if (!pdir->entries[i].present)
         continue;

      page_table_t *pt = KERNEL_PA_TO_VA(pdir->entries[i].ptaddr << PAGE_SHIFT);

      for (u32 j = 0; j < 1024; j++) {

         if (!pt->pages[j].present)
            continue;

         const u32 paddr = pt->pages[j].pageAddr << PAGE_SHIFT;

         if (pf_ref_count_dec(paddr) == 0)
            kfree2(KERNEL_PA_TO_VA(paddr), PAGE_SIZE);
      }

      // We freed all the pages, now free the whole page-table.
      kfree2(pt, sizeof(*pt));
   }

   // We freed all pages and all the page-tables, now free pdir.
   kfree2(pdir, sizeof(*pdir));
}


void map_4mb_page_int(page_directory_t *pdir,
                      void *vaddrp,
                      uptr paddr,
                      u32 flags)
{
   const u32 vaddr = (u32) vaddrp;
   const u32 page_dir_index = (vaddr >> (PAGE_SHIFT + 10));

   ASSERT(!(vaddr & (4*MB - 1))); // the vaddr must be 4MB-aligned
   ASSERT(!(paddr & (4*MB - 1))); // the paddr must be 4MB-aligned

   // Check that the entry has not been used.
   ASSERT(!pdir->entries[page_dir_index].present);

   // Check that there is no page table associated with this entry.
   ASSERT(!pdir->entries[page_dir_index].ptaddr);

   pdir->entries[page_dir_index].raw = flags | paddr;
}

/*
 * Page directories MUST BE page-size-aligned.
 */
static char kpdir_buf[sizeof(page_directory_t)] PAGE_SIZE_ALIGNED;

void init_paging(void)
{
   set_fault_handler(FAULT_PAGE_FAULT, handle_page_fault);
   set_fault_handler(FAULT_GENERAL_PROTECTION, handle_general_protection_fault);
   kernel_page_dir = (page_directory_t *) kpdir_buf;
}

void init_paging_cow(void)
{
   phys_mem_lim = get_phys_mem_size();

   /*
    * Allocate the buffer used for keeping a ref-count for each pageframe.
    * This is necessary for COW.
    */

   size_t pagesframes_refcount_bufsize =
      (get_phys_mem_size() >> PAGE_SHIFT) * sizeof(pageframes_refcount[0]);

   pageframes_refcount = kzmalloc(pagesframes_refcount_bufsize);

   if (!pageframes_refcount)
      panic("Unable to allocate pageframes_refcount");

   /*
    * Map a special vdso-like page used for the sysenter interface.
    * This is the only user-mapped page with a vaddr in the kernel space.
    */
   int rc = map_page(kernel_page_dir,
                     (void *)USER_VSDO_LIKE_PAGE_VADDR,
                     KERNEL_VA_TO_PA(&vsdo_like_page),
                     true,
                     false);

   if (rc < 0)
      panic("Unable to map the vsdo-like page");
}
