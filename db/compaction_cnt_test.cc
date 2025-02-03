//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#ifndef ROCKSDB_LITE

#include <algorithm>
#include <map>
#include <string>
#include <tuple>
#include <iostream>
#include <vector>

#include "db/column_family.h"
#include "db/compaction_job.h"
#include "db/error_handler.h"
#include "db/version_set.h"
#include "rocksdb/cache.h"
#include "rocksdb/db.h"
#include "rocksdb/options.h"
#include "rocksdb/write_buffer_manager.h"
#include "table/mock_table.h"
#include "util/file_reader_writer.h"
#include "util/string_util.h"
#include "util/testharness.h"
#include "util/testutil.h"
#include "utilities/merge_operators.h"

namespace rocksdb {

// TODO(icanadi) Make it simpler once we mock out VersionSet
class CompactionCntTest : public testing::Test {
 public:
  CompactionCntTest()
      : env_(Env::Default()),
        dbname_(test::PerThreadDBPath("compaction_job_test")),
        db_options_(),
        mutable_cf_options_(cf_options_),
        table_cache_(NewLRUCache(50000, 16)),
        write_buffer_manager_(db_options_.db_write_buffer_size),
        versions_(new VersionSet(dbname_, &db_options_, env_options_,
                                 table_cache_.get(), &write_buffer_manager_,
                                 &write_controller_)),
        shutting_down_(false),
        preserve_deletes_seqnum_(0),
        mock_table_factory_(new mock::MockTableFactory()),
        error_handler_(nullptr, db_options_, &mutex_) {
    EXPECT_OK(env_->CreateDirIfMissing(dbname_));
    db_options_.db_paths.emplace_back(dbname_,
                                      std::numeric_limits<uint64_t>::max());
  }

  std::string GenerateFileName(uint64_t file_number) {
    FileMetaData meta;
    std::vector<DbPath> db_paths;
    db_paths.emplace_back(dbname_, std::numeric_limits<uint64_t>::max());
    meta.fd = FileDescriptor(file_number, 0, 0);
    return TableFileName(db_paths, meta.fd.GetNumber(), meta.fd.GetPathId());
  }

  std::string KeyStr(const std::string& user_key, const SequenceNumber seq_num,
      const ValueType t) {
    return InternalKey(user_key, seq_num, t).Encode().ToString();
  }

  void AddMockFile(const stl_wrappers::KVMap& contents, int level = 0) {
    assert(contents.size() > 0);

    bool first_key = true;
    std::string smallest, largest;
    InternalKey smallest_key, largest_key;
    SequenceNumber smallest_seqno = kMaxSequenceNumber;
    SequenceNumber largest_seqno = 0;
    for (auto kv : contents) {
      ParsedInternalKey key;
      std::string skey;
      std::string value;
      std::tie(skey, value) = kv;
      ParseInternalKey(skey, &key);

      smallest_seqno = std::min(smallest_seqno, key.sequence);
      largest_seqno = std::max(largest_seqno, key.sequence);

      if (first_key ||
          cfd_->user_comparator()->Compare(key.user_key, smallest) < 0) {
        smallest.assign(key.user_key.data(), key.user_key.size());
        smallest_key.DecodeFrom(skey);
      }
      if (first_key ||
          cfd_->user_comparator()->Compare(key.user_key, largest) > 0) {
        largest.assign(key.user_key.data(), key.user_key.size());
        largest_key.DecodeFrom(skey);
      }

      first_key = false;
    }

    uint64_t file_number = versions_->NewFileNumber();
    EXPECT_OK(mock_table_factory_->CreateMockTable(
        env_, GenerateFileName(file_number), std::move(contents)));

    VersionEdit edit;
    edit.AddFile(level, file_number, 0, 10, smallest_key, largest_key,
        smallest_seqno, largest_seqno, false);

    mutex_.Lock();
    versions_->LogAndApply(versions_->GetColumnFamilySet()->GetDefault(),
                           mutable_cf_options_, &edit, &mutex_);
    mutex_.Unlock();
  }

