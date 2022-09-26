// Copyright (c) 2011-present, Facebook, Inc. All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#ifndef ROCKSDB_LITE

#include <inttypes.h>
#include <iostream>

#include "db/db_impl.h"
#include "rocksdb/db.h"
#include "rocksdb/sst_file_reader.h"
#include "rocksdb/sst_file_writer.h"
#include "table/sst_file_writer_collectors.h"
#include "util/testharness.h"
#include "util/testutil.h"
#include "utilities/merge_operators.h"

namespace rocksdb {

std::string EncodeAsString(uint64_t v) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%08" PRIu64, v);
  return std::string(buf);
}

std::string EncodeAsUint64(uint64_t v) {
  std::string dst;
  PutFixed64(&dst, v);
  return dst;
}

class SstFileSplitTest : public testing::Test {
 public:
  SstFileSplitTest() {
    options_.merge_operator = MergeOperators::CreateUInt64AddOperator();
    sst_name_ = test::PerThreadDBPath("sst_file");
  }

  ~SstFileSplitTest() {
    //Status s = Env::Default()->DeleteFile(sst_name_);
    //assert(s.ok());
  }

  DBImpl* dbfull(DB* db) { return reinterpret_cast<DBImpl*>(db) ;};

  void CreateMT(DB* db, ColumnFamilyHandle* cfh, const std::vector<std::string>& keys) {
    for (size_t i = 0; i < keys.size(); i ++) {
      db->Put(WriteOptions(), cfh, Slice(keys[i]), Slice(keys[i]));
    }

    /*for (size_t i = 0; i + 2 < keys.size(); i += 3) {
      db->Put(WriteOptions(), cfh, Slice(keys[i]), Slice(keys[i]));
      db->Merge(WriteOptions(), cfh, Slice(keys[i+1]), Slice(keys[i]));
      db->Delete(WriteOptions(), cfh, Slice(keys[i+2]));
    }*/
  }

  void CreateFile(const std::string& file_name,
                  const std::vector<std::string>& keys, int level) {
    SstFileWriter writer(soptions_, options_);
    ASSERT_OK(writer.Open(file_name, level));
    for (size_t i = 0; i + 2 < keys.size(); i += 3) {
      ASSERT_OK(writer.Put(keys[i], keys[i]));
      ASSERT_OK(writer.Merge(keys[i + 1], EncodeAsUint64(i + 1)));
      ASSERT_OK(writer.Delete(keys[i + 2]));
    }
    ASSERT_OK(writer.Finish());
  }


  void CreateFile(const std::string& file_name,
                  const std::vector<std::string>& keys) {
    SstFileWriter writer(soptions_, options_);
    ASSERT_OK(writer.Open(file_name));
    for (size_t i = 0; i + 2 < keys.size(); i += 3) {
      ASSERT_OK(writer.Put(keys[i], keys[i]));
      ASSERT_OK(writer.Merge(keys[i + 1], EncodeAsUint64(i + 1)));
      ASSERT_OK(writer.Delete(keys[i + 2]));
    }
    ASSERT_OK(writer.Finish());
  }

  void GetSstFiles(Env* env, std::string path,
                             std::vector<std::string>* files) {
    env->GetChildren(path, files);

    files->erase(
        std::remove_if(files->begin(), files->end(), [](std::string name) {
          uint64_t number;
          FileType type;
          return !(ParseFileName(name, &number, &type) && type == kTableFile);
        }), files->end());
  }


  int GetSstFileCount(std::string path) {
    std::vector<std::string> files;
    GetSstFiles(Env::Default(), path, &files);
    return static_cast<int>(files.size());
  }

  void CreateFileCF(const std::string& file_name,
                  const std::vector<std::string>& keys,
                  ColumnFamilyHandle* cfh 
                  ) {
    SstFileWriter writer(soptions_, options_, cfh);
    ASSERT_OK(writer.Open(file_name));
    for (size_t i = 0; i + 2 < keys.size(); i += 3) {
      ASSERT_OK(writer.Put(keys[i], keys[i]));
      ASSERT_OK(writer.Merge(keys[i + 1], EncodeAsUint64(i + 1)));
      ASSERT_OK(writer.Delete(keys[i + 2]));
    }
    ASSERT_OK(writer.Finish());
  }

