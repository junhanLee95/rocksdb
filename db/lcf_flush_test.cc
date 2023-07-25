// Copyright (c) 2011-present, Facebook, Inc. All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#ifndef ROCKSDB_LITE

#include <inttypes.h>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <chrono>

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

class LCFFlushTest : public testing::Test {
 public:
  static const int split_cnt = 8;
  LCFFlushTest() {
  }

  ~LCFFlushTest() {
  }

  DBImpl* dbfull(DB* db) { return reinterpret_cast<DBImpl*>(db) ;};

  std::string RandomString(Random* rnd, int len) {
    std::string r;
    test::RandomStringUserInt(rnd, len, &r);
    return r; 
  }

  int NumTableFilesAtLevel(DB* db, ColumnFamilyHandle* cfh, int level) {
    std::string value;
    dbfull(db)->GetProperty(cfh, "rocksdb.num-files-at-level" + ToString(level), &value); 
    return std::stoi(value);
  }

  void TEST_Split(DB* db, int from, int to, std::pair<std::string, std::string> (&ranges)[split_cnt])
  {
    ColumnFamilyHandle* cfh = dbfull(db)->DefaultColumnFamily();
    ColumnFamilyData* cfd =
      static_cast<ColumnFamilyHandleImpl*>(cfh)->cfd();
    ColumnFamilyData* cfd_from = cfd->GetColumnFamilySet()->GetColumnFamily(from);
    std::vector<SplitFileInfo> infos;
    FileMetaData* f = new FileMetaData;
    std::string s = ranges[to].first;
    std::string l = ranges[to].second;
    f->smallest = InternalKey(Slice(s), 0, kTypeValue);
    f->largest = InternalKey(Slice(l), 0, kTypeValue);
    infos.push_back(SplitFileInfo(f, cfd_from));
    dbfull(db)->SplitColumnFamilyFromSstFiles(infos);
    infos.clear();  
    delete f;
  }


};

