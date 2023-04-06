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
#include "rocksdb/experimental.h"
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

  std::string RandomString(Random* rnd, int len) {
    std::string r;
    test::RandomStringUserInt(rnd, len, &r);
    return r; 
  }
};

/*
// This tests for a bug that cause compact-while split in the same column family.
TEST_F(LCFPutTest, SplitWhileCompact) {
  Options options;
  options.create_if_missing = true;
  options.max_background_jobs = 32;
  options.max_write_buffer_number = 3;
  options.allow_column_family_split = true;
  options.atomic_flush = true;
  options.write_buffer_size = 110 << 10;
  options.arena_block_size = 4 << 10;
  options.level0_file_num_compaction_trigger = 4;
  options.num_levels = 4;
  options.compression = kNoCompression;
  options.max_bytes_for_level_base = 450 << 10;
  
  // Open
  std::string db_name = test::PerThreadDBPath("test_db");
  DB* db;
  ASSERT_OK(DB::Open(options, db_name, &db));
  ColumnFamilyHandle* cfh = dbfull(db)->DefaultColumnFamily();
  ColumnFamilyData* cfd =
      static_cast<ColumnFamilyHandleImpl*>(cfh)->cfd();

  // Fill up DB
  Random rnd(301);
  for (int num = 0; num < 10; num ++) {
    for (int i = 0; i < 51; i++) {
      std::string k = RandomString(&rnd, 8);
      std::string v = RandomString(&rnd, 200);
      db->Put(WriteOptions(), cfh, k, v);
    } 
    dbfull(db)->TEST_WaitForFlushMemTable();
    dbfull(db)->TEST_WaitForCompact();
  }
  db->CompactRange(CompactRangeOptions(), nullptr, nullptr);

  rocksdb::SyncPoint::GetInstance()->LoadDependency(
      {{"CompactionJob::Run():Start",
        "LCFPutTest::TEST1"},
       {"LCFPutTest::TEST2",
        "CompactionJob::Run():End"}});

  rocksdb::SyncPoint::GetInstance()->EnableProcessing();

  // Trigger L0 Compaction
  for (int num = 0; num < options.level0_file_num_compaction_trigger + 1;
       num ++) {
    for (int i = 0; i < 51; i++) {
      std::string k = RandomString(&rnd, 8);
      std::string v = RandomString(&rnd, 200);
      db->Put(WriteOptions(), cfh, k, v);
    } 
    ASSERT_OK(db->Flush(FlushOptions()));
  }

  TEST_SYNC_POINT("LCFPutTest::TEST1");
  fprintf(stdout, "LCFPutTest::TEST1\n");
  for (int i = 0; i < 51; i++) {
    std::string k = RandomString(&rnd, 8);
    std::string v = RandomString(&rnd, 200);
    db->Put(WriteOptions(), cfh, k, v);
  }
  dbfull(db)->TEST_WaitForFlushMemTable();
  ASSERT_OK(experimental::SuggestCompactRange(db, nullptr, nullptr));
  for (int num = 0; num < options.level0_file_num_compaction_trigger + 1;
       num ++) {
    for (int i = 0; i < 51; i++) {
      std::string k = RandomString(&rnd, 8);
      std::string v = RandomString(&rnd, 200);
      db->Put(WriteOptions(), cfh, k, v);
    }
    ASSERT_OK(db->Flush(FlushOptions())); 
  }
  TEST_SYNC_POINT("LCFPutTest::TEST2");
  fprintf(stdout, "LCFPutTest::TEST2\n");
  dbfull(db)->TEST_WaitForCompact();

   // Prepare two L0 
  for (int num = 0; num < 2;
       num ++) {
    for (int i = 0; i < 51; i++) {
      std::string k = RandomString(&rnd, 8);
      std::string v = RandomString(&rnd, 200);
      db->Put(WriteOptions(), cfh, k, v);
    } 
    ASSERT_OK(db->Flush(FlushOptions()));
  }

  std::vector<SplitFileInfo> infos;

  FileMetaData* f1 = new FileMetaData;
  std::string s1 = "user1200";
  std::string l1 = "user1400";
  f1->smallest = InternalKey(Slice(s1), 0, kTypeValue);
  f1->largest = InternalKey(Slice(l1), 0, kTypeValue);
  infos.push_back(SplitFileInfo(f1, cfd));

  rocksdb::SyncPoint::GetInstance()->LoadDependency(
    {
      {"CompactionJob::Run():Start" ,"LCFPutTest::TEST3"},
      { "LCFPutTest::TEST4", "CompactionJob::Run():End" }
    }
  );

  rocksdb::SyncPoint::GetInstance()->EnableProcessing();

  // trigger L0 compaction
  for (int num = 0; num < options.level0_file_num_compaction_trigger + 1;
       num ++) {
    for (int i = 0; i < 51; i++) {
      std::string k = RandomString(&rnd, 8);
      std::string v = RandomString(&rnd, 200);
      db->Put(WriteOptions(), cfh, k, v);
    }
    ASSERT_OK(db->Flush(FlushOptions())); 
  }
  // trigger Split
  TEST_SYNC_POINT("LCFPutTest::TEST3");
  fprintf(stdout, "[LCFPutTest] First Split Start (1/3)\n");
  dbfull(db)->SplitColumnFamilyFromSstFiles(infos);
  fprintf(stdout, "[LCFPutTest] First Split Finish (1/3)\n");
  TEST_SYNC_POINT("LCFPutTest::TEST4");

  dbfull(db)->TEST_WaitForCompact();
  dbfull(db)->TEST_WaitForSplit();

  infos.clear();
  // Close
  delete f1;
  delete db;
  db = nullptr;
}*/


