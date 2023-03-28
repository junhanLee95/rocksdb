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

class LCFPutTest : public testing::Test {
 public:
  LCFPutTest() {
  }

  ~LCFPutTest() {
  }

  DBImpl* dbfull(DB* db) { return reinterpret_cast<DBImpl*>(db) ;};
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

  delete f1;
  delete f2;
  delete f3;

  keys.clear();
  values1.clear();
  values2.clear();
  values3.clear();
  delete db;
  db = nullptr;
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

  delete f1;
  delete f2;
  delete f3;

  keys.clear();
  values1.clear();
  values2.clear();
  values3.clear();
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
