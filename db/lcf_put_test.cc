// Copyright (c) 2011-present, Facebook, Inc. All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#ifndef ROCKSDB_LITE

#include <inttypes.h>
#include <iostream>
#include <sstream>
#include <iomanip>

#include "db/db_impl.h"
#include "db/version_set.h"
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

class LCFPutTest : public testing::Test {
 public:
  LCFPutTest() {
    options_.merge_operator = MergeOperators::CreateUInt64AddOperator();
    sst_name_ = test::PerThreadDBPath("sst_file");
  }

  ~LCFPutTest() {
    //Status s = Env::Default()->DeleteFile(sst_name_);
    //assert(s.ok());
  }

  DBImpl* dbfull(DB* db) { return reinterpret_cast<DBImpl*>(db) ;};

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

TEST_F(LCFPutTest, ThreeLevelAfterPut) {
  Options options;
  options.create_if_missing = true;
  options.allow_column_family_split = true;
  options.atomic_flush = true;

  std::string db_name = test::PerThreadDBPath("test_db");
  DB* db;
  ASSERT_OK(DB::Open(options, db_name, &db));

  ColumnFamilyHandle* cfh = dbfull(db)->DefaultColumnFamily();
  ColumnFamilyData* cfd =
      static_cast<ColumnFamilyHandleImpl*>(cfh)->cfd();

  int numItem = 1000;
  std::vector<std::string> keys;
  std::vector<std::string> values1;
  std::vector<std::string> values2;
  std::vector<std::string> values3;

  for(int i = 1000; i < 1000 + numItem; i++) {
    std::string k =  "user" + std::to_string(i);
    std::string v = "v" + k;
    std::string v1 = v + std::to_string(1);
    std::string v2 = v + std::to_string(2);
    std::string v3 = v + std::to_string(3);
    keys.push_back(k); 
    values1.push_back(v1); 
    values2.push_back(v2); 
    values3.push_back(v3); 
  }

  for(int i = 0; i < numItem; i++) {
    db->Put(WriteOptions(), cfh, Slice(keys[i]), Slice(values1[i]));
  }

  std::vector<SplitFileInfo> infos;

  FileMetaData* f1 = new FileMetaData;
  std::string s1 = "user1200";
  std::string l1 = "user1400";
  f1->smallest = InternalKey(Slice(s1), 0, kTypeValue);
  f1->largest = InternalKey(Slice(l1), 0, kTypeValue);
  infos.push_back(SplitFileInfo(f1, cfd));
  dbfull(db)->SplitColumnFamilyFromSstFiles(infos);
  dbfull(db)->TEST_WaitForSplit();
  sleep(5); 
  ColumnFamilyData* cfd1 = cfd->GetColumnFamilySet()->GetColumnFamily(1);
  infos.clear();

  std::string res;
  for(int i = 0; i < numItem; i++) {
    db->Get(ReadOptions(), keys[i], &res);
    assert(res == values1[i]);

    db->Put(WriteOptions(), cfh, Slice(keys[i]), Slice(values2[i]));
  }


  FileMetaData* f2 = new FileMetaData;
  std::string s2 = "user1250";
  std::string l2 = "user1280";
  f2->smallest = InternalKey(Slice(s2), 0, kTypeValue);
  f2->largest = InternalKey(Slice(l2), 0, kTypeValue);
  infos.push_back(SplitFileInfo(f2, cfd1));
  dbfull(db)->SplitColumnFamilyFromSstFiles(infos);
  dbfull(db)->TEST_WaitForSplit();
  sleep(5); 

  for(int i = 0; i < numItem; i++) {
    db->Get(ReadOptions(), keys[i], &res);
    assert(res == values2[i]);

    db->Put(WriteOptions(), cfh, Slice(keys[i]), Slice(values3[i]));
  }
  infos.clear();


  FileMetaData* f3 = new FileMetaData;
  std::string s3 = "user1001";
  std::string l3 = "user1500";
  f3->smallest = InternalKey(Slice(s3), 0, kTypeValue);
  f3->largest = InternalKey(Slice(l3), 0, kTypeValue);
  infos.push_back(SplitFileInfo(f3, cfd));
  dbfull(db)->SplitColumnFamilyFromSstFiles(infos);
  dbfull(db)->TEST_WaitForSplit();
  sleep(5); 
  infos.clear();

  for(int i = 0; i < numItem; i++) {
    db->Get(ReadOptions(), keys[i], &res);
    assert(res == values3[i]);
  }

  DestroyDB(db_name, options);
  delete db;
}

TEST_F(LCFPutTest, ThreeLevelAfterPut2) {
  Options options;
  options.create_if_missing = true;
  options.allow_column_family_split = true;
  options.atomic_flush = true;

  std::string db_name = test::PerThreadDBPath("test_db");
  DB* db;
  ASSERT_OK(DB::Open(options, db_name, &db));

  ColumnFamilyHandle* cfh = dbfull(db)->DefaultColumnFamily();
  ColumnFamilyData* cfd =
      static_cast<ColumnFamilyHandleImpl*>(cfh)->cfd();

  int numItem = 1000;
  std::vector<std::string> keys;
  std::vector<std::string> values1;
  std::vector<std::string> values2;
  std::vector<std::string> values3;

  for(int i = 1000; i < 1000 + numItem; i++) {
    std::string k =  "user" + std::to_string(i);
    std::string v = "v" + k;
    std::string v1 = v + std::to_string(1);
    std::string v2 = v + std::to_string(2);
    std::string v3 = v + std::to_string(3);
    keys.push_back(k); 
    values1.push_back(v1); 
    values2.push_back(v2); 
    values3.push_back(v3); 
  }

  for(int i = 0; i < numItem; i++) {
    db->Put(WriteOptions(), cfh, Slice(keys[i]), Slice(values1[i]));
  }

  std::vector<SplitFileInfo> infos;

  FileMetaData* f1 = new FileMetaData;
  std::string s1 = "user1200";
  std::string l1 = "user1400";
  f1->smallest = InternalKey(Slice(s1), 0, kTypeValue);
  f1->largest = InternalKey(Slice(l1), 0, kTypeValue);
  infos.push_back(SplitFileInfo(f1, cfd));
  dbfull(db)->SplitColumnFamilyFromSstFiles(infos);
  dbfull(db)->TEST_WaitForSplit();
  sleep(5); 
  ColumnFamilyData* cfd1 = cfd->GetColumnFamilySet()->GetColumnFamily(1);
  infos.clear();

  std::string res;
  for(int i = 0; i < numItem; i++) {
    db->Get(ReadOptions(), keys[i], &res);
    assert(res == values1[i]);

    db->Put(WriteOptions(), cfh, Slice(keys[i]), Slice(values2[i]));
  }


  FileMetaData* f2 = new FileMetaData;
  std::string s2 = "user1250";
  std::string l2 = "user1280";
  f2->smallest = InternalKey(Slice(s2), 0, kTypeValue);
  f2->largest = InternalKey(Slice(l2), 0, kTypeValue);
  infos.push_back(SplitFileInfo(f2, cfd1));
  dbfull(db)->SplitColumnFamilyFromSstFiles(infos);
  dbfull(db)->TEST_WaitForSplit();
  sleep(5); 

  for(int i = 0; i < numItem; i++) {
    db->Get(ReadOptions(), keys[i], &res);
    assert(res == values2[i]);

    db->Put(WriteOptions(), cfh, Slice(keys[i]), Slice(values3[i]));
  }
  infos.clear();


  FileMetaData* f3 = new FileMetaData;
  std::string s3 = "user1001";
  std::string l3 = "user1500";
  f3->smallest = InternalKey(Slice(s3), 0, kTypeValue);
  f3->largest = InternalKey(Slice(l3), 0, kTypeValue);
  infos.push_back(SplitFileInfo(f3, cfd));
  dbfull(db)->SplitColumnFamilyFromSstFiles(infos);
  dbfull(db)->TEST_WaitForSplit();
  sleep(5); 
  infos.clear();

  for(int i = 0; i < numItem; i++) {
    db->Get(ReadOptions(), keys[i], &res);
    assert(res == values3[i]);
  }

  DestroyDB(db_name, options);
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
