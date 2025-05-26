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


class LCFSingleLvlLevelTest : public testing::Test {
 public:
  LCFSingleLvlLevelTest() {
  }

  ~LCFSingleLvlLevelTest() {
  }

  DBImpl* dbfull(DB* db) { return reinterpret_cast<DBImpl*>(db) ;};

  std::string RandomString(Random* rnd, int len) {
    std::string r;
    test::RandomStringUserInt(rnd, len, &r);
    return r; 
  }
};

TEST_F(LCFSingleLvlLevelTest, Basic) {
  Options options;
  options.create_if_missing = true;
  options.max_background_jobs =32;
  options.max_write_buffer_number =2;
  options.max_bytes_for_level_base = 256 * 1024 * 1024 / 4 * 3;
  options.compression = kNoCompression;
  options.allow_column_family_split = true;
  options.column_family_min_key_range = 0; // set no limit of splitting
  //options.atomic_flush = true;

  

  std::string db_name = test::PerThreadDBPath("test_db_one_two");
  DB* db;
  ASSERT_OK(DB::Open(options, db_name, &db));

  ColumnFamilyHandle* cfh = dbfull(db)->DefaultColumnFamily();
  ColumnFamilyData* cfd_default = static_cast<ColumnFamilyHandleImpl*>(cfh)->cfd();
  ColumnFamilyData* cfd_default1= cfd_default->GetChildrenNodes()[0]->cfd_;

  const auto opt = cfd_default->GetLatestCFOptions();
  for (auto& cf_path: opt.cf_paths) {
    std::cout <<"cf path : " << cf_path.path<< std::endl;
  }


  int key_size = 7;
  int val_size = 100;
  Random rnd(301);
  int kCnt = 40000;
  for (int num = 0; num < 2; num ++) {
    for (int i=0; i<kCnt; i++) {
      std::string k =  RandomString(&rnd, key_size);
      std::string v =  RandomString(&rnd, val_size);
      db->Put(WriteOptions(), cfh, k, v);  
    }
    ASSERT_OK(db->Flush(FlushOptions())); 
  }

  std::cout <<"cfd name : " << cfd_default1->GetName()<< std::endl;

  std::vector<SplitFileInfo> infos;
  for (int i = 0; i < 4; i++) {
    FileMetaData* f = new FileMetaData;
    std::string str_s = "";
    std::string str_l = "";

    if (i != 0) {
      str_s = "user"+ std::to_string(i) + "00";
    }
    if (i != 3) {
      str_l = "user"+std::to_string(i+1) + "00";
    }

    f->smallest = InternalKey(Slice(str_s), 0, kTypeValue);
    f->largest = InternalKey(Slice(str_l), 0, kTypeValue);
    int base_level = 1;
    uint64_t level_byte = 64*1024*1024;

    infos.push_back(SplitFileInfo(f, cfd_default1, base_level, level_byte, true));
  }

  dbfull(db)->SplitColumnFamilyFromSstFiles(cfd_default1, infos);

  sleep(3);

  // Compact second column family only.
  for (auto& node: cfd_default->GetChildrenNodes()) {
    ColumnFamilyData* cfd = node->cfd_;
    ColumnFamilyHandle* cfdh = dbfull(db)->GetColumnFamilyHandle(cfd->GetID());
    std::cout<< "cfh name : " << cfdh->GetName()<< std::endl;
    
    CompactRangeOptions cr_options;
    cr_options.change_level = true;
    cr_options.target_level = 1;
    dbfull(db)->CompactRange(cr_options, cfdh, nullptr, nullptr);
    sleep(3);
  }
  

  
 
  fprintf(stdout, "[LCFSingleLvlLevelTest] now shutdown db\n");
  //dbfull(db)->DestroyLogicalColumnFamilies();

  delete db;
  db = nullptr;
}

