#include "types.h"
#include "kernel.hh"
#include "spinlock.h"
#include "condvar.h"
#include "fs.h"
#include <uk/stat.h>
#include "kalloc.hh"
#include "file.hh"
#include "bits.hh"
#include "amd64.h"
#include "cpu.hh"
#include "sampler.h"
#include "major.h"
#include "apic.hh"
#include "percpu.hh"
#include "kstream.hh"

#define LOGHEADER_SZ (sizeof(struct logheader) + \
                      sizeof(((struct logheader*)0)->cpu[0])*NCPU)

// Maximum bytes in a log segment
#define LOG_SEGMENT_MAX (1024*1024)
// The total number of log segments
#define LOG_SEGMENTS (PERFSIZE / LOG_SEGMENT_MAX)
// The number of log segments per CPU
#define LOG_SEGMENTS_PER_CPU (LOG_SEGMENTS < NCPU ? 1 : LOG_SEGMENTS / NCPU)
// The number of pmuevents in a log segment
#define LOG_SEGMENT_COUNT (LOG_SEGMENT_MAX / sizeof(struct pmuevent))
// The byte size of a log segment
#define LOG_SEGMENT_SZ (LOG_SEGMENT_COUNT * sizeof(struct pmuevent))

#define LOG2_HASH_BUCKETS 12

static volatile u64 selector;
static volatile u64 period;

static const u64 wd_period = 24000000000ul;

#if defined(HW_josmp) || defined(HW_tom)
static const u64 wd_selector =
  1 << 24 | 
  1 << 22 |
  1 << 20 |
  1 << 17 | 
  1 << 16 | 
  0x76;
#elif defined(HW_ben)
static const u64 wd_selector =
  1 << 24 | 
  1 << 22 |
  1 << 20 |
  1 << 17 | 
  1 << 16 | 
  0x3c;
#else
static const u64 wd_selector = 0;
#endif

static void enable_nehalem_workaround(void);
static void wdcheck(struct trapframe*);

class pmu
{
public:
  virtual ~pmu() { };

  virtual bool try_init() = 0;
  virtual void initcore() { }
  virtual void configure(int counter, uint64_t selector, uint64_t value) = 0;
  virtual uint64_t get_overflow() = 0;
};

class pmu *pmu;

struct pmulog {
  u64 count;
  struct pmuevent *segments[LOG_SEGMENTS_PER_CPU];
  struct pmuevent *hash;

private:
  bool evict(struct pmuevent *event, size_t reserve);

public:
  bool log(struct trapframe *tf);
  void flush();
} __mpalign__;

percpu<struct pmulog, percpu_safety::internal> pmulog;

//
// AMD PMU
//

class amd_pmu : public pmu
{
  enum {
    COUNTER_BITS = 48,
    MAX_PERIOD = (1ull << 47) - 1,
  };

public:
  bool
  try_init() override
  {
    return true;
  }

  void
  configure(int ctr, uint64_t sel, uint64_t val) override
  {
    if (val > MAX_PERIOD)
      val = MAX_PERIOD;
    writemsr(MSR_AMD_PERF_SEL0 + ctr, 0);
    writemsr(MSR_AMD_PERF_CNT0 + ctr, -val);
    writemsr(MSR_AMD_PERF_SEL0 + ctr, sel);
  }

  uint64_t
  get_overflow() override
  {
    uint64_t ovf = 0;
    for (int pmc = 0; pmc < 2; ++pmc) {
      uint64_t cnt = rdpmc(pmc);
      if ((cnt & (1ull << (COUNTER_BITS - 1))) == 0)
        ovf |= 1 << pmc;
    }
    return ovf;
  }
};

//
// Intel PMU
//

class intel_pmu : public pmu
{
  enum {
    // From Intel Arch. Vol 3b:
    //   "On write operations, the lower 32-bits of the MSR may be
    //   written with any value, and the high-order bits are
    //   sign-extended from the value of bit 31."
    MAX_PERIOD = (1ull << 31) - 1,
  };

  int num_pmcs;

public:
  bool
  try_init() override
  {
    uint32_t eax;
    cpuid(CPUID_PERFMON, &eax, 0, 0, 0);
    if (PERFMON_EAX_VERSION(eax) < 2) {
      cprintf("initsamp: Unsupported performance monitor version %d\n",
              PERFMON_EAX_VERSION(eax));
      return false;
    }
    num_pmcs = PERFMON_EAX_NUM_COUNTERS(eax);
    return true;
  }

  void
  initcore() override
  {
    enable_nehalem_workaround();
  }

  void
  configure(int ctr, uint64_t sel, uint64_t val) override
  {
    if (val > MAX_PERIOD)
      val = MAX_PERIOD;
    writemsr(MSR_INTEL_PERF_SEL0 + ctr, 0);
    // Clear the overflow indicator
    writemsr(MSR_INTEL_PERF_GLOBAL_OVF_CTRL, 1<<ctr);
    writemsr(MSR_INTEL_PERF_CNT0 + ctr, -val);
    writemsr(MSR_INTEL_PERF_SEL0 + ctr, sel);
  }

