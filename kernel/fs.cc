// File system implementation.  Four layers:
//   + Blocks: allocator for raw disk blocks.
//   + Files: inode allocator, reading, writing, metadata.
//   + Directories: inode with special contents (list of other inodes!)
//   + Names: paths like /usr/rtm/xv6/fs.c for convenient naming.
//
// Disk layout is: superblock, inodes, block in-use bitmap, data blocks.
//
// This file contains the low-level file system manipulation
// routines.  The (higher-level) system call implementations
// are in sysfile.c.

/*
 * inode cache will be RCU-managed:
 *
 * - to evict, mark inode as a victim
 * - lookups that encounter a victim inode must return an error (-E_RETRY)
 * - E_RETRY rolls back to the beginning of syscall/pagefault and retries
 * - out-of-memory error should be treated like -E_RETRY
 * - once an inode is marked as victim, it can be gc_delayed()
 * - the do_gc() method should remove inode from the namespace & free it
 *
 * - inodes have a refcount that lasts beyond a GC epoch
 * - to bump refcount, first bump, then check victim flag
 * - if victim flag is set, reduce the refcount and -E_RETRY
 *
 */

#include "types.h"
#include <uk/stat.h>
#include "mmu.h"
#include "kernel.hh"
#include "spinlock.hh"
#include "condvar.hh"
#include "proc.hh"
#include "fs.h"
#include "file.hh"
#include "cpu.hh"
#include "kmtrace.hh"
#include "dirns.hh"
#include "kstream.hh"
#include "scalefs.hh"

#define IADDRSSZ (sizeof(u32)*NINDIRECT)
#define BLOCKROUNDUP(off) (((off)%BSIZE) ? (off)/BSIZE+1 : (off)/BSIZE)

// A hash-table to cache in-memory inode data-structures.
static chainhash<pair<u32, u32>, inode*> *ins;

static sref<inode> the_root;
static struct superblock sb_root;

// Read the super block.
static void
readsb(int dev, struct superblock *sb)
{
  sref<buf> bp = buf::get(dev, 1);
  auto copy = bp->read();
  memmove(sb, copy->data, sizeof(*sb));
}

void
get_superblock(struct superblock *sb, bool get_reclaim_inodes)
{
  sb->size = sb_root.size;
  sb->ninodes = sb_root.ninodes;
  sb->nblocks = sb_root.nblocks;

  if (get_reclaim_inodes) {
    sb->num_reclaim_inodes = sb_root.num_reclaim_inodes;
    for (int i = 0; i < sb_root.num_reclaim_inodes; i++)
      sb->reclaim_inodes[i] = sb_root.reclaim_inodes[i];
  }
}

// Zero the in-memory buffer-cache block corresponding to a disk block.
// If @writeback == true, immediately write back the zeroed block to disk
// (this is useful when clearing the journal's disk blocks).
static void
bzero(int dev, int bno, bool writeback = false)
{
  sref<buf> bp = buf::get(dev, bno, true);
  {
    auto locked = bp->write();
    memset(locked->data, 0, BSIZE);
  }
  if (writeback)
    bp->writeback_async();
}

class out_of_blocks : public std::exception
{
  virtual const char* what() const throw() override
  {
    return "Out of blocks";
  }
};

static void inline
throw_out_of_blocks()
{
#if EXCEPTIONS
  throw out_of_blocks();
#else
  panic("out of blocks");
#endif
}

// Allocate a disk block. This makes changes only to the in-memory
// free-bit-vector (maintained by rootfs_interface), not the one on the disk.
static u32
balloc(u32 dev, transaction *trans = NULL, bool zero_on_alloc = false)
{
  int b;

  if (dev == 1) {
    b = rootfs_interface->alloc_block();
    if (b < sb_root.size) {
      if (trans)
        trans->add_allocated_block(b);

      if (zero_on_alloc)
        bzero(dev, b);
      return b;
    }
  }

  throw_out_of_blocks();
  // Unreachable
  return 0;
}

// Free a disk block. We never zero out blocks during free (we do that only
// during allocation, if desired).
//
// This makes changes only to the in-memory free-bit-vector (maintained by
// rootfs_interface), not the one on the disk.
//
// delayed_free = true indicates that the block should not be marked free in the
// in-memory free-bit-vector just yet. This is delayed until the time that the
// transaction is processed. We need this to ensure that the blocks freed in a
// transaction are not available for reuse until that transaction commits.
static void
bfree(int dev, u64 x, transaction *trans = NULL, bool delayed_free = false)
{
  u32 b = x;

  if (dev == 1) {
    if (!delayed_free)
      rootfs_interface->free_block(b);
    if (trans)
      trans->add_free_block(b);
    return;
  }
}