  void SetLastSequence(const SequenceNumber sequence_number) {
    versions_->SetLastAllocatedSequence(sequence_number + 1);
    versions_->SetLastPublishedSequence(sequence_number + 1);
    versions_->SetLastSequence(sequence_number + 1);
  }

  // returns expected result after compaction
  stl_wrappers::KVMap CreateTwoFiles(bool gen_corrupted_keys) {
    auto expected_results = mock::MakeMockFile();
    const int kKeysPerFile = 10000;
    const int kCorruptKeysPerFile = 200;
    const int kMatchingKeys = kKeysPerFile / 2;
    SequenceNumber sequence_number = 0;

    auto corrupt_id = [&](int id) {
      return gen_corrupted_keys && id > 0 && id <= kCorruptKeysPerFile;
    };

    for (int i = 0; i < 2; ++i) {
      auto contents = mock::MakeMockFile();
      for (int k = 0; k < kKeysPerFile; ++k) {
        auto key = ToString(i * kMatchingKeys + k);
        auto value = ToString(i * kKeysPerFile + k);
        InternalKey internal_key(key, ++sequence_number, kTypeValue);

        // This is how the key will look like once it's written in bottommost
        // file
        InternalKey bottommost_internal_key(
            key, 0, kTypeValue);

        if (corrupt_id(k)) {
          test::CorruptKeyType(&internal_key);
          test::CorruptKeyType(&bottommost_internal_key);
        }
        contents.insert({ internal_key.Encode().ToString(), value });
        if (i == 1 || k < kMatchingKeys || corrupt_id(k - kMatchingKeys)) {
          expected_results.insert(
              { bottommost_internal_key.Encode().ToString(), value });
        }
      }

      AddMockFile(contents);
    }

    SetLastSequence(sequence_number);

    return expected_results;
  }

  void NewDB() {
    VersionEdit new_db;
    new_db.SetLogNumber(0);
    new_db.SetNextFile(2);
    new_db.SetLastSequence(0);

    const std::string manifest = DescriptorFileName(dbname_, 1);
    std::unique_ptr<WritableFile> file;
    Status s = env_->NewWritableFile(
        manifest, &file, env_->OptimizeForManifestWrite(env_options_));
    ASSERT_OK(s);
    std::unique_ptr<WritableFileWriter> file_writer(
        new WritableFileWriter(std::move(file), manifest, env_options_));
    {
      log::Writer log(std::move(file_writer), 0, false);
      std::string record;
      new_db.EncodeTo(&record);
      s = log.AddRecord(record);
    }
    ASSERT_OK(s);
    // Make "CURRENT" file that points to the new manifest file.
    s = SetCurrentFile(env_, dbname_, 1, nullptr);

    std::vector<ColumnFamilyDescriptor> column_families;
    cf_options_.table_factory = mock_table_factory_;
    cf_options_.merge_operator = merge_op_;
    cf_options_.compaction_filter = compaction_filter_.get();
    column_families.emplace_back(kDefaultColumnFamilyName, cf_options_);

    EXPECT_OK(versions_->Recover(column_families, false));
    cfd_ = versions_->GetColumnFamilySet()->GetDefault();
  }