  uint64_t
  get_overflow() override
  {
    auto ovf = readmsr(MSR_INTEL_PERF_GLOBAL_STATUS);
    writemsr(MSR_INTEL_PERF_GLOBAL_OVF_CTRL, ovf & 0xffffffff);
    return ovf;
  }
};

//
// No PMU
//

class no_pmu : public pmu
{
  bool
  try_init() override
  {
    return true;
  }

  void 
  configure(int ctr, uint64_t sel, uint64_t val) override
  {
  }

  uint64_t
  get_overflow() override
  {
    return 0;
  }
};

//
// Event log
//

static uintptr_t
samphash(struct pmuevent *ev)
{
  uintptr_t h = ev->rip ^ ev->idle;
  for (auto t : ev->trace)
    h ^= t;
  return h;
}

// Test if two events are the same except for their count.
static bool
sampequal(struct pmuevent *a, struct pmuevent *b)
{
  if (a->rip != b->rip || a->idle != b->idle)
    return false;
  for (int i = 0; i < NELEM(a->trace); ++i)
    if (a->trace[i] != b->trace[i])
      return false;
  return true;
}

// Evict an event from the hash table.  Does *not* clear the hash
// table entry.  Returns true if there is still room in the log, or
// false if the log is full.
bool
pmulog::evict(struct pmuevent *event, size_t reserve)
{
  if (count == LOG_SEGMENTS_PER_CPU * LOG_SEGMENT_COUNT - reserve)
    return false;
  size_t segment = count / LOG_SEGMENT_COUNT;
  assert(segment < LOG_SEGMENTS_PER_CPU);
  segments[segment][count % LOG_SEGMENT_COUNT] = *event;
  count++;
  return true;
}

// Record tf in the log.  Returns true if there is still room in the
// log, or false if the log is full.
bool
pmulog::log(struct trapframe *tf)
{
  struct pmuevent ev;
  ev.idle = (myproc() == idleproc());
  ev.rip = tf->rip;
  getcallerpcs((void*)tf->rbp, ev.trace, NELEM(ev.trace));

  // Put event in the hash table
  auto bucket = &hash[samphash(&ev) % (1 << LOG2_HASH_BUCKETS)];
  if (bucket->count) {
    // Bucket is in use.  Is it the same sample?
    if (sampequal(&ev, bucket)) {
      ++bucket->count;
      return true;
    } else {
      // Evict the sample currently in the hash table.  Reserve enough
      // space in the log that we can flush the whole hash table when
      // the sampler is disabled.
      if (!evict(bucket, 1 << LOG2_HASH_BUCKETS))
        return false;
    }
  }
  ev.count = 1;
  *bucket = ev;
  return true;
}

// Flush everything from the hash table in l.
void
pmulog::flush()
{
  size_t failed = 0;
  for (int i = 0; i < 1<<LOG2_HASH_BUCKETS; ++i) {
    if (hash[i].count) {
      if (!evict(&hash[i], 0))
        ++failed;
      hash[i].count = 0;
    }
  }
  if (failed)
    // This shouldn't happen because we reserved enough space for a
    // full flush while we were running.
    swarn.println("sampler: Failed to flush ", failed, " event(s)");
}

//
// Configuration and interrupt handling
//

void
sampconf(void)
{
  pushcli();
  if (selector & PERF_SEL_INT)
    pmulog[myid()].count = 0;
  pmu->configure(0, selector, period);
  popcli();
}

void
sampstart(void)
{
  pushcli();
  for(struct cpu *c = cpus; c < cpus+ncpu; c++) {
    if(c == cpus+mycpu()->id)
      continue;
    lapic->send_sampconf(c);
  }
  sampconf();
  popcli();
}

int
sampintr(struct trapframe *tf)
{
  int r = 0;

  // Acquire locks that we only acquire during NMI.
  // NMIs are disabled until the next iret.

  // Linux unmasks LAPIC.PC after every interrupt (perf_event.c)
  lapic->mask_pc(false);

  u64 overflow = pmu->get_overflow();

  if (overflow & (1<<0)) {
    ++r;
    if (pmulog->log(tf))
      pmu->configure(0, selector, period);
  }

  if (overflow & (1<<1)) {
    ++r;
    wdcheck(tf);
    pmu->configure(1, wd_selector, wd_period);
  }

  return r;
}

static int
readlog(char *dst, u32 off, u32 n)
{
  struct pmulog *q = &pmulog[NCPU];
  struct pmulog *p;
  int ret = 0;
  u64 cur = 0;

  for (p = &pmulog[0]; p != q && n != 0; p++) {
    p->flush();
    u64 len = p->count * sizeof(struct pmuevent);
    if (cur <= off && off < cur+len) {
      u64 boff = off-cur;
      u64 cc = MIN(len-boff, n);
      while (cc) {
        size_t segment = boff / LOG_SEGMENT_SZ;
        size_t segoff = boff % LOG_SEGMENT_SZ;
        char *buf = (char*)p->segments[segment];
        size_t segcc = MIN(cc, LOG_SEGMENT_SZ - segoff);
        memmove(dst, buf + segoff, segcc);
        cc -= segcc;
        n -= segcc;
        ret += segcc;
        off += segcc;
        dst += segcc;
      }
    }
    cur += len;
  }

  return ret;
}