// Mark blocks as allocated or freed in the on-disk bitmap.
// Allocate if @alloc == true, free otherwise.
void
balloc_free_on_disk(std::vector<u32>& blocks, transaction *trans, bool alloc)
{
  // Sort the blocks in ascending order, so that we update the bitmap blocks
  // on the disk one after another, without going back and forth.
  std::sort(blocks.begin(), blocks.end());

  // Aggregate all updates to the same free bitmap block and write it out
  // just once, using a single transaction_diskblock.
  for (auto bno = blocks.begin(); bno != blocks.end(); ) {
    u32 blocknum = BBLOCK(*bno, sb_root.ninodes);
    sref<buf> bp = buf::get(1, blocknum);
    auto locked = bp->write();

    // Record the highest block-number represented in this free bitmap block,
    // to facilitate merging of all updates that touch the same bitmap block.
    u32 max_bno = *bno | (BPB - 1);

    do {
      int bi = *bno % BPB;
      int m = 1 << (bi % 8);
      if (alloc) {
        if ((locked->data[bi/8] & m) != 0)
          panic("balloc_free_on_disk: block %d already in use", *bno);
        locked->data[bi/8] |= m;
      } else {
        if ((locked->data[bi/8] & m) == 0)
          panic("balloc_free_on_disk: block %d already free", *bno);
        locked->data[bi/8] &= ~m;
      }
    } while (++bno && bno != blocks.end() && *bno <= max_bno);

    bp->add_to_transaction(trans);
  }
}


// Inodes.
//
// An inode is a single, unnamed file in the file system. The inode disk
// structure holds metadata (the type, device numbers, and data size) along
// with a list of blocks where the associated data can be found.
//
// The inodes are laid out sequentially on disk immediately after the
// superblock.  The kernel keeps a cache of the in-use on-disk structures
// to provide a place for synchronizing access to inodes shared between
// multiple processes.
//
// ip->ref counts the number of pointer references to this cached inode;
// references are typically kept in struct file and in proc->cwd. When ip->ref
// falls to zero, the inode is no longer cached. It is an error to use an
// inode without holding a reference to it.
//
// Processes are only allowed to read and write inode metadata and contents
// when holding the inode's lock, represented by the I_BUSY flag in the
// in-memory copy. Because inode locks are held during disk accesses, they
// are implemented using a flag rather than with spin locks. Callers are
// responsible for locking inodes before passing them to routines in this file;
// leaving this responsibility with the caller makes it possible for them to
// create arbitrarily-sized atomic operations.
//
// To give maximum control over locking to the callers, the routines in this
// file that return inode pointers return pointers to *unlocked* inodes. It is
// the callers' responsibility to lock them before using them. A non-zero
// ip->ref keeps these unlocked inodes in the cache.

void
initinode(void)
{
  scoped_gc_epoch e;

  readsb(ROOTDEV, &sb_root); // Initialize sb_root by reading the superblock.
  ins = new chainhash<pair<u32, u32>, inode*>(NINODES_PRIME);

  the_root = inode::alloc(ROOTDEV, ROOTINO);
  if (!ins->insert(make_pair(the_root->dev, the_root->inum), the_root.get()))
    panic("initinode: Failed to insert the root inode into the cache\n");
  the_root->init();
}

static sref<inode>
try_ialloc(u32 inum, u32 dev, short type)
{
  sref<inode> ip = iget(dev, inum);
  if (ip->type || !cmpxch(&ip->type, (short) 0, type))
    return sref<inode>();

  ilock(ip, 1);
  auto w = ip->seq.write_begin();
  ip->gen += 1;
  if (ip->nlink() || ip->size || ip->addrs[0])
    panic("try_ialloc: inode not zeroed\n");
  iunlock(ip);
  return ip;
}

// Note down the last inode allocated by each CPU, so that we can try to
// allocate the subsequent inode number next.
DEFINE_PERCPU(int, last_inode);

// Allocate a new inode with the given type on device dev.
// Returns a locked inode.
sref<inode>
ialloc(u32 dev, short type)
{
  scoped_gc_epoch e;
  sref<inode> ip;

#if 0
  // TODO: Partitioning inodes by CPU number this way is great for scalability,
  // but it doesn't do a good job of handling situations that need a single CPU
  // to allocate a large number of inodes, well beyond IPB (especially when the
  // total number of inodes is limited). Fix that, and also use the last_inode[]
  // scheme.
  for (int k = myid()*IPB; k < sb_root.ninodes; k += (NCPU*IPB)) {
    for (inum = k; inum < k+IPB && inum < sb_root.ninodes; inum++) {
      if (inum == 0)
        continue;
      ip = try_ialloc(inum, dev, type);
      if (ip) {
        last_inode[myid()] = inum;
        return ip;
      }
    }
  }
#endif

  // search through all inodes

  bool all_scanned = false;
  for (int inum = (last_inode[myid()] + 1) % sb_root.ninodes;
       inum < sb_root.ninodes; inum++) {

    if (inum == 0)
      continue;

    ip = try_ialloc(inum, dev, type);
    if (ip) {
      last_inode[myid()] = inum;
      return ip;
    }

    if (inum == sb_root.ninodes - 1 && !all_scanned) {
      inum = 0;
      all_scanned = true;
      continue;
    }
  }

  cprintf("ialloc: 0/%u inodes\n", sb_root.ninodes);
  return sref<inode>();
}

