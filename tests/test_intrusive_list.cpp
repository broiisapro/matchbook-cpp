#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "lob/intrusive_list.hpp"
#include "lob/pool.hpp"

namespace {

struct Node {
  std::uint32_t prev = lob::kNullIndex;
  std::uint32_t next = lob::kNullIndex;
  int value = 0;
};

using Pool = lob::ObjectPool<Node>;
using List = lob::IntrusiveList<Node, &Node::prev, &Node::next>;

std::vector<std::uint32_t> to_indices(List list) {
  std::vector<std::uint32_t> indices;
  for (std::uint32_t index = list.front(); index != List::kNull; index = list.next(index)) {
    indices.push_back(index);
  }
  return indices;
}

TEST(IntrusiveList, PushBackPreservesOrder) {
  Pool pool(4);
  std::uint32_t head = List::kNull;
  std::uint32_t tail = List::kNull;
  List list{pool, head, tail};

  EXPECT_TRUE(list.empty());
  const std::uint32_t first = pool.acquire();
  const std::uint32_t second = pool.acquire();
  const std::uint32_t third = pool.acquire();
  list.push_back(first);
  list.push_back(second);
  list.push_back(third);

  EXPECT_FALSE(list.empty());
  EXPECT_EQ(list.front(), first);
  EXPECT_EQ(list.back(), third);
  EXPECT_EQ(list.next(first), second);
  EXPECT_EQ(list.next(second), third);
  EXPECT_EQ(list.next(third), List::kNull);
  EXPECT_EQ(to_indices(list), (std::vector<std::uint32_t>{first, second, third}));
}

TEST(IntrusiveList, EraseHeadMiddleAndTail) {
  Pool pool(4);
  std::uint32_t head = List::kNull;
  std::uint32_t tail = List::kNull;
  List list{pool, head, tail};

  const std::uint32_t a = pool.acquire();
  const std::uint32_t b = pool.acquire();
  const std::uint32_t c = pool.acquire();
  const std::uint32_t d = pool.acquire();
  list.push_back(a);
  list.push_back(b);
  list.push_back(c);
  list.push_back(d);

  list.erase(c);
  EXPECT_EQ(to_indices(list), (std::vector<std::uint32_t>{a, b, d}));
  EXPECT_EQ(pool[c].prev, lob::kNullIndex);
  EXPECT_EQ(pool[c].next, lob::kNullIndex);

  list.erase(a);
  EXPECT_EQ(list.front(), b);
  EXPECT_EQ(to_indices(list), (std::vector<std::uint32_t>{b, d}));

  list.erase(d);
  EXPECT_EQ(list.back(), b);
  EXPECT_EQ(list.next(b), List::kNull);
  EXPECT_EQ(to_indices(list), (std::vector<std::uint32_t>{b}));

  list.erase(b);
  EXPECT_TRUE(list.empty());
  EXPECT_EQ(head, List::kNull);
  EXPECT_EQ(tail, List::kNull);
}

TEST(IntrusiveList, EraseOnlyElementLeavesListEmpty) {
  Pool pool(3);
  std::uint32_t head = List::kNull;
  std::uint32_t tail = List::kNull;
  List list{pool, head, tail};

  const std::uint32_t only = pool.acquire();
  list.push_back(only);
  EXPECT_FALSE(list.empty());

  list.erase(only);
  EXPECT_TRUE(list.empty());
  EXPECT_EQ(head, List::kNull);
  EXPECT_EQ(tail, List::kNull);
  EXPECT_EQ(pool[only].prev, lob::kNullIndex);
  EXPECT_EQ(pool[only].next, lob::kNullIndex);
}

TEST(IntrusiveList, RearmAfterEmptying) {
  Pool pool(2);
  std::uint32_t head = List::kNull;
  std::uint32_t tail = List::kNull;
  List list{pool, head, tail};

  const std::uint32_t first = pool.acquire();
  const std::uint32_t second = pool.acquire();
  list.push_back(first);
  list.erase(first);
  list.push_back(second);
  list.push_back(first);

  EXPECT_EQ(list.front(), second);
  EXPECT_EQ(list.back(), first);
  EXPECT_EQ(to_indices(list), (std::vector<std::uint32_t>{second, first}));
}

TEST(IntrusiveList, ValuesLiveInThePooledNode) {
  Pool pool(2);
  std::uint32_t head = List::kNull;
  std::uint32_t tail = List::kNull;
  List list{pool, head, tail};

  const std::uint32_t index = pool.acquire();
  pool[index].value = 42;
  list.push_back(index);
  EXPECT_EQ(pool[list.front()].value, 42);
}

}  // namespace