static void
sampstat(mdev*, struct stat *st)
{
  struct pmulog *q = &pmulog[NCPU];
  struct pmulog *p;
  u64 sz = 0;
  
  sz += LOGHEADER_SZ;
  for (p = &pmulog[0]; p != q; p++)
    sz += p->count * sizeof(struct pmuevent);

  st->st_size = sz;
}

static int
sampread(mdev*, char *dst, u32 off, u32 n)
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
sampwrite(mdev*, const char *buf, u32 off, u32 n)
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

// Enable PMU Workaround for
// * Intel Errata AAK100 (model 26)
// * Intel Errata AAP53  (model 30)
// * Intel Errata BD53   (model 44)
// Without this, performance counters may fail to count
static void
enable_nehalem_workaround(void)
{
  static const unsigned long magic[4] = {
    0x4300B5,
    0x4300D2,
    0x4300B1,
    0x4300B1
  };

  uint32_t eax;
  cpuid(CPUID_PERFMON, &eax, nullptr, nullptr, nullptr);
  if (PERFMON_EAX_VERSION(eax) == 0)
    return;
  int num = PERFMON_EAX_NUM_COUNTERS(eax);
  if (num > 4)
    num = 4;

  writemsr(MSR_INTEL_PERF_GLOBAL_CTRL, 0x0);

  for (int i = 0; i < num; i++) {
    writemsr(MSR_INTEL_PERF_SEL0 + i, magic[i]);
    writemsr(MSR_INTEL_PERF_CNT0 + i, 0x0);
  }

  writemsr(MSR_INTEL_PERF_GLOBAL_CTRL, 0xf);
  writemsr(MSR_INTEL_PERF_GLOBAL_CTRL, 0x0);

  for (int i = 0; i < num; i++) {
    writemsr(MSR_INTEL_PERF_SEL0 + i, 0);
    writemsr(MSR_INTEL_PERF_CNT0 + i, 0);
  }

  writemsr(MSR_INTEL_PERF_GLOBAL_CTRL, 0x3);
}

void
initsamp(void)
{
  static class amd_pmu amd_pmu;
  static class intel_pmu intel_pmu;
  static class no_pmu no_pmu;

  if (myid() == 0) {
    u32 name[4];
    char *s = (char *)name;
    name[3] = 0;

    cpuid(0, 0, &name[0], &name[2], &name[1]);
    if (VERBOSE)
      cprintf("%s\n", s);
    if (strcmp(s, "AuthenticAMD") == 0 && amd_pmu.try_init())
      pmu = &amd_pmu;
    else if (strcmp(s, "GenuineIntel") == 0 && intel_pmu.try_init())
      pmu = &intel_pmu;
    else {
      cprintf("initsamp: Unknown manufacturer\n");
      pmu = &no_pmu;
      return;
    }
  }

  if (pmu == &no_pmu)
    return;

  // enable RDPMC at CPL > 0
  u64 cr4 = rcr4();
  lcr4(cr4 | CR4_PCE);

  for (int i = 0; i < LOG_SEGMENTS_PER_CPU; ++i) {
    auto l = &pmulog[myid()];
    l->segments[i] = (pmuevent*)kmalloc(LOG_SEGMENT_SZ, "perf");
    if (!l->segments[i])
      panic("initsamp: kalloc");
    l->hash = (pmuevent*)kmalloc((1<<LOG2_HASH_BUCKETS) * sizeof(pmuevent),
                                 "perfhash");
    if (!l->hash)
      panic("initsamp: kalloc hash");
    memset(l->hash, 0, (1<<LOG2_HASH_BUCKETS) * sizeof(pmuevent));
  }

  pmu->initcore();

  devsw[MAJ_SAMPLER].write = sampwrite;
  devsw[MAJ_SAMPLER].read = sampread;
  devsw[MAJ_SAMPLER].stat = sampstat;
}

//
// watchdog
//

static percpu<int> wd_count;
static spinlock wdlock("wdlock");

static void
wdcheck(struct trapframe* tf)
{
  if (*wd_count == 1) {
    auto l = wdlock.guard();
    // uartputc guarantees some output
    uartputc('W');
    uartputc('D');
    __cprintf(" cpu %u locked up\n", myid());
    __cprintf("  %016lx\n", tf->rip);
    printtrace(tf->rbp);
  }
  ++*wd_count;
}

void
wdpoke(void)
{
  *wd_count = 0;
}

void
initwd(void)
{
  wdpoke();
  pushcli();
  if (wd_selector)
    pmu->configure(1, wd_selector, wd_period);
  popcli();
}
