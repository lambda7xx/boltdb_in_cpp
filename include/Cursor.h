//
// Created by c6s on 18-4-27.
//

#ifndef BOLTDB_IN_CPP_CURSOR_H
#define BOLTDB_IN_CPP_CURSOR_H

#include <cstdint>
#include <vector>
#include <stack>
#include "boltDB_types.h"
namespace boltDB_CPP {
class Page;
class Node;
class Bucket;
//reference to an element on a given page/node
struct ElementRef {
  Page *page = nullptr;
  std::shared_ptr<Node> node = nullptr;
  uint64_t index = 0;//DO NOT change this default ctor build up a ref to the first element in a page
  //is this a leaf page/node
  bool isLeaf() const;

  //return the number of inodes or page elements
  size_t count() const;

  ElementRef(Page *page_p, std::shared_ptr<Node> node_p) : page(page_p), node(node_p) {}
};

struct Cursor {
  Bucket *bucket = nullptr;
  std::stack<ElementRef> stk;

  Cursor() = default;
  explicit Cursor(Bucket *bucket1) : bucket(bucket1) {}

  Bucket *getBucket() const {
    return bucket;
  }
  void search(const Item &key, page_id pageId);
  //search leaf node (which is on the top of the stack) for a Key
  void searchLeaf(const Item &key);
  void searchBranchNode(const Item &key, std::shared_ptr<Node> node);
  void searchBranchPage(const Item &key, Page *page);
  void keyValue(Item &key, Item &value, uint32_t &flag);

  //weird function signature
  //return kv of the search Key if searchkey exists
  //or return the next Key
  void do_seek(Item searchKey, Item &key, Item &value, uint32_t &flag);
  void seek(const Item &searchKey, Item &key, Item &value, uint32_t &flag);

  //return the node the cursor is currently on
  std::shared_ptr<Node>getNode() const;

  void do_next(Item &key, Item &value, uint32_t &flag);

  void do_first();
  void do_last();
  int remove();
  void prev(Item &key, Item &value);
  void next(Item &key, Item &value);
  void last(Item &key, Item &value);
  void first(Item &key, Item &value);
};

}

#endif //BOLTDB_IN_CPP_CURSOR_H
