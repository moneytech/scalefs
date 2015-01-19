#include "types.h"
#include "kernel.hh"
#include "buf.hh"
#include "weakcache.hh"
#include "mfs.hh"
#include "scalefs.hh"


static weakcache<buf::key_t, buf> bufcache(BSIZE << 10);
            //I don't think this was changed when BSIZE was changed from 512 to 4096

sref<buf>
buf::get(u32 dev, u64 block)
{
  buf::key_t k = { dev, block };
  for (;;) {
    sref<buf> b = bufcache.lookup(k);
    if (b.get() != nullptr) {
      // Wait for buffer to load, by getting a read seqlock,
      // which waits for the write seqlock bit to be cleared.
      b->seq_.read_begin();
      return b;
    }

    sref<buf> nb = sref<buf>::transfer(new buf(dev, block));
    auto locked = nb->write(); // marks the block as dirty automatically
    if (bufcache.insert(k, nb.get())) {
      nb->cache_pin(true); // keep it in the cache
      ideread(dev, locked->data, BSIZE, block*BSIZE);
      nb->mark_clean(); // we just loaded the contents from the disk!
      return nb;
    }
  }
}

void
buf::writeback()
{
  lock_guard<sleeplock> l(&writeback_lock_);
  auto copy = read();

  // write copy[] to disk; don't need to wait for write to finish,
  // as long as write order to disk has been established.
  idewrite(dev_, copy->data, BSIZE, block_*BSIZE);
  mark_clean();
}

void
buf::add_to_transaction(transaction *trans, u32 bno, char buf[BSIZE])
{
  assert(bno == (u32) block_);
  trans->add_unique_block(bno, buf);
  mark_clean();
}

void
buf::onzero()
{
  bufcache.cleanup(weakref_);
  delete this;
}
