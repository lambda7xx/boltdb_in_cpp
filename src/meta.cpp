//
// Created by c6s on 18-5-4.
//
extern "C" {
#include "fnv/fnv.h"
}
#include <cstring>
#include "meta.h"
#include "db.h"
namespace boltDB_CPP {
bool Meta::validate() {
  if (magic != MAGIC) {
    return false;
  }

  if (version != VERSION) {
    return false;
  }

  return !(checkSum != 0 && checkSum != sum64());
}
void Meta::write(Page *page) {
  if (rootBucketHeader.rootPageId >= totalPageNumber) {
    assert(false);
  }
  if (freeListPageNumber >= totalPageNumber) {
    assert(false);
  }

  page->pageId = txnId % 2;
  page->flag |= static_cast<uint32_t>(PageFlag::metaPageFlag);

  checkSum = 0;

  std::memcpy(page->metaPointer(), this, sizeof(Meta));
}
uint64_t Meta::sum64() {
  uint64_t result = 0;
  result = ::fnv_64a_buf(this, offsetof(Meta, checkSum), FNV1A_64_INIT);
  return result;
}
}  // namespace boltDB_CPP