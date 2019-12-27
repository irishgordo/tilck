/* SPDX-License-Identifier: BSD-2-Clause */

#include <tilck/common/basic_defs.h>
#include <tilck/common/string_util.h>

#include <tilck/kernel/sched.h>
#include <tilck/kernel/kmalloc.h>
#include <tilck/kernel/sync.h>
#include <tilck/kernel/ringbuf.h>
#include <tilck/kernel/datetime.h>
#include <tilck/kernel/elf_utils.h>
#include <tilck/kernel/bintree.h>
#include <tilck/kernel/syscalls.h>
#include <tilck/kernel/debug_utils.h>

#include <tilck/mods/tracing.h>

#define TRACE_BUF_SIZE                       (128 * KB)

struct symbol_node {

   struct bintree_node node;

   void *vaddr;
   const char *name;
};

static struct kmutex tracing_lock;
static struct kcond tracing_cond;
static struct ringbuf tracing_rb;
static void *tracing_buf;

static u32 syms_count;
static struct symbol_node *syms_buf;
static struct symbol_node *syms_bintree;

static int
elf_symbol_cb(struct elf_symbol_info *i, void *arg)
{
   if (!i->name || strncmp(i->name, "sys_", 4))
      return 0; /* not a syscall symbol */

   bintree_node_init(&syms_buf[syms_count].node);
   syms_buf[syms_count].vaddr = i->vaddr;
   syms_buf[syms_count].name = i->name;

   bintree_insert_ptr(&syms_bintree,
                      &syms_buf[syms_count],
                      struct symbol_node,
                      node,
                      vaddr);

   syms_count++;
   return 0;
}

const char *
tracing_get_syscall_name(u32 n)
{
   void *ptr;
   struct symbol_node *node;

   if (!(ptr = get_syscall_func_ptr(n)))
      return NULL;

   node = bintree_find_ptr(syms_bintree, ptr, struct symbol_node, node, vaddr);

   if (!node)
      return NULL;

   return node->name;
}

void
init_tracing(void)
{
   if (!(tracing_buf = kzmalloc(TRACE_BUF_SIZE)))
      panic("Unable to allocate the tracing buffer");

   if (!(syms_buf = kmalloc(sizeof(struct symbol_node) * MAX_SYSCALLS)))
      panic("Unable to allocate the syms_buf in tracing.c");

   ringbuf_init(&tracing_rb,
                TRACE_BUF_SIZE / sizeof(struct trace_event),
                sizeof(struct trace_event),
                tracing_buf);

   kmutex_init(&tracing_lock, 0);
   kcond_init(&tracing_cond);

   foreach_symbol(elf_symbol_cb, NULL);
}

void
trace_syscall_enter(u32 sys,
                    uptr a1, uptr a2, uptr a3, uptr a4, uptr a5, uptr a6)
{
   struct trace_event e = {
      .type = te_sys_enter,
      .tid = get_curr_tid(),
      .sys_time = get_sys_time(),
      .sys = sys,
      .args = {a1,a2,a3,a4,a5,a6}
   };

   kmutex_lock(&tracing_lock);
   {
      ringbuf_write_elem(&tracing_rb, &e);
      kcond_signal_one(&tracing_cond);
   }
   kmutex_unlock(&tracing_lock);
}

void
trace_syscall_exit(u32 sys, sptr retval,
                   uptr a1, uptr a2, uptr a3, uptr a4, uptr a5, uptr a6)
{
   struct trace_event e = {
      .type = te_sys_exit,
      .tid = get_curr_tid(),
      .sys_time = get_sys_time(),
      .sys = sys,
      .retval = retval,
      .args = {a1,a2,a3,a4,a5,a6}
   };

   kmutex_lock(&tracing_lock);
   {
      ringbuf_write_elem(&tracing_rb, &e);
      kcond_signal_one(&tracing_cond);
   }
   kmutex_unlock(&tracing_lock);
}

bool read_trace_event(struct trace_event *e, u32 timeout_ticks)
{
   bool ret;
   kmutex_lock(&tracing_lock);
   {
      if (ringbuf_is_empty(&tracing_rb))
         kcond_wait(&tracing_cond, &tracing_lock, timeout_ticks);

      ret = ringbuf_read_elem(&tracing_rb, e);
   }
   kmutex_unlock(&tracing_lock);
   return ret;
}
