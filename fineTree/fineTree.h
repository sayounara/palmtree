//
// Created by Zrs_y on 5/5/16.
//

#ifndef FINETREE_FINETREE_H
#define FINETREE_FINETREE_H

#include <boost/thread/locks.hpp>
#include <boost/thread/shared_mutex.hpp>
#include <glog/logging.h>
#include "immintrin.h"

#define UNUSED __attribute__((unused))

enum NodeType {
  INNERNODE = 0,
  LEAFNODE
};

static std::atomic<int> NODE_NUM(0);

template <typename KeyType,
  typename ValueType,
  typename PairType = std::pair<KeyType, ValueType>,
  typename KeyComparator = std::less<KeyType> >
class fineTree {

public:
  fineTree(KeyType min_key) {
    this->min_key = min_key;
    root = new InnerNode(nullptr, 1);
    auto child = new LeafNode(root, 0);

    add_item_inner(root, min_key, child);

  }

  int search(KeyType UNUSED key, ValueType &res) {
    auto ptr = (InnerNode *)root;
    ptr->lock_shared();
    for (;;) {
      CHECK(ptr->slot_used > 0) << "Search empty inner node";
      auto idx = this->search_inner(ptr->keys, ptr->slot_used, key);
      CHECK(idx != -1) << "search innerNode fail";
      CHECK(key_less(ptr->keys[idx], key) || key_eq(ptr->keys[idx], key));
      if(idx + 1 < ptr->slot_used) {
        CHECK(key_less(key, ptr->keys[idx + 1]));
      }
      auto child = ptr->values[idx];
      child->lock_shared();
      ptr->unlock_shared();
      if (child->type() == LEAFNODE) {
        auto leaf = (LeafNode *)child;
        idx = search_leaf(leaf->keys, leaf->slot_used, key);
        if (idx < 0) {
          child->unlock_shared();
          return -1;
        }else{
          res = leaf->values[idx];
          child->unlock_shared();
          return 0;
        }
      } else {
        ptr = (InnerNode *)child;
      }
    }


    return 0;
  }

  void insert(KeyType UNUSED key, ValueType UNUSED val) {

  }

private:
  KeyType min_key;
  // Max number of slots per inner node
  static const int INNER_MAX_SLOT = 256;
  // Max number of slots per leaf node
  static const int LEAF_MAX_SLOT = 64;

  struct Node {
    // Number of actually used slots
    int slot_used;
    int id;
    int level;
    KeyType lower_bound;
    Node *parent;


    Node() = delete;
    Node(Node *p, int lvl): slot_used(0), level(lvl), parent(p) {
      id = NODE_NUM++;
    };

    void lock_shared() {
      lock.lock_shared();
    }

    void unlock_shared() {
      lock.unlock_shared();
    }

    boost::shared_mutex lock;
    virtual ~Node() {};
    virtual std::string to_string() = 0;
    virtual NodeType type() const = 0;
    virtual bool is_few() = 0;
  };

  struct InnerNode : public Node {
    InnerNode() = delete;
    InnerNode(Node *parent, int level): Node(parent, level){};
    virtual ~InnerNode() {};
    // Keys for values
    KeyType keys[LEAF_MAX_SLOT];
    // Pointers for child nodes
    Node *values[LEAF_MAX_SLOT];

    virtual NodeType type() const {
      return INNERNODE;
    }

    virtual std::string to_string() {
      std::string res;
      res += "InnerNode[" + std::to_string(Node::id) + " @ " + std::to_string(Node::level) + "] ";
      // res += std::to_string(Node::slot_used);
      for (int i = 0 ; i < Node::slot_used ; i++) {
        res += " " + std::to_string(keys[i]) + ":" + std::to_string(values[i]->id);
      }
      return res;
    }

    inline bool is_full() const {
      return Node::slot_used == MAX_SLOT();
    }


    inline size_t MAX_SLOT() const {
      return LEAF_MAX_SLOT;
    }

    virtual inline bool is_few() {
      return Node::slot_used < MAX_SLOT()/4 || Node::slot_used == 0;
    }

  };

