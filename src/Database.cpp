//
// Created by c6s on 18-4-27.
//

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <algorithm>
#include <utility>
#include <cstring>
#include <zconf.h>
#include "Database.h"
#include "Util.h"
#include "Txn.h"
#include "Meta.h"
namespace boltDB_CPP {

Page *boltDB_CPP::Database::getPage(page_id pageId) {
  assert(pageSize != 0);
  uint64_t pos = pageId * pageSize;
  return reinterpret_cast<Page *>(&data[pos]);
}
FreeList &Database::getFreeLIst() {
  return freeList;
}
uint64_t Database::getPageSize() const {
  return pageSize;
}

Meta *Database::meta() {
  auto m0 = meta0;
  auto m1 = meta1;
  if (meta0->txnId > meta1->txnId) {
    m0 = meta1;
    m1 = meta0;
  }
  if (m0->validate()) {
    return m0;
  }

  if (m1->validate()) {
    return m1;
  }
  assert(false);
}
void Database::removeTxn(Txn *txn) {
  mmapLock.readUnlock();
  metaLock.lock();

  for (auto iter = txs.begin(); iter != txs.end(); iter++) {
    if (*iter == txn) {
      txs.erase(iter);
      break;
    }
  }

  metaLock.unlock();

}
int Database::grow(size_t sz) {
  if (sz <= fileSize) {
    return 0;
  }

  if (dataSize <= allocSize) {
    sz = dataSize;
  } else {
    sz += allocSize;
  }

  if (!noGrowSync && !readOnly) {
    //increase file's size
    if (ftruncate(fd, sz)) {
      return -1;
    }
    //make sure that file size is written into metadata
    if (fsync(fd)) {
      return -1;
    }
  }

  fileSize = sz;
  return 0;
}
int Database::init() {
  //hard code page size
  this->pageSize = DEFAULTPAGESIZE;

  std::vector<char> buf(pageSize * 4);
  for (page_id i = 0; i < 2; i++) {
    auto p = pageInBuffer(buf.data(), buf.size(), i);
    p->pageId = i;
    p->flag = static_cast<uint16_t >(PageFlag::metaPageFlag);

    auto m = p->getMeta();
    m->magic = MAGIC;
    m->version = VERSION;
    m->pageSize = pageSize;
    m->freeListPageNumber = 2;
    m->rootBucketHeader.root = 3;
    m->totalPageNumber = 4;
    m->txnId = i;

    //todo: add check sum

  }

  {
    auto p = pageInBuffer(buf.data(), buf.size(), 2);
    p->pageId = 2;
    p->flag |= static_cast<uint16_t >(PageFlag::freelistPageFlag);
    p->count = 0;
  }

  {
    auto p = pageInBuffer(buf.data(), buf.size(), 3);
    p->pageId = 3;
    p->flag |= static_cast<uint16_t >(PageFlag::leafPageFlag);
    p->count = 0;
  }

  if (writeAt(buf.data(), buf.size(), 0)) {
    return -1;
  }

  if (file_data_sync(fd)) {
    return -1;
  }

  return 0;
}
Page *Database::pageInBuffer(char *ptr, size_t length, page_id pageId) {
  assert(length > pageId * pageSize);
  return reinterpret_cast<Page *>(ptr + pageId * pageSize);
}

struct OnClose {
  OnClose(std::function<void()> function) : fn(std::move(function)) {}
  std::function<void()> fn;
  ~OnClose() {
    if (fn) {
      fn();
    }
  }
};
Database *Database::openDB(const std::string &path_p, uint16_t mode, const Options &options) {
  OnClose{std::bind(&Database::closeDB, this)};
  opened = true;

  noGrowSync = options.noGrowSync;
  mmapFlags = options.mmapFlag;

  maxBatchDelayMillionSeconds = DEFAULTMAXBATCHDELAYMILLIIONSEC;
  maxBatchSize = DEFAULTMAXBATCHSIZE;
  allocSize = DEFAULTALLOCATIONSIZE;

  uint32_t flag = O_RDWR;
  if (options.readOnly) {
    flag = O_RDONLY;
    readOnly = true;
  }

  this->path = path_p;

  {
    //open db file
    auto ret = ::open(path.c_str(), flag | O_CREAT, mode);
    if (ret == -1) {
      perror("open db file");
      return nullptr;
    }
    this->fd = ret;
  }

  {
    if (readOnly) {
      auto ret = file_Rlock(fd);
      if (ret == -1) {
        perror("flock read");
//        closeDB();
        return nullptr;
      }
    } else {
      auto ret = file_Wlock(fd);
      if (ret == -1) {
        perror("flock write");
//        closeDB();
        return nullptr;
      }
    }
  }

  struct stat stat1;
  {
    {
      auto ret = fstat(fd, &stat1);
      if (ret == -1) {
        perror("stat");
//      closeDB();
        return nullptr;
      }
    }

    if (stat1.st_size == 0) {
      if (init()) {
        return nullptr;
      }
    } else {
      //currently not dealing with corrupted db
      std::vector<char> localBuff(0x1000); //4k page
      auto ret = ::pread(fd, localBuff.data(), localBuff.size(), 0);
      if (ret == -1) {
        return nullptr;
      }
      auto m = pageInBuffer(localBuff.data(), localBuff.size(), 0)->getMeta();
      if (!m->validate()) {
        pageSize = DEFAULTPAGESIZE;
      } else {
        pageSize = m->pageSize;
      }
    }
  }

  //init meta
  if (initMeta(stat1.st_size, options.initalMmapSize)) {
    return nullptr;
  }

  //init freelist
  freeList.read(getPage(meta()->freeListPageNumber));
  return nullptr;
}
void Database::closeDB() {
  std::lock_guard<std::mutex> guard1(rwlock);
  std::lock_guard<std::mutex> guard2(metaLock);
  mmapLock.readLock();
  do_closeDB();
  mmapLock.readUnlock();
}

void Database::do_closeDB() {
  if (!opened) {
    return;
  }

  opened = false;
  freeList.reset();
  munmap_db_file(this);

  if (fd) {
    if (!readOnly) {
      file_Unlock(fd);
    }
    close(fd);
    fd = -1;
  }
  path.clear();
}

int Database::initMeta(off_t fileSize, off_t minMmapSize) {
  mmapLock.writeLock();
  OnClose{std::bind(&RWLock::writeUnlock, &mmapLock)};

  if (fileSize < pageSize * 2) {
    //there should be at least 2 page of meta page
    return -1;
  }

  auto targetSize = std::max(fileSize, minMmapSize);
  if (mmapSize(targetSize) == -1) {
    return -1;
  }

  //dereference before unmapping. deference?
  //dereference means make a copy
  //clone data which nodes are pointing to
  //or on unmapping, these data points in nodes will be pointing to undefined value
  if (rwtx) {
    rwtx->rootBucket.dereference();
  }

  //unmapping current db file
  if (munmap_db_file(this)) {
    return -1;
  }

  if (mmap_db_file(this, targetSize)) {
    return -1;
  }

  meta0 = getPage(0)->getMeta();
  meta1 = getPage(1)->getMeta();

  //if one fails validation, it can be recovered from the other one.
  //fail when both meta pages are broken
  if (!meta0->validate() && !meta1->validate()) {
    return -1;
  }
  return 0;
}

int Database::mmapSize(off_t &targetSize) {
  //get lowest size not less than targetSize
  //from 32k to 1g, double every try
  for (size_t i = 15; i <= 30; i++) {
    if (targetSize <= (1UL << i)) {
      targetSize = 1UL << i;
      return 0;
    }
  }

  if (targetSize > MAXMAPSIZE) {
    return -1;
  }

  //not dealing with file size larger than 1GB now
  assert(false);
  std::cerr << "not dealing with db file larger than 1GB now" << std::endl;
  exit(1);
  return 0;
}
int Database::update(std::function<int(Txn *tx)> fn) {
  auto tx = beginRWTx();
  if (tx == nullptr) {
    return -1;
  }
  tx->managed = true;
  auto ret = fn(tx);
  tx->managed = false;
  if (ret != 0) {
    tx->rollback();
    return -1;
  }

  return tx->commit();
}

//when commit a rw txn, rwlock must be released
Txn *Database::beginRWTx() {
  //this property will only be set once
  if (readOnly) {
    return nullptr;
  }

  //unlock on commit/rollback; only one writer transaction at a time
  rwlock.lock();

  //exclusively update transaction
  std::lock_guard<std::mutex> guard(metaLock);

  //this needs to be protected under rwlock/not sure if it needs metaLock
  if (!opened) {
    rwlock.unlock();
    return nullptr;
  }
  auto txn = txnPool.allocate<Txn>();
  txn->writable = true;
  txn->init(this);
  rwtx = txn;

  //release pages of finished read only txns
  auto minId = UINT64_MAX;
  for (auto item : txs) {
    minId = std::min(minId, item->metaData->txnId);
  }

  if (minId > 0) {
    freeList.release(minId - 1);
  }
  return txn;
}

//when commit/abort a read only txn, mmaplock must be released
Txn *Database::beginTx() {
  std::lock_guard<std::mutex> guard(metaLock);
  mmapLock.readLock();
  if (!opened) {
    mmapLock.readUnlock();
    return nullptr;
  }

  auto txn = txnPool.allocate<Txn>();
  txn->init(this);
  txs.push_back(txn);

  return txn;
}

Page *Database::allocate(size_t count, Txn *txn) {
  //buffer len for continuous page
  size_t len = count * pageSize;
  assert(count < UINT32_MAX);
  //allocate memory for these pages
  auto page = reinterpret_cast<Page *>(txn->pool.allocateByteArray(len));
  //set up overflow. overflow + 1 is the total page count
  page->overflow = static_cast<uint32_t >(count) - 1;
  //set up page id

  //1.allocate page numbers from freelist
  auto pid = freeList.allocate(count);
  if (pid != 0) {
    page->pageId = pid;
    return page;
  }

  //2.need to expand mmap file
  page->pageId = rwtx->metaData->totalPageNumber;
  //no sure what the '1' indicates
  size_t minLen = (page->pageId + count + 1) * pageSize;
  if (minLen > dataSize) {
    struct stat stat1;
    {
      auto ret = fstat(fd, &stat1);
      if (ret == -1) {
        perror("stat");
//      closeDB();
        return nullptr;
      }
    }
    if (initMeta(stat1.st_size, minLen)) {
      return nullptr;
    }
  }

  rwtx->metaData->totalPageNumber += count;
  return page;
}
int Database::view(std::function<int(Txn *tx)> fn) {
  auto tx = beginTx();
  if(tx == nullptr){
    return -1;
  }

  tx->managed = true;

  auto ret = fn(tx);

  tx->managed = false;

  if (ret != 0) {
    tx->rollback();
    return -1;
  }

  tx->rollback();
  return 0;
}

void FreeList::free(txn_id tid, Page *page) {
  //meta page will never be freed
  if (page->pageId <= 1) {
    assert(false);
  }
  auto &idx = pending[tid];
  for (auto iter = page->pageId; iter <= page->pageId + page->overflow; iter++) {
    if (cache[iter]) {

      assert(false);
    }

    idx.push_back(iter);
    cache[iter] = true;
  }
}

size_t FreeList::size() {
  auto ret = count();

  if (ret >= 0xffff) {
    ret++;
  }

  return PAGEHEADERSIZE + ret * sizeof(page_id);
}

size_t FreeList::count() {
  return free_count() + pending_count();
}

size_t FreeList::free_count() {
  return pageIds.size();
}

size_t FreeList::pending_count() {
  size_t result = 0;
  for (auto &item : pending) {
    result += item.second.size();
  }
  return result;
}

void FreeList::copyall(std::vector<page_id> &dest) {
  std::vector<page_id> tmp;
  for (auto item : pending) {
    std::copy(item.second.begin(), item.second.end(), std::back_inserter(tmp));
  }
  std::sort(tmp.begin(), tmp.end());
  mergePageIds(dest, tmp, pageIds);
}

page_id FreeList::allocate(size_t sz) {
  if (pageIds.empty()) {
    return 0;
  }

  page_id init = 0;
  page_id prev = 0;

  for (size_t i = 0; i < pageIds.size(); i++) {
    page_id id = pageIds[i];
    if (id <= 1) {
      assert(false);
    }

    if (prev == 0 || id - prev != 1) {
      init = id;
    }

    if (id - prev == sz + 1) {
      for (size_t j = 0; j < sz; j++) {
        pageIds[i + 1 - sz] = pageIds[i + 1 + j];
      }

      for (size_t j = 0; j < sz; j++) {
        cache.erase(init + j);
      }

      return init;
    }

    prev = id;
  }

  return 0;
}

/**
 * release pages belong to txns of tid from zero to parameter tid
 * @param tid : largest tid of txn whose pages are to be freed
 */
void FreeList::release(txn_id tid) {
  std::vector<page_id> list;
  for (auto iter = pending.begin(); iter != pending.end();) {
    if (iter->first <= tid) {
      std::copy(iter->second.begin(), iter->second.end(), std::back_inserter(list));
      iter = pending.erase(iter);
    } else {
      iter++;
    }
  }

  std::sort(list.begin(), list.end());
  pageIds = merge(pageIds, list);
}

void FreeList::rollback(txn_id tid) {
  for (auto item : pending[tid]) {
    cache.erase(item);
  }

  pending.erase(tid);
}

bool FreeList::freed(page_id pageId) {
  return cache[pageId];
}

void FreeList::read(Page *page) {
  size_t idx = 0;
  size_t count = page->count;
  if (count == 0xffff) {
    idx = 1;
    count = *reinterpret_cast<page_id *>(&page->ptr);
  }

  if (count == 0) {
    pageIds.clear();
  } else {
    pageIds.clear();
    page_id *ptr = reinterpret_cast<page_id *>(&page->ptr) + idx;
    for (size_t i = 0; i < count; i++) {
      pageIds.push_back(*ptr);
      ptr++;
    }

    std::sort(pageIds.begin(), pageIds.end());
  }

  reindex();
}

void FreeList::reindex() {
  cache.clear();
  for (auto item : pageIds) {
    cache[item] = true;
  }

  for (auto &item : pending) {
    for (auto inner : item.second) {
      cache[inner] = true;
    }
  }
}

//curious about how to deal with overflow pages
//allocate will return a continuous block of memory
//if the write overflow one page, it should be written into the next page
//but I havn't find out in which procedure the overflow is updated
int FreeList::write(Page *page) {
  page->flag |= static_cast<uint16_t >(PageFlag::freelistPageFlag);
  auto count = this->count();
  if (count == 0) {
    page->count = static_cast<uint16_t >(count);
  } else if (count < 0xffff) {
    page->count = static_cast<uint16_t >(count);
    //re-implement it to avoid copying later
    std::vector<page_id> dest;
    copyall(dest);
    auto ptr = reinterpret_cast<page_id *>(&page->ptr);
    for (auto item : dest) {
      *ptr = item;
      ptr++;
    }
  } else {
    page->count = 0xffff;
    auto ptr = reinterpret_cast<page_id *>(&page->ptr);
    *ptr = count;
    std::vector<page_id> dest;
    copyall(dest);
    ptr++;
    for (auto item : dest) {
      *ptr = item;
      ptr++;
    }
  }
  return 0;
}

void FreeList::reload(Page *page) {
  read(page);

  //filter out current pending pages from those just read from page file
  std::map<page_id, bool> curPending;
  for (auto item : pending) {
    for (auto inner : item.second) {
      curPending[inner] = true;
    }
  }

  std::vector<page_id> newIds;
  for (auto item : pageIds) {
    if (curPending.find(item) == curPending.end()) {
      newIds.push_back(item);
    }
  }

  pageIds = newIds;
  reindex();
}
void FreeList::reset() {
  pageIds.clear();
  pending.clear();
  cache.clear();
}

//uint64_t MetaData::sum64() {
//
//  return 0;
//}
}