  void RunCompaction(
      const std::vector<std::vector<FileMetaData*>>& input_files,
      std::vector<int>& input_levels,
      int output_level,
      const std::vector<SequenceNumber>& snapshots = {},
      SequenceNumber earliest_write_conflict_snapshot = kMaxSequenceNumber) {
    auto cfd = versions_->GetColumnFamilySet()->GetDefault();

    size_t num_input_files = 0;
    std::vector<CompactionInputFiles> compaction_input_files;
    for (size_t level = 0; level < input_files.size(); level++) {
      auto level_files = input_files[level];
      CompactionInputFiles compaction_level;
      compaction_level.level = input_levels[level];
      compaction_level.files.insert(compaction_level.files.end(),
          level_files.begin(), level_files.end());
      compaction_input_files.push_back(compaction_level);
      num_input_files += level_files.size();
    }

    Compaction compaction(cfd->current()->storage_info(), *cfd->ioptions(),
                          *cfd->GetLatestMutableCFOptions(),
                          compaction_input_files, output_level, 1024 * 1024,
                          10 * 1024 * 1024, 0, kNoCompression,
                          cfd->ioptions()->compression_opts, 0, {}, true);
    compaction.SetInputVersion(cfd->current());

    LogBuffer log_buffer(InfoLogLevel::INFO_LEVEL, db_options_.info_log.get());
    mutex_.Lock();
    EventLogger event_logger(db_options_.info_log.get());
    // TODO(yiwu) add a mock snapshot checker and add test for it.
    ColumnFamilyData* cfd_to_split=nullptr;
    std::vector<FileMetaData*> sst_split_files;

    SnapshotChecker* snapshot_checker = nullptr;
    CompactionJob compaction_job(
        0, &compaction, db_options_, env_options_, versions_.get(),
        &shutting_down_, preserve_deletes_seqnum_, &log_buffer, nullptr,
        nullptr, nullptr, &mutex_, &error_handler_, snapshots,
        earliest_write_conflict_snapshot, snapshot_checker, table_cache_,
        &event_logger, false, false, dbname_, &compaction_job_stats_,
        Env::Priority::USER,
        sst_split_files, &cfd_to_split);

    compaction_job.Prepare();
    mutex_.Unlock();
    Status s;
    s = compaction_job.Run();
    ASSERT_OK(s);
    mutex_.Lock();
    ASSERT_OK(compaction_job.Install(*cfd->GetLatestMutableCFOptions()));
    mutex_.Unlock();

    

    mock_table_factory_->ScanAllFiles();
  }

  Env* env_;
  std::string dbname_;
  EnvOptions env_options_;
  ImmutableDBOptions db_options_;
  ColumnFamilyOptions cf_options_;
  MutableCFOptions mutable_cf_options_;
  std::shared_ptr<Cache> table_cache_;
  WriteController write_controller_;
  WriteBufferManager write_buffer_manager_;
  std::unique_ptr<VersionSet> versions_;
  InstrumentedMutex mutex_;
  std::atomic<bool> shutting_down_;
  SequenceNumber preserve_deletes_seqnum_;
  std::shared_ptr<mock::MockTableFactory> mock_table_factory_;
  CompactionJobStats compaction_job_stats_;
  ColumnFamilyData* cfd_;
  std::unique_ptr<CompactionFilter> compaction_filter_;
  std::shared_ptr<MergeOperator> merge_op_;
  ErrorHandler error_handler_;
};

TEST_F(CompactionCntTest, SimpleNonLastLevel) {
  NewDB();
  auto file0 = mock::MakeMockFile({
      {KeyStr("a", 0, kTypeValue), "val5"},
      {KeyStr("b", 0, kTypeValue), "val6"},
  });
  AddMockFile(file0, 2);

  for(int i=0; i<100; i++){
    auto file1 = mock::MakeMockFile({
        {KeyStr("a", 5U + i, kTypeValue), "val2"},
        {KeyStr("b", 6U + i, kTypeValue), "val3"},
        });
    AddMockFile(file1, 1);
    auto lvl0_files = cfd_->current()->storage_info()->LevelFiles(1);
    auto lvl1_files = cfd_->current()->storage_info()->LevelFiles(2);
    std::vector<int> input_levels = {1, 2};
    RunCompaction({lvl0_files, lvl1_files}, input_levels, 2);
  }
}
}  // namespace rocksdb

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

#else
#include <stdio.h>

int main(int /*argc*/, char** /*argv*/) {
  fprintf(stderr,
          "SKIPPED as CompactionJobStats is not supported in ROCKSDB_LITE\n");
  return 0;
}

#endif  // ROCKSDB_LITE