  void CheckFile(const std::string& file_name,
                 const std::vector<std::string>& keys,
                 bool check_global_seqno = false) {
    ReadOptions ropts;
    SstFileReader reader(options_);
    ASSERT_OK(reader.Open(file_name));
    ASSERT_OK(reader.VerifyChecksum());
    std::unique_ptr<Iterator> iter(reader.NewIterator(ropts));
    iter->SeekToFirst();
    for (size_t i = 0; i + 2 < keys.size(); i += 3) {
      ASSERT_TRUE(iter->Valid());
      ASSERT_EQ(iter->key().compare(keys[i]), 0);
      ASSERT_EQ(iter->value().compare(keys[i]), 0);
      iter->Next();
      ASSERT_TRUE(iter->Valid());
      ASSERT_EQ(iter->key().compare(keys[i + 1]), 0);
      ASSERT_EQ(iter->value().compare(EncodeAsUint64(i + 1)), 0);
      iter->Next();
    }
    ASSERT_FALSE(iter->Valid());
    if (check_global_seqno) {
      auto properties = reader.GetTableProperties();
      std::cout << "[CheckFile] property cf id   : " << properties->column_family_id << std::endl;
      std::cout << "[CheckFile] property cf name : " << properties->column_family_name << std::endl;
      ASSERT_TRUE(properties);
      auto& user_properties = properties->user_collected_properties;
      ASSERT_TRUE(
          user_properties.count(ExternalSstFilePropertyNames::kGlobalSeqno));
    }
  }

  void CreateFileAndCheck(const std::vector<std::string>& keys) {
    CreateFile(sst_name_, keys);
    CheckFile(sst_name_, keys);
  }

 protected:
  Options options_;
  EnvOptions soptions_;
  std::string sst_name_;
};

const uint64_t kNumKeys = 1000*1000;


TEST_F(SstFileSplitTest, SplitColumnFamilyBackground) {
  std::vector<std::string> keys;
  for (uint64_t i = 0; i < kNumKeys; i++) {
    keys.emplace_back(EncodeAsString(i));
  }

  Options options;
  options.create_if_missing = true;
  std::string db_name = test::PerThreadDBPath("test_db");
  DB* db;
  ASSERT_OK(DB::Open(options, db_name, &db));

  ColumnFamilyHandle* cfh;
  std::string cf_name = "cf_anon";

  std::unique_ptr<ColumnFamilyOptions> cfo(new ColumnFamilyOptions());
  cfo->compaction_style = kCompactionStyleLevel;
  cfo->num_levels = 2;
  cfo->write_buffer_size = 64 << 20; // 64MB
  cfo->level0_file_num_compaction_trigger = 4;
  cfo->target_file_size_base = 4*1024*1024; // 4MB
  cfo->report_bg_io_stats = true;

  db->CreateColumnFamily(*(cfo.get()), cf_name, &cfh);

  CreateMT(db, cfh, keys);
  db->Flush(FlushOptions(), cfh);
  dbfull(db)->TEST_WaitForCompact();
  
  CreateMT(db, cfh, keys);
  db->Flush(FlushOptions(), cfh);
  dbfull(db)->TEST_WaitForCompact();

  CreateMT(db, cfh, keys);
  db->Flush(FlushOptions(), cfh);
  dbfull(db)->TEST_WaitForCompact();

  CreateMT(db, cfh, keys);
  db->Flush(FlushOptions(), cfh);
  dbfull(db)->TEST_WaitForCompact();

  std::cout << "After compaction: sst count : " << GetSstFileCount(db->GetName()) << std::endl;

  db->DestroyColumnFamilyHandle(cfh);

  delete db;
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
          "SKIPPED as SstFileSplit is not supported in ROCKSDB_LITE\n");
  return 0;
}

#endif  // ROCKSDB_LITE