TEST_F(LCFPutTest, SingleSplit) {
  Options options;
  options.create_if_missing = true;
  options.max_background_jobs =32;
  options.max_write_buffer_number =3;
  options.allow_column_family_split = true;
  options.atomic_flush = true;

  std::string db_name = test::PerThreadDBPath("test_db");
  DB* db;
  ASSERT_OK(DB::Open(options, db_name, &db));

  ColumnFamilyHandle* cfh = dbfull(db)->DefaultColumnFamily();
  ColumnFamilyData* cfd =
      static_cast<ColumnFamilyHandleImpl*>(cfh)->cfd();

  Random rnd(301);
  // Prepare one Level 1 sstable file
  // trigger L0 compaction
  for (int num = 0; num < options.level0_file_num_compaction_trigger + 1;
       num ++) {
    for (int i=0; i<1000; i++) {
      std::string k = RandomString(&rnd, 8);
      std::string v = RandomString(&rnd, 200);
      db->Put(WriteOptions(), cfh, k, v);  
    }
    ASSERT_OK(db->Flush(FlushOptions())); 
  }
  dbfull(db)->TEST_WaitForCompact();
  // Prepare #1 Level 1
  for (int num = 0; num < 1;
       num ++) {
    for (int i=0; i<1000; i++) {
      std::string k = RandomString(&rnd, 8);
      std::string v = RandomString(&rnd, 200);
      db->Put(WriteOptions(), cfh, k, v);  
    }
    ASSERT_OK(db->Flush(FlushOptions())); 
  }
  
  // Prepare Memtable
  for (int i=0; i<1000; i++) {
    std::string k = RandomString(&rnd, 8);
    std::string v = RandomString(&rnd, 200);
    db->Put(WriteOptions(), cfh, k, v);  
  }

  std::string val0;
  dbfull(db)->GetProperty(cfh, "rocksdb.num-files-at-level0", &val0);
  int num_level0 = std::stoi(val0);
  std::string val1;
  dbfull(db)->GetProperty(cfh, "rocksdb.num-files-at-level1", &val1);
  int num_level1 = std::stoi(val1);

  fprintf(stdout, "Level 0 has : %d\n",num_level0 );
  fprintf(stdout, "Level 1 has : %d\n",num_level1);
  assert(num_level0 == 2);
  assert(num_level1 == 1);

  std::vector<SplitFileInfo> infos;

  FileMetaData* f1 = new FileMetaData;
  std::string s1 = "user1200";
  std::string l1 = "user1400";
  f1->smallest = InternalKey(Slice(s1), 0, kTypeValue);
  f1->largest = InternalKey(Slice(l1), 0, kTypeValue);
  infos.push_back(SplitFileInfo(f1, cfd));

  fprintf(stdout, "[LCFPutTest] First Split Start [%s, %s] (1/3)\n", s1.c_str(), l1.c_str());
  dbfull(db)->SplitColumnFamilyFromSstFiles(infos);
  fprintf(stdout, "[LCFPutTest] First Split Finish (1/3)\n");
  //dbfull(db)->TEST_WaitForSplit();
  infos.clear();
 

  dbfull(db)->TEST_WaitForSplit();
  fprintf(stdout, "[LCFPutTest] now shutdown db\n");

  delete f1;


  delete db;
  db = nullptr;
}