// Copy inode, which has changed, from memory to disk.
void
iupdate(sref<inode> ip, transaction *trans)
{
  // XXX call iupdate to flush in-memory inode state to
  // buffer cache.  use seq value to detect updates.

  scoped_gc_epoch e;

  sref<buf> bp;
  {
    bp = buf::get(ip->dev, IBLOCK(ip->inum));
    auto locked = bp->write();

    dinode *dip = (struct dinode*)locked->data + ip->inum%IPB;
    dip->type = ip->type;
    dip->major = ip->major;
    dip->minor = ip->minor;
    dip->nlink = ip->nlink();
    dip->size = ip->size;
    dip->gen = ip->gen;
    memmove(dip->addrs, ip->addrs, sizeof(ip->addrs));
    if (trans)
      bp->add_to_transaction(trans);
  }

  if (ip->addrs[NDIRECT] != 0) {
    bp = buf::get(ip->dev, ip->addrs[NDIRECT]);
    auto locked = bp->write();
    if (ip->iaddrs.load() != nullptr)
      memmove(locked->data, (void*)ip->iaddrs.load(), IADDRSSZ);
    if (trans)
      bp->add_to_transaction(trans);
  }

}

// Find the inode with number inum on device dev
// and return the in-memory copy.
// The inode is not locked, so someone else might
// be modifying it.
// But it has a ref count, so it won't be freed or reused.
// Though unlocked, all fields will be present,
// so looking a ip->inum and ip->gen are OK even w/o lock.
inode::inode(u32 d, u32 i)
  : rcu_freed("inode", this, sizeof(*this)),
    dev(d), inum(i),
    dir_offset(0),
    valid(false),
    busy(false),
    readbusy(0)
{
  dir.store(nullptr);
  iaddrs.store(nullptr);
}

inode::~inode()
{
  auto d = dir.load();
  if (d) {
    d->remove(strbuf<DIRSIZ>("."));
    d->remove(strbuf<DIRSIZ>(".."));
    gc_delayed(d);
    assert(cmpxch(&dir, d, (decltype(d)) 0));
  }
  if (iaddrs.load() != nullptr) {
    kmfree((void*)iaddrs.load(), IADDRSSZ);
    iaddrs.store(nullptr);
  }
}

sref<inode>
iget(u32 dev, u32 inum)
{
  sref<inode> ip;

  // Assumes caller is holding a gc_epoch

 retry:
  // Try for cached inode.
  inode *iptr = nullptr;
  if (ins->lookup(make_pair(dev, inum), &iptr))
    ip = sref<inode>::newref(iptr);

  if (ip) {
    if (!ip->valid.load()) {
      acquire(&ip->lock);
      while (!ip->valid)
        ip->cv.sleep(&ip->lock);
      release(&ip->lock);
    }
    return ip;
  }

  // Allocate fresh inode cache slot.
  ip = inode::alloc(dev, inum);
  if (ip == nullptr)
    panic("iget: should throw_bad_alloc()");

  // Lock the inode
  ip->busy = true;
  ip->readbusy = 1;

  if (!ins->insert(make_pair(ip->dev, ip->inum), ip.get())) {
    iunlock(ip);
    // reference counting will clean up memory allocation.
    goto retry;
  }

  ip->init();
  iunlock(ip);
  return ip;
}

sref<inode>
inode::alloc(u32 dev, u32 inum)
{
  sref<inode> ip = sref<inode>::transfer(new inode(dev, inum));
  if (ip == nullptr)
    return sref<inode>();

  snprintf(ip->lockname, sizeof(ip->lockname), "cv:ino:%d", ip->inum);
  ip->lock = spinlock(ip->lockname+3, LOCKSTAT_FS);
  ip->cv = condvar(ip->lockname);
  return ip;
}

void
inode::init(void)
{
  scoped_gc_epoch e;
  sref<buf> bp = buf::get(dev, IBLOCK(inum));
  auto copy = bp->read();
  const dinode *dip = (const struct dinode*)copy->data + inum%IPB;

  type = dip->type;
  major = dip->major;
  minor = dip->minor;
  nlink_ = dip->nlink;
  size = dip->size;
  gen = dip->gen;
  memmove(addrs, dip->addrs, sizeof(addrs));

  if (nlink_ > 0)
    inc();

  // Perform another increment. This is decremented when the corresponding
  // mnode's onzero() method is invoked. This is to help keep the inode
  // around until all the open file descriptors of this file have been
  // closed, even if that happens after unlink().
  inc();

  valid.store(true);
}