// why default3 has overlapping ranges..
/*
TEST_F(LCFFlushTest, Search) { 
  Options options;
  options.create_if_missing = true;
  options.max_background_jobs =32;
  options.max_write_buffer_number =2;
  options.allow_column_family_split = true;
  options.atomic_flush = false;

  std::string db_name = test::PerThreadDBPath("test_db");
  DB* db;
  ASSERT_OK(DB::Open(options, db_name, &db));

  ColumnFamilyHandle* cfh = dbfull(db)->DefaultColumnFamily();
  ColumnFamilyData* cfd =
      static_cast<ColumnFamilyHandleImpl*>(cfh)->cfd();

  // Prepare Memtable
  for(int i = 1000; i< 2000; i++) {
    std::string key = "user" + std::to_string(i); 
    std::string value = "abcdef" + std::to_string(i) + "ghijk";
    db->Put(WriteOptions(), cfh, key, value);
  } 

  // Next, we construct three-level partition tree.
  std::vector<SplitFileInfo> infos;

  FileMetaData* f1 = new FileMetaData;
  std::string s1 = "user1200";
  std::string l1 = "user1400";
  f1->smallest = InternalKey(Slice(s1), 0, kTypeValue);
  f1->largest = InternalKey(Slice(l1), 0, kTypeValue);
  infos.push_back(SplitFileInfo(f1, cfd));

  fprintf(stdout, "[LCFFlushTest] First Split Start [%s, %s] (1/3)\n", s1.c_str(), l1.c_str());
  dbfull(db)->SplitColumnFamilyFromSstFiles(infos);
  fprintf(stdout, "[LCFFlushTest] First Split Finish (1/3)\n");
  //dbfull(db)->TEST_WaitForSplit();
  ColumnFamilyData* cfd1 = cfd->GetColumnFamilySet()->GetColumnFamily(1);
  infos.clear();
  
  FileMetaData* f2 = new FileMetaData;
  std::string s2 = "user1250";
  std::string l2 = "user1280";
  f2->smallest = InternalKey(Slice(s2), 0, kTypeValue);
  f2->largest = InternalKey(Slice(l2), 0, kTypeValue);
  infos.push_back(SplitFileInfo(f2, cfd1));
  fprintf(stdout, "[LCFFlushTest] Second Split Start [%s, %s] (2/3)\n", s2.c_str(), l2.c_str());
  dbfull(db)->SplitColumnFamilyFromSstFiles(infos);
  fprintf(stdout, "[LCFFlushTest] Second Split Finish (2/3)\n");
  //dbfull(db)->TEST_WaitForSplit();

  FileMetaData* f3 = new FileMetaData;
  std::string s3 = "user1001";
  std::string l3 = "user1500";
  f3->smallest = InternalKey(Slice(s3), 0, kTypeValue);
  f3->largest = InternalKey(Slice(l3), 0, kTypeValue);
  infos.push_back(SplitFileInfo(f3, cfd));
  fprintf(stdout, "[LCFFlushTest] Third Split Start [%s, %s] (3/3)\n", s3.c_str(), l3.c_str());
  dbfull(db)->SplitColumnFamilyFromSstFiles(infos);
  fprintf(stdout, "[LCFFlushTest] Third Split Finish (3/3)\n");
  //dbfull(db)->TEST_WaitForSplit();
  infos.clear();



  // Flush Memtable
  db->Flush(FlushOptions(), cfh);

  // we need to verify the number of L0 within default column family and its child
  // is 1, whereas other grandchildren's L0 cnt is zero.
  for (size_t i=0; i<5 ;i++) {
    ColumnFamilyHandle* c_cfh = dbfull(db)->GetColumnFamilyHandle(i);
    int c_l0_cnt = NumTableFilesAtLevel(db, c_cfh, 0);
    fprintf(stdout, "[LCFFlushTest] cf[%s] L0 count : %d\n",
            c_cfh->GetName().c_str(),
            c_l0_cnt);
    if (i == 0 || i == 1) {
      assert(c_l0_cnt == 1); 
    } else {
      assert(c_l0_cnt == 0); 
    }
  }

  fprintf(stdout, "[LCFFlushTest] now shutdown db\n");

  delete f1;
  delete f2;
  delete f3;

  delete db;
  db = nullptr;

}*/

