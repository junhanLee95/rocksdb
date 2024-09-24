//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#include <string>
#include <vector>
#include <algorithm>
#include <utility>
#include <iostream>

#include "db/memtable_list.h"
#include "db/compaction_iterator.h"
#include "db/db_iter.h"
#include "db/dbformat.h"
#include "rocksdb/comparator.h"
#include "rocksdb/options.h"
#include "rocksdb/perf_context.h"
#include "rocksdb/slice.h"
#include "rocksdb/statistics.h"
#include "table/iterator_wrapper.h"
#include "table/merging_iterator.h"
#include "util/string_util.h"
#include "util/testutil.h"
#include "util/sync_point.h"
#include "util/testharness.h"
#include "utilities/merge_operators.h"


namespace rocksdb {
/*
static uint64_t TestGetTickerCount(const Options& options,
                                   Tickers ticker_type) {
  return options.statistics->getTickerCount(ticker_type);
}*/

class TestIterator : public InternalIterator {
 public:
  explicit TestIterator(const Comparator* comparator)
      : initialized_(false),
        valid_(false),
        sequence_number_(0),
        iter_(0),
        cmp(comparator) {
    data_.reserve(16);
  }

  void AddPut(std::string argkey, std::string argvalue) {
    Add(argkey, kTypeValue, argvalue);
  }

  void AddDeletion(std::string argkey) {
    Add(argkey, kTypeDeletion, std::string());
  }

  void AddSingleDeletion(std::string argkey) {
    Add(argkey, kTypeSingleDeletion, std::string());
  }

  void AddMerge(std::string argkey, std::string argvalue) {
    Add(argkey, kTypeMerge, argvalue);
  }

  void Add(std::string argkey, ValueType type, std::string argvalue) {
    Add(argkey, type, argvalue, sequence_number_++);
  }

  void Add(std::string argkey, ValueType type, std::string argvalue,
           size_t seq_num, bool update_iter = false) {
    valid_ = true;
    ParsedInternalKey internal_key(argkey, seq_num, type);
    data_.push_back(
        std::pair<std::string, std::string>(std::string(), argvalue));
    AppendInternalKey(&data_.back().first, internal_key);
    if (update_iter && valid_ && cmp.Compare(data_.back().first, key()) < 0) {
      // insert a key smaller than current key
      Finish();
      // data_[iter_] is not anymore the current element of the iterator.
      // Increment it to reposition it to the right position.
      iter_++;
    }
  }

  // should be called before operations with iterator
  void Finish() {
    initialized_ = true;
    std::sort(data_.begin(), data_.end(),
              [this](std::pair<std::string, std::string> a,
                     std::pair<std::string, std::string> b) {
      return (cmp.Compare(a.first, b.first) < 0);
    });
  }

  // Removes the key from the set of keys over which this iterator iterates.
  // Not to be confused with AddDeletion().
  // If the iterator is currently positioned on this key, the deletion will
  // apply next time the iterator moves.
  // Used for simulating ForwardIterator updating to a new version that doesn't
  // have some of the keys (e.g. after compaction with a filter).
  void Vanish(std::string _key) {
    if (valid_ && data_[iter_].first == _key) {
      delete_current_ = true;
      return;
    }
    for (auto it = data_.begin(); it != data_.end(); ++it) {
      ParsedInternalKey ikey;
      bool ok __attribute__((__unused__)) = ParseInternalKey(it->first, &ikey);
      assert(ok);
      if (ikey.user_key != _key) {
        continue;
      }
      if (valid_ && data_.begin() + iter_ > it) {
        --iter_;
      }
      data_.erase(it);
      return;
    }
    assert(false);
  }

  // Number of operations done on this iterator since construction.
  size_t steps() const { return steps_; }

  bool Valid() const override {
    assert(initialized_);
    return valid_;
  }

  void SeekToFirst() override {
    assert(initialized_);
    ++steps_;
    DeleteCurrentIfNeeded();
    valid_ = (data_.size() > 0);
    iter_ = 0;
  }

  void SeekToLast() override {
    assert(initialized_);
    ++steps_;
    DeleteCurrentIfNeeded();
    valid_ = (data_.size() > 0);
    iter_ = data_.size() - 1;
  }

  void Seek(const Slice& target) override {
    assert(initialized_);
    SeekToFirst();
    ++steps_;
    if (!valid_) {
      return;
    }
    while (iter_ < data_.size() &&
           (cmp.Compare(data_[iter_].first, target) < 0)) {
      ++iter_;
    }

    if (iter_ == data_.size()) {
      valid_ = false;
    }
  }

  void SeekForPrev(const Slice& target) override {
    assert(initialized_);
    DeleteCurrentIfNeeded();
    SeekForPrevImpl(target, &cmp);
  }

  void Next() override {
    assert(initialized_);
    assert(valid_);
    assert(iter_ < data_.size());

    ++steps_;
    if (delete_current_) {
      DeleteCurrentIfNeeded();
    } else {
      ++iter_;
    }
    valid_ = iter_ < data_.size();
  }

  void Prev() override {
    assert(initialized_);
    assert(valid_);
    assert(iter_ < data_.size());

    ++steps_;
    DeleteCurrentIfNeeded();
    if (iter_ == 0) {
      valid_ = false;
    } else {
      --iter_;
    }
  }

  Slice key() const override {
    assert(initialized_);
    return data_[iter_].first;
  }

  Slice value() const override {
    assert(initialized_);
    return data_[iter_].second;
  }

  Status status() const override {
    assert(initialized_);
    return Status::OK();
  }

  bool IsKeyPinned() const override { return true; }
  bool IsValuePinned() const override { return true; }

 private:
  bool initialized_;
  bool valid_;
  size_t sequence_number_;
  size_t iter_;
  size_t steps_ = 0;

  InternalKeyComparator cmp;
  std::vector<std::pair<std::string, std::string>> data_;
  bool delete_current_ = false;

  void DeleteCurrentIfNeeded() {
    if (!delete_current_) {
      return;
    }
    data_.erase(data_.begin() + iter_);
    delete_current_ = false;
  }
};

class DBIteratorTest : public testing::Test {
 public:
  Env* env_;