  struct LeafNode : public Node {
    LeafNode() = delete;
    LeafNode(Node *parent, int level): Node(parent, level){};
    virtual ~LeafNode() {};

    // Keys and values for leaf node
    KeyType keys[INNER_MAX_SLOT];
    ValueType values[INNER_MAX_SLOT];

    virtual NodeType type() const {
      return LEAFNODE;
    }

    virtual std::string to_string() {
      std::string res;
      res += "LeafNode[" + std::to_string(Node::id) + " @ " + std::to_string(Node::level) + "] ";

      for (int i = 0 ; i < Node::slot_used ; i++) {
        res += " " + std::to_string(keys[i]) + ":" + std::to_string(values[i]);
      }
      return res;
    }

    inline bool is_full() const {
      return Node::slot_used == MAX_SLOT();
    }

    inline size_t MAX_SLOT() const {
      return INNER_MAX_SLOT;
    }

    virtual inline bool is_few() {
      return Node::slot_used < MAX_SLOT()/4 || Node::slot_used == 0;
    }
  };

  // Return true if k1 < k2
  inline bool key_less(const KeyType &k1, const KeyType &k2) {
    return k1 < k2;
  }
  // Return true if k1 == k2
  inline bool key_eq(const KeyType &k1, const KeyType &k2) {
    return k1 == k2;
  }

  // Return the index of the largest slot whose key <= @target
  // assume there is no duplicated element
  int search_inner(const KeyType *input, int size, const KeyType &target) {
    // auto bt = CycleTimer::currentTicks();
    int low = 0, high = size - 1;
    while (low != high) {
      int mid = (low + high) / 2 + 1;
      if (key_less(target, input[mid])) {
        // target < input[mid]
        high = mid - 1;
      }
      else {
        // target >= input[mid];
        low = mid;
      }
    }
    // STAT.add_stat(0, "search_inner", CycleTimer::currentTicks() - bt);

    if (low == size) {
      return -1;
    }
    return low;
  }

  int search_leaf(const KeyType *data, int size, const KeyType &target) {
    // auto bt = CycleTimer::currentTicks();
    const __m256i keys = _mm256_set1_epi32(target);

    const auto n = size;
    const auto rounded = 8 * (n/8);

    for (int i=0; i < rounded; i += 8) {

      const __m256i vec1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(&data[i]));

      const __m256i cmp1 = _mm256_cmpeq_epi32(vec1, keys);

      const uint32_t mask = _mm256_movemask_epi8(cmp1);

      if (mask != 0) {
        // STAT.add_stat(0, "search_leaf", CycleTimer::currentTicks() - bt);
        return i + __builtin_ctz(mask)/4;
      }
    }

    for (int i = rounded; i < n; i++) {
      if (data[i] == target) {
        // STAT.add_stat(0, "search_leaf", CycleTimer::currentTicks() - bt);
        return i;
      }
    }

    // STAT.add_stat(0, "search_leaf", CycleTimer::currentTicks() - bt);
    return -1;
  }



  void add_item_leaf(LeafNode *node, KeyType key, ValueType value) {
    auto idx = node->slot_used++;
    node->keys[idx] = key;
    node->values[idx] = value;
    return;
  }

  void add_item_inner(InnerNode *node, KeyType key, Node *value) {
    // add item to inner node
    // ensure it's order
    DLOG(INFO) << "search inner begin";
    auto idx = search_inner(node->keys, node->slot_used, key);

    CHECK(idx != -1) << "search innerNode fail" << key <<" " <<node->keys[0];
    CHECK(key_less(node->keys[idx], key) || key_eq(node->keys[idx], key));
    if(idx + 1 < node->slot_used) {
      CHECK(key_less(key, node->keys[idx + 1])) << "search inner fail";
    }

    DLOG(INFO) << "search inner end";
    auto k = key;
    auto v = value;

    for(int i = idx + 1; i < node->slot_used; i++) {
      std::swap(node->keys[i], k);
      std::swap(node->values[i], v);
    }

    node->keys[node->slot_used] = k;
    node->values[node->slot_used] = v;
    node->slot_used++;
  }

  InnerNode *root;

};


#endif //FINETREE_FINETREE_H