void
inode::link(void)
{
  // Must hold ilock if inode is accessible by multiple threads
  auto w = seq.write_begin();
  if (++nlink_ == 1) {
    // A non-zero nlink_ holds a reference to the inode
    inc();
  }
}

void
inode::unlink(void)
{
  // Must hold ilock if inode is accessible by multiple threads
  auto w = seq.write_begin();
  if (--nlink_ == 0) {
    // This should never be the last reference..
    dec();
  }
}

short
inode::nlink(void)
{
  // Must hold ilock if inode is accessible by multiple threads
  return nlink_;
}

void
inode::onzero(void)
{
  acquire(&lock);
  /*if (nlink())
    panic("iput [%d]: nlink %u\n", inum, nlink());*/

  // inode is no longer used: truncate and free inode.
  if (busy || readbusy)
    panic("iput busy"); // race with iget

  if (!valid)
    panic("iput not valid");

  busy = true;
  readbusy++;

  // XXX: use gc_delayed() to truncate the inode later.
  // flag it as a victim in the meantime.

  /*itrunc(sref<inode>::transfer(this));

  {
    auto w = seq.write_begin();
    type = 0;
    major = 0;
    minor = 0;
    gen += 1;
  }*/

  release(&lock);

  inode* ip = this;
  ins->remove(make_pair(dev, inum));
  gc_delayed(ip);
  return;
}

// Lock the given inode.
// XXX why does ilock() read the inode from disk?
// why doesn't the iget() that allocated the inode cache entry
// read the inode from disk?
void
ilock(sref<inode> ip, int writer)
{
  if (ip == 0)
    panic("ilock");

  acquire(&ip->lock);
  if (writer) {
    while (ip->busy || ip->readbusy)
      ip->cv.sleep(&ip->lock);
    ip->busy = true;
  } else {
    while (ip->busy)
      ip->cv.sleep(&ip->lock);
  }
  ip->readbusy++;
  release(&ip->lock);

  if (!ip->valid)
    panic("ilock");
}

// Unlock the given inode.
void
iunlock(sref<inode> ip)
{
  if (ip == 0)
    panic("iunlock");
  if (!ip->readbusy && !ip->busy)
    panic("iunlock");

  acquire(&ip->lock);
  --ip->readbusy;
  ip->busy = false;
  ip->cv.wake_all();
  release(&ip->lock);
}

//PAGEBREAK!
// Inode contents
//
// The contents (data) associated with each inode is stored
// in a sequence of blocks on the disk.  The first NDIRECT blocks
// are listed in ip->addrs[].  The next NINDIRECT blocks are
// listed in the block ip->addrs[NDIRECT].  The next NINDIRECT^2
// blocks are doubly-indirect from ip->addrs[NDIRECT+1].

// Return the disk block address of the nth block in inode ip.
// If there is no such block, bmap allocates one.
static u32
bmap(sref<inode> ip, u32 bn, transaction *trans = NULL, bool zero_on_alloc = false)
{
  scoped_gc_epoch e;

  u32* ap;
  u32 addr;

  if (bn < NDIRECT) {
  retry0:
    if ((addr = ip->addrs[bn]) == 0) {
      addr = balloc(ip->dev, trans, zero_on_alloc);
      if (!cmpxch(&ip->addrs[bn], (u32)0, addr)) {
        cprintf("bmap: race1\n");
        bfree(ip->dev, addr, trans);
        goto retry0;
      }
    }
    return addr;
  }
  bn -= NDIRECT;

  if (bn < NINDIRECT) {
  retry1:
    if (ip->iaddrs == nullptr) {
      if ((addr = ip->addrs[NDIRECT]) == 0) {
        addr = balloc(ip->dev, trans, true);
        if (!cmpxch(&ip->addrs[NDIRECT], (u32)0, addr)) {
          cprintf("bmap: race2\n");
          bfree(ip->dev, addr, trans);
          goto retry1;
        }
      }

      volatile u32* iaddrs = (u32*)kmalloc(IADDRSSZ, "iaddrs");
      sref<buf> bp = buf::get(ip->dev, addr);
      auto copy = bp->read();
      memmove((void*)iaddrs, copy->data, IADDRSSZ);

      if (!cmpxch(&ip->iaddrs, (volatile u32*)nullptr, iaddrs)) {
        kmfree((void*)iaddrs, IADDRSSZ);
        goto retry1;
      }
    }

  retry2:
    if ((addr = ip->iaddrs[bn]) == 0) {
      addr = balloc(ip->dev, trans, zero_on_alloc);
      if (!__sync_bool_compare_and_swap(&ip->iaddrs[bn], (u32)0, addr)) {
        cprintf("bmap: race4\n");
        bfree(ip->dev, addr, trans);
        goto retry2;
      }
      if (trans) {
        char charbuf[BSIZE];
        memmove(charbuf, (void*)ip->iaddrs.load(), IADDRSSZ);
        trans->add_block(ip->addrs[NDIRECT], charbuf);
      }
    }

    return addr;
  }
  bn -= NINDIRECT;

  if (bn >= NINDIRECT * NINDIRECT)
    panic("bmap: %d out of range", bn);

  // Doubly-indirect blocks are currently "slower" because we do not
  // cache an equivalent of ip->iaddrs.

retry3:
  if (ip->addrs[NDIRECT+1] == 0) {
    addr = balloc(ip->dev, trans, true);
    if (!cmpxch(&ip->addrs[NDIRECT+1], (u32)0, addr)) {
      cprintf("bmap: race5\n");
      bfree(ip->dev, addr, trans);
      goto retry3;
    }
  }

  sref<buf> wb = buf::get(ip->dev, ip->addrs[NDIRECT+1]);

  for (;;) {
    auto copy = wb->read();
    ap = (u32*)copy->data;
    if (ap[bn / NINDIRECT] == 0) {
      auto locked = wb->write();
      ap = (u32*)locked->data;
      if (ap[bn / NINDIRECT] == 0) {
        ap[bn / NINDIRECT] = balloc(ip->dev, trans, true);
        if (trans)
          wb->add_to_transaction(trans);
      }
      continue;
    }
    addr = ap[bn / NINDIRECT];
    break;
  }

  wb = buf::get(ip->dev, addr);

  for (;;) {
    auto copy = wb->read();
    ap = (u32*)copy->data;
    if (ap[bn % NINDIRECT] == 0) {
      auto locked = wb->write();
      ap = (u32*)locked->data;
      if (ap[bn % NINDIRECT] == 0) {
        ap[bn % NINDIRECT] = balloc(ip->dev, trans, zero_on_alloc);
        if (trans)
          wb->add_to_transaction(trans);
      }
      continue;
    }
    addr = ap[bn % NINDIRECT];
    break;
  }

  return addr;
}


