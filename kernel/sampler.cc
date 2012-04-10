#include "types.h"
#include "kernel.hh"
#include "spinlock.h"
#include "condvar.h"
#include "fs.h"
#include "stat.h"
#include "kalloc.hh"
#include "file.hh"
#include "bits.hh"
#include "amd64.h"
#include "cpu.hh"
#include "sampler.h"
#include "major.h"

#define LOGHEADER_SZ (sizeof(struct logheader) + \
                      sizeof(((struct logheader*)0)->cpu[0])*NCPU)

static volatile u64 selector;
static volatile u64 period;

struct pmu {
  void (*config)(u64 ctr, u64 sel, u64 val);  
  u64 cntval_bits;
};
struct pmu pmu;

struct pmulog {
  u64 count;
  u64 capacity;
  struct pmuevent *event;
  __padout__;
} __mpalign__;

struct pmulog pmulog[NCPU] __mpalign__;

//
// AMD stuff
//
static void
amdconfig(u64 ctr, u64 sel, u64 val)
{
  writemsr(MSR_AMD_PERF_SEL0 | ctr, 0);
  writemsr(MSR_AMD_PERF_CNT0 | ctr, val);
  writemsr(MSR_AMD_PERF_SEL0 | ctr, sel);
}

struct pmu amdpmu = { amdconfig, 48 };

//
// Intel stuff
//
static void
intelconfig(u64 ctr, u64 sel, u64 val)
{
  writemsr(MSR_INTEL_PERF_SEL0 | ctr, 0);
  writemsr(MSR_INTEL_PERF_CNT0 | ctr, val);
  writemsr(MSR_INTEL_PERF_SEL0 | ctr, sel);
}

// XXX
struct pmu intelpmu = { intelconfig, 48 };

void
sampdump(void)
{
  for (int c = 0; c < NCPU; c++) {
    struct pmulog *l = &pmulog[c];    
    cprintf("%u samples %lu\n", c, l->count);
    for (u64 i = 0; i < 4 && i < l->count; i++)
      cprintf(" %lx\n", l->event[i].rip);
  }
}

void
sampconf(void)
{
  pushcli();
  if (selector & PERF_SEL_INT)
    pmulog[myid()].count = 0;
  pmu.config(0, selector, -period);
  popcli();
}

void
sampstart(void)
{
  pushcli();
  for(struct cpu *c = cpus; c < cpus+ncpu; c++) {
    if(c == cpus+mycpu()->id)
      continue;
    lapic_sampconf(c->hwid);
  }
  sampconf();
  popcli();
}

static int
samplog(struct trapframe *tf)
{
  struct pmulog *l;
  struct pmuevent *e;
  l = &pmulog[mycpu()->id];

  if (l->count == l->capacity)
    return 0;

  e = &l->event[l->count];

  e->idle = (myproc() == idleproc());
  e->rip = tf->rip;
  getcallerpcs((void*)tf->rbp, e->trace, NELEM(e->trace));
  l->count++;
  return 1;
}

int
sampintr(struct trapframe *tf)
{
  // Acquire locks that we only acquire during NMI.
  // NMIs are disabled until the next iret.

  // Linux unmasks LAPIC.PC after every interrupt (perf_event.c)
  lapicpc(0);
  // Only level-triggered interrupts require an lapiceoi.

  u64 cnt = rdpmc(0);
  if (cnt & (1ULL << (pmu.cntval_bits - 1)))
    return 0;

  if (samplog(tf))
    pmu.config(0, selector, -period);

  return 1;
}

static int
readlog(char *dst, u32 off, u32 n)
{
  struct pmulog *q = &pmulog[NCPU];
  struct pmulog *p;
  int ret = 0;
  u64 cur = 0;

  for (p = &pmulog[0]; p != q && n != 0; p++) {
    u64 len = p->count * sizeof(struct pmuevent);
    char *buf = (char*)p->event;
    if (cur <= off && off < cur+len) {
      u64 boff = off-cur;
      u64 cc = MIN(len-boff, n);
      memmove(dst, buf+boff, cc);

      n -= cc;
      ret += cc;
      off += cc;
      dst += cc;
    }
    cur += len;
  }

  return ret;
}

static void
sampstat(struct inode *ip, struct stat *st)
{
  struct pmulog *q = &pmulog[NCPU];
  struct pmulog *p;
  u64 sz = 0;
  
  sz += LOGHEADER_SZ;
  for (p = &pmulog[0]; p != q; p++)
    sz += p->count * sizeof(struct pmuevent);

  st->dev = ip->dev;
  st->ino = ip->inum;
  st->type = ip->type;
  st->nlink = ip->nlink;
  st->size = sz;
}

static int
sampread(struct inode *ip, char *dst, u32 off, u32 n)
{
  struct pmulog *q = &pmulog[NCPU];
  struct pmulog *p;
  struct logheader *hdr;
  int ret;
  int i;
  
  ret = 0;
  if (off < LOGHEADER_SZ) {
    u64 len = LOGHEADER_SZ;
    u64 cc;
    
    hdr = (logheader*) kmalloc(len, "logheader");
    if (hdr == nullptr)
      return -1;
    hdr->ncpus = NCPU;
    i = 0;
    for (p = &pmulog[0]; p != q; p++) {
      u64 sz = p->count * sizeof(struct pmuevent);
      hdr->cpu[i].offset = len;
      hdr->cpu[i].size = sz;
      len += sz;
      i++;
    }

    cc = MIN(LOGHEADER_SZ-off, n);
    memmove(dst, (char*)hdr + off, cc);
    kmfree(hdr, LOGHEADER_SZ);

    n -= cc;
    ret += cc;
    off += cc;
    dst += cc;
  }

  if (off >= LOGHEADER_SZ)
    ret += readlog(dst, off-LOGHEADER_SZ, n);
  return ret;
}

static int
sampwrite(struct inode *ip, const char *buf, u32 off, u32 n)
{
  struct sampconf *conf;

  if (n != sizeof(*conf))
    return -1;
  conf = (struct sampconf*)buf;

  switch(conf->op) {
  case SAMP_ENABLE:
    selector = conf->selector;
    period = conf->period;
    sampstart();
    break;
  case SAMP_DISABLE:
    selector = 0;
    period = 0;
    sampstart();
    break;
  }

  return n;
}

void
initsamp(void)
{
  if (myid() == mpbcpu()) {
    u32 name[4];
    char *s = (char *)name;
    name[3] = 0;

    cpuid(0, 0, &name[0], &name[2], &name[1]);
    if (VERBOSE)
      cprintf("%s\n", s);
    if (!strcmp(s, "AuthenticAMD"))
      pmu = amdpmu;
    else if (!strcmp(s, "GenuineIntel"))
      pmu = intelpmu;
    else
      panic("Unknown Manufacturer");
  }

  // enable RDPMC at CPL > 0
  u64 cr4 = rcr4();
  lcr4(cr4 | CR4_PCE);
  
  void *p = ksalloc(slab_perf);
  if (p == nullptr)
    panic("initprof: ksalloc");
  pmulog[myid()].event = (pmuevent*) p;
  pmulog[myid()].capacity = PERFSIZE / sizeof(struct pmuevent);

  devsw[MAJ_SAMPLER].write = sampwrite;
  devsw[MAJ_SAMPLER].read = sampread;
  devsw[MAJ_SAMPLER].stat = sampstat;
}