  DBIteratorTest() : env_(Env::Default()) {}
};
/*
TEST_F(DBIteratorTest, DBIteratorPrevNext) {
  Options options;
  ImmutableCFOptions cf_options = ImmutableCFOptions(options);
  MutableCFOptions mutable_cf_options = MutableCFOptions(options);
  {
    TestIterator* internal_iter = new TestIterator(BytewiseComparator());
    internal_iter->AddDeletion("a");
    internal_iter->AddDeletion("a");
    internal_iter->AddDeletion("a");
    internal_iter->AddDeletion("a");
    internal_iter->AddPut("a", "val_a");

    internal_iter->AddPut("b", "val_b");
    internal_iter->Finish();

    ReadOptions ro;
    std::unique_ptr<Iterator> db_iter(NewDBIterator(
        env_, ro, cf_options, mutable_cf_options, BytewiseComparator(),
        internal_iter, 10, options.max_sequential_skip_in_iterations,
        nullptr ));

    db_iter->SeekToLast();
    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(db_iter->key().ToString(), "b");
    ASSERT_EQ(db_iter->value().ToString(), "val_b");

    db_iter->Prev();
    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(db_iter->key().ToString(), "a");
    ASSERT_EQ(db_iter->value().ToString(), "val_a");

    db_iter->Next();
    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(db_iter->key().ToString(), "b");
    ASSERT_EQ(db_iter->value().ToString(), "val_b");

    db_iter->Next();
    ASSERT_TRUE(!db_iter->Valid());
  }
  // Test to check the SeekToLast() with iterate_upper_bound not set
  {
    TestIterator* internal_iter = new TestIterator(BytewiseComparator());
    internal_iter->AddPut("a", "val_a");
    internal_iter->AddPut("b", "val_b");
    internal_iter->AddPut("b", "val_b");
    internal_iter->AddPut("c", "val_c");
    internal_iter->Finish();

    ReadOptions ro;
    std::unique_ptr<Iterator> db_iter(NewDBIterator(
        env_, ro, cf_options, mutable_cf_options, BytewiseComparator(),
        internal_iter, 10, options.max_sequential_skip_in_iterations,
        nullptr ));

    db_iter->SeekToLast();
    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(db_iter->key().ToString(), "c");
  }

  // Test to check the SeekToLast() with iterate_upper_bound set
  {
    TestIterator* internal_iter = new TestIterator(BytewiseComparator());

    internal_iter->AddPut("a", "val_a");
    internal_iter->AddPut("b", "val_b");
    internal_iter->AddPut("c", "val_c");
    internal_iter->AddPut("d", "val_d");
    internal_iter->AddPut("e", "val_e");
    internal_iter->AddPut("f", "val_f");
    internal_iter->Finish();

    Slice prefix("d");

    ReadOptions ro;
    ro.iterate_upper_bound = &prefix;

    std::unique_ptr<Iterator> db_iter(NewDBIterator(
        env_, ro, cf_options, mutable_cf_options, BytewiseComparator(),
        internal_iter, 10, options.max_sequential_skip_in_iterations,
        nullptr ));

    db_iter->SeekToLast();
    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(db_iter->key().ToString(), "c");

    db_iter->Next();
    ASSERT_TRUE(!db_iter->Valid());

    db_iter->SeekToLast();
    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(db_iter->key().ToString(), "c");
  }
  // Test to check the SeekToLast() iterate_upper_bound set to a key that
  // is not Put yet
  {
    TestIterator* internal_iter = new TestIterator(BytewiseComparator());

    internal_iter->AddPut("a", "val_a");
    internal_iter->AddPut("a", "val_a");
    internal_iter->AddPut("b", "val_b");
    internal_iter->AddPut("c", "val_c");
    internal_iter->AddPut("d", "val_d");
    internal_iter->Finish();

    Slice prefix("z");

    ReadOptions ro;
    ro.iterate_upper_bound = &prefix;

    std::unique_ptr<Iterator> db_iter(NewDBIterator(
        env_, ro, cf_options, mutable_cf_options, BytewiseComparator(),
        internal_iter, 10, options.max_sequential_skip_in_iterations,
        nullptr ));

    db_iter->SeekToLast();
    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(db_iter->key().ToString(), "d");

    db_iter->Next();
    ASSERT_TRUE(!db_iter->Valid());

    db_iter->SeekToLast();
    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(db_iter->key().ToString(), "d");

    db_iter->Prev();
    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(db_iter->key().ToString(), "c");
  }
  // Test to check the SeekToLast() with iterate_upper_bound set to the
  // first key
  {
    TestIterator* internal_iter = new TestIterator(BytewiseComparator());
    internal_iter->AddPut("a", "val_a");
    internal_iter->AddPut("a", "val_a");
    internal_iter->AddPut("a", "val_a");
    internal_iter->AddPut("b", "val_b");
    internal_iter->AddPut("b", "val_b");
    internal_iter->Finish();

    Slice prefix("a");

    ReadOptions ro;
    ro.iterate_upper_bound = &prefix;

    std::unique_ptr<Iterator> db_iter(NewDBIterator(
        env_, ro, cf_options, mutable_cf_options, BytewiseComparator(),
        internal_iter, 10, options.max_sequential_skip_in_iterations,
        nullptr ));

    db_iter->SeekToLast();
    ASSERT_TRUE(!db_iter->Valid());
  }
  // Test case to check SeekToLast with iterate_upper_bound set
  // (same key put may times - SeekToLast should start with the
  // maximum sequence id of the upper bound)

  {
    TestIterator* internal_iter = new TestIterator(BytewiseComparator());
    internal_iter->AddPut("a", "val_a");
    internal_iter->AddPut("b", "val_b");
    internal_iter->AddPut("c", "val_c");
    internal_iter->AddPut("c", "val_c");
    internal_iter->AddPut("c", "val_c");
    internal_iter->AddPut("c", "val_c");
    internal_iter->AddPut("c", "val_c");
    internal_iter->AddPut("c", "val_c");
    internal_iter->AddPut("c", "val_c");
    internal_iter->Finish();

    Slice prefix("c");

    ReadOptions ro;
    ro.iterate_upper_bound = &prefix;

    std::unique_ptr<Iterator> db_iter(NewDBIterator(
        env_, ro, cf_options, mutable_cf_options, BytewiseComparator(),
        internal_iter, 7, options.max_sequential_skip_in_iterations,
        nullptr ));

    SetPerfLevel(kEnableCount);
    ASSERT_TRUE(GetPerfLevel() == kEnableCount);

    get_perf_context()->Reset();
    db_iter->SeekToLast();

    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(static_cast<int>(get_perf_context()->internal_key_skipped_count), 1);
    ASSERT_EQ(db_iter->key().ToString(), "b");

    SetPerfLevel(kDisable);
  }
  // Test to check the SeekToLast() with the iterate_upper_bound set
  // (Checking the value of the key which has sequence ids greater than
  // and less that the iterator's sequence id)
  {
    TestIterator* internal_iter = new TestIterator(BytewiseComparator());

    internal_iter->AddPut("a", "val_a1");
    internal_iter->AddPut("a", "val_a2");
    internal_iter->AddPut("b", "val_b1");
    internal_iter->AddPut("c", "val_c1");
    internal_iter->AddPut("c", "val_c2");
    internal_iter->AddPut("c", "val_c3");
    internal_iter->AddPut("b", "val_b2");
    internal_iter->AddPut("d", "val_d1");
    internal_iter->Finish();

    Slice prefix("c");

    ReadOptions ro;
    ro.iterate_upper_bound = &prefix;

    std::unique_ptr<Iterator> db_iter(NewDBIterator(
        env_, ro, cf_options, mutable_cf_options, BytewiseComparator(),
        internal_iter, 4, options.max_sequential_skip_in_iterations,
        nullptr ));

    db_iter->SeekToLast();
    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(db_iter->key().ToString(), "b");
    ASSERT_EQ(db_iter->value().ToString(), "val_b1");
  }

  // Test to check the SeekToLast() with the iterate_upper_bound set to the
  // key that is deleted
  {
    TestIterator* internal_iter = new TestIterator(BytewiseComparator());
    internal_iter->AddPut("a", "val_a");
    internal_iter->AddDeletion("a");
    internal_iter->AddPut("b", "val_b");
    internal_iter->AddPut("c", "val_c");
    internal_iter->Finish();

    Slice prefix("a");

    ReadOptions ro;
    ro.iterate_upper_bound = &prefix;

    std::unique_ptr<Iterator> db_iter(NewDBIterator(
        env_, ro, cf_options, mutable_cf_options, BytewiseComparator(),
        internal_iter, 10, options.max_sequential_skip_in_iterations,
        nullptr ));

    db_iter->SeekToLast();
    ASSERT_TRUE(!db_iter->Valid());
  }
  // Test to check the SeekToLast() with the iterate_upper_bound set
  // (Deletion cases)
  {
    TestIterator* internal_iter = new TestIterator(BytewiseComparator());
    internal_iter->AddPut("a", "val_a");
    internal_iter->AddPut("b", "val_b");
    internal_iter->AddDeletion("b");
    internal_iter->AddPut("c", "val_c");
    internal_iter->Finish();

    Slice prefix("c");

    ReadOptions ro;
    ro.iterate_upper_bound = &prefix;

    std::unique_ptr<Iterator> db_iter(NewDBIterator(
        env_, ro, cf_options, mutable_cf_options, BytewiseComparator(),
        internal_iter, 10, options.max_sequential_skip_in_iterations,
        nullptr ));

    db_iter->SeekToLast();
    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(db_iter->key().ToString(), "a");

    db_iter->Next();
    ASSERT_TRUE(!db_iter->Valid());

    db_iter->SeekToLast();
    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(db_iter->key().ToString(), "a");
  }
  // Test to check the SeekToLast() with iterate_upper_bound set
  // (Deletion cases - Lot of internal keys after the upper_bound
  // is deleted)
  {
    TestIterator* internal_iter = new TestIterator(BytewiseComparator());
    internal_iter->AddPut("a", "val_a");
    internal_iter->AddPut("b", "val_b");
    internal_iter->AddDeletion("c");
    internal_iter->AddDeletion("d");
    internal_iter->AddDeletion("e");
    internal_iter->AddDeletion("f");
    internal_iter->AddDeletion("g");
    internal_iter->AddDeletion("h");
    internal_iter->Finish();

    Slice prefix("c");

    ReadOptions ro;
    ro.iterate_upper_bound = &prefix;

    std::unique_ptr<Iterator> db_iter(NewDBIterator(
        env_, ro, cf_options, mutable_cf_options, BytewiseComparator(),
        internal_iter, 7, options.max_sequential_skip_in_iterations,
        nullptr ));

    SetPerfLevel(kEnableCount);
    ASSERT_TRUE(GetPerfLevel() == kEnableCount);

    get_perf_context()->Reset();
    db_iter->SeekToLast();

    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(static_cast<int>(get_perf_context()->internal_delete_skipped_count), 0);
    ASSERT_EQ(db_iter->key().ToString(), "b");

    SetPerfLevel(kDisable);
  }

  {
    TestIterator* internal_iter = new TestIterator(BytewiseComparator());
    internal_iter->AddDeletion("a");
    internal_iter->AddDeletion("a");
    internal_iter->AddDeletion("a");
    internal_iter->AddDeletion("a");
    internal_iter->AddPut("a", "val_a");

    internal_iter->AddPut("b", "val_b");
    internal_iter->Finish();

    ReadOptions ro;
    std::unique_ptr<Iterator> db_iter(NewDBIterator(
        env_, ro, cf_options, mutable_cf_options, BytewiseComparator(),
        internal_iter, 10, options.max_sequential_skip_in_iterations,
        nullptr ));

    db_iter->SeekToFirst();
    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(db_iter->key().ToString(), "a");
    ASSERT_EQ(db_iter->value().ToString(), "val_a");

    db_iter->Next();
    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(db_iter->key().ToString(), "b");
    ASSERT_EQ(db_iter->value().ToString(), "val_b");

    db_iter->Prev();
    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(db_iter->key().ToString(), "a");
    ASSERT_EQ(db_iter->value().ToString(), "val_a");

    db_iter->Prev();
    ASSERT_TRUE(!db_iter->Valid());
  }

  {
    TestIterator* internal_iter = new TestIterator(BytewiseComparator());
    internal_iter->AddPut("a", "val_a");
    internal_iter->AddPut("b", "val_b");

    internal_iter->AddPut("a", "val_a");
    internal_iter->AddPut("b", "val_b");

    internal_iter->AddPut("a", "val_a");
    internal_iter->AddPut("b", "val_b");

    internal_iter->AddPut("a", "val_a");
    internal_iter->AddPut("b", "val_b");

    internal_iter->AddPut("a", "val_a");
    internal_iter->AddPut("b", "val_b");
    internal_iter->Finish();

    ReadOptions ro;
    std::unique_ptr<Iterator> db_iter(NewDBIterator(
        env_, ro, cf_options, mutable_cf_options, BytewiseComparator(),
        internal_iter, 2, options.max_sequential_skip_in_iterations,
        nullptr ));
    db_iter->SeekToLast();
    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(db_iter->key().ToString(), "b");
    ASSERT_EQ(db_iter->value().ToString(), "val_b");

    db_iter->Next();
    ASSERT_TRUE(!db_iter->Valid());

    db_iter->SeekToLast();
    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(db_iter->key().ToString(), "b");
    ASSERT_EQ(db_iter->value().ToString(), "val_b");
  }

  {
    TestIterator* internal_iter = new TestIterator(BytewiseComparator());
    internal_iter->AddPut("a", "val_a");
    internal_iter->AddPut("a", "val_a");
    internal_iter->AddPut("a", "val_a");
    internal_iter->AddPut("a", "val_a");
    internal_iter->AddPut("a", "val_a");

    internal_iter->AddPut("b", "val_b");

    internal_iter->AddPut("c", "val_c");
    internal_iter->Finish();

    ReadOptions ro;
    std::unique_ptr<Iterator> db_iter(NewDBIterator(
        env_, ro, cf_options, mutable_cf_options, BytewiseComparator(),
        internal_iter, 10, options.max_sequential_skip_in_iterations,
        nullptr ));
    db_iter->SeekToLast();
    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(db_iter->key().ToString(), "c");
    ASSERT_EQ(db_iter->value().ToString(), "val_c");

    db_iter->Prev();
    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(db_iter->key().ToString(), "b");
    ASSERT_EQ(db_iter->value().ToString(), "val_b");

    db_iter->Next();
    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(db_iter->key().ToString(), "c");
    ASSERT_EQ(db_iter->value().ToString(), "val_c");
  }
}

TEST_F(DBIteratorTest, DBIteratorEmpty) {
  Options options;
  ImmutableCFOptions cf_options = ImmutableCFOptions(options);
  MutableCFOptions mutable_cf_options = MutableCFOptions(options);
  ReadOptions ro;

  {
    TestIterator* internal_iter = new TestIterator(BytewiseComparator());
    internal_iter->Finish();

    std::unique_ptr<Iterator> db_iter(NewDBIterator(
        env_, ro, cf_options, mutable_cf_options, BytewiseComparator(),
        internal_iter, 0, options.max_sequential_skip_in_iterations,
        nullptr ));
    db_iter->SeekToLast();
    ASSERT_TRUE(!db_iter->Valid());
  }

  {
    TestIterator* internal_iter = new TestIterator(BytewiseComparator());
    internal_iter->Finish();

    std::unique_ptr<Iterator> db_iter(NewDBIterator(
        env_, ro, cf_options, mutable_cf_options, BytewiseComparator(),
        internal_iter, 0, options.max_sequential_skip_in_iterations,
        nullptr ));
    db_iter->SeekToFirst();
    ASSERT_TRUE(!db_iter->Valid());
  }
}

TEST_F(DBIteratorTest, DBIteratorUseSkipCountSkips) {
  ReadOptions ro;
  Options options;
  options.statistics = rocksdb::CreateDBStatistics();
  options.merge_operator = MergeOperators::CreateFromStringId("stringappend");

  TestIterator* internal_iter = new TestIterator(BytewiseComparator());
  for (size_t i = 0; i < 200; ++i) {
    internal_iter->AddPut("a", "a");
    internal_iter->AddPut("b", "b");
    internal_iter->AddPut("c", "c");
  }
  internal_iter->Finish();

  std::unique_ptr<Iterator> db_iter(NewDBIterator(
      env_, ro, ImmutableCFOptions(options), MutableCFOptions(options),
      BytewiseComparator(), internal_iter, 2,
      options.max_sequential_skip_in_iterations, nullptr ));
  db_iter->SeekToLast();
  ASSERT_TRUE(db_iter->Valid());
  ASSERT_EQ(db_iter->key().ToString(), "c");
  ASSERT_EQ(db_iter->value().ToString(), "c");
  ASSERT_EQ(TestGetTickerCount(options, NUMBER_OF_RESEEKS_IN_ITERATION), 1u);

  db_iter->Prev();
  ASSERT_TRUE(db_iter->Valid());
  ASSERT_EQ(db_iter->key().ToString(), "b");
  ASSERT_EQ(db_iter->value().ToString(), "b");
  ASSERT_EQ(TestGetTickerCount(options, NUMBER_OF_RESEEKS_IN_ITERATION), 2u);

  db_iter->Prev();
  ASSERT_TRUE(db_iter->Valid());
  ASSERT_EQ(db_iter->key().ToString(), "a");
  ASSERT_EQ(db_iter->value().ToString(), "a");
  ASSERT_EQ(TestGetTickerCount(options, NUMBER_OF_RESEEKS_IN_ITERATION), 3u);

  db_iter->Prev();
  ASSERT_TRUE(!db_iter->Valid());
  ASSERT_EQ(TestGetTickerCount(options, NUMBER_OF_RESEEKS_IN_ITERATION), 3u);
}

TEST_F(DBIteratorTest, DBIteratorUseSkip) {
  ReadOptions ro;
  Options options;
  options.merge_operator = MergeOperators::CreateFromStringId("stringappend");
  ImmutableCFOptions cf_options = ImmutableCFOptions(options);
  MutableCFOptions mutable_cf_options = MutableCFOptions(options);

  {
    for (size_t i = 0; i < 200; ++i) {
      TestIterator* internal_iter = new TestIterator(BytewiseComparator());
      internal_iter->AddMerge("b", "merge_1");
      internal_iter->AddMerge("a", "merge_2");
      for (size_t k = 0; k < 200; ++k) {
        internal_iter->AddPut("c", ToString(k));
      }
      internal_iter->Finish();

      options.statistics = rocksdb::CreateDBStatistics();
      std::unique_ptr<Iterator> db_iter(NewDBIterator(
          env_, ro, cf_options, mutable_cf_options, BytewiseComparator(),
          internal_iter, i + 2, options.max_sequential_skip_in_iterations,
          nullptr ));
      db_iter->SeekToLast();
      ASSERT_TRUE(db_iter->Valid());

      ASSERT_EQ(db_iter->key().ToString(), "c");
      ASSERT_EQ(db_iter->value().ToString(), ToString(i));
      db_iter->Prev();
      ASSERT_TRUE(db_iter->Valid());

      ASSERT_EQ(db_iter->key().ToString(), "b");
      ASSERT_EQ(db_iter->value().ToString(), "merge_1");
      db_iter->Prev();
      ASSERT_TRUE(db_iter->Valid());

      ASSERT_EQ(db_iter->key().ToString(), "a");
      ASSERT_EQ(db_iter->value().ToString(), "merge_2");
      db_iter->Prev();

      ASSERT_TRUE(!db_iter->Valid());
    }
  }

  {
    for (size_t i = 0; i < 200; ++i) {
      TestIterator* internal_iter = new TestIterator(BytewiseComparator());
      internal_iter->AddMerge("b", "merge_1");
      internal_iter->AddMerge("a", "merge_2");
      for (size_t k = 0; k < 200; ++k) {
        internal_iter->AddDeletion("c");
      }
      internal_iter->AddPut("c", "200");
      internal_iter->Finish();

      std::unique_ptr<Iterator> db_iter(NewDBIterator(
          env_, ro, cf_options, mutable_cf_options, BytewiseComparator(),
          internal_iter, i + 2, options.max_sequential_skip_in_iterations,
          nullptr  ));
      db_iter->SeekToLast();
      ASSERT_TRUE(db_iter->Valid());

      ASSERT_EQ(db_iter->key().ToString(), "b");
      ASSERT_EQ(db_iter->value().ToString(), "merge_1");
      db_iter->Prev();
      ASSERT_TRUE(db_iter->Valid());

      ASSERT_EQ(db_iter->key().ToString(), "a");
      ASSERT_EQ(db_iter->value().ToString(), "merge_2");
      db_iter->Prev();

      ASSERT_TRUE(!db_iter->Valid());
    }

    {
      TestIterator* internal_iter = new TestIterator(BytewiseComparator());
      internal_iter->AddMerge("b", "merge_1");
      internal_iter->AddMerge("a", "merge_2");
      for (size_t i = 0; i < 200; ++i) {
        internal_iter->AddDeletion("c");
      }
      internal_iter->AddPut("c", "200");
      internal_iter->Finish();

      std::unique_ptr<Iterator> db_iter(NewDBIterator(
          env_, ro, cf_options, mutable_cf_options, BytewiseComparator(),
          internal_iter, 202, options.max_sequential_skip_in_iterations,
          nullptr  ));
      db_iter->SeekToLast();
      ASSERT_TRUE(db_iter->Valid());

      ASSERT_EQ(db_iter->key().ToString(), "c");
      ASSERT_EQ(db_iter->value().ToString(), "200");
      db_iter->Prev();
      ASSERT_TRUE(db_iter->Valid());

      ASSERT_EQ(db_iter->key().ToString(), "b");
      ASSERT_EQ(db_iter->value().ToString(), "merge_1");
      db_iter->Prev();
      ASSERT_TRUE(db_iter->Valid());

      ASSERT_EQ(db_iter->key().ToString(), "a");
      ASSERT_EQ(db_iter->value().ToString(), "merge_2");
      db_iter->Prev();

      ASSERT_TRUE(!db_iter->Valid());
    }
  }

  {
    for (size_t i = 0; i < 200; ++i) {
      TestIterator* internal_iter = new TestIterator(BytewiseComparator());
      for (size_t k = 0; k < 200; ++k) {
        internal_iter->AddDeletion("c");
      }
      internal_iter->AddPut("c", "200");
      internal_iter->Finish();
      std::unique_ptr<Iterator> db_iter(NewDBIterator(
          env_, ro, cf_options, mutable_cf_options, BytewiseComparator(),
          internal_iter, i, options.max_sequential_skip_in_iterations,
          nullptr  ));
      db_iter->SeekToLast();
      ASSERT_TRUE(!db_iter->Valid());

      db_iter->SeekToFirst();
      ASSERT_TRUE(!db_iter->Valid());
    }

    TestIterator* internal_iter = new TestIterator(BytewiseComparator());
    for (size_t i = 0; i < 200; ++i) {
      internal_iter->AddDeletion("c");
    }
    internal_iter->AddPut("c", "200");
    internal_iter->Finish();
    std::unique_ptr<Iterator> db_iter(NewDBIterator(
        env_, ro, cf_options, mutable_cf_options, BytewiseComparator(),
        internal_iter, 200, options.max_sequential_skip_in_iterations,
        nullptr  ));
    db_iter->SeekToLast();
    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(db_iter->key().ToString(), "c");
    ASSERT_EQ(db_iter->value().ToString(), "200");

    db_iter->Prev();
    ASSERT_TRUE(!db_iter->Valid());

    db_iter->SeekToFirst();
    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(db_iter->key().ToString(), "c");
    ASSERT_EQ(db_iter->value().ToString(), "200");

    db_iter->Next();
    ASSERT_TRUE(!db_iter->Valid());
  }

  {
    for (size_t i = 0; i < 200; ++i) {
      TestIterator* internal_iter = new TestIterator(BytewiseComparator());
      internal_iter->AddMerge("b", "merge_1");
      internal_iter->AddMerge("a", "merge_2");
      for (size_t k = 0; k < 200; ++k) {
        internal_iter->AddPut("d", ToString(k));
      }

      for (size_t k = 0; k < 200; ++k) {
        internal_iter->AddPut("c", ToString(k));
      }
      internal_iter->Finish();

      std::unique_ptr<Iterator> db_iter(NewDBIterator(
          env_, ro, cf_options, mutable_cf_options, BytewiseComparator(),
          internal_iter, i + 2, options.max_sequential_skip_in_iterations,
          nullptr  ));
      db_iter->SeekToLast();
      ASSERT_TRUE(db_iter->Valid());

      ASSERT_EQ(db_iter->key().ToString(), "d");
      ASSERT_EQ(db_iter->value().ToString(), ToString(i));
      db_iter->Prev();
      ASSERT_TRUE(db_iter->Valid());

      ASSERT_EQ(db_iter->key().ToString(), "b");
      ASSERT_EQ(db_iter->value().ToString(), "merge_1");
      db_iter->Prev();
      ASSERT_TRUE(db_iter->Valid());

      ASSERT_EQ(db_iter->key().ToString(), "a");
      ASSERT_EQ(db_iter->value().ToString(), "merge_2");
      db_iter->Prev();

      ASSERT_TRUE(!db_iter->Valid());
    }
  }

  {
    for (size_t i = 0; i < 200; ++i) {
      TestIterator* internal_iter = new TestIterator(BytewiseComparator());
      internal_iter->AddMerge("b", "b");
      internal_iter->AddMerge("a", "a");
      for (size_t k = 0; k < 200; ++k) {
        internal_iter->AddMerge("c", ToString(k));
      }
      internal_iter->Finish();

      std::unique_ptr<Iterator> db_iter(NewDBIterator(
          env_, ro, cf_options, mutable_cf_options, BytewiseComparator(),
          internal_iter, i + 2, options.max_sequential_skip_in_iterations,
          nullptr  ));
      db_iter->SeekToLast();
      ASSERT_TRUE(db_iter->Valid());

      ASSERT_EQ(db_iter->key().ToString(), "c");
      std::string merge_result = "0";
      for (size_t j = 1; j <= i; ++j) {
        merge_result += "," + ToString(j);
      }
      ASSERT_EQ(db_iter->value().ToString(), merge_result);

      db_iter->Prev();
      ASSERT_TRUE(db_iter->Valid());
      ASSERT_EQ(db_iter->key().ToString(), "b");
      ASSERT_EQ(db_iter->value().ToString(), "b");

      db_iter->Prev();
      ASSERT_TRUE(db_iter->Valid());
      ASSERT_EQ(db_iter->key().ToString(), "a");
      ASSERT_EQ(db_iter->value().ToString(), "a");

      db_iter->Prev();
      ASSERT_TRUE(!db_iter->Valid());
    }
  }
}

TEST_F(DBIteratorTest, DBIteratorSkipInternalKeys) {
  Options options;
  ImmutableCFOptions cf_options = ImmutableCFOptions(options);
  MutableCFOptions mutable_cf_options = MutableCFOptions(options);
  ReadOptions ro;

  // Basic test case ... Make sure explicityly passing the default value works.
  // Skipping internal keys is disabled by default, when the value is 0.
  {
    TestIterator* internal_iter = new TestIterator(BytewiseComparator());
    internal_iter->AddPut("a", "val_a");
    internal_iter->AddDeletion("b");
    internal_iter->AddDeletion("b");
    internal_iter->AddPut("c", "val_c");
    internal_iter->AddPut("c", "val_c");
    internal_iter->AddDeletion("c");
    internal_iter->AddPut("d", "val_d");
    internal_iter->Finish();

    ro.max_skippable_internal_keys = 0;
    std::unique_ptr<Iterator> db_iter(NewDBIterator(
        env_, ro, cf_options, mutable_cf_options, BytewiseComparator(),
        internal_iter, 10, options.max_sequential_skip_in_iterations,
        nullptr  ));

    db_iter->SeekToFirst();
    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(db_iter->key().ToString(), "a");
    ASSERT_EQ(db_iter->value().ToString(), "val_a");

    db_iter->Next();
    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(db_iter->key().ToString(), "d");
    ASSERT_EQ(db_iter->value().ToString(), "val_d");

    db_iter->Next();
    ASSERT_TRUE(!db_iter->Valid());
    ASSERT_TRUE(db_iter->status().ok());

    db_iter->SeekToLast();
    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(db_iter->key().ToString(), "d");
    ASSERT_EQ(db_iter->value().ToString(), "val_d");

    db_iter->Prev();
    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(db_iter->key().ToString(), "a");
    ASSERT_EQ(db_iter->value().ToString(), "val_a");

    db_iter->Prev();
    ASSERT_TRUE(!db_iter->Valid());
    ASSERT_TRUE(db_iter->status().ok());
  }

  // Test to make sure that the request will *not* fail as incomplete if
  // num_internal_keys_skipped is *equal* to max_skippable_internal_keys
  // threshold. (It will fail as incomplete only when the threshold is
  // exceeded.)
  {
    TestIterator* internal_iter = new TestIterator(BytewiseComparator());
    internal_iter->AddPut("a", "val_a");
    internal_iter->AddDeletion("b");
    internal_iter->AddDeletion("b");
    internal_iter->AddPut("c", "val_c");
    internal_iter->Finish();

    ro.max_skippable_internal_keys = 2;
    std::unique_ptr<Iterator> db_iter(NewDBIterator(
        env_, ro, cf_options, mutable_cf_options, BytewiseComparator(),
        internal_iter, 10, options.max_sequential_skip_in_iterations,
        nullptr  ));

    db_iter->SeekToFirst();
    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(db_iter->key().ToString(), "a");
    ASSERT_EQ(db_iter->value().ToString(), "val_a");

    db_iter->Next();
    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(db_iter->key().ToString(), "c");
    ASSERT_EQ(db_iter->value().ToString(), "val_c");

    db_iter->Next();
    ASSERT_TRUE(!db_iter->Valid());
    ASSERT_TRUE(db_iter->status().ok());

    db_iter->SeekToLast();
    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(db_iter->key().ToString(), "c");
    ASSERT_EQ(db_iter->value().ToString(), "val_c");

    db_iter->Prev();
    ASSERT_EQ(db_iter->key().ToString(), "a");
    ASSERT_EQ(db_iter->value().ToString(), "val_a");

    db_iter->Prev();
    ASSERT_TRUE(!db_iter->Valid());
    ASSERT_TRUE(db_iter->status().ok());
  }

  // Fail the request as incomplete when num_internal_keys_skipped >
  // max_skippable_internal_keys
  {
    TestIterator* internal_iter = new TestIterator(BytewiseComparator());
    internal_iter->AddPut("a", "val_a");
    internal_iter->AddDeletion("b");
    internal_iter->AddDeletion("b");
    internal_iter->AddDeletion("b");
    internal_iter->AddPut("c", "val_c");
    internal_iter->Finish();

    ro.max_skippable_internal_keys = 2;
    std::unique_ptr<Iterator> db_iter(NewDBIterator(
        env_, ro, cf_options, mutable_cf_options, BytewiseComparator(),
        internal_iter, 10, options.max_sequential_skip_in_iterations,
        nullptr  ));

    db_iter->SeekToFirst();
    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(db_iter->key().ToString(), "a");
    ASSERT_EQ(db_iter->value().ToString(), "val_a");

    db_iter->Next();
    ASSERT_TRUE(!db_iter->Valid());
    ASSERT_TRUE(db_iter->status().IsIncomplete());

    db_iter->SeekToLast();
    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(db_iter->key().ToString(), "c");
    ASSERT_EQ(db_iter->value().ToString(), "val_c");

    db_iter->Prev();
    ASSERT_TRUE(!db_iter->Valid());
    ASSERT_TRUE(db_iter->status().IsIncomplete());
  }

  // Test that the num_internal_keys_skipped counter resets after a successful
  // read.
  {
    TestIterator* internal_iter = new TestIterator(BytewiseComparator());
    internal_iter->AddPut("a", "val_a");
    internal_iter->AddDeletion("b");
    internal_iter->AddDeletion("b");
    internal_iter->AddPut("c", "val_c");
    internal_iter->AddDeletion("d");
    internal_iter->AddDeletion("d");
    internal_iter->AddDeletion("d");
    internal_iter->AddPut("e", "val_e");
    internal_iter->Finish();

    ro.max_skippable_internal_keys = 2;
    std::unique_ptr<Iterator> db_iter(NewDBIterator(
        env_, ro, cf_options, mutable_cf_options, BytewiseComparator(),
        internal_iter, 10, options.max_sequential_skip_in_iterations,
        nullptr  ));

    db_iter->SeekToFirst();
    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(db_iter->key().ToString(), "a");
    ASSERT_EQ(db_iter->value().ToString(), "val_a");

    db_iter->Next();
    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(db_iter->key().ToString(), "c");
    ASSERT_EQ(db_iter->value().ToString(), "val_c");

    db_iter->Next();  // num_internal_keys_skipped counter resets here.
    ASSERT_TRUE(!db_iter->Valid());
    ASSERT_TRUE(db_iter->status().IsIncomplete());
  }

  // Test that the num_internal_keys_skipped counter resets after a successful
  // read.
  // Reverse direction
  {
    TestIterator* internal_iter = new TestIterator(BytewiseComparator());
    internal_iter->AddPut("a", "val_a");
    internal_iter->AddDeletion("b");
    internal_iter->AddDeletion("b");
    internal_iter->AddDeletion("b");
    internal_iter->AddPut("c", "val_c");
    internal_iter->AddDeletion("d");
    internal_iter->AddDeletion("d");
    internal_iter->AddPut("e", "val_e");
    internal_iter->Finish();

    ro.max_skippable_internal_keys = 2;
    std::unique_ptr<Iterator> db_iter(NewDBIterator(
        env_, ro, cf_options, mutable_cf_options, BytewiseComparator(),
        internal_iter, 10, options.max_sequential_skip_in_iterations,
        nullptr  ));

    db_iter->SeekToLast();
    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(db_iter->key().ToString(), "e");
    ASSERT_EQ(db_iter->value().ToString(), "val_e");

    db_iter->Prev();
    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(db_iter->key().ToString(), "c");
    ASSERT_EQ(db_iter->value().ToString(), "val_c");

    db_iter->Prev();  // num_internal_keys_skipped counter resets here.
    ASSERT_TRUE(!db_iter->Valid());
    ASSERT_TRUE(db_iter->status().IsIncomplete());
  }

  // Test that skipping separate keys is handled
  {
    TestIterator* internal_iter = new TestIterator(BytewiseComparator());
    internal_iter->AddPut("a", "val_a");
    internal_iter->AddDeletion("b");
    internal_iter->AddDeletion("c");
    internal_iter->AddDeletion("d");
    internal_iter->AddPut("e", "val_e");
    internal_iter->Finish();

    ro.max_skippable_internal_keys = 2;
    std::unique_ptr<Iterator> db_iter(NewDBIterator(
        env_, ro, cf_options, mutable_cf_options, BytewiseComparator(),
        internal_iter, 10, options.max_sequential_skip_in_iterations,
        nullptr  ));

    db_iter->SeekToFirst();
    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(db_iter->key().ToString(), "a");
    ASSERT_EQ(db_iter->value().ToString(), "val_a");

    db_iter->Next();
    ASSERT_TRUE(!db_iter->Valid());
    ASSERT_TRUE(db_iter->status().IsIncomplete());

    db_iter->SeekToLast();
    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(db_iter->key().ToString(), "e");
    ASSERT_EQ(db_iter->value().ToString(), "val_e");

    db_iter->Prev();
    ASSERT_TRUE(!db_iter->Valid());
    ASSERT_TRUE(db_iter->status().IsIncomplete());
  }

  // Test if alternating puts and deletes of the same key are handled correctly.
  {
    TestIterator* internal_iter = new TestIterator(BytewiseComparator());
    internal_iter->AddPut("a", "val_a");
    internal_iter->AddPut("b", "val_b");
    internal_iter->AddDeletion("b");
    internal_iter->AddPut("c", "val_c");
    internal_iter->AddDeletion("c");
    internal_iter->AddPut("d", "val_d");
    internal_iter->AddDeletion("d");
    internal_iter->AddPut("e", "val_e");
    internal_iter->Finish();

    ro.max_skippable_internal_keys = 2;
    std::unique_ptr<Iterator> db_iter(NewDBIterator(
        env_, ro, cf_options, mutable_cf_options, BytewiseComparator(),
        internal_iter, 10, options.max_sequential_skip_in_iterations,
        nullptr  ));

    db_iter->SeekToFirst();
    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(db_iter->key().ToString(), "a");
    ASSERT_EQ(db_iter->value().ToString(), "val_a");

    db_iter->Next();
    ASSERT_TRUE(!db_iter->Valid());
    ASSERT_TRUE(db_iter->status().IsIncomplete());

    db_iter->SeekToLast();
    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(db_iter->key().ToString(), "e");
    ASSERT_EQ(db_iter->value().ToString(), "val_e");

    db_iter->Prev();
    ASSERT_TRUE(!db_iter->Valid());
    ASSERT_TRUE(db_iter->status().IsIncomplete());
  }

  // Test for large number of skippable internal keys with *default*
  // max_sequential_skip_in_iterations.
  {
    for (size_t i = 1; i <= 200; ++i) {
      TestIterator* internal_iter = new TestIterator(BytewiseComparator());
      internal_iter->AddPut("a", "val_a");
      for (size_t j = 1; j <= i; ++j) {
        internal_iter->AddPut("b", "val_b");
        internal_iter->AddDeletion("b");
      }
      internal_iter->AddPut("c", "val_c");
      internal_iter->Finish();

      ro.max_skippable_internal_keys = i;
      std::unique_ptr<Iterator> db_iter(NewDBIterator(
          env_, ro, cf_options, mutable_cf_options, BytewiseComparator(),
          internal_iter, 2 * i + 1, options.max_sequential_skip_in_iterations,
          nullptr  ));

      db_iter->SeekToFirst();
      ASSERT_TRUE(db_iter->Valid());
      ASSERT_EQ(db_iter->key().ToString(), "a");
      ASSERT_EQ(db_iter->value().ToString(), "val_a");

      db_iter->Next();
      if ((options.max_sequential_skip_in_iterations + 1) >=
          ro.max_skippable_internal_keys) {
        ASSERT_TRUE(!db_iter->Valid());
        ASSERT_TRUE(db_iter->status().IsIncomplete());
      } else {
        ASSERT_TRUE(db_iter->Valid());
        ASSERT_EQ(db_iter->key().ToString(), "c");
        ASSERT_EQ(db_iter->value().ToString(), "val_c");
      }

      db_iter->SeekToLast();
      ASSERT_TRUE(db_iter->Valid());
      ASSERT_EQ(db_iter->key().ToString(), "c");
      ASSERT_EQ(db_iter->value().ToString(), "val_c");

      db_iter->Prev();
      if ((options.max_sequential_skip_in_iterations + 1) >=
          ro.max_skippable_internal_keys) {
        ASSERT_TRUE(!db_iter->Valid());
        ASSERT_TRUE(db_iter->status().IsIncomplete());
      } else {
        ASSERT_TRUE(db_iter->Valid());
        ASSERT_EQ(db_iter->key().ToString(), "a");
        ASSERT_EQ(db_iter->value().ToString(), "val_a");
      }
    }
  }

  // Test for large number of skippable internal keys with a *non-default*
  // max_sequential_skip_in_iterations.
  {
    for (size_t i = 1; i <= 200; ++i) {
      TestIterator* internal_iter = new TestIterator(BytewiseComparator());
      internal_iter->AddPut("a", "val_a");
      for (size_t j = 1; j <= i; ++j) {
        internal_iter->AddPut("b", "val_b");
        internal_iter->AddDeletion("b");
      }
      internal_iter->AddPut("c", "val_c");
      internal_iter->Finish();

      options.max_sequential_skip_in_iterations = 1000;
      ro.max_skippable_internal_keys = i;
      std::unique_ptr<Iterator> db_iter(NewDBIterator(
          env_, ro, cf_options, mutable_cf_options, BytewiseComparator(),
          internal_iter, 2 * i + 1, options.max_sequential_skip_in_iterations,
          nullptr  ));

      db_iter->SeekToFirst();
      ASSERT_TRUE(db_iter->Valid());
      ASSERT_EQ(db_iter->key().ToString(), "a");
      ASSERT_EQ(db_iter->value().ToString(), "val_a");

      db_iter->Next();
      ASSERT_TRUE(!db_iter->Valid());
      ASSERT_TRUE(db_iter->status().IsIncomplete());

      db_iter->SeekToLast();
      ASSERT_TRUE(db_iter->Valid());
      ASSERT_EQ(db_iter->key().ToString(), "c");
      ASSERT_EQ(db_iter->value().ToString(), "val_c");

      db_iter->Prev();
      ASSERT_TRUE(!db_iter->Valid());
      ASSERT_TRUE(db_iter->status().IsIncomplete());
    }
  }
}

TEST_F(DBIteratorTest, DBIterator1) {
  ReadOptions ro;
  Options options;
  options.merge_operator = MergeOperators::CreateFromStringId("stringappend");

  TestIterator* internal_iter = new TestIterator(BytewiseComparator());
  internal_iter->AddPut("a", "0");
  internal_iter->AddPut("b", "0");
  internal_iter->AddDeletion("b");
  internal_iter->AddMerge("a", "1");
  internal_iter->AddMerge("b", "2");
  internal_iter->Finish();

  std::unique_ptr<Iterator> db_iter(NewDBIterator(
      env_, ro, ImmutableCFOptions(options), MutableCFOptions(options),
      BytewiseComparator(), internal_iter, 1,
      options.max_sequential_skip_in_iterations, nullptr  ));
  db_iter->SeekToFirst();
  ASSERT_TRUE(db_iter->Valid());
  ASSERT_EQ(db_iter->key().ToString(), "a");
  ASSERT_EQ(db_iter->value().ToString(), "0");
  db_iter->Next();
  ASSERT_TRUE(db_iter->Valid());
  ASSERT_EQ(db_iter->key().ToString(), "b");
  db_iter->Next();
  ASSERT_FALSE(db_iter->Valid());
}

TEST_F(DBIteratorTest, DBIterator2) {
  ReadOptions ro;
  Options options;
  options.merge_operator = MergeOperators::CreateFromStringId("stringappend");

  TestIterator* internal_iter = new TestIterator(BytewiseComparator());
  internal_iter->AddPut("a", "0");
  internal_iter->AddPut("b", "0");
  internal_iter->AddDeletion("b");
  internal_iter->AddMerge("a", "1");
  internal_iter->AddMerge("b", "2");
  internal_iter->Finish();

  std::unique_ptr<Iterator> db_iter(NewDBIterator(
      env_, ro, ImmutableCFOptions(options), MutableCFOptions(options),
      BytewiseComparator(), internal_iter, 0,
      options.max_sequential_skip_in_iterations, nullptr  ));
  db_iter->SeekToFirst();
  ASSERT_TRUE(db_iter->Valid());
  ASSERT_EQ(db_iter->key().ToString(), "a");
  ASSERT_EQ(db_iter->value().ToString(), "0");
  db_iter->Next();
  ASSERT_TRUE(!db_iter->Valid());
}

TEST_F(DBIteratorTest, DBIterator3) {
  ReadOptions ro;
  Options options;
  options.merge_operator = MergeOperators::CreateFromStringId("stringappend");

  TestIterator* internal_iter = new TestIterator(BytewiseComparator());
  internal_iter->AddPut("a", "0");
  internal_iter->AddPut("b", "0");
  internal_iter->AddDeletion("b");
  internal_iter->AddMerge("a", "1");
  internal_iter->AddMerge("b", "2");
  internal_iter->Finish();

  std::unique_ptr<Iterator> db_iter(NewDBIterator(
      env_, ro, ImmutableCFOptions(options), MutableCFOptions(options),
      BytewiseComparator(), internal_iter, 2,
      options.max_sequential_skip_in_iterations, nullptr  ));
  db_iter->SeekToFirst();
  ASSERT_TRUE(db_iter->Valid());
  ASSERT_EQ(db_iter->key().ToString(), "a");
  ASSERT_EQ(db_iter->value().ToString(), "0");
  db_iter->Next();
  ASSERT_TRUE(!db_iter->Valid());
}

TEST_F(DBIteratorTest, DBIterator4) {
  ReadOptions ro;
  Options options;
  options.merge_operator = MergeOperators::CreateFromStringId("stringappend");

  TestIterator* internal_iter = new TestIterator(BytewiseComparator());
  internal_iter->AddPut("a", "0");
  internal_iter->AddPut("b", "0");
  internal_iter->AddDeletion("b");
  internal_iter->AddMerge("a", "1");
  internal_iter->AddMerge("b", "2");
  internal_iter->Finish();

  std::unique_ptr<Iterator> db_iter(NewDBIterator(
      env_, ro, ImmutableCFOptions(options), MutableCFOptions(options),
      BytewiseComparator(), internal_iter, 4,
      options.max_sequential_skip_in_iterations, nullptr  ));
  db_iter->SeekToFirst();
  ASSERT_TRUE(db_iter->Valid());
  ASSERT_EQ(db_iter->key().ToString(), "a");
  ASSERT_EQ(db_iter->value().ToString(), "0,1");
  db_iter->Next();
  ASSERT_TRUE(db_iter->Valid());
  ASSERT_EQ(db_iter->key().ToString(), "b");
  ASSERT_EQ(db_iter->value().ToString(), "2");
  db_iter->Next();
  ASSERT_TRUE(!db_iter->Valid());
}

TEST_F(DBIteratorTest, DBIterator5) {
  ReadOptions ro;
  Options options;
  options.merge_operator = MergeOperators::CreateFromStringId("stringappend");
  ImmutableCFOptions cf_options = ImmutableCFOptions(options);
  MutableCFOptions mutable_cf_options = MutableCFOptions(options);

  {
    TestIterator* internal_iter = new TestIterator(BytewiseComparator());
    internal_iter->AddMerge("a", "merge_1");
    internal_iter->AddMerge("a", "merge_2");
    internal_iter->AddMerge("a", "merge_3");
    internal_iter->AddPut("a", "put_1");
    internal_iter->AddMerge("a", "merge_4");
    internal_iter->AddMerge("a", "merge_5");
    internal_iter->AddMerge("a", "merge_6");
    internal_iter->Finish();

    std::unique_ptr<Iterator> db_iter(NewDBIterator(
        env_, ro, cf_options, mutable_cf_options, BytewiseComparator(),
        internal_iter, 0, options.max_sequential_skip_in_iterations,
        nullptr  ));
    db_iter->SeekToLast();
    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(db_iter->key().ToString(), "a");
    ASSERT_EQ(db_iter->value().ToString(), "merge_1");
    db_iter->Prev();
    ASSERT_TRUE(!db_iter->Valid());
  }

  {
    TestIterator* internal_iter = new TestIterator(BytewiseComparator());
    internal_iter->AddMerge("a", "merge_1");
    internal_iter->AddMerge("a", "merge_2");
    internal_iter->AddMerge("a", "merge_3");
    internal_iter->AddPut("a", "put_1");
    internal_iter->AddMerge("a", "merge_4");
    internal_iter->AddMerge("a", "merge_5");
    internal_iter->AddMerge("a", "merge_6");
    internal_iter->Finish();

    std::unique_ptr<Iterator> db_iter(NewDBIterator(
        env_, ro, cf_options, mutable_cf_options, BytewiseComparator(),
        internal_iter, 1, options.max_sequential_skip_in_iterations,
        nullptr  ));
    db_iter->SeekToLast();
    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(db_iter->key().ToString(), "a");
    ASSERT_EQ(db_iter->value().ToString(), "merge_1,merge_2");
    db_iter->Prev();
    ASSERT_TRUE(!db_iter->Valid());
  }

  {
    TestIterator* internal_iter = new TestIterator(BytewiseComparator());
    internal_iter->AddMerge("a", "merge_1");
    internal_iter->AddMerge("a", "merge_2");
    internal_iter->AddMerge("a", "merge_3");
    internal_iter->AddPut("a", "put_1");
    internal_iter->AddMerge("a", "merge_4");
    internal_iter->AddMerge("a", "merge_5");
    internal_iter->AddMerge("a", "merge_6");
    internal_iter->Finish();

    std::unique_ptr<Iterator> db_iter(NewDBIterator(
        env_, ro, cf_options, mutable_cf_options, BytewiseComparator(),
        internal_iter, 2, options.max_sequential_skip_in_iterations,
        nullptr  ));
    db_iter->SeekToLast();
    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(db_iter->key().ToString(), "a");
    ASSERT_EQ(db_iter->value().ToString(), "merge_1,merge_2,merge_3");
    db_iter->Prev();
    ASSERT_TRUE(!db_iter->Valid());
  }

  {
    TestIterator* internal_iter = new TestIterator(BytewiseComparator());
    internal_iter->AddMerge("a", "merge_1");
    internal_iter->AddMerge("a", "merge_2");
    internal_iter->AddMerge("a", "merge_3");
    internal_iter->AddPut("a", "put_1");
    internal_iter->AddMerge("a", "merge_4");
    internal_iter->AddMerge("a", "merge_5");
    internal_iter->AddMerge("a", "merge_6");
    internal_iter->Finish();

    std::unique_ptr<Iterator> db_iter(NewDBIterator(
        env_, ro, cf_options, mutable_cf_options, BytewiseComparator(),
        internal_iter, 3, options.max_sequential_skip_in_iterations,
        nullptr  ));
    db_iter->SeekToLast();
    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(db_iter->key().ToString(), "a");
    ASSERT_EQ(db_iter->value().ToString(), "put_1");
    db_iter->Prev();
    ASSERT_TRUE(!db_iter->Valid());
  }

  {
    TestIterator* internal_iter = new TestIterator(BytewiseComparator());
    internal_iter->AddMerge("a", "merge_1");
    internal_iter->AddMerge("a", "merge_2");
    internal_iter->AddMerge("a", "merge_3");
    internal_iter->AddPut("a", "put_1");
    internal_iter->AddMerge("a", "merge_4");
    internal_iter->AddMerge("a", "merge_5");
    internal_iter->AddMerge("a", "merge_6");
    internal_iter->Finish();

    std::unique_ptr<Iterator> db_iter(NewDBIterator(
        env_, ro, cf_options, mutable_cf_options, BytewiseComparator(),
        internal_iter, 4, options.max_sequential_skip_in_iterations,
        nullptr  ));
    db_iter->SeekToLast();
    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(db_iter->key().ToString(), "a");
    ASSERT_EQ(db_iter->value().ToString(), "put_1,merge_4");
    db_iter->Prev();
    ASSERT_TRUE(!db_iter->Valid());
  }

  {
    TestIterator* internal_iter = new TestIterator(BytewiseComparator());
    internal_iter->AddMerge("a", "merge_1");
    internal_iter->AddMerge("a", "merge_2");
    internal_iter->AddMerge("a", "merge_3");
    internal_iter->AddPut("a", "put_1");
    internal_iter->AddMerge("a", "merge_4");
    internal_iter->AddMerge("a", "merge_5");
    internal_iter->AddMerge("a", "merge_6");
    internal_iter->Finish();

    std::unique_ptr<Iterator> db_iter(NewDBIterator(
        env_, ro, cf_options, mutable_cf_options, BytewiseComparator(),
        internal_iter, 5, options.max_sequential_skip_in_iterations,
        nullptr  ));
    db_iter->SeekToLast();
    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(db_iter->key().ToString(), "a");
    ASSERT_EQ(db_iter->value().ToString(), "put_1,merge_4,merge_5");
    db_iter->Prev();
    ASSERT_TRUE(!db_iter->Valid());
  }

  {
    TestIterator* internal_iter = new TestIterator(BytewiseComparator());
    internal_iter->AddMerge("a", "merge_1");
    internal_iter->AddMerge("a", "merge_2");
    internal_iter->AddMerge("a", "merge_3");
    internal_iter->AddPut("a", "put_1");
    internal_iter->AddMerge("a", "merge_4");
    internal_iter->AddMerge("a", "merge_5");
    internal_iter->AddMerge("a", "merge_6");
    internal_iter->Finish();

    std::unique_ptr<Iterator> db_iter(NewDBIterator(
        env_, ro, cf_options, mutable_cf_options, BytewiseComparator(),
        internal_iter, 6, options.max_sequential_skip_in_iterations,
        nullptr  ));
    db_iter->SeekToLast();
    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(db_iter->key().ToString(), "a");
    ASSERT_EQ(db_iter->value().ToString(), "put_1,merge_4,merge_5,merge_6");
    db_iter->Prev();
    ASSERT_TRUE(!db_iter->Valid());
  }

  {
    // put, singledelete, merge
    TestIterator* internal_iter = new TestIterator(BytewiseComparator());
    internal_iter->AddPut("a", "val_a");
    internal_iter->AddSingleDeletion("a");
    internal_iter->AddMerge("a", "merge_1");
    internal_iter->AddMerge("a", "merge_2");
    internal_iter->AddPut("b", "val_b");
    internal_iter->Finish();
    std::unique_ptr<Iterator> db_iter(NewDBIterator(
        env_, ro, cf_options, mutable_cf_options, BytewiseComparator(),
        internal_iter, 10, options.max_sequential_skip_in_iterations,
        nullptr  ));
    db_iter->Seek("b");
    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(db_iter->key().ToString(), "b");
    db_iter->Prev();
    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(db_iter->key().ToString(), "a");
  }
}

TEST_F(DBIteratorTest, DBIterator6) {
  ReadOptions ro;
  Options options;
  options.merge_operator = MergeOperators::CreateFromStringId("stringappend");
  ImmutableCFOptions cf_options = ImmutableCFOptions(options);
  MutableCFOptions mutable_cf_options = MutableCFOptions(options);

  {
    TestIterator* internal_iter = new TestIterator(BytewiseComparator());
    internal_iter->AddMerge("a", "merge_1");
    internal_iter->AddMerge("a", "merge_2");
    internal_iter->AddMerge("a", "merge_3");
    internal_iter->AddDeletion("a");
    internal_iter->AddMerge("a", "merge_4");
    internal_iter->AddMerge("a", "merge_5");
    internal_iter->AddMerge("a", "merge_6");
    internal_iter->Finish();

    std::unique_ptr<Iterator> db_iter(NewDBIterator(
        env_, ro, cf_options, mutable_cf_options, BytewiseComparator(),
        internal_iter, 0, options.max_sequential_skip_in_iterations,
        nullptr  ));
    db_iter->SeekToLast();
    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(db_iter->key().ToString(), "a");
    ASSERT_EQ(db_iter->value().ToString(), "merge_1");
    db_iter->Prev();
    ASSERT_TRUE(!db_iter->Valid());
  }

  {
    TestIterator* internal_iter = new TestIterator(BytewiseComparator());
    internal_iter->AddMerge("a", "merge_1");
    internal_iter->AddMerge("a", "merge_2");
    internal_iter->AddMerge("a", "merge_3");
    internal_iter->AddDeletion("a");
    internal_iter->AddMerge("a", "merge_4");
    internal_iter->AddMerge("a", "merge_5");
    internal_iter->AddMerge("a", "merge_6");
    internal_iter->Finish();

    std::unique_ptr<Iterator> db_iter(NewDBIterator(
        env_, ro, cf_options, mutable_cf_options, BytewiseComparator(),
        internal_iter, 1, options.max_sequential_skip_in_iterations,
        nullptr  ));
    db_iter->SeekToLast();
    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(db_iter->key().ToString(), "a");
    ASSERT_EQ(db_iter->value().ToString(), "merge_1,merge_2");
    db_iter->Prev();
    ASSERT_TRUE(!db_iter->Valid());
  }

  {
    TestIterator* internal_iter = new TestIterator(BytewiseComparator());
    internal_iter->AddMerge("a", "merge_1");
    internal_iter->AddMerge("a", "merge_2");
    internal_iter->AddMerge("a", "merge_3");
    internal_iter->AddDeletion("a");
    internal_iter->AddMerge("a", "merge_4");
    internal_iter->AddMerge("a", "merge_5");
    internal_iter->AddMerge("a", "merge_6");
    internal_iter->Finish();

    std::unique_ptr<Iterator> db_iter(NewDBIterator(
        env_, ro, cf_options, mutable_cf_options, BytewiseComparator(),
        internal_iter, 2, options.max_sequential_skip_in_iterations,
        nullptr  ));
    db_iter->SeekToLast();
    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(db_iter->key().ToString(), "a");
    ASSERT_EQ(db_iter->value().ToString(), "merge_1,merge_2,merge_3");
    db_iter->Prev();
    ASSERT_TRUE(!db_iter->Valid());
  }

  {
    TestIterator* internal_iter = new TestIterator(BytewiseComparator());
    internal_iter->AddMerge("a", "merge_1");
    internal_iter->AddMerge("a", "merge_2");
    internal_iter->AddMerge("a", "merge_3");
    internal_iter->AddDeletion("a");
    internal_iter->AddMerge("a", "merge_4");
    internal_iter->AddMerge("a", "merge_5");
    internal_iter->AddMerge("a", "merge_6");
    internal_iter->Finish();

    std::unique_ptr<Iterator> db_iter(NewDBIterator(
        env_, ro, cf_options, mutable_cf_options, BytewiseComparator(),
        internal_iter, 3, options.max_sequential_skip_in_iterations,
        nullptr  ));
    db_iter->SeekToLast();
    ASSERT_TRUE(!db_iter->Valid());
  }

  {
    TestIterator* internal_iter = new TestIterator(BytewiseComparator());
    internal_iter->AddMerge("a", "merge_1");
    internal_iter->AddMerge("a", "merge_2");
    internal_iter->AddMerge("a", "merge_3");
    internal_iter->AddDeletion("a");
    internal_iter->AddMerge("a", "merge_4");
    internal_iter->AddMerge("a", "merge_5");
    internal_iter->AddMerge("a", "merge_6");
    internal_iter->Finish();

    std::unique_ptr<Iterator> db_iter(NewDBIterator(
        env_, ro, cf_options, mutable_cf_options, BytewiseComparator(),
        internal_iter, 4, options.max_sequential_skip_in_iterations,
        nullptr  ));
    db_iter->SeekToLast();
    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(db_iter->key().ToString(), "a");
    ASSERT_EQ(db_iter->value().ToString(), "merge_4");
    db_iter->Prev();
    ASSERT_TRUE(!db_iter->Valid());
  }

  {
    TestIterator* internal_iter = new TestIterator(BytewiseComparator());
    internal_iter->AddMerge("a", "merge_1");
    internal_iter->AddMerge("a", "merge_2");
    internal_iter->AddMerge("a", "merge_3");
    internal_iter->AddDeletion("a");
    internal_iter->AddMerge("a", "merge_4");
    internal_iter->AddMerge("a", "merge_5");
    internal_iter->AddMerge("a", "merge_6");
    internal_iter->Finish();

    std::unique_ptr<Iterator> db_iter(NewDBIterator(
        env_, ro, cf_options, mutable_cf_options, BytewiseComparator(),
        internal_iter, 5, options.max_sequential_skip_in_iterations,
        nullptr  ));
    db_iter->SeekToLast();
    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(db_iter->key().ToString(), "a");
    ASSERT_EQ(db_iter->value().ToString(), "merge_4,merge_5");
    db_iter->Prev();
    ASSERT_TRUE(!db_iter->Valid());
  }

  {
    TestIterator* internal_iter = new TestIterator(BytewiseComparator());
    internal_iter->AddMerge("a", "merge_1");
    internal_iter->AddMerge("a", "merge_2");
    internal_iter->AddMerge("a", "merge_3");
    internal_iter->AddDeletion("a");
    internal_iter->AddMerge("a", "merge_4");
    internal_iter->AddMerge("a", "merge_5");
    internal_iter->AddMerge("a", "merge_6");
    internal_iter->Finish();

    std::unique_ptr<Iterator> db_iter(NewDBIterator(
        env_, ro, cf_options, mutable_cf_options, BytewiseComparator(),
        internal_iter, 6, options.max_sequential_skip_in_iterations,
        nullptr  ));
    db_iter->SeekToLast();
    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(db_iter->key().ToString(), "a");
    ASSERT_EQ(db_iter->value().ToString(), "merge_4,merge_5,merge_6");
    db_iter->Prev();
    ASSERT_TRUE(!db_iter->Valid());
  }
}

TEST_F(DBIteratorTest, DBIterator7) {
  ReadOptions ro;
  Options options;
  options.merge_operator = MergeOperators::CreateFromStringId("stringappend");
  ImmutableCFOptions cf_options = ImmutableCFOptions(options);
  MutableCFOptions mutable_cf_options = MutableCFOptions(options);

  {
    TestIterator* internal_iter = new TestIterator(BytewiseComparator());
    internal_iter->AddMerge("a", "merge_1");
    internal_iter->AddPut("b", "val");
    internal_iter->AddMerge("b", "merge_2");

    internal_iter->AddDeletion("b");
    internal_iter->AddMerge("b", "merge_3");

    internal_iter->AddMerge("c", "merge_4");
    internal_iter->AddMerge("c", "merge_5");

    internal_iter->AddDeletion("b");
    internal_iter->AddMerge("b", "merge_6");
    internal_iter->AddMerge("b", "merge_7");
    internal_iter->AddMerge("b", "merge_8");
    internal_iter->AddMerge("b", "merge_9");
    internal_iter->AddMerge("b", "merge_10");
    internal_iter->AddMerge("b", "merge_11");

    internal_iter->AddDeletion("c");
    internal_iter->Finish();

    std::unique_ptr<Iterator> db_iter(NewDBIterator(
        env_, ro, cf_options, mutable_cf_options, BytewiseComparator(),
        internal_iter, 0, options.max_sequential_skip_in_iterations,
        nullptr  ));
    db_iter->SeekToLast();
    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(db_iter->key().ToString(), "a");
    ASSERT_EQ(db_iter->value().ToString(), "merge_1");
    db_iter->Prev();
    ASSERT_TRUE(!db_iter->Valid());
  }

  {
    TestIterator* internal_iter = new TestIterator(BytewiseComparator());
    internal_iter->AddMerge("a", "merge_1");
    internal_iter->AddPut("b", "val");
    internal_iter->AddMerge("b", "merge_2");

    internal_iter->AddDeletion("b");
    internal_iter->AddMerge("b", "merge_3");

    internal_iter->AddMerge("c", "merge_4");
    internal_iter->AddMerge("c", "merge_5");

    internal_iter->AddDeletion("b");
    internal_iter->AddMerge("b", "merge_6");
    internal_iter->AddMerge("b", "merge_7");
    internal_iter->AddMerge("b", "merge_8");
    internal_iter->AddMerge("b", "merge_9");
    internal_iter->AddMerge("b", "merge_10");
    internal_iter->AddMerge("b", "merge_11");

    internal_iter->AddDeletion("c");
    internal_iter->Finish();

    std::unique_ptr<Iterator> db_iter(NewDBIterator(
        env_, ro, cf_options, mutable_cf_options, BytewiseComparator(),
        internal_iter, 2, options.max_sequential_skip_in_iterations,
        nullptr  ));
    db_iter->SeekToLast();
    ASSERT_TRUE(db_iter->Valid());

    ASSERT_EQ(db_iter->key().ToString(), "b");
    ASSERT_EQ(db_iter->value().ToString(), "val,merge_2");
    db_iter->Prev();
    ASSERT_TRUE(db_iter->Valid());

    ASSERT_EQ(db_iter->key().ToString(), "a");
    ASSERT_EQ(db_iter->value().ToString(), "merge_1");
    db_iter->Prev();
    ASSERT_TRUE(!db_iter->Valid());
  }

  {
    TestIterator* internal_iter = new TestIterator(BytewiseComparator());
    internal_iter->AddMerge("a", "merge_1");
    internal_iter->AddPut("b", "val");
    internal_iter->AddMerge("b", "merge_2");

    internal_iter->AddDeletion("b");
    internal_iter->AddMerge("b", "merge_3");

    internal_iter->AddMerge("c", "merge_4");
    internal_iter->AddMerge("c", "merge_5");

    internal_iter->AddDeletion("b");
    internal_iter->AddMerge("b", "merge_6");
    internal_iter->AddMerge("b", "merge_7");
    internal_iter->AddMerge("b", "merge_8");
    internal_iter->AddMerge("b", "merge_9");
    internal_iter->AddMerge("b", "merge_10");
    internal_iter->AddMerge("b", "merge_11");

    internal_iter->AddDeletion("c");
    internal_iter->Finish();

    std::unique_ptr<Iterator> db_iter(NewDBIterator(
        env_, ro, cf_options, mutable_cf_options, BytewiseComparator(),
        internal_iter, 4, options.max_sequential_skip_in_iterations,
        nullptr  ));
    db_iter->SeekToLast();
    ASSERT_TRUE(db_iter->Valid());

    ASSERT_EQ(db_iter->key().ToString(), "b");
    ASSERT_EQ(db_iter->value().ToString(), "merge_3");
    db_iter->Prev();
    ASSERT_TRUE(db_iter->Valid());

    ASSERT_EQ(db_iter->key().ToString(), "a");
    ASSERT_EQ(db_iter->value().ToString(), "merge_1");
    db_iter->Prev();
    ASSERT_TRUE(!db_iter->Valid());
  }

  {
    TestIterator* internal_iter = new TestIterator(BytewiseComparator());
    internal_iter->AddMerge("a", "merge_1");
    internal_iter->AddPut("b", "val");
    internal_iter->AddMerge("b", "merge_2");

    internal_iter->AddDeletion("b");
    internal_iter->AddMerge("b", "merge_3");

    internal_iter->AddMerge("c", "merge_4");
    internal_iter->AddMerge("c", "merge_5");

    internal_iter->AddDeletion("b");
    internal_iter->AddMerge("b", "merge_6");
    internal_iter->AddMerge("b", "merge_7");
    internal_iter->AddMerge("b", "merge_8");
    internal_iter->AddMerge("b", "merge_9");
    internal_iter->AddMerge("b", "merge_10");
    internal_iter->AddMerge("b", "merge_11");

    internal_iter->AddDeletion("c");
    internal_iter->Finish();

    std::unique_ptr<Iterator> db_iter(NewDBIterator(
        env_, ro, cf_options, mutable_cf_options, BytewiseComparator(),
        internal_iter, 5, options.max_sequential_skip_in_iterations,
        nullptr  ));
    db_iter->SeekToLast();
    ASSERT_TRUE(db_iter->Valid());

    ASSERT_EQ(db_iter->key().ToString(), "c");
    ASSERT_EQ(db_iter->value().ToString(), "merge_4");
    db_iter->Prev();

    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(db_iter->key().ToString(), "b");
    ASSERT_EQ(db_iter->value().ToString(), "merge_3");
    db_iter->Prev();
    ASSERT_TRUE(db_iter->Valid());

    ASSERT_EQ(db_iter->key().ToString(), "a");
    ASSERT_EQ(db_iter->value().ToString(), "merge_1");
    db_iter->Prev();
    ASSERT_TRUE(!db_iter->Valid());
  }

  {
    TestIterator* internal_iter = new TestIterator(BytewiseComparator());
    internal_iter->AddMerge("a", "merge_1");
    internal_iter->AddPut("b", "val");
    internal_iter->AddMerge("b", "merge_2");

    internal_iter->AddDeletion("b");
    internal_iter->AddMerge("b", "merge_3");

    internal_iter->AddMerge("c", "merge_4");
    internal_iter->AddMerge("c", "merge_5");

    internal_iter->AddDeletion("b");
    internal_iter->AddMerge("b", "merge_6");
    internal_iter->AddMerge("b", "merge_7");
    internal_iter->AddMerge("b", "merge_8");
    internal_iter->AddMerge("b", "merge_9");
    internal_iter->AddMerge("b", "merge_10");
    internal_iter->AddMerge("b", "merge_11");

    internal_iter->AddDeletion("c");
    internal_iter->Finish();

    std::unique_ptr<Iterator> db_iter(NewDBIterator(
        env_, ro, cf_options, mutable_cf_options, BytewiseComparator(),
        internal_iter, 6, options.max_sequential_skip_in_iterations,
        nullptr  ));
    db_iter->SeekToLast();
    ASSERT_TRUE(db_iter->Valid());

    ASSERT_EQ(db_iter->key().ToString(), "c");
    ASSERT_EQ(db_iter->value().ToString(), "merge_4,merge_5");
    db_iter->Prev();
    ASSERT_TRUE(db_iter->Valid());

    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(db_iter->key().ToString(), "b");
    ASSERT_EQ(db_iter->value().ToString(), "merge_3");
    db_iter->Prev();
    ASSERT_TRUE(db_iter->Valid());

    ASSERT_EQ(db_iter->key().ToString(), "a");
    ASSERT_EQ(db_iter->value().ToString(), "merge_1");
    db_iter->Prev();
    ASSERT_TRUE(!db_iter->Valid());
  }

  {
    TestIterator* internal_iter = new TestIterator(BytewiseComparator());
    internal_iter->AddMerge("a", "merge_1");
    internal_iter->AddPut("b", "val");
    internal_iter->AddMerge("b", "merge_2");

    internal_iter->AddDeletion("b");
    internal_iter->AddMerge("b", "merge_3");

    internal_iter->AddMerge("c", "merge_4");
    internal_iter->AddMerge("c", "merge_5");

    internal_iter->AddDeletion("b");
    internal_iter->AddMerge("b", "merge_6");
    internal_iter->AddMerge("b", "merge_7");
    internal_iter->AddMerge("b", "merge_8");
    internal_iter->AddMerge("b", "merge_9");
    internal_iter->AddMerge("b", "merge_10");
    internal_iter->AddMerge("b", "merge_11");

    internal_iter->AddDeletion("c");
    internal_iter->Finish();

    std::unique_ptr<Iterator> db_iter(NewDBIterator(
        env_, ro, cf_options, mutable_cf_options, BytewiseComparator(),
        internal_iter, 7, options.max_sequential_skip_in_iterations,
        nullptr  ));
    db_iter->SeekToLast();
    ASSERT_TRUE(db_iter->Valid());

    ASSERT_EQ(db_iter->key().ToString(), "c");
    ASSERT_EQ(db_iter->value().ToString(), "merge_4,merge_5");
    db_iter->Prev();
    ASSERT_TRUE(db_iter->Valid());

    ASSERT_EQ(db_iter->key().ToString(), "a");
    ASSERT_EQ(db_iter->value().ToString(), "merge_1");
    db_iter->Prev();
    ASSERT_TRUE(!db_iter->Valid());
  }

  {
    TestIterator* internal_iter = new TestIterator(BytewiseComparator());
    internal_iter->AddMerge("a", "merge_1");
    internal_iter->AddPut("b", "val");
    internal_iter->AddMerge("b", "merge_2");

    internal_iter->AddDeletion("b");
    internal_iter->AddMerge("b", "merge_3");

    internal_iter->AddMerge("c", "merge_4");
    internal_iter->AddMerge("c", "merge_5");

    internal_iter->AddDeletion("b");
    internal_iter->AddMerge("b", "merge_6");
    internal_iter->AddMerge("b", "merge_7");
    internal_iter->AddMerge("b", "merge_8");
    internal_iter->AddMerge("b", "merge_9");
    internal_iter->AddMerge("b", "merge_10");
    internal_iter->AddMerge("b", "merge_11");

    internal_iter->AddDeletion("c");
    internal_iter->Finish();

    std::unique_ptr<Iterator> db_iter(NewDBIterator(
        env_, ro, cf_options, mutable_cf_options, BytewiseComparator(),
        internal_iter, 9, options.max_sequential_skip_in_iterations,
        nullptr  ));
    db_iter->SeekToLast();
    ASSERT_TRUE(db_iter->Valid());

    ASSERT_EQ(db_iter->key().ToString(), "c");
    ASSERT_EQ(db_iter->value().ToString(), "merge_4,merge_5");
    db_iter->Prev();
    ASSERT_TRUE(db_iter->Valid());

    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(db_iter->key().ToString(), "b");
    ASSERT_EQ(db_iter->value().ToString(), "merge_6,merge_7");
    db_iter->Prev();
    ASSERT_TRUE(db_iter->Valid());

    ASSERT_EQ(db_iter->key().ToString(), "a");
    ASSERT_EQ(db_iter->value().ToString(), "merge_1");
    db_iter->Prev();
    ASSERT_TRUE(!db_iter->Valid());
  }

  {
    TestIterator* internal_iter = new TestIterator(BytewiseComparator());
    internal_iter->AddMerge("a", "merge_1");
    internal_iter->AddPut("b", "val");
    internal_iter->AddMerge("b", "merge_2");

    internal_iter->AddDeletion("b");
    internal_iter->AddMerge("b", "merge_3");

    internal_iter->AddMerge("c", "merge_4");
    internal_iter->AddMerge("c", "merge_5");

    internal_iter->AddDeletion("b");
    internal_iter->AddMerge("b", "merge_6");
    internal_iter->AddMerge("b", "merge_7");
    internal_iter->AddMerge("b", "merge_8");
    internal_iter->AddMerge("b", "merge_9");
    internal_iter->AddMerge("b", "merge_10");
    internal_iter->AddMerge("b", "merge_11");

    internal_iter->AddDeletion("c");
    internal_iter->Finish();

    std::unique_ptr<Iterator> db_iter(NewDBIterator(
        env_, ro, cf_options, mutable_cf_options, BytewiseComparator(),
        internal_iter, 13, options.max_sequential_skip_in_iterations,
        nullptr  ));
    db_iter->SeekToLast();
    ASSERT_TRUE(db_iter->Valid());

    ASSERT_EQ(db_iter->key().ToString(), "c");
    ASSERT_EQ(db_iter->value().ToString(), "merge_4,merge_5");
    db_iter->Prev();
    ASSERT_TRUE(db_iter->Valid());

    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(db_iter->key().ToString(), "b");
    ASSERT_EQ(db_iter->value().ToString(),
              "merge_6,merge_7,merge_8,merge_9,merge_10,merge_11");
    db_iter->Prev();
    ASSERT_TRUE(db_iter->Valid());

    ASSERT_EQ(db_iter->key().ToString(), "a");
    ASSERT_EQ(db_iter->value().ToString(), "merge_1");
    db_iter->Prev();
    ASSERT_TRUE(!db_iter->Valid());
  }

  {
    TestIterator* internal_iter = new TestIterator(BytewiseComparator());
    internal_iter->AddMerge("a", "merge_1");
    internal_iter->AddPut("b", "val");
    internal_iter->AddMerge("b", "merge_2");

    internal_iter->AddDeletion("b");
    internal_iter->AddMerge("b", "merge_3");

    internal_iter->AddMerge("c", "merge_4");
    internal_iter->AddMerge("c", "merge_5");

    internal_iter->AddDeletion("b");
    internal_iter->AddMerge("b", "merge_6");
    internal_iter->AddMerge("b", "merge_7");
    internal_iter->AddMerge("b", "merge_8");
    internal_iter->AddMerge("b", "merge_9");
    internal_iter->AddMerge("b", "merge_10");
    internal_iter->AddMerge("b", "merge_11");

    internal_iter->AddDeletion("c");
    internal_iter->Finish();

    std::unique_ptr<Iterator> db_iter(NewDBIterator(
        env_, ro, cf_options, mutable_cf_options, BytewiseComparator(),
        internal_iter, 14, options.max_sequential_skip_in_iterations,
        nullptr  ));
    db_iter->SeekToLast();
    ASSERT_TRUE(db_iter->Valid());

    ASSERT_EQ(db_iter->key().ToString(), "b");
    ASSERT_EQ(db_iter->value().ToString(),
              "merge_6,merge_7,merge_8,merge_9,merge_10,merge_11");
    db_iter->Prev();
    ASSERT_TRUE(db_iter->Valid());

    ASSERT_EQ(db_iter->key().ToString(), "a");
    ASSERT_EQ(db_iter->value().ToString(), "merge_1");
    db_iter->Prev();
    ASSERT_TRUE(!db_iter->Valid());
  }
}

TEST_F(DBIteratorTest, DBIterator8) {
  ReadOptions ro;
  Options options;
  options.merge_operator = MergeOperators::CreateFromStringId("stringappend");

  TestIterator* internal_iter = new TestIterator(BytewiseComparator());
  internal_iter->AddDeletion("a");
  internal_iter->AddPut("a", "0");
  internal_iter->AddPut("b", "0");
  internal_iter->Finish();

  std::unique_ptr<Iterator> db_iter(NewDBIterator(
      env_, ro, ImmutableCFOptions(options), MutableCFOptions(options),
      BytewiseComparator(), internal_iter, 10,
      options.max_sequential_skip_in_iterations, nullptr  ));
  db_iter->SeekToLast();
  ASSERT_TRUE(db_iter->Valid());
  ASSERT_EQ(db_iter->key().ToString(), "b");
  ASSERT_EQ(db_iter->value().ToString(), "0");

  db_iter->Prev();
  ASSERT_TRUE(db_iter->Valid());
  ASSERT_EQ(db_iter->key().ToString(), "a");
  ASSERT_EQ(db_iter->value().ToString(), "0");
}

// TODO(3.13): fix the issue of Seek() then Prev() which might not necessary
//             return the biggest element smaller than the seek key.
TEST_F(DBIteratorTest, DBIterator9) {
  ReadOptions ro;
  Options options;
  options.merge_operator = MergeOperators::CreateFromStringId("stringappend");
  {
    TestIterator* internal_iter = new TestIterator(BytewiseComparator());
    internal_iter->AddMerge("a", "merge_1");
    internal_iter->AddMerge("a", "merge_2");
    internal_iter->AddMerge("b", "merge_3");
    internal_iter->AddMerge("b", "merge_4");
    internal_iter->AddMerge("d", "merge_5");
    internal_iter->AddMerge("d", "merge_6");
    internal_iter->Finish();

    std::unique_ptr<Iterator> db_iter(NewDBIterator(
        env_, ro, ImmutableCFOptions(options), MutableCFOptions(options),
        BytewiseComparator(), internal_iter, 10,
        options.max_sequential_skip_in_iterations, nullptr  ));

    db_iter->SeekToLast();
    ASSERT_TRUE(db_iter->Valid());
    db_iter->Prev();
    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(db_iter->key().ToString(), "b");
    ASSERT_EQ(db_iter->value().ToString(), "merge_3,merge_4");
    db_iter->Next();
    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(db_iter->key().ToString(), "d");
    ASSERT_EQ(db_iter->value().ToString(), "merge_5,merge_6");

    db_iter->Seek("b");
    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(db_iter->key().ToString(), "b");
    ASSERT_EQ(db_iter->value().ToString(), "merge_3,merge_4");
    db_iter->Prev();
    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(db_iter->key().ToString(), "a");
    ASSERT_EQ(db_iter->value().ToString(), "merge_1,merge_2");

    db_iter->SeekForPrev("b");
    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(db_iter->key().ToString(), "b");
    ASSERT_EQ(db_iter->value().ToString(), "merge_3,merge_4");
    db_iter->Next();
    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(db_iter->key().ToString(), "d");
    ASSERT_EQ(db_iter->value().ToString(), "merge_5,merge_6");

    db_iter->Seek("c");
    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(db_iter->key().ToString(), "d");
    ASSERT_EQ(db_iter->value().ToString(), "merge_5,merge_6");
    db_iter->Prev();
    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(db_iter->key().ToString(), "b");
    ASSERT_EQ(db_iter->value().ToString(), "merge_3,merge_4");

    db_iter->SeekForPrev("c");
    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(db_iter->key().ToString(), "b");
    ASSERT_EQ(db_iter->value().ToString(), "merge_3,merge_4");
    db_iter->Next();
    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(db_iter->key().ToString(), "d");
    ASSERT_EQ(db_iter->value().ToString(), "merge_5,merge_6");
  }
}

// TODO(3.13): fix the issue of Seek() then Prev() which might not necessary
//             return the biggest element smaller than the seek key.
TEST_F(DBIteratorTest, DBIterator10) {
  ReadOptions ro;
  Options options;

  TestIterator* internal_iter = new TestIterator(BytewiseComparator());
  internal_iter->AddPut("a", "1");
  internal_iter->AddPut("b", "2");
  internal_iter->AddPut("c", "3");
  internal_iter->AddPut("d", "4");
  internal_iter->Finish();

  std::unique_ptr<Iterator> db_iter(NewDBIterator(
      env_, ro, ImmutableCFOptions(options), MutableCFOptions(options),
      BytewiseComparator(), internal_iter, 10,
      options.max_sequential_skip_in_iterations, nullptr  ));

  db_iter->Seek("c");
  ASSERT_TRUE(db_iter->Valid());
  db_iter->Prev();
  ASSERT_TRUE(db_iter->Valid());
  ASSERT_EQ(db_iter->key().ToString(), "b");
  ASSERT_EQ(db_iter->value().ToString(), "2");

  db_iter->Next();
  ASSERT_TRUE(db_iter->Valid());
  ASSERT_EQ(db_iter->key().ToString(), "c");
  ASSERT_EQ(db_iter->value().ToString(), "3");

  db_iter->SeekForPrev("c");
  ASSERT_TRUE(db_iter->Valid());
  db_iter->Next();
  ASSERT_TRUE(db_iter->Valid());
  ASSERT_EQ(db_iter->key().ToString(), "d");
  ASSERT_EQ(db_iter->value().ToString(), "4");

  db_iter->Prev();
  ASSERT_TRUE(db_iter->Valid());
  ASSERT_EQ(db_iter->key().ToString(), "c");
  ASSERT_EQ(db_iter->value().ToString(), "3");
}

TEST_F(DBIteratorTest, SeekToLastOccurrenceSeq0) {
  ReadOptions ro;
  Options options;
  options.merge_operator = nullptr;

  TestIterator* internal_iter = new TestIterator(BytewiseComparator());
  internal_iter->AddPut("a", "1");
  internal_iter->AddPut("b", "2");
  internal_iter->Finish();

  std::unique_ptr<Iterator> db_iter(NewDBIterator(
      env_, ro, ImmutableCFOptions(options), MutableCFOptions(options),
      BytewiseComparator(), internal_iter, 10, 0,
      nullptr  ));
  db_iter->SeekToFirst();
  ASSERT_TRUE(db_iter->Valid());
  ASSERT_EQ(db_iter->key().ToString(), "a");
  ASSERT_EQ(db_iter->value().ToString(), "1");
  db_iter->Next();
  ASSERT_TRUE(db_iter->Valid());
  ASSERT_EQ(db_iter->key().ToString(), "b");
  ASSERT_EQ(db_iter->value().ToString(), "2");
  db_iter->Next();
  ASSERT_FALSE(db_iter->Valid());
}

TEST_F(DBIteratorTest, DBIterator11) {
  ReadOptions ro;
  Options options;
  options.merge_operator = MergeOperators::CreateFromStringId("stringappend");

  TestIterator* internal_iter = new TestIterator(BytewiseComparator());
  internal_iter->AddPut("a", "0");
  internal_iter->AddPut("b", "0");
  internal_iter->AddSingleDeletion("b");
  internal_iter->AddMerge("a", "1");
  internal_iter->AddMerge("b", "2");
  internal_iter->Finish();

  std::unique_ptr<Iterator> db_iter(NewDBIterator(
      env_, ro, ImmutableCFOptions(options), MutableCFOptions(options),
      BytewiseComparator(), internal_iter, 1,
      options.max_sequential_skip_in_iterations, nullptr  ));
  db_iter->SeekToFirst();
  ASSERT_TRUE(db_iter->Valid());
  ASSERT_EQ(db_iter->key().ToString(), "a");
  ASSERT_EQ(db_iter->value().ToString(), "0");
  db_iter->Next();
  ASSERT_TRUE(db_iter->Valid());
  ASSERT_EQ(db_iter->key().ToString(), "b");
  db_iter->Next();
  ASSERT_FALSE(db_iter->Valid());
}

TEST_F(DBIteratorTest, DBIterator12) {
  ReadOptions ro;
  Options options;
  options.merge_operator = nullptr;

  TestIterator* internal_iter = new TestIterator(BytewiseComparator());
  internal_iter->AddPut("a", "1");
  internal_iter->AddPut("b", "2");
  internal_iter->AddPut("c", "3");
  internal_iter->AddSingleDeletion("b");
  internal_iter->Finish();

  std::unique_ptr<Iterator> db_iter(NewDBIterator(
      env_, ro, ImmutableCFOptions(options), MutableCFOptions(options),
      BytewiseComparator(), internal_iter, 10, 0, nullptr  ));
  db_iter->SeekToLast();
  ASSERT_TRUE(db_iter->Valid());
  ASSERT_EQ(db_iter->key().ToString(), "c");
  ASSERT_EQ(db_iter->value().ToString(), "3");
  db_iter->Prev();
  ASSERT_TRUE(db_iter->Valid());
  ASSERT_EQ(db_iter->key().ToString(), "a");
  ASSERT_EQ(db_iter->value().ToString(), "1");
  db_iter->Prev();
  ASSERT_FALSE(db_iter->Valid());
}

TEST_F(DBIteratorTest, DBIterator13) {
  ReadOptions ro;
  Options options;
  options.merge_operator = nullptr;

  std::string key;
  key.resize(9);
  key.assign(9, static_cast<char>(0));
  key[0] = 'b';

  TestIterator* internal_iter = new TestIterator(BytewiseComparator());
  internal_iter->AddPut(key, "0");
  internal_iter->AddPut(key, "1");
  internal_iter->AddPut(key, "2");
  internal_iter->AddPut(key, "3");
  internal_iter->AddPut(key, "4");
  internal_iter->AddPut(key, "5");
  internal_iter->AddPut(key, "6");
  internal_iter->AddPut(key, "7");
  internal_iter->AddPut(key, "8");
  internal_iter->Finish();

  std::unique_ptr<Iterator> db_iter(NewDBIterator(
      env_, ro, ImmutableCFOptions(options), MutableCFOptions(options),
      BytewiseComparator(), internal_iter, 2, 3, nullptr  ));
  db_iter->Seek("b");
  ASSERT_TRUE(db_iter->Valid());
  ASSERT_EQ(db_iter->key().ToString(), key);
  ASSERT_EQ(db_iter->value().ToString(), "2");
}

TEST_F(DBIteratorTest, DBIterator14) {
  ReadOptions ro;
  Options options;
  options.merge_operator = nullptr;

  std::string key("b");
  TestIterator* internal_iter = new TestIterator(BytewiseComparator());
  internal_iter->AddPut("b", "0");
  internal_iter->AddPut("b", "1");
  internal_iter->AddPut("b", "2");
  internal_iter->AddPut("b", "3");
  internal_iter->AddPut("a", "4");
  internal_iter->AddPut("a", "5");
  internal_iter->AddPut("a", "6");
  internal_iter->AddPut("c", "7");
  internal_iter->AddPut("c", "8");
  internal_iter->AddPut("c", "9");
  internal_iter->Finish();

  std::unique_ptr<Iterator> db_iter(NewDBIterator(
      env_, ro, ImmutableCFOptions(options), MutableCFOptions(options),
      BytewiseComparator(), internal_iter, 4, 1, nullptr  ));
  db_iter->Seek("b");
  ASSERT_TRUE(db_iter->Valid());
  ASSERT_EQ(db_iter->key().ToString(), "b");
  ASSERT_EQ(db_iter->value().ToString(), "3");
  db_iter->SeekToFirst();
  ASSERT_EQ(db_iter->key().ToString(), "a");
  ASSERT_EQ(db_iter->value().ToString(), "4");
}

TEST_F(DBIteratorTest, DBIteratorTestDifferentialSnapshots) {
  { // test that KVs earlier that iter_start_seqnum are filtered out
    ReadOptions ro;
    ro.iter_start_seqnum=5;
    Options options;
    options.statistics = rocksdb::CreateDBStatistics();

    TestIterator* internal_iter = new TestIterator(BytewiseComparator());
    for (size_t i = 0; i < 10; ++i) {
      internal_iter->AddPut(std::to_string(i), std::to_string(i) + "a");
      internal_iter->AddPut(std::to_string(i), std::to_string(i) + "b");
      internal_iter->AddPut(std::to_string(i), std::to_string(i) + "c");
    }
    internal_iter->Finish();

    std::unique_ptr<Iterator> db_iter(NewDBIterator(
        env_, ro, ImmutableCFOptions(options), MutableCFOptions(options),
        BytewiseComparator(), internal_iter, 13,
        options.max_sequential_skip_in_iterations, nullptr));
    // Expecting InternalKeys in [5,8] range with correct type
    int seqnums[4] = {5,8,11,13};
    std::string user_keys[4] = {"1","2","3","4"};
    std::string values[4] = {"1c", "2c", "3c", "4b"};
    int i = 0;
    for (db_iter->SeekToFirst(); db_iter->Valid(); db_iter->Next()) {
      FullKey fkey;
      ParseFullKey(db_iter->key(), &fkey);
      ASSERT_EQ(user_keys[i], fkey.user_key.ToString());
      ASSERT_EQ(EntryType::kEntryPut, fkey.type);
      ASSERT_EQ(seqnums[i], fkey.sequence);
      ASSERT_EQ(values[i], db_iter->value().ToString());
      i++;
    }
    ASSERT_EQ(i, 4);
  }

  { // Test that deletes are returned correctly as internal KVs
    ReadOptions ro;
    ro.iter_start_seqnum=5;
    Options options;
    options.statistics = rocksdb::CreateDBStatistics();

    TestIterator* internal_iter = new TestIterator(BytewiseComparator());
    for (size_t i = 0; i < 10; ++i) {
      internal_iter->AddPut(std::to_string(i), std::to_string(i) + "a");
      internal_iter->AddPut(std::to_string(i), std::to_string(i) + "b");
      internal_iter->AddDeletion(std::to_string(i));
    }
    internal_iter->Finish();

    std::unique_ptr<Iterator> db_iter(NewDBIterator(
        env_, ro, ImmutableCFOptions(options), MutableCFOptions(options),
        BytewiseComparator(), internal_iter, 13,
        options.max_sequential_skip_in_iterations, nullptr));
    // Expecting InternalKeys in [5,8] range with correct type
    int seqnums[4] = {5,8,11,13};
    EntryType key_types[4] = {EntryType::kEntryDelete,EntryType::kEntryDelete,
      EntryType::kEntryDelete,EntryType::kEntryPut};
    std::string user_keys[4] = {"1","2","3","4"};
    std::string values[4] = {"", "", "", "4b"};
    int i = 0;
    for (db_iter->SeekToFirst(); db_iter->Valid(); db_iter->Next()) {
      FullKey fkey;
      ParseFullKey(db_iter->key(), &fkey);
      ASSERT_EQ(user_keys[i], fkey.user_key.ToString());
      ASSERT_EQ(key_types[i], fkey.type);
      ASSERT_EQ(seqnums[i], fkey.sequence);
      ASSERT_EQ(values[i], db_iter->value().ToString());
      i++;
    }
    ASSERT_EQ(i, 4);
  }
}

class DBIterWithMergeIterTest : public testing::Test {
 public:
  DBIterWithMergeIterTest()
      : env_(Env::Default()), icomp_(BytewiseComparator()) {
    options_.merge_operator = nullptr;

    internal_iter1_ = new TestIterator(BytewiseComparator());
    internal_iter1_->Add("a", kTypeValue, "1", 3u);
    internal_iter1_->Add("f", kTypeValue, "2", 5u);
    internal_iter1_->Add("g", kTypeValue, "3", 7u);
    internal_iter1_->Finish();

    internal_iter2_ = new TestIterator(BytewiseComparator());
    internal_iter2_->Add("a", kTypeValue, "4", 6u);
    internal_iter2_->Add("b", kTypeValue, "5", 1u);
    internal_iter2_->Add("c", kTypeValue, "6", 2u);
    internal_iter2_->Add("d", kTypeValue, "7", 3u);
    internal_iter2_->Finish();

    std::vector<InternalIterator*> child_iters;
    child_iters.push_back(internal_iter1_);
    child_iters.push_back(internal_iter2_);
    InternalKeyComparator icomp(BytewiseComparator());
    InternalIterator* merge_iter =
        NewMergingIterator(&icomp_, &child_iters[0], 2u);

    db_iter_.reset(NewDBIterator(
        env_, ro_, ImmutableCFOptions(options_), MutableCFOptions(options_),
        BytewiseComparator(), merge_iter,
        8 ,
        3 , nullptr  ));
  }

  Env* env_;
  ReadOptions ro_;
  Options options_;
  TestIterator* internal_iter1_;
  TestIterator* internal_iter2_;
  InternalKeyComparator icomp_;
  Iterator* merge_iter_;
  std::unique_ptr<Iterator> db_iter_;
};

TEST_F(DBIterWithMergeIterTest, InnerMergeIterator1) {
  db_iter_->SeekToFirst();
  ASSERT_TRUE(db_iter_->Valid());
  ASSERT_EQ(db_iter_->key().ToString(), "a");
  ASSERT_EQ(db_iter_->value().ToString(), "4");
  db_iter_->Next();
  ASSERT_TRUE(db_iter_->Valid());
  ASSERT_EQ(db_iter_->key().ToString(), "b");
  ASSERT_EQ(db_iter_->value().ToString(), "5");
  db_iter_->Next();
  ASSERT_TRUE(db_iter_->Valid());
  ASSERT_EQ(db_iter_->key().ToString(), "c");
  ASSERT_EQ(db_iter_->value().ToString(), "6");
  db_iter_->Next();
  ASSERT_TRUE(db_iter_->Valid());
  ASSERT_EQ(db_iter_->key().ToString(), "d");
  ASSERT_EQ(db_iter_->value().ToString(), "7");
  db_iter_->Next();
  ASSERT_TRUE(db_iter_->Valid());
  ASSERT_EQ(db_iter_->key().ToString(), "f");
  ASSERT_EQ(db_iter_->value().ToString(), "2");
  db_iter_->Next();
  ASSERT_TRUE(db_iter_->Valid());
  ASSERT_EQ(db_iter_->key().ToString(), "g");
  ASSERT_EQ(db_iter_->value().ToString(), "3");
  db_iter_->Next();
  ASSERT_FALSE(db_iter_->Valid());
}

TEST_F(DBIterWithMergeIterTest, InnerMergeIterator2) {
  // Test Prev() when one child iterator is at its end.
  db_iter_->SeekForPrev("g");
  ASSERT_TRUE(db_iter_->Valid());
  ASSERT_EQ(db_iter_->key().ToString(), "g");
  ASSERT_EQ(db_iter_->value().ToString(), "3");
  db_iter_->Prev();
  ASSERT_TRUE(db_iter_->Valid());
  ASSERT_EQ(db_iter_->key().ToString(), "f");
  ASSERT_EQ(db_iter_->value().ToString(), "2");
  db_iter_->Prev();
  ASSERT_TRUE(db_iter_->Valid());
  ASSERT_EQ(db_iter_->key().ToString(), "d");
  ASSERT_EQ(db_iter_->value().ToString(), "7");
  db_iter_->Prev();
  ASSERT_TRUE(db_iter_->Valid());
  ASSERT_EQ(db_iter_->key().ToString(), "c");
  ASSERT_EQ(db_iter_->value().ToString(), "6");
  db_iter_->Prev();
  ASSERT_TRUE(db_iter_->Valid());
  ASSERT_EQ(db_iter_->key().ToString(), "b");
  ASSERT_EQ(db_iter_->value().ToString(), "5");
  db_iter_->Prev();
  ASSERT_TRUE(db_iter_->Valid());
  ASSERT_EQ(db_iter_->key().ToString(), "a");
  ASSERT_EQ(db_iter_->value().ToString(), "4");
}

TEST_F(DBIterWithMergeIterTest, InnerMergeIteratorDataRace1) {
  // Test Prev() when one child iterator is at its end but more rows
  // are added.
  db_iter_->Seek("f");
  ASSERT_TRUE(db_iter_->Valid());
  ASSERT_EQ(db_iter_->key().ToString(), "f");
  ASSERT_EQ(db_iter_->value().ToString(), "2");

  // Test call back inserts a key in the end of the mem table after
  // MergeIterator::Prev() realized the mem table iterator is at its end
  // and before an SeekToLast() is called.
  rocksdb::SyncPoint::GetInstance()->SetCallBack(
      "MergeIterator::Prev:BeforePrev",
      [&](void* ) { internal_iter2_->Add("z", kTypeValue, "7", 12u); });
  rocksdb::SyncPoint::GetInstance()->EnableProcessing();

  db_iter_->Prev();
  ASSERT_TRUE(db_iter_->Valid());
  ASSERT_EQ(db_iter_->key().ToString(), "d");
  ASSERT_EQ(db_iter_->value().ToString(), "7");
  db_iter_->Prev();
  ASSERT_TRUE(db_iter_->Valid());
  ASSERT_EQ(db_iter_->key().ToString(), "c");
  ASSERT_EQ(db_iter_->value().ToString(), "6");
  db_iter_->Prev();
  ASSERT_TRUE(db_iter_->Valid());
  ASSERT_EQ(db_iter_->key().ToString(), "b");
  ASSERT_EQ(db_iter_->value().ToString(), "5");
  db_iter_->Prev();
  ASSERT_TRUE(db_iter_->Valid());
  ASSERT_EQ(db_iter_->key().ToString(), "a");
  ASSERT_EQ(db_iter_->value().ToString(), "4");

  rocksdb::SyncPoint::GetInstance()->DisableProcessing();
}

TEST_F(DBIterWithMergeIterTest, InnerMergeIteratorDataRace2) {
  // Test Prev() when one child iterator is at its end but more rows
  // are added.
  db_iter_->Seek("f");
  ASSERT_TRUE(db_iter_->Valid());
  ASSERT_EQ(db_iter_->key().ToString(), "f");
  ASSERT_EQ(db_iter_->value().ToString(), "2");

  // Test call back inserts entries for update a key in the end of the
  // mem table after MergeIterator::Prev() realized the mem tableiterator is at
  // its end and before an SeekToLast() is called.
  rocksdb::SyncPoint::GetInstance()->SetCallBack(
      "MergeIterator::Prev:BeforePrev", [&](void* ) {
        internal_iter2_->Add("z", kTypeValue, "7", 12u);
        internal_iter2_->Add("z", kTypeValue, "7", 11u);
      });
  rocksdb::SyncPoint::GetInstance()->EnableProcessing();

  db_iter_->Prev();
  ASSERT_TRUE(db_iter_->Valid());
  ASSERT_EQ(db_iter_->key().ToString(), "d");
  ASSERT_EQ(db_iter_->value().ToString(), "7");
  db_iter_->Prev();
  ASSERT_TRUE(db_iter_->Valid());
  ASSERT_EQ(db_iter_->key().ToString(), "c");
  ASSERT_EQ(db_iter_->value().ToString(), "6");
  db_iter_->Prev();
  ASSERT_TRUE(db_iter_->Valid());
  ASSERT_EQ(db_iter_->key().ToString(), "b");
  ASSERT_EQ(db_iter_->value().ToString(), "5");
  db_iter_->Prev();
  ASSERT_TRUE(db_iter_->Valid());
  ASSERT_EQ(db_iter_->key().ToString(), "a");
  ASSERT_EQ(db_iter_->value().ToString(), "4");

  rocksdb::SyncPoint::GetInstance()->DisableProcessing();
}

TEST_F(DBIterWithMergeIterTest, InnerMergeIteratorDataRace3) {
  // Test Prev() when one child iterator is at its end but more rows
  // are added and max_skipped is triggered.
  db_iter_->Seek("f");
  ASSERT_TRUE(db_iter_->Valid());
  ASSERT_EQ(db_iter_->key().ToString(), "f");
  ASSERT_EQ(db_iter_->value().ToString(), "2");

  // Test call back inserts entries for update a key in the end of the
  // mem table after MergeIterator::Prev() realized the mem table iterator is at
  // its end and before an SeekToLast() is called.
  rocksdb::SyncPoint::GetInstance()->SetCallBack(
      "MergeIterator::Prev:BeforePrev", [&](void* ) {
        internal_iter2_->Add("z", kTypeValue, "7", 16u, true);
        internal_iter2_->Add("z", kTypeValue, "7", 15u, true);
        internal_iter2_->Add("z", kTypeValue, "7", 14u, true);
        internal_iter2_->Add("z", kTypeValue, "7", 13u, true);
        internal_iter2_->Add("z", kTypeValue, "7", 12u, true);
        internal_iter2_->Add("z", kTypeValue, "7", 11u, true);
      });
  rocksdb::SyncPoint::GetInstance()->EnableProcessing();

  db_iter_->Prev();
  ASSERT_TRUE(db_iter_->Valid());
  ASSERT_EQ(db_iter_->key().ToString(), "d");
  ASSERT_EQ(db_iter_->value().ToString(), "7");
  db_iter_->Prev();
  ASSERT_TRUE(db_iter_->Valid());
  ASSERT_EQ(db_iter_->key().ToString(), "c");
  ASSERT_EQ(db_iter_->value().ToString(), "6");
  db_iter_->Prev();
  ASSERT_TRUE(db_iter_->Valid());
  ASSERT_EQ(db_iter_->key().ToString(), "b");
  ASSERT_EQ(db_iter_->value().ToString(), "5");
  db_iter_->Prev();
  ASSERT_TRUE(db_iter_->Valid());
  ASSERT_EQ(db_iter_->key().ToString(), "a");
  ASSERT_EQ(db_iter_->value().ToString(), "4");

  rocksdb::SyncPoint::GetInstance()->DisableProcessing();
}

TEST_F(DBIterWithMergeIterTest, InnerMergeIteratorDataRace4) {
  // Test Prev() when one child iterator has more rows inserted
  // between Seek() and Prev() when changing directions.
  internal_iter2_->Add("z", kTypeValue, "9", 4u);

  db_iter_->Seek("g");
  ASSERT_TRUE(db_iter_->Valid());
  ASSERT_EQ(db_iter_->key().ToString(), "g");
  ASSERT_EQ(db_iter_->value().ToString(), "3");

  // Test call back inserts entries for update a key before "z" in
  // mem table after MergeIterator::Prev() calls mem table iterator's
  // Seek() and before calling Prev()
  rocksdb::SyncPoint::GetInstance()->SetCallBack(
      "MergeIterator::Prev:BeforePrev", [&](void* arg) {
        IteratorWrapper* it = reinterpret_cast<IteratorWrapper*>(arg);
        if (it->key().starts_with("z")) {
          internal_iter2_->Add("x", kTypeValue, "7", 16u, true);
          internal_iter2_->Add("x", kTypeValue, "7", 15u, true);
          internal_iter2_->Add("x", kTypeValue, "7", 14u, true);
          internal_iter2_->Add("x", kTypeValue, "7", 13u, true);
          internal_iter2_->Add("x", kTypeValue, "7", 12u, true);
          internal_iter2_->Add("x", kTypeValue, "7", 11u, true);
        }
      });
  rocksdb::SyncPoint::GetInstance()->EnableProcessing();

  db_iter_->Prev();
  ASSERT_TRUE(db_iter_->Valid());
  ASSERT_EQ(db_iter_->key().ToString(), "f");
  ASSERT_EQ(db_iter_->value().ToString(), "2");
  db_iter_->Prev();
  ASSERT_TRUE(db_iter_->Valid());
  ASSERT_EQ(db_iter_->key().ToString(), "d");
  ASSERT_EQ(db_iter_->value().ToString(), "7");
  db_iter_->Prev();
  ASSERT_TRUE(db_iter_->Valid());
  ASSERT_EQ(db_iter_->key().ToString(), "c");
  ASSERT_EQ(db_iter_->value().ToString(), "6");
  db_iter_->Prev();
  ASSERT_TRUE(db_iter_->Valid());
  ASSERT_EQ(db_iter_->key().ToString(), "b");
  ASSERT_EQ(db_iter_->value().ToString(), "5");
  db_iter_->Prev();
  ASSERT_TRUE(db_iter_->Valid());
  ASSERT_EQ(db_iter_->key().ToString(), "a");
  ASSERT_EQ(db_iter_->value().ToString(), "4");

  rocksdb::SyncPoint::GetInstance()->DisableProcessing();
}

TEST_F(DBIterWithMergeIterTest, InnerMergeIteratorDataRace5) {
  internal_iter2_->Add("z", kTypeValue, "9", 4u);

  // Test Prev() when one child iterator has more rows inserted
  // between Seek() and Prev() when changing directions.
  db_iter_->Seek("g");
  ASSERT_TRUE(db_iter_->Valid());
  ASSERT_EQ(db_iter_->key().ToString(), "g");
  ASSERT_EQ(db_iter_->value().ToString(), "3");

  // Test call back inserts entries for update a key before "z" in
  // mem table after MergeIterator::Prev() calls mem table iterator's
  // Seek() and before calling Prev()
  rocksdb::SyncPoint::GetInstance()->SetCallBack(
      "MergeIterator::Prev:BeforePrev", [&](void* arg) {
        IteratorWrapper* it = reinterpret_cast<IteratorWrapper*>(arg);
        if (it->key().starts_with("z")) {
          internal_iter2_->Add("x", kTypeValue, "7", 16u, true);
          internal_iter2_->Add("x", kTypeValue, "7", 15u, true);
        }
      });
  rocksdb::SyncPoint::GetInstance()->EnableProcessing();

  db_iter_->Prev();
  ASSERT_TRUE(db_iter_->Valid());
  ASSERT_EQ(db_iter_->key().ToString(), "f");
  ASSERT_EQ(db_iter_->value().ToString(), "2");
  db_iter_->Prev();
  ASSERT_TRUE(db_iter_->Valid());
  ASSERT_EQ(db_iter_->key().ToString(), "d");
  ASSERT_EQ(db_iter_->value().ToString(), "7");
  db_iter_->Prev();
  ASSERT_TRUE(db_iter_->Valid());
  ASSERT_EQ(db_iter_->key().ToString(), "c");
  ASSERT_EQ(db_iter_->value().ToString(), "6");
  db_iter_->Prev();
  ASSERT_TRUE(db_iter_->Valid());
  ASSERT_EQ(db_iter_->key().ToString(), "b");
  ASSERT_EQ(db_iter_->value().ToString(), "5");
  db_iter_->Prev();
  ASSERT_TRUE(db_iter_->Valid());
  ASSERT_EQ(db_iter_->key().ToString(), "a");
  ASSERT_EQ(db_iter_->value().ToString(), "4");

  rocksdb::SyncPoint::GetInstance()->DisableProcessing();
}

TEST_F(DBIterWithMergeIterTest, InnerMergeIteratorDataRace6) {
  internal_iter2_->Add("z", kTypeValue, "9", 4u);

  // Test Prev() when one child iterator has more rows inserted
  // between Seek() and Prev() when changing directions.
  db_iter_->Seek("g");
  ASSERT_TRUE(db_iter_->Valid());
  ASSERT_EQ(db_iter_->key().ToString(), "g");
  ASSERT_EQ(db_iter_->value().ToString(), "3");

  // Test call back inserts an entry for update a key before "z" in
  // mem table after MergeIterator::Prev() calls mem table iterator's
  // Seek() and before calling Prev()
  rocksdb::SyncPoint::GetInstance()->SetCallBack(
      "MergeIterator::Prev:BeforePrev", [&](void* arg) {
        IteratorWrapper* it = reinterpret_cast<IteratorWrapper*>(arg);
        if (it->key().starts_with("z")) {
          internal_iter2_->Add("x", kTypeValue, "7", 16u, true);
        }
      });
  rocksdb::SyncPoint::GetInstance()->EnableProcessing();

  db_iter_->Prev();
  ASSERT_TRUE(db_iter_->Valid());
  ASSERT_EQ(db_iter_->key().ToString(), "f");
  ASSERT_EQ(db_iter_->value().ToString(), "2");
  db_iter_->Prev();
  ASSERT_TRUE(db_iter_->Valid());
  ASSERT_EQ(db_iter_->key().ToString(), "d");
  ASSERT_EQ(db_iter_->value().ToString(), "7");
  db_iter_->Prev();
  ASSERT_TRUE(db_iter_->Valid());
  ASSERT_EQ(db_iter_->key().ToString(), "c");
  ASSERT_EQ(db_iter_->value().ToString(), "6");
  db_iter_->Prev();
  ASSERT_TRUE(db_iter_->Valid());
  ASSERT_EQ(db_iter_->key().ToString(), "b");
  ASSERT_EQ(db_iter_->value().ToString(), "5");
  db_iter_->Prev();
  ASSERT_TRUE(db_iter_->Valid());
  ASSERT_EQ(db_iter_->key().ToString(), "a");
  ASSERT_EQ(db_iter_->value().ToString(), "4");

  rocksdb::SyncPoint::GetInstance()->DisableProcessing();
}

TEST_F(DBIterWithMergeIterTest, InnerMergeIteratorDataRace7) {
  internal_iter1_->Add("u", kTypeValue, "10", 4u);
  internal_iter1_->Add("v", kTypeValue, "11", 4u);
  internal_iter1_->Add("w", kTypeValue, "12", 4u);
  internal_iter2_->Add("z", kTypeValue, "9", 4u);

  // Test Prev() when one child iterator has more rows inserted
  // between Seek() and Prev() when changing directions.
  db_iter_->Seek("g");
  ASSERT_TRUE(db_iter_->Valid());
  ASSERT_EQ(db_iter_->key().ToString(), "g");
  ASSERT_EQ(db_iter_->value().ToString(), "3");

  // Test call back inserts entries for update a key before "z" in
  // mem table after MergeIterator::Prev() calls mem table iterator's
  // Seek() and before calling Prev()
  rocksdb::SyncPoint::GetInstance()->SetCallBack(
      "MergeIterator::Prev:BeforePrev", [&](void* arg) {
        IteratorWrapper* it = reinterpret_cast<IteratorWrapper*>(arg);
        if (it->key().starts_with("z")) {
          internal_iter2_->Add("x", kTypeValue, "7", 16u, true);
          internal_iter2_->Add("x", kTypeValue, "7", 15u, true);
          internal_iter2_->Add("x", kTypeValue, "7", 14u, true);
          internal_iter2_->Add("x", kTypeValue, "7", 13u, true);
          internal_iter2_->Add("x", kTypeValue, "7", 12u, true);
          internal_iter2_->Add("x", kTypeValue, "7", 11u, true);
        }
      });
  rocksdb::SyncPoint::GetInstance()->EnableProcessing();

  db_iter_->Prev();
  ASSERT_TRUE(db_iter_->Valid());
  ASSERT_EQ(db_iter_->key().ToString(), "f");
  ASSERT_EQ(db_iter_->value().ToString(), "2");
  db_iter_->Prev();
  ASSERT_TRUE(db_iter_->Valid());
  ASSERT_EQ(db_iter_->key().ToString(), "d");
  ASSERT_EQ(db_iter_->value().ToString(), "7");
  db_iter_->Prev();
  ASSERT_TRUE(db_iter_->Valid());
  ASSERT_EQ(db_iter_->key().ToString(), "c");
  ASSERT_EQ(db_iter_->value().ToString(), "6");
  db_iter_->Prev();
  ASSERT_TRUE(db_iter_->Valid());
  ASSERT_EQ(db_iter_->key().ToString(), "b");
  ASSERT_EQ(db_iter_->value().ToString(), "5");
  db_iter_->Prev();
  ASSERT_TRUE(db_iter_->Valid());
  ASSERT_EQ(db_iter_->key().ToString(), "a");
  ASSERT_EQ(db_iter_->value().ToString(), "4");

  rocksdb::SyncPoint::GetInstance()->DisableProcessing();
}

TEST_F(DBIterWithMergeIterTest, InnerMergeIteratorDataRace8) {
  // internal_iter1_: a, f, g
  // internal_iter2_: a, b, c, d, adding (z)
  internal_iter2_->Add("z", kTypeValue, "9", 4u);

  // Test Prev() when one child iterator has more rows inserted
  // between Seek() and Prev() when changing directions.
  db_iter_->Seek("g");
  ASSERT_TRUE(db_iter_->Valid());
  ASSERT_EQ(db_iter_->key().ToString(), "g");
  ASSERT_EQ(db_iter_->value().ToString(), "3");

  // Test call back inserts two keys before "z" in mem table after
  // MergeIterator::Prev() calls mem table iterator's Seek() and
  // before calling Prev()
  rocksdb::SyncPoint::GetInstance()->SetCallBack(
      "MergeIterator::Prev:BeforePrev", [&](void* arg) {
        IteratorWrapper* it = reinterpret_cast<IteratorWrapper*>(arg);
        if (it->key().starts_with("z")) {
          internal_iter2_->Add("x", kTypeValue, "7", 16u, true);
          internal_iter2_->Add("y", kTypeValue, "7", 17u, true);
        }
      });
  rocksdb::SyncPoint::GetInstance()->EnableProcessing();

  db_iter_->Prev();
  ASSERT_TRUE(db_iter_->Valid());
  ASSERT_EQ(db_iter_->key().ToString(), "f");
  ASSERT_EQ(db_iter_->value().ToString(), "2");
  db_iter_->Prev();
  ASSERT_TRUE(db_iter_->Valid());
  ASSERT_EQ(db_iter_->key().ToString(), "d");
  ASSERT_EQ(db_iter_->value().ToString(), "7");

  rocksdb::SyncPoint::GetInstance()->DisableProcessing();
}
*/

/* JH */
class CompactionIterTest : public testing::Test {
 public:
  // A wrapper around Compaction. Has a much smaller interface, only what
  // CompactionIterator uses. Tests can override it.
  class CompactionProxy {
   public:
    explicit CompactionProxy(const Compaction* compaction)
        : compaction_(compaction) {}