// Fill the file with zeroes till offset. Used to "clear" the journal file.
void
zero_fill(sref<inode> ip, u32 offset)
{
  scoped_gc_epoch e;

  auto w = ip->seq.write_begin();
  u32 bno = BLOCKROUNDUP(offset);

  if (bno < NDIRECT) {
  for (int i = 0; i < bno; i++)
    if (ip->addrs[i])
      bzero(ip->dev, ip->addrs[i], true);
  }
  bno -= NDIRECT;

  if (bno < NINDIRECT) {
    if (ip->addrs[NDIRECT]) {
      sref<buf> bp = buf::get(ip->dev, ip->addrs[NDIRECT]);
      auto copy = bp->read();
      u32* a = (u32*)copy->data;
      for (int i = 0; i < bno; i++)
        if (a[i])
          bzero(ip->dev, a[i], true);
    }
  }
  bno -= NINDIRECT;

  if (bno < NINDIRECT * NINDIRECT) {
    if (ip->addrs[NDIRECT+1]) {
      sref<buf> bp1 = buf::get(ip->dev, ip->addrs[NDIRECT+1]);
      auto copy1 = bp1->read();
      u32* a1 = (u32*)copy1->data;
      u32 end1 = (bno%NINDIRECT) ? bno/NINDIRECT+1 : bno/NINDIRECT;
      for (int i = 0; i < end1; i++) {
        if (a1[i]) {
          sref<buf> bp2 = buf::get(ip->dev, a1[i]);
          auto copy2 = bp2->read();
          u32* a2 = (u32*)copy2->data;
          u32 end2 = (i < end1-1) ? NINDIRECT : bno%NINDIRECT;
          for (int j = 0; j < end2; j++)
            if (a2[j])
              bzero(ip->dev, a2[j], true);
        }
      }
    }
  }
}

// Drop the (clean) buffer-cache blocks associated with this file.
void
drop_bufcache(sref<inode> ip)
{
  scoped_gc_epoch e;

  for (int i = 0; i < NDIRECT; i++) {
    if (ip->addrs[i])
      buf::put(ip->dev, ip->addrs[i]);
  }

  if (ip->addrs[NDIRECT]) {
    sref<buf> bp = buf::get(ip->dev, ip->addrs[NDIRECT]);
    auto copy = bp->read();
    u32 *a = (u32*)copy->data;
    for (int i = 0; i < NINDIRECT; i++) {
      if (a[i])
        buf::put(ip->dev, a[i]);
    }
    // Drop the indirect block too, from the buffer-cache.
    buf::put(ip->dev, ip->addrs[NDIRECT]);
  }

  if (ip->addrs[NDIRECT+1]) {
    sref<buf> bp1 = buf::get(ip->dev, ip->addrs[NDIRECT+1]);
    auto copy1 = bp1->read();
    u32 *a1 = (u32*)copy1->data;

    for (int i = 0; i < NINDIRECT; i++) {
      if (a1[i]) {
        sref<buf> bp2 = buf::get(ip->dev, a1[i]);
        auto copy2 = bp2->read();
        u32 *a2 = (u32*)copy2->data;
        for (int j = 0; j < NINDIRECT; j++) {
          if (a2[j])
            buf::put(ip->dev, a2[j]);
        }
        // Drop the second-level doubly-indirect block too, from the
        // buffer-cache.
        buf::put(ip->dev, a1[i]);
      }
    }

    // Drop the first-level doubly-indirect block too, from the buffer-cache.
    buf::put(ip->dev, ip->addrs[NDIRECT+1]);
  }
}