/*
TEST_F(LCFFlushTest, ThreeLevelSplitAndFlush) {
  Options options;
  options.create_if_missing = true;
  options.max_background_jobs =32;
  options.max_write_buffer_number =2;
  options.allow_column_family_split = true;
  options.atomic_flush = false;

  std::string db_name = test::PerThreadDBPath("test_db");
  DB* db;
  ASSERT_OK(DB::Open(options, db_name, &db));

  ColumnFamilyHandle* cfh = dbfull(db)->DefaultColumnFamily();
  ColumnFamilyData* cfd =
      static_cast<ColumnFamilyHandleImpl*>(cfh)->cfd();

  // Prepare Memtable
  for(int i = 1000; i< 2000; i++) {
    std::string key = "user" + std::to_string(i); 
    std::string value = "abcdef" + std::to_string(i) + "ghijk";
    db->Put(WriteOptions(), cfh, key, value);
  } 

  // Next, we construct three-level partition tree.
  std::vector<SplitFileInfo> infos;

  FileMetaData* f1 = new FileMetaData;
  std::string s1 = "user1200";
  std::string l1 = "user1400";
  f1->smallest = InternalKey(Slice(s1), 0, kTypeValue);
  f1->largest = InternalKey(Slice(l1), 0, kTypeValue);
  infos.push_back(SplitFileInfo(f1, cfd));

  fprintf(stdout, "[LCFFlushTest] First Split Start [%s, %s] (1/3)\n", s1.c_str(), l1.c_str());
  dbfull(db)->SplitColumnFamilyFromSstFiles(infos);
  fprintf(stdout, "[LCFFlushTest] First Split Finish (1/3)\n");
  //dbfull(db)->TEST_WaitForSplit();
  ColumnFamilyData* cfd1 = cfd->GetColumnFamilySet()->GetColumnFamily(1);
  infos.clear();
  
  FileMetaData* f2 = new FileMetaData;
  std::string s2 = "user1250";
  std::string l2 = "user1280";
  f2->smallest = InternalKey(Slice(s2), 0, kTypeValue);
  f2->largest = InternalKey(Slice(l2), 0, kTypeValue);
  infos.push_back(SplitFileInfo(f2, cfd1));
  fprintf(stdout, "[LCFFlushTest] Second Split Start [%s, %s] (2/3)\n", s2.c_str(), l2.c_str());
  dbfull(db)->SplitColumnFamilyFromSstFiles(infos);
  fprintf(stdout, "[LCFFlushTest] Second Split Finish (2/3)\n");
  //dbfull(db)->TEST_WaitForSplit();

  FileMetaData* f3 = new FileMetaData;
  std::string s3 = "user1001";
  std::string l3 = "user1500";
  f3->smallest = InternalKey(Slice(s3), 0, kTypeValue);
  f3->largest = InternalKey(Slice(l3), 0, kTypeValue);
  infos.push_back(SplitFileInfo(f3, cfd));
  fprintf(stdout, "[LCFFlushTest] Third Split Start [%s, %s] (3/3)\n", s3.c_str(), l3.c_str());
  dbfull(db)->SplitColumnFamilyFromSstFiles(infos);
  fprintf(stdout, "[LCFFlushTest] Third Split Finish (3/3)\n");
  //dbfull(db)->TEST_WaitForSplit();
  infos.clear();



  // Flush Memtable
  db->Flush(FlushOptions(), cfh);

  // we need to verify the number of L0 within default column family and its child
  // is 1, whereas other grandchildren's L0 cnt is zero.
  for (size_t i=0; i<5 ;i++) {
    ColumnFamilyHandle* c_cfh = dbfull(db)->GetColumnFamilyHandle(i);
    int c_l0_cnt = NumTableFilesAtLevel(db, c_cfh, 0);
    fprintf(stdout, "[LCFFlushTest] cf[%s] L0 count : %d\n",
            c_cfh->GetName().c_str(),
            c_l0_cnt);
    if (i == 0 || i == 1) {
      assert(c_l0_cnt == 1); 
    } else {
      assert(c_l0_cnt == 0); 
    }
  }

  fprintf(stdout, "[LCFFlushTest] now shutdown db\n");

  delete f1;
  delete f2;
  delete f3;

  delete db;
  db = nullptr;
}*/

