//  Copyright (c) 2014, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.

#include <algorithm>
#include <iostream>
#include <map>
#include <string>
#include <vector>
#include "src/util/consistent_hash.h"
#include "src/util/testharness.h"
#include "src/util/testutil.h"

using std::string;
using std::vector;

namespace rocketspeed {

class ConsistentHashTest : public ::testing::Test { };

TEST_F(ConsistentHashTest, BasicAPI) {
  ConsistentHash<string, string> hash;
  ASSERT_EQ(hash.SlotCount(), 0);
  ASSERT_EQ(hash.VirtualSlotCount(), 0);
  ASSERT_EQ(hash.SlotRatio("foo"), 0.0);
  ASSERT_EQ(hash.SlotRatio("bar"), 0.0);

  hash.Add("foo", 10);
  ASSERT_EQ(hash.SlotCount(), 1);
  ASSERT_EQ(hash.VirtualSlotCount(), 10);
  ASSERT_EQ(hash.SlotRatio("foo"), 1.0);
  ASSERT_EQ(hash.SlotRatio("bar"), 0.0);
  ASSERT_EQ(hash.Get("anything"), "foo");

  hash.Add("bar", 20);
  ASSERT_EQ(hash.SlotCount(), 2);
  ASSERT_EQ(hash.VirtualSlotCount(), 30);
  ASSERT_LT(hash.SlotRatio("foo"), 1.0);
  ASSERT_GT(hash.SlotRatio("foo"), 0.0);
  ASSERT_LT(hash.SlotRatio("bar"), 1.0);
  ASSERT_GT(hash.SlotRatio("bar"), 0.0);

  hash.Remove("foo");
  ASSERT_EQ(hash.SlotCount(), 1);
  ASSERT_EQ(hash.VirtualSlotCount(), 20);
  ASSERT_EQ(hash.SlotRatio("foo"), 0.0);
  ASSERT_EQ(hash.SlotRatio("bar"), 1.0);
  ASSERT_EQ(hash.Get("anything"), "bar");

  hash.Remove("bar");
  ASSERT_EQ(hash.SlotCount(), 0);
  ASSERT_EQ(hash.VirtualSlotCount(), 0);
  ASSERT_EQ(hash.SlotRatio("foo"), 0.0);
  ASSERT_EQ(hash.SlotRatio("bar"), 0.0);
}

TEST_F(ConsistentHashTest, Distribution) {
  ConsistentHash<string, string> hash;
  string hosts[] = { "host1", "host2", "host3", "host4" };
  for (const string& host : hosts) {
    hash.Add(host, 100);
  }

  // All should be around 0.25 (evenly distributed)
  for (const string& host : hosts) {
    ASSERT_GT(hash.SlotRatio(host), 0.2);
    ASSERT_LT(hash.SlotRatio(host), 0.3);
  }
}

TEST_F(ConsistentHashTest, Weighting) {
  ConsistentHash<string, string> hash;
  hash.Add("foo", 100);
  hash.Add("bar", 1000);
  double ratio = hash.SlotRatio("bar") / hash.SlotRatio("foo");
  // should be about 10x, but not exact
  ASSERT_GT(ratio, 9.0);
  ASSERT_LT(ratio, 11.0);
}

TEST_F(ConsistentHashTest, SlotRatioTest) {
  ConsistentHash<size_t, string> hash;
  string hosts[] = { "host1", "host2", "host3", "host4" };
  int weights[] = { 100, 200, 300, 400 };
  for (int i = 0; i < 4; ++i) {
    hash.Add(hosts[i], weights[i]);
  }

  // Ensure that empirical tests give correct distribution
  int counts[] = { 0, 0, 0, 0 };
  const int num = 1000000;
  for (size_t key = 0; key < num; ++key) {
    const string& host = hash.Get(key);
    for (int h = 0; h < 4; ++h) {
      if (host == hosts[h]) {
        counts[h]++;
      }
    }
  }

  double expected[] = { 0.1, 0.2, 0.3, 0.4 };
  for (int h = 0; h < 4; ++h) {
    double actual = static_cast<double>(counts[h]) / num;
    ASSERT_GT(actual, expected[h] * 0.7);  // 30% tolerance
    ASSERT_LT(actual, expected[h] * 1.3);  // 30% tolerance
  }
}

TEST_F(ConsistentHashTest, Consistency) {
  ConsistentHash<size_t, string> hash;
  string myhost = "abcde";
  do {
    // Add all permutations of abcde as hosts (1*2*3*4*5 == 120)
    hash.Add(myhost);
  } while (std::next_permutation(myhost.begin(), myhost.end()));

  // Now check that adding a new host doesn't change the mappings too much.
  std::map<size_t, string> original;
  size_t num = 1000;
  for (size_t key = 0; key < num; ++key) {
    original[key] = hash.Get(key);
  }

  // Try adding a few new hosts and check mapping
  string newHosts[] = { "host1", "host2", "host3" };
  for (const string& host : newHosts) {
    hash.Add(host);
    uint64_t changed = 0;
    for (size_t key = 0; key < num; ++key) {
      if (hash.Get(key) != original[key]) {
        changed++;
      }
    }
    uint64_t expected = num / hash.SlotCount();
    ASSERT_LT(changed, expected * 2);
    ASSERT_GT(changed, expected / 2);
    hash.Remove(host);
  }

  // Now do the same, but remove some hosts
  string removeHosts[] = { "abcde", "bcdea", "cdeab" };
  for (const string& host : removeHosts) {
    hash.Remove(host);
    // Check mapping
    int changed = 0;
    for (size_t key = 0; key < num; ++key) {
      if (hash.Get(key) != original[key]) {
        changed++;
      }
    }
    ASSERT_LT(changed, 50);
    hash.Add(host);
  }
}

TEST_F(ConsistentHashTest, Collisions) {
  typedef std::pair<size_t, size_t> Value;
  typedef std::function<size_t(Value)> HashFunc;
  HashFunc first_func = [](Value val) { return val.first; };

  ConsistentHash<Value, Value, HashFunc, HashFunc> hash(first_func, first_func);

  hash.Add(Value(42, 1), 1);
  hash.Add(Value(42, 2), 1);
  hash.Add(Value(42, 3), 1);
  hash.Add(Value(256, 20), 1);
  hash.Add(Value(256, 10), 1);

  // can't use ASSERT_EQ because it needs an operator<<(ostream&, value)
  ASSERT_TRUE(hash.Get(Value(10, 0)) == Value(42, 1));
  ASSERT_TRUE(hash.Get(Value(42, 0)) == Value(42, 1));
  ASSERT_TRUE(hash.Get(Value(100, 0)) == Value(256, 20));
  ASSERT_TRUE(hash.Get(Value(1000, 0)) == Value(42, 1));

  hash.Remove(Value(42, 2));

  ASSERT_TRUE(hash.Get(Value(10, 0)) == Value(42, 1));
  ASSERT_TRUE(hash.Get(Value(42, 0)) == Value(42, 1));
  ASSERT_TRUE(hash.Get(Value(100, 0)) == Value(256, 20));
  ASSERT_TRUE(hash.Get(Value(1000, 0)) == Value(42, 1));

  hash.Remove(Value(42, 1));

  ASSERT_TRUE(hash.Get(Value(10, 0)) == Value(42, 3));
  ASSERT_TRUE(hash.Get(Value(42, 0)) == Value(42, 3));
  ASSERT_TRUE(hash.Get(Value(100, 0)) == Value(256, 20));
  ASSERT_TRUE(hash.Get(Value(1000, 0)) == Value(42, 3));

  hash.Remove(Value(42, 1));
  hash.Remove(Value(42, 3));
  hash.Remove(Value(256, 20));

  ASSERT_TRUE(hash.Get(Value(10, 0)) == Value(256, 10));
  ASSERT_TRUE(hash.Get(Value(42, 0)) == Value(256, 10));
  ASSERT_TRUE(hash.Get(Value(100, 0)) == Value(256, 10));
  ASSERT_TRUE(hash.Get(Value(1000, 0)) == Value(256, 10));
}

TEST_F(ConsistentHashTest, Multiget) {
  ConsistentHash<size_t, string> hash;
  string myhost = "abcde";
  do {
    // Add all permutations of abcde as hosts (1*2*3*4*5 == 120)
    hash.Add(myhost);
  } while (std::next_permutation(myhost.begin(), myhost.end()));

  const int max_count = 5;
  const size_t num_keys = 1000;

  // Check that multiget(key, count) is a prefix of multiget(key, count+1)
  // and that get(key) == multiget(key, 1).
  std::map<size_t, vector<string>> original;
  for (size_t key = 0; key < num_keys; ++key) {
    vector<string> all(max_count);
    hash.MultiGet(key, max_count, all.begin());
    original[key] = all;

    ASSERT_TRUE(hash.Get(key) == all[0]);

    vector<string> prefix(max_count - 1);
    for (size_t count = 1; count < max_count; ++count) {
      hash.MultiGet(key, count, prefix.begin());
      ASSERT_TRUE(std::equal(prefix.begin(), prefix.begin() + count,
                             all.begin()));
    }
  }

  // Check that adding a new host doesn't move keys between other hosts
  // and preserves order.
  string newHosts[] = { "host1", "host2", "host3" };
  for (const string& host : newHosts) {
    hash.Add(host);
    vector<string> current(max_count);
    for (size_t key = 0; key < num_keys; ++key) {
      const vector<string>& old = original[key];

      hash.MultiGet(key, max_count, current.begin());
      auto end = std::remove(current.begin(), current.end(), host);

      ASSERT_LE(current.end() - end, 1);
      ASSERT_TRUE(std::equal(current.begin(), end, old.begin()));
    }
    hash.Remove(host);
  }

  // Now do the same, but remove some hosts
  string removeHosts[] = { "abcde", "bcdea", "cdeab" };
  for (const string& host : removeHosts) {
    hash.Remove(host);
    vector<string> current(max_count);
    for (size_t key = 0; key < num_keys; ++key) {
      vector<string> old = original[key];
      auto end = std::remove(old.begin(), old.end(), host);

      hash.MultiGet(key, max_count, current.begin());

      ASSERT_LE(old.end() - end, 1);
      ASSERT_TRUE(std::equal(old.begin(), end, current.begin()));
    }
    hash.Add(host);
  }
}

}  // namespace rocketspeed

int main(int argc, char** argv) {
  return rocketspeed::test::RunAllTests(argc, argv);
}