void
itrunc(sref<inode> ip, u32 offset, transaction *trans)
{
  scoped_gc_epoch e;

  // XXX how to serialize itrunc w.r.t. concurrent itrunc or expansion?
  // Could lock disk blocks (buf's), or could lock the inode?

  auto w = ip->seq.write_begin();
  if (ip->size <= offset)
    return;

  for (int i = BLOCKROUNDUP(offset); i < NDIRECT; i++) {
    if (ip->addrs[i]) {
      bfree(ip->dev, ip->addrs[i], trans, true);
      ip->addrs[i] = 0;
    }
  }

  if (ip->addrs[NDIRECT]) {
    int start = (offset >= NDIRECT*BSIZE) ?
      BLOCKROUNDUP(offset - NDIRECT*BSIZE) : 0;
    {
      sref<buf> bp = buf::get(ip->dev, ip->addrs[NDIRECT]);
      auto locked = bp->write();
      if (ip->iaddrs.load() != nullptr)
        memmove(locked->data, (void*)ip->iaddrs.load(), IADDRSSZ);

      u32* a = (u32*)locked->data;
      for (int i = start; i < NINDIRECT; i++) {
        if (a[i]) {
          bfree(ip->dev, a[i], trans, true);
          a[i] = 0;
        }
      }
      if (trans && start != 0)
        bp->add_to_transaction(trans);
    }

    if (start == 0) {
      bfree(ip->dev, ip->addrs[NDIRECT], trans, true);
      ip->addrs[NDIRECT] = 0;
    }
    if (ip->iaddrs.load() != nullptr) {
      kmfree((void*)ip->iaddrs.load(), IADDRSSZ);
      ip->iaddrs.store(nullptr);
    }
  }

  if (ip->addrs[NDIRECT+1]) {
    int bno = (offset >= (NDIRECT+NINDIRECT)*BSIZE)?
      BLOCKROUNDUP(offset-(NDIRECT+NINDIRECT)*BSIZE): 0;
    {
      sref<buf> bp1 = buf::get(ip->dev, ip->addrs[NDIRECT+1]);
      auto locked1 = bp1->write();
      u32* a1 = (u32*)locked1->data;
      for (int i = bno/NINDIRECT; i < NINDIRECT; i++) {
        if (!a1[i])
          continue;
        int start = (i == bno/NINDIRECT)? bno%NINDIRECT : 0;
        {
          sref<buf> bp2 = buf::get(ip->dev, a1[i]);
          auto locked2 = bp2->write();
          u32* a2 = (u32*)locked2->data;
          for (int j = start; j < NINDIRECT; j++) {
            if (!a2[j])
              continue;

            bfree(ip->dev, a2[j], trans, true);
            a2[j] = 0;
          }
          if (trans && start != 0)
            bp2->add_to_transaction(trans);
        }

        if (start == 0) {
          bfree(ip->dev, a1[i], trans, true);
          a1[i] = 0;
        }
      }
      if (trans && bno != 0)
        bp1->add_to_transaction(trans);
    }

    if (bno == 0) {
      bfree(ip->dev, ip->addrs[NDIRECT+1], trans, true);
      ip->addrs[NDIRECT+1] = 0;
    }
  }

  ip->size = offset;
}

//PAGEBREAK!
// Read data from inode.
int
readi(sref<inode> ip, char *dst, u32 off, u32 n)
{
  scoped_gc_epoch e;

  u32 tot, m;
  sref<buf> bp;

  if (ip->type == T_DEV)
    return -1;

  if (off > ip->size || off + n < off)
    return -1;
  if (off + n > ip->size)
    n = ip->size - off;

  for (tot=0; tot<n; tot+=m, off+=m, dst+=m) {
    try {
      bp = buf::get(ip->dev, bmap(ip, off/BSIZE, NULL, true));
    } catch (out_of_blocks& e) {
      // Read operations should never cause out-of-blocks conditions
      panic("readi: out of blocks");
    }
    m = std::min(n - tot, BSIZE - off%BSIZE);

    auto copy = bp->read();
    memmove(dst, copy->data + off%BSIZE, m);
  }
  return n;
}