TEST_F(LCFFlushTest, Prepare) {
  Options options;
  options.create_if_missing = true;
  options.max_background_jobs =32;
  options.max_write_buffer_number =2;
  //options.allow_column_family_split = false;
  options.allow_column_family_split = true;
  options.atomic_flush = false;
  int kv_size = 65536;
  int num_cf = 16;

  std::string db_name = "/mnt/rocksdb_test";
  DB* db;
  ASSERT_OK(DB::Open(options, db_name, &db));

  ColumnFamilyHandle* cfh = dbfull(db)->DefaultColumnFamily();
  ColumnFamilyData* cfd =
      static_cast<ColumnFamilyHandleImpl*>(cfh)->cfd();

  const std::string CHARACTERS = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdef";

  std::random_device random_device;
  std::mt19937 generator(random_device());
  std::uniform_int_distribution<> distribution(0, CHARACTERS.size() -1);

  // Prepare Memtable
  // Generate 64k KV-pair
  int kv_base = 10000;
  for(int i = kv_base; i < kv_base + kv_size; i++) {
	// 23-byte key 
    std::string key = "user00000000000000" + std::to_string(i); 
    //std::string value = "abcde" + std::to_string(i) + "fghijklmno"+ "pqr";
		// 1000-byte value
	  std::string value;
		for(int j = 0; i < 1000; j++) {
			value += CHARACTERS[distribution(generator)];
		}
    db->Put(WriteOptions(), cfh, key, value);
  } 

  // Next, we construct two-level partition tree.
  
  double total_split_time = 0.0;
  int cf_size = kv_size / num_cf;
	int cf_intv = cf_size / num_cf;

  for (int i = 0 ; i < num_cf -1 ; i++) {
    std::vector<SplitFileInfo> infos;
    FileMetaData* f1 = new FileMetaData;

		size_t start_num = kv_base + i * cf_size + (i+1) * cf_intv;
		size_t end_num =  kv_base + (i+1) * cf_size + (i+1) * cf_intv;

    std::string s1 = "user00000000000000" + std::to_string(start_num);
    std::string l1 = "user00000000000000" + std::to_string(end_num);
    f1->smallest = InternalKey(Slice(s1), 0, kTypeValue);
    f1->largest = InternalKey(Slice(l1), 0, kTypeValue);
    infos.push_back(SplitFileInfo(f1, cfd));

    auto t0 = std::chrono::steady_clock::now();
    dbfull(db)->SplitColumnFamilyFromSstFiles(infos);
    auto t1 = std::chrono::steady_clock::now();
    total_split_time += std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    std::cout << "Time for SplitColumnFamilyFromSstFiles() = " <<  std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count() << "[us]" << std::endl;
    infos.clear();  
    delete f1;
  }
 

  // Flush Memtable
  std::cout << "Time for Split() = " << total_split_time << "[us]" << std::endl;
  auto f0 = std::chrono::steady_clock::now();
  db->Flush(FlushOptions(), cfh);
  auto f1 = std::chrono::steady_clock::now();
  std::cout << "Time for Flush() = " << std::chrono::duration_cast<std::chrono::microseconds>(f1 - f0).count() << "[us]" << std::endl;

  delete db;
  db = nullptr;
}
/*
TEST_F(LCFFlushTest, SyncTest) {
  Options options;
  options.create_if_missing = true;
  options.max_background_jobs =32;
  options.max_write_buffer_number =2;
  options.allow_column_family_split = true;
  options.atomic_flush = false;

  std::string db_name = "/mnt/rocksdb_test";
  DB* db;
  ASSERT_OK(DB::Open(options, db_name, &db));

  ColumnFamilyHandle* cfh = dbfull(db)->DefaultColumnFamily();

  // Prepare Memtable
  for(int i = 100000000; i< 300000000; i=i+100000) {
    std::string key = "user00000000000" + std::to_string(i); 
    std::string value = "abcdef" + std::to_string(i) + "ghijk";
    db->Put(WriteOptions(), cfh, key, value);
  } 

  std::pair<std::string, std::string> ranges[split_cnt];
  // default
  ranges[0].first = "";
  ranges[0].second = "";
  // default1
  ranges[1].first = "user00000000000236767394";
  ranges[1].second = "user00000000000247956368";
  // default2
  ranges[2].first = "user00000000000197950326";
  ranges[2].second = "user00000000000203711025";
  // default3
  ranges[3].first = "user00000000000241853162";
  ranges[3].second = "user00000000000244677830";
  // default4
  ranges[4].first = "user00000000000058503907";
  ranges[4].second = " user00000000000064967958";
  // default5
  ranges[5].first = "user00000000000070155474";
  ranges[5].second = "user00000000000072724504";
  // default6
  ranges[6].first = "user00000000000012247591";
  ranges[6].second = "user00000000000017499706";
  // default7
  ranges[7].first = "user00000000000016353858";
  ranges[7].second = "user00000000000211468523";

  // [SPLIT] default -> default1
  TEST_Split(db, 0, 1, ranges);
  // [SPLIT] default -> default2
  TEST_Split(db, 0, 2, ranges);
  // [SPLIT] default1 -> default3
  TEST_Split(db, 1, 3, ranges);
  // [SPLIT] default -> default4
  TEST_Split(db, 0, 4, ranges);
  // [SPLIT] default -> default5
  TEST_Split(db, 0, 5, ranges);
  // [SPLIT] default -> default6
  TEST_Split(db, 0, 6, ranges);
  // [SPLIT] default6 -> default7
  //TEST_Split(db, 6, 7, ranges);


  // Flush
  db->Flush(FlushOptions(), cfh);

  // clean up
  delete db;
  db = nullptr;
}*/