    virtual ~CompactionProxy() = default;
    virtual int level(size_t /*compaction_input_level*/ = 0) const {
      return compaction_->level();
    }
    virtual bool KeyNotExistsBeyondOutputLevel(
        const Slice& user_key, std::vector<size_t>* level_ptrs) const {
      return compaction_->KeyNotExistsBeyondOutputLevel(user_key, level_ptrs);
    }
    virtual bool bottommost_level() const {
      return compaction_->bottommost_level();
    }
    virtual int number_levels() const { return compaction_->number_levels(); }
    virtual Slice GetLargestUserKey() const {
      return compaction_->GetLargestUserKey();
    }
    virtual bool allow_ingest_behind() const {
      return compaction_->immutable_cf_options()->allow_ingest_behind;
    }
    virtual bool preserve_deletes() const {
      return compaction_->immutable_cf_options()->preserve_deletes;
    }
   protected:
    CompactionProxy() = default;

   private:
    const Compaction* compaction_;
  };

  CompactionIterTest()
      : env_(Env::Default()), cmp_(BytewiseComparator()),
        icmp_(cmp_), snapshots_({}), snapshots2_({}), memtables({}) {
    options_.merge_operator = nullptr;
  }

  Env* env_;
  ReadOptions ro_;
  Options options_;
  TestIterator* internal_iter1_;
  TestIterator* internal_iter2_;
  std::unique_ptr<InternalIterator> merge_iter_;
  std::unique_ptr<InternalIterator> merge_iter2_;
  const Comparator* cmp_;
  const InternalKeyComparator icmp_;
  std::vector<SequenceNumber> snapshots_;
  std::vector<SequenceNumber> snapshots2_;
  std::vector<InternalIterator*> memtables;
  // A map of valid snapshot to last visible sequence to the snapshot.
  std::unordered_map<SequenceNumber, SequenceNumber> snapshot_map_;
  std::unordered_map<SequenceNumber, SequenceNumber> snapshot_map2_;
  std::unique_ptr<MemTable> mem_;
  std::unique_ptr<MemTable> mem2_;
  std::unique_ptr<MergeHelper> merge_helper_;
  std::unique_ptr<MergeHelper> merge_helper2_;
  std::unique_ptr<CompactionIterator> c_iter_;
  std::unique_ptr<CompactionIterator> c_iter2_;
  std::unique_ptr<CompactionRangeDelAggregator> range_del_agg_;
  std::unique_ptr<CompactionRangeDelAggregator> range_del_agg2_;
  std::unique_ptr<SnapshotChecker> snapshot_checker_;
  std::unique_ptr<SnapshotChecker> snapshot_checker2_;
  std::atomic<bool> shutting_down_{false};
  std::atomic<bool> shutting_down2_{false};
  Arena arena_;
};

class SstableCompactionIterTest : public testing::Test {
 public:

  SstableCompactionIterTest()
      : env_(Env::Default())  {
    db_name_ = test::PerThreadDBPath("sstable_compaction_iter_test");
  }

  Env* env_;
  std::string db_name_;
};

TEST_F(CompactionIterTest, SimpleCiter) {
  /* Prepare for SimpleCiter */
  internal_iter1_ = new TestIterator(BytewiseComparator());
  internal_iter1_->Add("a1", kTypeValue, "A2", 2u);
  internal_iter1_->Add("b1", kTypeValue, "B1", 1u);
  internal_iter1_->Add("c1", kTypeValue, "C1", 1u);
  internal_iter1_->Finish();

  internal_iter2_ = new TestIterator(BytewiseComparator());
  internal_iter2_->Add("a1", kTypeValue, "A3", 3u);
  internal_iter2_->Add("b1", kTypeValue, "B2", 2u);
  internal_iter2_->Add("c1", kTypeValue, "C2", 2u);
  internal_iter2_->Add("a1", kTypeValue, "A1", 1u);
  internal_iter2_->Finish();

  std::vector<InternalIterator*> child_iters;
  child_iters.push_back(internal_iter1_);
  child_iters.push_back(internal_iter2_);
  InternalKeyComparator icomp(BytewiseComparator());
  merge_iter_.reset(NewMergingIterator(&icmp_, &child_iters[0], 2u));

  std::unique_ptr<InternalIterator> unfragmented_range_del_iter(
      new test::VectorIterator({}, {}));
  auto tombstone_list = std::make_shared<FragmentedRangeTombstoneList>(
      std::move(unfragmented_range_del_iter), icmp_);

  std::unique_ptr<FragmentedRangeTombstoneIterator> range_del_iter(
      new FragmentedRangeTombstoneIterator(tombstone_list, icmp_,
        kMaxSequenceNumber));

  range_del_agg_.reset(new CompactionRangeDelAggregator(&icmp_, snapshots_));
  range_del_agg_->AddTombstones(std::move(range_del_iter));

  CompactionFilter* filter = nullptr;

  merge_helper_.reset(
      new MergeHelper(env_, cmp_, nullptr, filter, nullptr, false,
        0, snapshot_checker_.get(),
        0, nullptr, &shutting_down_));

  c_iter_.reset(new CompactionIterator(
        merge_iter_.get(), cmp_, merge_helper_.get(), kMaxSequenceNumber, &snapshots_,
        kMaxSequenceNumber, snapshot_checker_.get(),
        env_, false, false,
        range_del_agg_.get()));


  c_iter_->SeekToFirst();
	ParsedInternalKey pkey;
	ParseInternalKey(c_iter_->key(), &pkey);
	std::cout << "pkey: " << pkey.DebugString() << std::endl;
	std::cout << "key : " << c_iter_->user_key().ToString() <<", value : " <<c_iter_->value().ToString() << std::endl;
  ASSERT_TRUE(c_iter_->Valid());
  ASSERT_EQ(c_iter_->value().ToString(), "A3");
  ASSERT_EQ(c_iter_->user_key().ToString(), "a1");
  //ASSERT_EQ("a1", "a1");
  c_iter_->Next();
  ASSERT_TRUE(c_iter_->Valid());
  ASSERT_EQ(c_iter_->user_key().ToString(), "b1");
  ASSERT_EQ(c_iter_->value().ToString(), "B2");
  c_iter_->Next();
  ASSERT_TRUE(c_iter_->Valid());
  ASSERT_EQ(c_iter_->user_key().ToString(), "c1");
  ASSERT_EQ(c_iter_->value().ToString(), "C2");
  c_iter_->Next();
  ASSERT_FALSE(c_iter_->Valid());
}

TEST_F(CompactionIterTest, SimpleCiter2) {
  internal_iter1_ = new TestIterator(BytewiseComparator());
  internal_iter1_->Add("a1", kTypeValue, "A2", 2u);
  internal_iter1_->Add("b1", kTypeValue, "B1", 1u);
  internal_iter1_->Add("c1", kTypeValue, "C1", 1u);
  internal_iter1_->Finish();

  internal_iter2_ = new TestIterator(BytewiseComparator());
  internal_iter2_->Add("a1", kTypeValue, "A3", 3u);
  internal_iter2_->Add("b1", kTypeValue, "B2", 2u);
  internal_iter2_->Add("c1", kTypeValue, "C2", 2u);
  internal_iter2_->Add("a1", kTypeValue, "A1", 1u);
  internal_iter2_->Finish();

  std::vector<InternalIterator*> child_iters;
  child_iters.push_back(internal_iter1_);
  child_iters.push_back(internal_iter2_);
  InternalKeyComparator icomp(BytewiseComparator());
  merge_iter_.reset(NewMergingIterator(&icmp_, &child_iters[0], 2u));

  std::unique_ptr<InternalIterator> unfragmented_range_del_iter(
      new test::VectorIterator({}, {}));
  auto tombstone_list = std::make_shared<FragmentedRangeTombstoneList>(
      std::move(unfragmented_range_del_iter), icmp_);

  std::unique_ptr<FragmentedRangeTombstoneIterator> range_del_iter(
      new FragmentedRangeTombstoneIterator(tombstone_list, icmp_,
        kMaxSequenceNumber));

  range_del_agg_.reset(new CompactionRangeDelAggregator(&icmp_, snapshots_));
  range_del_agg_->AddTombstones(std::move(range_del_iter));

  CompactionFilter* filter = nullptr;

  merge_helper_.reset(
      new MergeHelper(env_, cmp_, nullptr, filter, nullptr, false,
        0, snapshot_checker_.get(),
        0, nullptr, &shutting_down_));

  //std::cout << "MergingIterator is valid? " << merge_iter_->Valid() << std::endl;
  c_iter_.reset(new CompactionIterator(
        merge_iter_.get(), cmp_, merge_helper_.get(), kMaxSequenceNumber, &snapshots_,
        kMaxSequenceNumber, snapshot_checker_.get(),
        env_, false, false,
        range_del_agg_.get()));

  //std::cout << "is valid? " << c_iter_->Valid() << std::endl;
  c_iter_->SeekToFirst();
  //std::cout << "is valid(2)? " << c_iter_->Valid() << std::endl;
  ASSERT_TRUE(c_iter_->Valid());
  ASSERT_EQ(c_iter_->user_key().ToString(), "a1");
  ASSERT_EQ(c_iter_->value().ToString(), "A3");
  c_iter_->Next();
  ASSERT_TRUE(c_iter_->Valid());
  ASSERT_EQ(c_iter_->user_key().ToString(), "b1");
  ASSERT_EQ(c_iter_->value().ToString(), "B2");
  c_iter_->Next();
  ASSERT_TRUE(c_iter_->Valid());
  ASSERT_EQ(c_iter_->user_key().ToString(), "c1");
  ASSERT_EQ(c_iter_->value().ToString(), "C2");
  c_iter_->Next();
  ASSERT_FALSE(c_iter_->Valid());
}
/*
TEST_F(CompactionIterTest, SimpleMemtableToCiter) {
  auto factory = std::make_shared<SkipListFactory>();
  options_.memtable_factory = factory;
  ImmutableCFOptions ioptions(options_);

  WriteBufferManager wb(options_.db_write_buffer_size);
  mem_.reset(new MemTable(icmp_, ioptions, MutableCFOptions(options_), &wb,
                               kMaxSequenceNumber, 0));
  mem_->SetID(0);
  mem_->Ref();

  // Write some keys to this memtable
  mem_->Add(1, kTypeValue, "a2", "A22");
  mem_->Add(3, kTypeValue, "a1", "A1");
  mem_->Add(5, kTypeValue, "a2", "A2");
  mem_->Add(7, kTypeValue, "a3", "A3");
  
  mem2_.reset(new MemTable(icmp_, ioptions, MutableCFOptions(options_), &wb,
                               kMaxSequenceNumber, 0));
  mem2_->SetID(1);
  mem2_->Ref();

  // Write some keys to this memtable
  mem2_->Add(2, kTypeValue, "a2", "B22");
  mem2_->Add(4, kTypeValue, "a1", "B1");
  mem2_->Add(6, kTypeValue, "a2", "B2");
  mem2_->Add(8, kTypeValue, "a3", "B3");


  ReadOptions ro;
  ro.total_order_seek = true;

  memtables.push_back(mem_->NewIterator(ro, &arena_));
  memtables.push_back(mem2_->NewIterator(ro, &arena_));

  std::cout << "memIterator is valid? " << memtables[0]->Valid() << std::endl;
  std::cout << "memIterator2 is valid? " << memtables[1]->Valid() << std::endl;
  memtables[0]->SeekToFirst();
  memtables[1]->SeekToFirst();
  std::cout << "memIterator is valid(2)? " << memtables[0]->Valid() << std::endl;
  std::cout << "memIterator2 is valid(2)? " << memtables[1]->Valid() << std::endl;
  // Scan test


  merge_iter2_.reset(NewMergingIterator(&icmp_, &memtables[0],
        static_cast<int>(memtables.size()), &arena_));

  std::cout << "MergingIterator is valid? " << merge_iter2_->Valid() << std::endl;


  std::unique_ptr<InternalIterator> unfragmented_range_del_iter(
      new test::VectorIterator({}, {}));
  auto tombstone_list = std::make_shared<FragmentedRangeTombstoneList>(
      std::move(unfragmented_range_del_iter), icmp_);

  std::unique_ptr<FragmentedRangeTombstoneIterator> range_del_iter(
      new FragmentedRangeTombstoneIterator(tombstone_list, icmp_,
        kMaxSequenceNumber));

  range_del_agg2_.reset(new CompactionRangeDelAggregator(&icmp_, snapshots2_));
  range_del_agg2_->AddTombstones(std::move(range_del_iter));

  CompactionFilter* filter = nullptr;

  merge_helper2_.reset(
      new MergeHelper(env_, cmp_, nullptr, filter, nullptr, false,
        0, snapshot_checker2_.get(),
        0, nullptr, &shutting_down2_));

  c_iter2_.reset(new CompactionIterator(
        merge_iter2_.get(), cmp_, merge_helper2_.get(), kMaxSequenceNumber, &snapshots2_,
        kMaxSequenceNumber, snapshot_checker2_.get(),
        env_, false, false,
        range_del_agg2_.get()));
  std::cout << "is valid? " << c_iter2_->Valid() << std::endl;
  c_iter2_->SeekToFirst();
  std::cout << "is valid(2)? " << c_iter2_->Valid() << std::endl;
  std::cout << "(k,v) : " << c_iter2_->user_key().ToString() << ", " << c_iter2_->value().ToString() << std::endl; 
  while(c_iter2_->Valid()) {
    std::cout << "(k,v) : " << c_iter2_->user_key().ToString() << ", " << c_iter2_->value().ToString() << std::endl; 
    c_iter2_->Next();
  }
  ASSERT_FALSE(c_iter2_->Valid());
  mem_->Unref();
  mem2_->Unref();
}*/

/*
TEST_F(SstableCompactionIterTest, SingleMemtableToFlush) {
  db_name_ = test::PerThreadDBPath("sstable_compaction_iter_test");
  std::cout << "[JH]\n";
  // 1. Open
  Options options;
  options.create_if_missing =true;
  
  DB* db;
  ASSERT_OK(DB::Open(options, db_name_, &db));
  ColumnFamilyHandle* cfh = reinterpret_cast<DBImpl*>(db)->DefaultColumnFamily();

  // 2. Put queries and prepare sstables
  //db->Put(WriteOptions(), "k1", "v1");
  db->Put(WriteOptions(), cfh, "k1", "v1");
  db->Put(WriteOptions(), cfh, "k2", "v2");
  db->Put(WriteOptions(), cfh, "k3", "v3");
  db->Put(WriteOptions(), cfh, "k4", "v4");
  db->Put(WriteOptions(), cfh, "k1", "v5");
  db->Put(WriteOptions(), cfh, "k2", "v6");
  db->Put(WriteOptions(), cfh, "k3", "v7");
  db->Put(WriteOptions(), cfh, "k1", "v8");
  db->Put(WriteOptions(), cfh, "k2", "v9");
  db->Put(WriteOptions(), cfh, "k1", "v10");

  // 3. Compact SSTables
  db->Flush(FlushOptions());
  // 4. Close
  delete db;
  db = nullptr;
}

TEST_F(SstableCompactionIterTest, TwoMemtablesToFlush) {

  db_name_ = test::PerThreadDBPath("sstable_compaction_iter_test");
  std::cout << "[JH]\n";
  // 1. Open
  Options options;
  options.create_if_missing =true;
  options.max_write_buffer_number = 3;
  
  DB* db;
  ASSERT_OK(DB::Open(options, db_name_, &db));
  DBImpl* dbimpl = reinterpret_cast<DBImpl*>(db);
  ColumnFamilyHandle* cfh = reinterpret_cast<DBImpl*>(db)->DefaultColumnFamily();

  // 2. Put queries and prepare sstables
  //db->Put(WriteOptions(), "k1", "v1");
  db->Put(WriteOptions(), cfh, "k1", "v1");
  db->Put(WriteOptions(), cfh, "k2", "v2");
  db->Put(WriteOptions(), cfh, "k3", "v3");
  db->Put(WriteOptions(), cfh, "k4", "v4");
  db->Put(WriteOptions(), cfh, "k1", "v5");
  db->Put(WriteOptions(), cfh, "k2", "v6");
  db->Put(WriteOptions(), cfh, "k3", "v7");
  db->Put(WriteOptions(), cfh, "k1", "v8");
  db->Put(WriteOptions(), cfh, "k2", "v9");
  db->Put(WriteOptions(), cfh, "k1", "v10");
  dbimpl->TEST_SwitchMemtable();
  // create second memtable
  db->Put(WriteOptions(), cfh, "k1", "v1");
  db->Put(WriteOptions(), cfh, "k2", "v2");
  db->Put(WriteOptions(), cfh, "k3", "v3");
  db->Put(WriteOptions(), cfh, "k4", "v4");
  db->Put(WriteOptions(), cfh, "k1", "v5");
  db->Put(WriteOptions(), cfh, "k2", "v6");
  db->Put(WriteOptions(), cfh, "k3", "v7");
  db->Put(WriteOptions(), cfh, "k1", "v8");
  db->Put(WriteOptions(), cfh, "k2", "v9");
  db->Put(WriteOptions(), cfh, "k1", "v10");

  // 3. Compact SSTables
  db->Flush(FlushOptions());
  // 4. Close
  delete db;
  db = nullptr;
}
*/

TEST_F(SstableCompactionIterTest, TwoSstablesToCompact) {

  db_name_ = test::PerThreadDBPath("sstable_compaction_iter_test") + "_2";
  //std::cout << "[JH]\n";
  // 1. Open
  Options options;
  options.create_if_missing =true;
  options.max_write_buffer_number = 3;
  
  DB* db;
  ASSERT_OK(DB::Open(options, db_name_, &db));
  DBImpl* dbimpl = reinterpret_cast<DBImpl*>(db);
  ColumnFamilyHandle* cfh = reinterpret_cast<DBImpl*>(db)->DefaultColumnFamily();

  // 2. Put queries and prepare sstable
  //db->Put(WriteOptions(), "k1", "v1");
  // (1,2,3,4)
  db->Put(WriteOptions(), cfh, "k3", "v3");
  db->Put(WriteOptions(), cfh, "k3", "v3");
  db->Put(WriteOptions(), cfh, "k1", "v1");
  db->Put(WriteOptions(), cfh, "k3", "v3");
  db->Flush(FlushOptions());
  
  // 3. Prepare another sstable
  // create second memtable
  // (4,3,2,1)
  db->Put(WriteOptions(), cfh, "k1", "v1");
  db->Put(WriteOptions(), cfh, "k1", "v1");
  db->Put(WriteOptions(), cfh, "k1", "v1");
  db->Put(WriteOptions(), cfh, "k2", "v2");
  db->Put(WriteOptions(), cfh, "k2", "v2");
  db->Put(WriteOptions(), cfh, "k2", "v2");
  db->Put(WriteOptions(), cfh, "k1", "v1");
  db->Flush(FlushOptions());
  

  // 4. Compact two sstables
  CompactRangeOptions compact_options;
  compact_options.exclusive_manual_compaction = false;
  db->CompactRange(compact_options, cfh, nullptr, nullptr);
  dbimpl->TEST_WaitForCompact();

  // cnt(k1) = 5
  // cnt(k2) = 5
  // cnt(k3) = 5
  // cnt(k4) = 5
  // 5. Close
  delete db;
  db = nullptr;
}

TEST_F(SstableCompactionIterTest, ThreeSstablesToCompact) {

  db_name_ = test::PerThreadDBPath("sstable_compaction_iter_test") + "_3";
  //std::cout << "[JH]\n";
  // 1. Open
  Options options;
  options.create_if_missing =true;
  options.max_write_buffer_number = 3;
  
  DB* db;
  ASSERT_OK(DB::Open(options, db_name_, &db));
  DBImpl* dbimpl = reinterpret_cast<DBImpl*>(db);
  ColumnFamilyHandle* cfh = reinterpret_cast<DBImpl*>(db)->DefaultColumnFamily();

  // 2. Put queries and prepare sstable
  //db->Put(WriteOptions(), "k1", "v1");
  // (1,2,3,4)
  db->Put(WriteOptions(), cfh, "k3", "v3");
  db->Put(WriteOptions(), cfh, "k2", "v2");
  db->Put(WriteOptions(), cfh, "k4", "v4");
  db->Put(WriteOptions(), cfh, "k4", "v4");
  db->Put(WriteOptions(), cfh, "k2", "v2");
  db->Put(WriteOptions(), cfh, "k3", "v3");
  db->Put(WriteOptions(), cfh, "k4", "v4");
  db->Put(WriteOptions(), cfh, "k3", "v3");
  db->Put(WriteOptions(), cfh, "k4", "v4");
  db->Flush(FlushOptions());

  // 3. Prepare another sstable
  // create second memtable
  // (4,3,2,1)
  db->Put(WriteOptions(), cfh, "k3", "v3");
  db->Put(WriteOptions(), cfh, "k1", "v1");
  db->Put(WriteOptions(), cfh, "k1", "v1");
  db->Put(WriteOptions(), cfh, "k1", "v1");
  db->Put(WriteOptions(), cfh, "k1", "v1");
  db->Put(WriteOptions(), cfh, "k3", "v3");
  db->Flush(FlushOptions());

  // (4,3,2,1)
  db->Put(WriteOptions(), cfh, "k1", "v4");
  db->Put(WriteOptions(), cfh, "k1", "v6");
  db->Put(WriteOptions(), cfh, "k2", "v2");
  db->Put(WriteOptions(), cfh, "k2", "v1");
  db->Put(WriteOptions(), cfh, "k2", "v1");
  db->Put(WriteOptions(), cfh, "k3", "v4");
  db->Put(WriteOptions(), cfh, "k3", "v5");
  db->Put(WriteOptions(), cfh, "k4", "v5");
  db->Put(WriteOptions(), cfh, "k1", "v7");
  db->Put(WriteOptions(), cfh, "k1", "v3");
  db->Flush(FlushOptions());

  // 4. Compact two sstables
  CompactRangeOptions compact_options;
  compact_options.exclusive_manual_compaction = false;
  db->CompactRange(compact_options, cfh, nullptr, nullptr);
  dbimpl->TEST_WaitForCompact();

  // cnt(k1) = 9
  // cnt(k2) = 8
  // cnt(k3) = 7
  // cnt(k4) = 6
  // 5. Close
  delete db;
  db = nullptr;
}

TEST_F(SstableCompactionIterTest, TwoLevelsToCompact) {

  db_name_ = test::PerThreadDBPath("sstable_compaction_iter_test") + "_2lvl";
  //std::cout << "[JH]\n";
  // 1. Open
  Options options;
  options.create_if_missing =true;
  options.max_write_buffer_number = 3;
  
  DB* db;
  ASSERT_OK(DB::Open(options, db_name_, &db));
  DBImpl* dbimpl = reinterpret_cast<DBImpl*>(db);
  ColumnFamilyHandle* cfh = reinterpret_cast<DBImpl*>(db)->DefaultColumnFamily();

  // 2. Put queries and prepare sstable
  //db->Put(WriteOptions(), "k1", "v1");
  // (1,2,3,4)
  db->Put(WriteOptions(), cfh, "k3", "v3");
  db->Put(WriteOptions(), cfh, "k2", "v2");
  db->Put(WriteOptions(), cfh, "k4", "v4");
  db->Put(WriteOptions(), cfh, "k4", "v4");
  db->Put(WriteOptions(), cfh, "k2", "v2");
  db->Put(WriteOptions(), cfh, "k3", "v3");
  db->Put(WriteOptions(), cfh, "k4", "v4");
  db->Put(WriteOptions(), cfh, "k3", "v3");
  db->Put(WriteOptions(), cfh, "k4", "v4");
  db->Flush(FlushOptions());

  // 3. Prepare another sstable
  // create second memtable
  // (4,3,2,1)
  db->Put(WriteOptions(), cfh, "k3", "v3");
  db->Put(WriteOptions(), cfh, "k1", "v1");
  db->Put(WriteOptions(), cfh, "k1", "v1");
  db->Put(WriteOptions(), cfh, "k1", "v1");
  db->Put(WriteOptions(), cfh, "k4", "v4");
  db->Put(WriteOptions(), cfh, "k1", "v1");
  db->Put(WriteOptions(), cfh, "k3", "v3");
  db->Flush(FlushOptions());

  // (4,3,2,1)
  db->Put(WriteOptions(), cfh, "k2", "v2");
  db->Put(WriteOptions(), cfh, "k2", "v1");
  db->Put(WriteOptions(), cfh, "k2", "v1");
  db->Put(WriteOptions(), cfh, "k3", "v4");
  db->Put(WriteOptions(), cfh, "k3", "v5");
  db->Flush(FlushOptions());

  // 4. Compact two sstables
  CompactRangeOptions compact_options;
  compact_options.exclusive_manual_compaction = false;
  db->CompactRange(compact_options, cfh, nullptr, nullptr);
  dbimpl->TEST_WaitForCompact();

  // 5. Prepare another sstable
  // create second memtable
  // (3,1,3,1)
  db->Put(WriteOptions(), cfh, "k3", "v3");
  db->Put(WriteOptions(), cfh, "k1", "v1");
  db->Put(WriteOptions(), cfh, "k1", "v1");
  db->Put(WriteOptions(), cfh, "k2", "v2");
  db->Put(WriteOptions(), cfh, "k4", "v4");
  db->Put(WriteOptions(), cfh, "k1", "v1");
  db->Put(WriteOptions(), cfh, "k3", "v3");
  db->Put(WriteOptions(), cfh, "k3", "v3");
  db->Flush(FlushOptions());


  // 6. Compact two sstables
  db->CompactRange(compact_options, cfh, nullptr, nullptr);
  dbimpl->TEST_WaitForCompact();


  // cnt(k1) = 12
  // cnt(k2) = 9
  // cnt(k3) = 10
  // cnt(k4) = 7

  // 7. Close
  delete db;
  db = nullptr;
}

/*

TEST_F(DBIteratorTest, SeekPrefixTombstones) {
  ReadOptions ro;
  Options options;
  options.prefix_extractor.reset(NewNoopTransform());
  TestIterator* internal_iter = new TestIterator(BytewiseComparator());
  internal_iter->AddDeletion("b");
  internal_iter->AddDeletion("c");
  internal_iter->AddDeletion("d");
  internal_iter->AddDeletion("e");
  internal_iter->AddDeletion("f");
  internal_iter->AddDeletion("g");
  internal_iter->Finish();

  ro.prefix_same_as_start = true;
  std::unique_ptr<Iterator> db_iter(NewDBIterator(
      env_, ro, ImmutableCFOptions(options), MutableCFOptions(options),
      BytewiseComparator(), internal_iter, 10,
      options.max_sequential_skip_in_iterations, nullptr  ));

  int skipped_keys = 0;

  get_perf_context()->Reset();
  db_iter->SeekForPrev("z");
  skipped_keys =
      static_cast<int>(get_perf_context()->internal_key_skipped_count);
  ASSERT_EQ(skipped_keys, 0);

  get_perf_context()->Reset();
  db_iter->Seek("a");
  skipped_keys =
      static_cast<int>(get_perf_context()->internal_key_skipped_count);
  ASSERT_EQ(skipped_keys, 0);
}


TEST_F(DBIteratorTest, SeekToFirstLowerBound) {
  const int kNumKeys = 3;
  for (int i = 0; i < kNumKeys + 2; ++i) {
    // + 2 for two special cases: lower bound before and lower bound after the
    // internal iterator's keys
    TestIterator* internal_iter = new TestIterator(BytewiseComparator());
    for (int j = 1; j <= kNumKeys; ++j) {
      internal_iter->AddPut(std::to_string(j), "val");
    }
    internal_iter->Finish();

    ReadOptions ro;
    auto lower_bound_str = std::to_string(i);
    Slice lower_bound(lower_bound_str);
    ro.iterate_lower_bound = &lower_bound;
    Options options;
    std::unique_ptr<Iterator> db_iter(NewDBIterator(
        env_, ro, ImmutableCFOptions(options), MutableCFOptions(options),
        BytewiseComparator(), internal_iter, 10 ,
        options.max_sequential_skip_in_iterations,
        nullptr ));

    db_iter->SeekToFirst();
    if (i == kNumKeys + 1) {
      // lower bound was beyond the last key
      ASSERT_FALSE(db_iter->Valid());
    } else {
      ASSERT_TRUE(db_iter->Valid());
      int expected;
      if (i == 0) {
        // lower bound was before the first key
        expected = 1;
      } else {
        // lower bound was at the ith key
        expected = i;
      }
      ASSERT_EQ(std::to_string(expected), db_iter->key().ToString());
    }
  }
}

TEST_F(DBIteratorTest, PrevLowerBound) {
  const int kNumKeys = 3;
  const int kLowerBound = 2;
  TestIterator* internal_iter = new TestIterator(BytewiseComparator());
  for (int j = 1; j <= kNumKeys; ++j) {
    internal_iter->AddPut(std::to_string(j), "val");
  }
  internal_iter->Finish();

  ReadOptions ro;
  auto lower_bound_str = std::to_string(kLowerBound);
  Slice lower_bound(lower_bound_str);
  ro.iterate_lower_bound = &lower_bound;
  Options options;
  std::unique_ptr<Iterator> db_iter(NewDBIterator(
      env_, ro, ImmutableCFOptions(options), MutableCFOptions(options),
      BytewiseComparator(), internal_iter, 10 ,
      options.max_sequential_skip_in_iterations, nullptr ));

  db_iter->SeekToLast();
  for (int i = kNumKeys; i >= kLowerBound; --i) {
    ASSERT_TRUE(db_iter->Valid());
    ASSERT_EQ(std::to_string(i), db_iter->key().ToString());
    db_iter->Prev();
  }
  ASSERT_FALSE(db_iter->Valid());
}

TEST_F(DBIteratorTest, SeekLessLowerBound) {
  const int kNumKeys = 3;
  const int kLowerBound = 2;
  TestIterator* internal_iter = new TestIterator(BytewiseComparator());
  for (int j = 1; j <= kNumKeys; ++j) {
    internal_iter->AddPut(std::to_string(j), "val");
  }
  internal_iter->Finish();

  ReadOptions ro;
  auto lower_bound_str = std::to_string(kLowerBound);
  Slice lower_bound(lower_bound_str);
  ro.iterate_lower_bound = &lower_bound;
  Options options;
  std::unique_ptr<Iterator> db_iter(NewDBIterator(
      env_, ro, ImmutableCFOptions(options), MutableCFOptions(options),
      BytewiseComparator(), internal_iter, 10 ,
      options.max_sequential_skip_in_iterations, nullptr ));

  auto before_lower_bound_str = std::to_string(kLowerBound - 1);
  Slice before_lower_bound(lower_bound_str);

  db_iter->Seek(before_lower_bound);
  ASSERT_TRUE(db_iter->Valid());
  ASSERT_EQ(lower_bound_str, db_iter->key().ToString());
}

TEST_F(DBIteratorTest, ReverseToForwardWithDisappearingKeys) {
  Options options;
  options.prefix_extractor.reset(NewCappedPrefixTransform(0));

  TestIterator* internal_iter = new TestIterator(BytewiseComparator());
  internal_iter->AddPut("a", "A");
  internal_iter->AddPut("b", "B");
  for (int i = 0; i < 100; ++i) {
    internal_iter->AddPut("c" + ToString(i), "");
  }
  internal_iter->Finish();

  std::unique_ptr<Iterator> db_iter(NewDBIterator(
      env_, ReadOptions(), ImmutableCFOptions(options),
      MutableCFOptions(options), BytewiseComparator(), internal_iter, 10,
      options.max_sequential_skip_in_iterations, nullptr  ));

  db_iter->SeekForPrev("a");
  ASSERT_TRUE(db_iter->Valid());
  ASSERT_OK(db_iter->status());
  ASSERT_EQ("a", db_iter->key().ToString());

  internal_iter->Vanish("a");
  db_iter->Next();
  ASSERT_TRUE(db_iter->Valid());
  ASSERT_OK(db_iter->status());
  ASSERT_EQ("b", db_iter->key().ToString());

  // A (sort of) bug used to cause DBIter to pointlessly drag the internal
  // iterator all the way to the end. But this doesn't really matter at the time
  // of writing because the only iterator that can see disappearing keys is
  // ForwardIterator, which doesn't support SeekForPrev().
  EXPECT_LT(internal_iter->steps(), 20);
}*/

}  // namespace rocksdb

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