// PAGEBREAK!
// Write data to inode.
// writeback = true indicates that the data block is not logged in the journal.
// It is written back to the disk directly.
int
writei(sref<inode> ip, const char *src, u32 off, u32 n, transaction *trans,
      bool writeback)
{
  scoped_gc_epoch e;

  int tot, m;
  sref<buf> bp;
  u32 blocknum;

  if (ip->type == T_DEV)
    return -1;

  //if (off > ip->size || off + n < off)
  if (off + n < off)
    return -1;

  if (off + n > MAXFILE*BSIZE)
    n = MAXFILE*BSIZE - off;

  for (tot=0; tot<n; tot+=m, off+=m, src+=m) {

    bool skip_disk_read = false;
    m = std::min(n - tot, BSIZE - off%BSIZE);

    try {

      // Skip reading the block from disk if we are going to overwrite the
      // entire block anyway.
      if (off % BSIZE == 0 && m == BSIZE)
        skip_disk_read = true;

      blocknum = bmap(ip, off/BSIZE, trans, !skip_disk_read);
      bp = buf::get(ip->dev, blocknum, skip_disk_read);

    } catch (out_of_blocks& e) {
      console.println("writei: out of blocks");
      // If we haven't written anything, return an error
      if (tot == 0)
        tot = -1;
      break;
    }

    {
      auto locked = bp->write();
      memmove(locked->data + off%BSIZE, src, m);

      // If adding the block to the transaction, we need to copy the contents
      // of the block with the write-lock held, so that we add *this* particular
      // version of the block-contents to the transaction. Also, this placement
      // helps ensure that the buf is marked clean at the right moment.
      if (!writeback && trans)
        bp->add_to_transaction(trans);
    }

    if (writeback)
      bp->writeback_async();
  }
  // Don't update inode yet. Wait till all the pages have been written to and then
  // call update_size to update the inode just once.

  return tot;
}

void
update_size(sref<inode> ip, u32 size, transaction *trans)
{
  auto w = ip->seq.write_begin();
  ip->size = size;
  iupdate(ip, trans);
}

//PAGEBREAK!
// Directories

int
namecmp(const char *s, const char *t)
{
  return strncmp(s, t, DIRSIZ);
}

u64
namehash(const strbuf<DIRSIZ> &n)
{
  u64 h = 0;
  for (int i = 0; i < DIRSIZ && n.buf_[i]; i++) {
    u64 c = n.buf_[i];
    // Lifted from dcache.h in Linux v3.3
    h = (h + (c << 4) + (c >> 4)) * 11;
    // XXX(sbw) this doesn't seem to do well with the names
    // in dirbench (the low-order bits get clumped).
    // h = ((h << 8) ^ c) % 0xdeadbeef;
  }
  return h;
}

void
dir_init(sref<inode> dp)
{
  scoped_gc_epoch e;

  if (dp->dir)
    return;
  if (dp->type != T_DIR)
    panic("dir_init not DIR");

  auto dir = new dirns();
  u32 dir_offset = 0;
  for (u32 off = 0; off < dp->size; off += BSIZE) {
    assert(dir_offset == off);
    sref<buf> bp;
    try {
      bp = buf::get(dp->dev, bmap(dp, off / BSIZE, NULL, true));
    } catch (out_of_blocks& e) {
      // Read operations should never cause out-of-blocks conditions
      panic("dir_init: out of blocks");
    }
    auto copy = bp->read();
    for (const struct dirent *de = (const struct dirent *) copy->data;
	 de < (const struct dirent *) (copy->data + BSIZE);
	 de++) {

      if (de->inum)
        dir->insert(strbuf<DIRSIZ>(de->name),
                    dir_entry_info(de->inum, dir_offset));

      dir_offset += sizeof(*de);
    }
  }

  if (!cmpxch(&dp->dir, (decltype(dir)) 0, dir)) {
    // XXX free all the dirents
    delete dir;
  }

  if (!cmpxch(&dp->dir_offset, (u32) 0, dir_offset)) {

    cmpxch(&dp->dir, dir, (decltype(dir)) 0);
    delete dir;
  }
}

void
dir_flush_entry(sref<inode> dp, const char *name, transaction *trans)
{
  if (!dp->dir)
    return;

  auto de_info = dp->dir.load()->lookup(strbuf<DIRSIZ>(name));
  struct dirent de;
  strncpy(de.name, name, DIRSIZ);
  de.inum = de_info.inum_;

  if (writei(dp, (char *)&de, de_info.offset_, sizeof(de), trans) != sizeof(de))
    panic("dir_flush_entry");

  if (dp->size < de_info.offset_ + sizeof(de)) {
    auto w = dp->seq.write_begin();
    dp->size = de_info.offset_ + sizeof(de);
  }

  iupdate(dp, trans);
}

// Look for a directory entry in a directory.
sref<inode>
dirlookup(sref<inode> dp, char *name)
{
  dir_init(dp);

  auto de_info = dp->dir.load()->lookup(strbuf<DIRSIZ>(name));

  if (de_info.inum_ == 0)
    return sref<inode>();
  return iget(dp->dev, de_info.inum_);
}