/*
TEST_F(LCFSingleLvlLevelTest, Basic) {
  Options options;
  options.create_if_missing = true;
  options.max_background_jobs =32;
  options.max_write_buffer_number =3;
  options.allow_column_family_split = true;
  options.column_family_min_key_range = 0; // set no limit of splitting
  //options.atomic_flush = true;

  std::string db_name = test::PerThreadDBPath("test_db_one_two");
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
      std::string k = "user" + std::to_string(i);
      std::string v = "v" + std::to_string(i) ;
      db->Put(WriteOptions(), cfh, k, v);  
    }
    ASSERT_OK(db->Flush(FlushOptions())); 
  }
  dbfull(db)->TEST_WaitForCompact();
  // Prepare #1 Level 1
  for (int num = 0; num < 1;
       num ++) {
    for (int i=0; i<1000; i++) {
      std::string k = "user" + std::to_string(i);
      std::string v = "v" + std::to_string(i) ;
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

  fprintf(stdout, "Level 0 has : %d\n",num_level0);
  fprintf(stdout, "Level 1 has : %d\n",num_level1);


  // FIRST Split
  std::vector<FileMetaData*> infos;

  FileMetaData* f1 = new FileMetaData;
  std::string s1 = "";
  std::string l1 = "user1000";
  f1->smallest = InternalKey(Slice(s1), 0, kTypeValue);
  f1->largest = InternalKey(Slice(l1), 0, kTypeValue);
  infos.push_back(f1);

  FileMetaData* f2 = new FileMetaData;
  std::string s2 = "user1000";
  std::string l2 = "user2000";
  f2->smallest = InternalKey(Slice(s2), 0, kTypeValue);
  f2->largest = InternalKey(Slice(l2), 0, kTypeValue);
  infos.push_back(f2);

  FileMetaData* f3 = new FileMetaData;
  std::string s3 = "user2000";
  std::string l3 = "user3000";
  f3->smallest = InternalKey(Slice(s3), 0, kTypeValue);
  f3->largest = InternalKey(Slice(l3), 0, kTypeValue);
  infos.push_back(f3);

  FileMetaData* f4 = new FileMetaData;
  std::string s4 = "user3000";
  std::string l4 = "user4000";
  f4->smallest = InternalKey(Slice(s4), 0, kTypeValue);
  f4->largest = InternalKey(Slice(l4), 0, kTypeValue);
  infos.push_back(f4);

  FileMetaData* f5 = new FileMetaData;
  std::string s5 = "user4000";
  std::string l5 = "";
  f5->smallest = InternalKey(Slice(s5), 0, kTypeValue);
  f5->largest = InternalKey(Slice(l5), 0, kTypeValue);
  infos.push_back(f5);


  fprintf(stdout, "[LCFSingleLvlLevelTest] First Split Start [%s, %s] (1/3)\n", s1.c_str(), l1.c_str());
  dbfull(db)->SplitColumnFamilyFromSstFiles(cfd, infos);
  fprintf(stdout, "[LCFSingleLvlLevelTest] First Split Finish (1/3)\n");
  //dbfull(db)->TEST_WaitForSplit();
  infos.clear();
 

  dbfull(db)->TEST_WaitForSplit();


  fprintf(stdout, "[LCFSingleLvlLevelTest] flush\n");
  for (int num = 0; num < 1;
       num ++) {
    for (int i=0; i<1000; i++) {
      std::string k = RandomString(&rnd, 9);
      std::string v = RandomString(&rnd, 200);
      db->Put(WriteOptions(), cfh, k, v);  
    }
    ASSERT_OK(db->Flush(FlushOptions())); 
  }

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
 
 
  fprintf(stdout, "[LCFSingleLvlLevelTest] now shutdown db\n");
  //dbfull(db)->DestroyLogicalColumnFamilies();

  delete db;
  db = nullptr;
}*/
/*
TEST_F(LCFSingleLvlLevelTest, OneTwoSplit) {
  Options options;
  options.create_if_missing = true;
  options.max_background_jobs =32;
  options.max_write_buffer_number =3;
  options.allow_column_family_split = false;
  options.column_family_min_key_range = 0; // set no limit of splitting
  //options.atomic_flush = true;

  std::string db_name = test::PerThreadDBPath("test_db_one_two");
  DB* db;
  ASSERT_OK(DB::Open(options, db_name, &db));

  ColumnFamilyHandle* cfh = dbfull(db)->DefaultColumnFamily();
  //ColumnFamilyData* cfd =
  //    static_cast<ColumnFamilyHandleImpl*>(cfh)->cfd();

  Random rnd(301);
  // Prepare one Level 1 sstable file
  // trigger L0 compaction
  for (int num = 0; num < options.level0_file_num_compaction_trigger + 1;
       num ++) {
    for (int i=0; i<1000; i++) {
      std::string k = "user" + std::to_string(i);
      std::string v = "v" + std::to_string(i) ;
      db->Put(WriteOptions(), cfh, k, v);  
    }
    ASSERT_OK(db->Flush(FlushOptions())); 
  }
  dbfull(db)->TEST_WaitForCompact();
  // Prepare #1 Level 1
  for (int num = 0; num < 1;
       num ++) {
    for (int i=0; i<1000; i++) {
      std::string k = "user" + std::to_string(i);
      std::string v = "v" + std::to_string(i) ;
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

  fprintf(stdout, "Level 0 has : %d\n",num_level0);
  fprintf(stdout, "Level 1 has : %d\n",num_level1);


  // FIRST Split
  std::vector<FileMetaData*> infos;

  FileMetaData* f1 = new FileMetaData;
  std::string s1 = "";
  std::string l1 = "user1000";
  f1->smallest = InternalKey(Slice(s1), 0, kTypeValue);
  f1->largest = InternalKey(Slice(l1), 0, kTypeValue);
  infos.push_back(f1);

  FileMetaData* f2 = new FileMetaData;
  std::string s2 = "user1000";
  std::string l2 = "user2000";
  f2->smallest = InternalKey(Slice(s2), 0, kTypeValue);
  f2->largest = InternalKey(Slice(l2), 0, kTypeValue);
  infos.push_back(f2);

  FileMetaData* f3 = new FileMetaData;
  std::string s3 = "user2000";
  std::string l3 = "user3000";
  f3->smallest = InternalKey(Slice(s3), 0, kTypeValue);
  f3->largest = InternalKey(Slice(l3), 0, kTypeValue);
  infos.push_back(f3);

  FileMetaData* f4 = new FileMetaData;
  std::string s4 = "user3000";
  std::string l4 = "user4000";
  f4->smallest = InternalKey(Slice(s4), 0, kTypeValue);
  f4->largest = InternalKey(Slice(l4), 0, kTypeValue);
  infos.push_back(f4);

  FileMetaData* f5 = new FileMetaData;
  std::string s5 = "user4000";
  std::string l5 = "";
  f5->smallest = InternalKey(Slice(s5), 0, kTypeValue);
  f5->largest = InternalKey(Slice(l5), 0, kTypeValue);
  infos.push_back(f5);


  fprintf(stdout, "[LCFSingleLvlLevelTest] First Split Start [%s, %s] (1/3)\n", s1.c_str(), l1.c_str());
  dbfull(db)->SplitColumnFamilyFromSstFiles(cfd, infos);
  fprintf(stdout, "[LCFSingleLvlLevelTest] First Split Finish (1/3)\n");
  //dbfull(db)->TEST_WaitForSplit();
  infos.clear();
 

  dbfull(db)->TEST_WaitForSplit();


  fprintf(stdout, "[LCFSingleLvlLevelTest] flush\n");
  for (int num = 0; num < 1;
       num ++) {
    for (int i=0; i<1000; i++) {
      std::string k = RandomString(&rnd, 9);
      std::string v = RandomString(&rnd, 200);
      db->Put(WriteOptions(), cfh, k, v);  
    }
    ASSERT_OK(db->Flush(FlushOptions())); 
  }

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
 
 
  fprintf(stdout, "[LCFSingleLvlLevelTest] now shutdown db\n");
  //dbfull(db)->DestroyLogicalColumnFamilies();

  delete db;
  db = nullptr;
}*/
/*
TEST_F(LCFSingleLvlLevelTest, OneThreeSplit) {
  Options options;
  options.create_if_missing = true;
  options.max_background_jobs =32;
  options.max_write_buffer_number =3;
  options.allow_column_family_split = true;
  options.column_family_min_key_range = 0; // set no limit of splitting
  //options.atomic_flush = true;

  std::string db_name = test::PerThreadDBPath("test_db_one_three");
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

  std::vector<FileMetaData*> infos;

  FileMetaData* f1 = new FileMetaData;
  std::string s1 = "user1200";
  std::string l1 = "user1400";
  f1->smallest = InternalKey(Slice(s1), 0, kTypeValue);
  f1->largest = InternalKey(Slice(l1), 0, kTypeValue);
  infos.push_back(f1);

  fprintf(stdout, "[LCFSingleLvlLevelTest] First Split Start [%s, %s] (1/3)\n", s1.c_str(), l1.c_str());
  dbfull(db)->SplitColumnFamilyFromSstFiles(cfd, infos);
  fprintf(stdout, "[LCFSingleLvlLevelTest] First Split Finish (1/3)\n");
  //dbfull(db)->TEST_WaitForSplit();
  infos.clear();
 

  dbfull(db)->TEST_WaitForSplit();


  FileMetaData* f2 = new FileMetaData;
  std::string s2 = "user1100";
  std::string l2 = "user1500";
  f2->smallest = InternalKey(Slice(s2), 0, kTypeValue);
  f2->largest = InternalKey(Slice(l2), 0, kTypeValue);
  infos.push_back(f2);

  fprintf(stdout, "[LCFSingleLvlLevelTest] First Split Start [%s, %s] (2/3)\n", s2.c_str(), l2.c_str());
  dbfull(db)->SplitColumnFamilyFromSstFiles(cfd, infos);
  fprintf(stdout, "[LCFSingleLvlLevelTest] First Split Finish (2/3)\n");
  //dbfull(db)->TEST_WaitForSplit();
  infos.clear();
 

  dbfull(db)->TEST_WaitForSplit();

  fprintf(stdout, "[LCFSingleLvlLevelTest] flush\n");
  for (int num = 0; num < 1;
       num ++) {
    for (int i=0; i<1000; i++) {
      std::string k = RandomString(&rnd, 8);
      std::string v = RandomString(&rnd, 200);
      db->Put(WriteOptions(), cfh, k, v);  
    }
    db->Put(WriteOptions(), cfh, "user1200", "12345");
    db->Put(WriteOptions(), cfh, "user1400", "12345");
    db->Put(WriteOptions(), cfh, "user1400", "12345");
    db->Put(WriteOptions(), cfh, "user1100", "12345");
    db->Put(WriteOptions(), cfh, "user1100", "12345");
    db->Put(WriteOptions(), cfh, "user1100", "12345");
    db->Put(WriteOptions(), cfh, "user1500", "12345");
    db->Put(WriteOptions(), cfh, "user1500", "12345");
    db->Put(WriteOptions(), cfh, "user1500", "12345");
    ASSERT_OK(db->Flush(FlushOptions())); 
  }
 
  fprintf(stdout, "[LCFSingleLvlLevelTest] now shutdown db\n");
  //dbfull(db)->DestroyLogicalColumnFamilies();

  delete db;
  db = nullptr;
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