/*
TEST_F(LCFPutTest, ThreeLevelAfterPut) {
  Options options;
  options.create_if_missing = true;
  options.max_background_jobs =32;
  options.max_write_buffer_number =3;
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

  fprintf(stdout, "[LCFPutTest] First Split Start [%s, %s] (1/3)\n", s1.c_str(), l1.c_str());
  dbfull(db)->SplitColumnFamilyFromSstFiles(infos);
  fprintf(stdout, "[LCFPutTest] First Split Finish (1/3)\n");
  //dbfull(db)->TEST_WaitForSplit();
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
  fprintf(stdout, "[LCFPutTest] Second Split Start [%s, %s] (2/3)\n", s2.c_str(), l2.c_str());
  dbfull(db)->SplitColumnFamilyFromSstFiles(infos);
  fprintf(stdout, "[LCFPutTest] Second Split Finish (2/3)\n");
  //dbfull(db)->TEST_WaitForSplit();

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
  fprintf(stdout, "[LCFPutTest] Third Split Start [%s, %s] (3/3)\n", s3.c_str(), l3.c_str());
  dbfull(db)->SplitColumnFamilyFromSstFiles(infos);
  fprintf(stdout, "[LCFPutTest] Third Split Finish (3/3)\n");
  //dbfull(db)->TEST_WaitForSplit();
  infos.clear();

  for(int i = 0; i < numItem; i++) {
    db->Get(ReadOptions(), keys[i], &res);
    assert(res == values3[i]);
  }

  fprintf(stdout, "[LCFPutTest] now shutdown db\n");
  sleep(10);

  delete f1;
  delete f2;
  delete f3;

  keys.clear();
  values1.clear();
  values2.clear();
  values3.clear();
  delete db;
  db = nullptr;
}*/

/*
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
  fprintf(stdout, "[LCFPutTest] First Split Start (1/3)\n");
  dbfull(db)->SplitColumnFamilyFromSstFiles(infos);
  fprintf(stdout, "[LCFPutTest] First Split Finish (1/3)\n");
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
  fprintf(stdout, "[LCFPutTest] Second Split Start (2/3)\n");
  dbfull(db)->SplitColumnFamilyFromSstFiles(infos);
  fprintf(stdout, "[LCFPutTest] Second Split Finish (2/3)\n");
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
  fprintf(stdout, "[LCFPutTest] Third Split Start (3/3)\n");
  dbfull(db)->SplitColumnFamilyFromSstFiles(infos);
  fprintf(stdout, "[LCFPutTest] Third Split Finish (3/3)\n");
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
  fprintf(stdout, "[LCFPutTest] now shutdown db\n");
  delete db;
}*/

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