// Write a new directory entry (name, inum) into the directory dp.
int
dirlink(sref<inode> dp, const char *name, u32 inum, bool inc_link,
        transaction *trans)
{
  dir_init(dp);

  //cprintf("dirlink: %x (%d): %s -> %d\n", dp, dp->inum, name, inum);
  u32 dir_offset = dp->dir_offset.load();
  if (!dp->dir.load()->insert(strbuf<DIRSIZ>(name),
                              dir_entry_info(inum, dir_offset)))
    return -1;

  if (!cmpxch(&dp->dir_offset, dir_offset,
              (u32)(dir_offset + sizeof(struct dirent)))) {

    dp->dir.load()->remove(strbuf<DIRSIZ>(name));
    return -1;
  }

  sref<inode> i = iget(1, inum);
  if (i)
    i->link();
  if (inc_link)
    dp->link();

  dir_flush_entry(dp, name, trans);

  return 0;
}

// Remove a directory entry (name, inum) from the directory dp.
int
dirunlink(sref<inode> dp, const char *name, u32 inum, bool dec_link,
          transaction *trans)
{
  dir_init(dp);

  //cprintf("dirunlink: %x (%d): %s -> %d\n", dp, dp->inum, name, inum);

  auto de_info = dp->dir.load()->lookup(strbuf<DIRSIZ>(name));
  if (!dp->dir.load()->remove(strbuf<DIRSIZ>(name)))
    return -1;

  if (!dp->dir.load()->insert(strbuf<DIRSIZ>(name),
                              dir_entry_info(0, de_info.offset_)))
    return -1;

  sref<inode> i = iget(1, inum);
  if (i)
    i->unlink();
  if (dec_link)
    dp->unlink();

  dir_flush_entry(dp, name, trans);

  dp->dir.load()->remove(strbuf<DIRSIZ>(name));

  return 0;
}

// Paths

// Copy the next path element from path into name.
// Update the pointer to the element following the copied one.
// The returned path has no leading slashes,
// so the caller can check *path=='\0' to see if the name is the last one.
//
// If copied into name, return 1.
// If no name to remove, return 0.
// If the name is longer than DIRSIZ, return -1;
//
// Examples:
//   skipelem("a/bb/c", name) = "bb/c", setting name = "a"
//   skipelem("///a//bb", name) = "bb", setting name = "a"
//   skipelem("a", name) = "", setting name = "a"
//   skipelem("", name) = skipelem("////", name) = 0
//
static int
skipelem(const char **rpath, char *name)
{
  const char *path = *rpath;
  const char *s;
  int len;

  while (*path == '/')
    path++;
  if (*path == 0)
    return 0;
  s = path;
  while (*path != '/' && *path != 0)
    path++;
  len = path - s;
  if (len > DIRSIZ) {
    cprintf("Error: Path component longer than DIRSIZ"
            " (%d characters)\n", DIRSIZ);
    return -1;
  } else {
    memmove(name, s, len);
    if (len < DIRSIZ)
      name[len] = 0;
  }
  while (*path == '/')
    path++;
  *rpath = path;
  return 1;
}

// Look up and return the inode for a path name.
// If parent != 0, return the inode for the parent and copy the final
// path element into name, which must have room for DIRSIZ bytes.
static sref<inode>
namex(sref<inode> cwd, const char *path, int nameiparent, char *name)
{
  // Assumes caller is holding a gc_epoch

  sref<inode> ip;
  sref<inode> next;
  int r;

  if (*path == '/')
    ip = the_root;
  else
    ip = cwd;

  while ((r = skipelem(&path, name)) == 1) {
    // XXX Doing this here requires some annoying reasoning about all
    // of the callers of namei/nameiparent.  Also, since the abstract
    // scope is implicit, it might be wrong (or non-existent) and
    // documenting the abstract object sets of each scope becomes
    // difficult and probably unmaintainable.  We have to compute this
    // information here because it's the only place that's canonical.
    // Maybe this should return the set of inodes traversed and let
    // the caller declare the variables?  Would it help for the caller
    // to pass in an abstract scope?
    mtreadavar("inode:%x.%x", ip->dev, ip->inum);
    if (ip->type == 0)
      panic("namex");
    if (ip->type != T_DIR)
      return sref<inode>();
    if (nameiparent && *path == '\0') {
      // Stop one level early.
      return ip;
    }

    if ((next = dirlookup(ip, name)) == 0)
      return sref<inode>();
    ip = next;
  }

  if (r == -1 || nameiparent)
    return sref<inode>();

  return ip;
}

sref<inode>
namei(sref<inode> cwd, const char *path)
{
  // Assumes caller is holding a gc_epoch
  char name[DIRSIZ];
  return namex(cwd, path, 0, name);
}

sref<inode>
nameiparent(sref<inode> cwd, const char *path, char *name)
{
  // Assumes caller is holding a gc_epoch
  return namex(cwd, path, 1, name);
}
