//
// Created by c6s on 18-4-27.
//

#include <cassert>
#include <iostream>
#include <algorithm>
#include "Node.h"
#include "utility.h"
#include "Cursor.h"
#include "Database.h"

namespace boltDB_CPP {

bool ElementRef::isLeaf() const {
  if (node != nullptr) {
    return node->isLeaf;
  }
  assert(page);
  return (page->flag & static_cast<uint16_t >( PageFlag::leafPageFlag)) != 0;
}

size_t ElementRef::count() const {
  if (node != nullptr) {
    return node->inodeList.size();
  }
  assert(page);
  return page->count;
}

void Cursor::keyValue(Item &key, Item &value, uint32_t &flag) {
  assert(!stk.empty());
  auto ref = stk.top();
  if (ref.count() == 0 || ref.index >= ref.count()) {
    std::cerr << "get Key/value from empty bucket / index out of range" << std::endl;
    return;
  }

  //are those values sitting a node?
  if (ref.node) {
    auto inode = ref.node->inodeList[ref.index];
    key = inode->Key();
    value = inode->Value();
    flag = inode->flag;
    return;
  }

  //let's get them from page
  auto ret = ref.page->getLeafPageElement(ref.index);
  key = ret->Key();
  value = ret->Value();
  flag = ret->flag;
  return;
}

void Cursor::search(const Item &key, page_id pageId) {
  std::shared_ptr<Node> node;
  Page *page = nullptr;
  bucket->getPageNode(pageId, node, page);
  if (page && (page->getFlag()
      & (static_cast<uint16_t >(PageFlag::branchPageFlag) | static_cast<uint16_t >(PageFlag::leafPageFlag)))) {
    assert(false);
  }
  ElementRef ref{page, node};
  stk.push(ref);
  if (ref.isLeaf()) {
    searchLeaf(key);
    return;
  }

  if (node) {
    searchBranchNode(key, node);
    return;
  }
  searchBranchPage(key, page);
}

void Cursor::searchLeaf(const Item &key) {
  assert(!stk.empty());
  ElementRef &ref = stk.top();

  bool found = false;
  if (ref.node) {
    //search through inodeList for a matching Key
    //inodelist should be sorted in ascending order
    ref.index =
        static_cast<uint32_t >(binary_search(ref.node->inodeList,
                                             key,
                                             cmp_wrapper<Inode *>,
                                             ref.node->inodeList.size(),
                                             found
        ));
    return;
  }

  auto ptr = ref.page->getLeafPageElement(0);
  ref.index = static_cast<uint32_t >(binary_search(ptr,
                                                   key,
                                                   cmp_wrapper<LeafPageElement>,
                                                   ref.page->count,
                                                   found
  ));
}
void Cursor::searchBranchNode(const Item &key, std::shared_ptr<Node> node) {
  bool found = false;
  auto index = binary_search(node->inodeList, key, cmp_wrapper<Inode *>, node->inodeList.size(), found);
  if (!found && index > 0) {
    index--;
  }
  assert(!stk.empty());
  stk.top().index = index;
  search(key, node->inodeList[index]->pageId);
}
void Cursor::searchBranchPage(const Item &key, Page *page) {
  auto branchElements = page->getBranchPageElement(0);
  bool found = false;
  auto index = binary_search(branchElements, key, cmp_wrapper<BranchPageElement>, page->count, found);
  if (!found && index > 0) {
    index--;
  }
  assert(!stk.empty());
  stk.top().index = index;
  search(key, branchElements[index].pageId);
}
void Cursor::do_seek(Item searchKey, Item &key, Item &value, uint32_t &flag) {
  {
    decltype(stk) tmp;
    swap(stk, tmp);
  }
  search(searchKey, bucket->getRoot());

  auto &ref = stk.top();
  if (ref.index >= ref.count()) {
    key.reset();
    value.reset();
    flag = 0;
    return;
  }
  keyValue(key, value, flag);
}

/**
 * refactory this after main components are implemented
 * @return
 */
std::shared_ptr<Node> Cursor::getNode() const {
  if (!stk.empty() && stk.top().node && stk.top().isLeaf()) {
    stk.top().node;
  }

  std::stack<ElementRef> stk_cpy = stk;
  std::vector<ElementRef> v;
  while (!stk_cpy.empty()) {
    v.push_back(stk_cpy.top());
    stk_cpy.pop();
  }
  std::reverse(v.begin(), v.end());

  assert(!v.empty());
  std::shared_ptr<Node> node = v[0].node;
  if (node == nullptr) {
    node = bucket->getNode(v[0].page->pageId, nullptr);
  }

  for (size_t i = 0; i + 1 < v.size(); i++) {
    assert(!node->isLeaf);
    node = node->childAt(stk.top().index);
  }

  assert(node->isLeaf);
  return node;
}
void Cursor::do_next(Item &key, Item &value, uint32_t &flag) {
  while (true) {
    while (!stk.empty()) {
      auto &ref = stk.top();
      //not the last element
      if (ref.index < ref.count() - 1) {
        ref.index++;
        break;
      }
      stk.pop();
    }

    if (stk.empty()) {
      key.reset();
      value.reset();
      flag = 0;
      return;
    }

    do_first();
    //not sure what this intends to do
    if (stk.top().count() == 0) {
      continue;
    }

    keyValue(key, value, flag);
    return;
  }
}

//get to first leaf element under the last page in the stack
void Cursor::do_first() {
  while (true) {
    assert(!stk.empty());
    if (stk.top().isLeaf()) {
      break;
    }

    auto &ref = stk.top();
    page_id pageId = 0;
    if (ref.node != nullptr) {
      pageId = ref.node->inodeList[ref.index]->pageId;
    } else {
      pageId = ref.page->getBranchPageElement(ref.index)->pageId;
    }

    Page *page = nullptr;
    std::shared_ptr<Node> node;
    bucket->getPageNode(pageId, node, page);
    ElementRef element(page, node);
    stk.push(element);
  }
}
void Cursor::do_last() {
  while (true) {
    auto &ref = stk.top();
    if (ref.isLeaf()) {
      break;
    }

    page_id pageId = 0;
    if (ref.node != nullptr) {
      pageId = ref.node->inodeList[ref.index]->pageId;
    } else {
      pageId = ref.page->getBranchPageElement(ref.index)->pageId;
    }

    Page *page = nullptr;
    std::shared_ptr<Node> node = nullptr;
    bucket->getPageNode(pageId, node, page);
    ElementRef element(page, node);
    element.index = element.count() - 1;
    stk.push(element);
  }
}
int Cursor::remove() {
  if (bucket->getTransaction()->db == nullptr) {
    std::cerr << "db closed" << std::endl;
    return -1;
  }

  if (!bucket->isWritable()) {
    std::cerr << "txn not writable" << std::endl;
    return -1;
  }

  Item key;
  Item value;
  uint32_t flag;
  keyValue(key, value, flag);

  if (flag & static_cast<uint32_t>(PageFlag::bucketLeafFlag)) {
    std::cerr << "current value is a bucket| try removing a branch bucket other than kv in leaf node" << std::endl;
    return -1;
  }

  getNode()->do_remove(key);
  return 0;
}
void Cursor::seek(const Item &searchKey, Item &key, Item &value, uint32_t &flag) {
  key.reset();
  value.reset();
  flag = 0;
  do_seek(searchKey, key, value, flag);
  auto &ref = stk.top();
  if (ref.index >= ref.count()) {
    do_next(key, value, flag);
  }
}
void Cursor::prev(Item &key, Item &value) {
  key.reset();
  value.reset();
  while (!stk.empty()) {
    auto &ref = stk.top();
    if (ref.index > 0) {
      ref.index--;
      break;
    }
    stk.pop();
  }

  if (stk.empty()) {
    return;
  }

  do_last();
  uint32_t flag = 0;
  keyValue(key, value, flag);
  //I think there's no need to clear value if current node is a branch node

}
void Cursor::next(Item &key, Item &value) {
  key.reset();
  value.reset();
  uint32_t flag = 0;
  do_next(key, value, flag);
}
void Cursor::last(Item &key, Item &value) {
  key.reset();
  value.reset();
  {
    decltype(stk) tmp;
    swap(stk, tmp);
  }
  Page *page = nullptr;
  std::shared_ptr<Node> node = nullptr;
  bucket->getPageNode(bucket->getRoot(), node, page);
  ElementRef element{page, node};
  element.index = element.count() - 1;
  stk.push(element);
  do_last();
  uint32_t flag = 0;
  keyValue(key, value, flag);
}
void Cursor::first(Item &key, Item &value) {
  key.reset();
  value.reset();
  {
    decltype(stk) tmp;
    swap(stk, tmp);
  }
  Page *page = nullptr;
  std::shared_ptr<Node> node = nullptr;
  bucket->getPageNode(bucket->getRoot(), node, page);
  ElementRef element{page, node};

  stk.push(element);
  do_first();

  uint32_t flag = 0;
  //what does this do?
  if (stk.top().count() == 0) {
    do_next(key, value, flag);
  }

  keyValue(key, value, flag);
}
}