/*
TEST_F(LCFFlushTest, TimeAnalysis) {
  Options options;
  options.create_if_missing = true;
  options.max_background_jobs =32;
  options.max_write_buffer_number =2;
  options.allow_column_family_split = true;
  //options.allow_column_family_split = true;
  options.atomic_flush = false;

  std::string db_name = "/mnt/lcf_db_path";
  DB* db;
  ASSERT_OK(DB::Open(options, db_name, &db));

  ColumnFamilyHandle* cfh = dbfull(db)->DefaultColumnFamily();
  ColumnFamilyData* cfd =
      static_cast<ColumnFamilyHandleImpl*>(cfh)->cfd();

  // Prepare Memtable
  for(int i = 1000; i< 8000; i++) {
    std::string key = "user" + std::to_string(i); 
    std::string value = "abcdef" + std::to_string(i) + "ghijk";
    db->Put(WriteOptions(), cfh, key, value);
  } 

  // Next, we construct two-level partition tree.
  
  double total_split_time = 0.0;
  for (size_t i = 0 ; i < 8; i++) {
    std::vector<SplitFileInfo> infos;
    FileMetaData* f1 = new FileMetaData;
    std::string s1 = "user" + std::to_string((i+1) * 1000);
    std::string l1 = "user" + std::to_string((i+2) * 1000);
    f1->smallest = InternalKey(Slice(s1), 0, kTypeValue);
    f1->largest = InternalKey(Slice(l1), 0, kTypeValue);
    infos.push_back(SplitFileInfo(f1, cfd));

    auto t0 = std::chrono::steady_clock::now();
    dbfull(db)->SplitColumnFamilyFromSstFiles(infos);
    auto t1 = std::chrono::steady_clock::now();
    total_split_time += std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    std::cout << "Time for SplitColumnFamilyFromSstFiles() = " <<  std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count() << "[us]" << std::endl;
    infos.clear();  
    delete f1;
  }
 
  // Flush Memtable
  std::cout << "Time for Split() = " << total_split_time << "[us]" << std::endl;
  auto f0 = std::chrono::steady_clock::now();
  db->Flush(FlushOptions(), cfh);
  auto f1 = std::chrono::steady_clock::now();
  std::cout << "Time for Flush() = " << std::chrono::duration_cast<std::chrono::microseconds>(f1 - f0).count() << "[us]" << std::endl;

  // Time for creating column family
// ColumnFamilyHandle* cfh;
 // std::string cf_name = "cf_anon";

 //  std::unique_ptr<ColumnFamilyOptions> cfo(new ColumnFamilyOptions());
 //  cfo->compaction_style = kCompactionStyleLevel;
 // cfo->num_levels = 7;
 // cfo->write_buffer_size = 64 << 20; // 64MB
 // cfo->level0_file_num_compaction_trigger = 4;
 // cfo->target_file_size_base = 64 << 20; // 4MB
 // cfo->report_bg_io_stats = true;
 // auto t0 = std::chrono::steady_clock::now();
 // db->CreateColumnFamily(*(cfo.get()), cf_name, &cfh);
 // auto t1 = std::chrono::steady_clock::now();

//  std::cout << "Time for CreateColumnFamily() = " << std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count() << "[us]" << std::endl;

  delete db;
  db = nullptr;
}
*/
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
