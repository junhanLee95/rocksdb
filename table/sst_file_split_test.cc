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

  void CreateMTD(DB* db,  const std::vector<std::string>& keys) {
    for (size_t i = 0; i < keys.size(); i ++) {
      db->Put(WriteOptions(), Slice(keys[i]), Slice(keys[i]));
    }

    /*for (size_t i = 0; i + 2 < keys.size(); i += 3) {
      db->Put(WriteOptions(), cfh, Slice(keys[i]), Slice(keys[i]));
      db->Merge(WriteOptions(), cfh, Slice(keys[i+1]), Slice(keys[i]));
      db->Delete(WriteOptions(), cfh, Slice(keys[i+2]));
    }*/
  }

  void CreateMTBatch(DB* db, ColumnFamilyHandle* cfh, const std::vector<std::string>& keys) {
    WriteBatch batch;
    for (size_t i = 0; i < keys.size(); i ++) {
      batch.Put(cfh, Slice(keys[i]), Slice(keys[i]));
    }
    db->Write(WriteOptions(), &batch);
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

// two L1, empty L0, empty MemTables in cf_anon
/*
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
  std::vector<std::string> families;
  db->ListColumnFamilies(options, db_name, &families);
  int cf_id = 0;
  for (std::string name: families) {
    std::cout << "cf[" << cf_id << "] : " << name << std::endl;
    cf_id++;
  }

  dbfull(db)->DestroyLogicalColumnFamilies();

  std::cout << "===========After Destroy LCF========" << std::endl;
  db->ListColumnFamilies(options, db_name, &families);
  cf_id = 0;
  for (std::string name: families) {
    std::cout << "cf[" << cf_id << "] : " << name << std::endl;
    cf_id++;
  }

  dbfull(db)->PrintLogicalColumnFamily();

  delete db;
} */
/*
// two L1, empty L0, one MemTables in cf_anon
TEST_F(SstFileSplitTest, SplitColumnFamilyBackground2) {
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
  std::cout << "cfh : " << cfh->GetName().c_str() << std::endl;
  CreateMT(db, cfh, keys);
  db->Flush(FlushOptions(), cfh);
  dbfull(db)->TEST_WaitForFlushMemTable(cfh);
  
  CreateMT(db, cfh, keys);
  db->Flush(FlushOptions(), cfh);
  dbfull(db)->TEST_WaitForFlushMemTable(cfh);

  CreateMT(db, cfh, keys);
  db->Flush(FlushOptions(), cfh);
  dbfull(db)->TEST_WaitForFlushMemTable(cfh);

  CreateMT(db, cfh, keys);
  db->Flush(FlushOptions(), cfh);
  dbfull(db)->TEST_WaitForFlushMemTable(cfh);

  CreateMT(db, cfh, keys);
  db->Flush(FlushOptions(), cfh);
  dbfull(db)->TEST_WaitForFlushMemTable(cfh);


  CreateMTBatch(db, cfh, keys);
  dbfull(db)->TEST_WaitForCompact();

  std::cout << "After compaction: sst count : " << GetSstFileCount(db->GetName()) << std::endl;

  dbfull(db)->DestroyLogicalColumnFamilies();
  delete db;
}*/

// two L1(which consists of odd keys),
// empty L0, one MemTables(which consists of even keys) in cf_anon.
/*
TEST_F(SstFileSplitTest, SplitColumnFamilyBackground3) {
  std::vector<std::string> odd_keys;
  std::vector<std::string> even_keys;
  for (uint64_t i = 0; i < kNumKeys; i++) {
    odd_keys.emplace_back(EncodeAsString(2*i+1));
    even_keys.emplace_back(EncodeAsString(2*i));
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
  cfo->level0_file_num_compaction_trigger = 3;
  cfo->target_file_size_base = 4*1024*1024; // 4MB
  cfo->report_bg_io_stats = true;

  db->CreateColumnFamily(*(cfo.get()), cf_name, &cfh);

  CreateMT(db, cfh, odd_keys);
  db->Flush(FlushOptions(), cfh);
  dbfull(db)->TEST_WaitForCompact();
  
  CreateMT(db, cfh, odd_keys);
  db->Flush(FlushOptions(), cfh);
  dbfull(db)->TEST_WaitForCompact();

  CreateMT(db, cfh, odd_keys);
  db->Flush(FlushOptions(), cfh);
  dbfull(db)->TEST_WaitForCompact();

  CreateMT(db, cfh, odd_keys);
  db->Flush(FlushOptions(), cfh);

  CreateMTBatch(db, cfh, even_keys);
  dbfull(db)->TEST_WaitForCompact();

  std::cout << "After compaction: sst count : " << GetSstFileCount(db->GetName()) << std::endl;

  dbfull(db)->DestroyLogicalColumnFamilies();
  delete db;
}*/

// Parent(default) is splitted to create Child(default0)
// Child(default0) is splitted to create GrandChild(default00)
// We test whether GrandChild(default00) is successfully created.
TEST_F(SstFileSplitTest, GrandChildSplit) {
  std::vector<std::string> odd_keys;
  std::vector<std::string> even_keys;
  for (uint64_t i = 0; i < kNumKeys; i++) {
    odd_keys.emplace_back(EncodeAsString(2*i+1));
    even_keys.emplace_back(EncodeAsString(2*i));
  }
  FlushOptions foptions;
  foptions.allow_write_stall = true;

  Options options;
  options.create_if_missing = true;
  options.allow_column_family_split = true;
  options.atomic_flush = true;

  std::string db_name = test::PerThreadDBPath("test_db");
  DB* db;
  ASSERT_OK(DB::Open(options, db_name, &db));

  CreateMTD(db, odd_keys);
  db->Flush(FlushOptions());
  //dbfull(db)->TEST_WaitForFlushMemTable();
 
  CreateMTD(db, odd_keys);
  db->Flush(FlushOptions());
 // dbfull(db)->TEST_WaitForFlushMemTable();
 
  CreateMTD(db, odd_keys);
  db->Flush(FlushOptions());
  //dbfull(db)->TEST_WaitForFlushMemTable();
 
  CreateMTD(db, odd_keys);
  db->Flush(FlushOptions());
  //dbfull(db)->TEST_WaitForFlushMemTable();
 
  CreateMTD(db, even_keys);
  db->Flush(foptions, dbfull(db)->GetColumnFamilyHandle(1));
  
  CreateMTD(db, even_keys);
  db->Flush(foptions, dbfull(db)->GetColumnFamilyHandle(1));
 
  CreateMTD(db, even_keys);
  db->Flush(foptions, dbfull(db)->GetColumnFamilyHandle(1));
  
  CreateMTD(db, even_keys);
  db->Flush(foptions, dbfull(db)->GetColumnFamilyHandle(1));

  // pair : {TestName, key}
  std::vector<std::pair<std::string, uint32_t>> GetTestKey 
    = {{"Parent Node search (odd key)", kNumKeys + 1},
       {"Parent Node search (odd key)", kNumKeys + kNumKeys/2 + 1},
       {"Parent Node search (odd key)", kNumKeys - kNumKeys/2 + 1},
       {"Parent search (odd key)", kNumKeys + 1},
       {"Child Node search (even key)", kNumKeys/4},
       {"Child Node search (even key)", kNumKeys + kNumKeys/2},
       {"Child Node search (even key)", kNumKeys - 2},
       {"Child Node search (even key)", kNumKeys},
       {"Child Node search (even key)", kNumKeys + 2}};
  
  for (auto p: GetTestKey) {
    std::ostringstream ss;
    ss << std::setw(8) << std::setfill('0') << p.second;
    std::string key = ss.str();
    std::string test_name = p.first; 
    std::string value; 
    Status s; 

    s = db->Get(ReadOptions(), key, &value);
   
    fprintf(stdout, "[SplitTest] [%s] Key [%s] ", test_name.c_str(), key.c_str());

    if (s.IsNotFound())
      fprintf(stdout, "Not Found...\n");
    if (s.ok())
      fprintf(stdout, "==> %s\n", value.c_str());
  }

  std::cout << "After compaction: sst count : " << GetSstFileCount(db->GetName()) << std::endl;

  dbfull(db)->DestroyLogicalColumnFamilies();
  delete db;
}

// Parent(default) is splitted to create Child(default0)
// We call flush on Parent(default) to verify flush is successful after split.
// At this time, we allow write stall when flush.

TEST_F(SstFileSplitTest, FlushParentAfterSplitAllowWriteStall) {
  std::vector<std::string> odd_keys;
  std::vector<std::string> even_keys;
  std::vector<std::string> odd_big_keys;
  for (uint64_t i = 0; i < kNumKeys; i++) {
    odd_keys.emplace_back(EncodeAsString(2*i+1));
    even_keys.emplace_back(EncodeAsString(2*i));
    odd_big_keys.emplace_back(EncodeAsString(kNumKeys*2+2*i+1));
  }
  FlushOptions foptions;
  foptions.allow_write_stall =true;

  Options options;
  options.create_if_missing = true;
  options.allow_column_family_split = true;
  options.max_write_buffer_number = 2;
  options.atomic_flush = true;

  std::string db_name = test::PerThreadDBPath("test_db");
  DB* db;
  ASSERT_OK(DB::Open(options, db_name, &db));

  CreateMTD(db, odd_keys);
  db->Flush(FlushOptions());
  //dbfull(db)->TEST_WaitForFlushMemTable();
 
  CreateMTD(db, odd_keys);
  db->Flush(FlushOptions());
 // dbfull(db)->TEST_WaitForFlushMemTable();
 
  CreateMTD(db, odd_keys);
  db->Flush(FlushOptions());
 // dbfull(db)->TEST_WaitForFlushMemTable();
 
  CreateMTD(db, odd_keys);
  db->Flush(FlushOptions());
 // dbfull(db)->TEST_WaitForFlushMemTable();

  /////
  CreateMTD(db, odd_big_keys);
  db->Flush(foptions);
  
  CreateMTD(db, odd_big_keys);
  db->Flush(foptions);
 
  CreateMTD(db, odd_big_keys);
  db->Flush(foptions);
 



  // pair : {TestName, key}
  std::vector<std::pair<std::string, uint32_t>> GetTestKey 
    = {{"Parent Node search (odd key)", kNumKeys + 1},
       {"Parent Node search (odd key)", kNumKeys + kNumKeys/2 + 1},
       {"Parent Node search (odd key)", kNumKeys - kNumKeys/2 + 1},
       {"Parent search (odd key)", kNumKeys + 1},
       {"Child Node search (even key)", kNumKeys/4},
       {"Child Node search (even key)", kNumKeys + kNumKeys/2},
       {"Child Node search (even key)", kNumKeys - 2},
       {"Child Node search (even key)", kNumKeys},
       {"Child Node search (even key)", kNumKeys + 2}};
  
  for (auto p: GetTestKey) {
    std::ostringstream ss;
    ss << std::setw(8) << std::setfill('0') << p.second;
    std::string key = ss.str();
    std::string test_name = p.first; 
    std::string value; 
    Status s; 

    s = db->Get(ReadOptions(), key, &value);
   
    fprintf(stdout, "[SplitTest] [%s] Key [%s] ", test_name.c_str(), key.c_str());

    if (s.IsNotFound())
      fprintf(stdout, "Not Found...\n");
    if (s.ok())
      fprintf(stdout, "==> %s\n", value.c_str());
  }

  std::cout << "After compaction: sst count : " << GetSstFileCount(db->GetName()) << std::endl;

  dbfull(db)->DestroyLogicalColumnFamilies();
  delete db;
} 


// Parent(default) is splitted to create Child(default0)
// We call flush on Parent(default) to verify flush is successful after split.
// At this time, we don't allow write stall when flush.

TEST_F(SstFileSplitTest, FlushParentAfterSplitDisallowWriteStall) {
  std::vector<std::string> odd_keys;
  std::vector<std::string> even_keys;
  std::vector<std::string> odd_big_keys;
  for (uint64_t i = 0; i < kNumKeys; i++) {
    odd_keys.emplace_back(EncodeAsString(2*i+1));
    even_keys.emplace_back(EncodeAsString(2*i));
    odd_big_keys.emplace_back(EncodeAsString(kNumKeys*2+2*i+1));
  }
  FlushOptions foptions;
  foptions.allow_write_stall = false;

  Options options;
  options.create_if_missing = true;
  options.allow_column_family_split = true;
  options.max_write_buffer_number = 2;
  options.atomic_flush = true;

  std::string db_name = test::PerThreadDBPath("test_db");
  DB* db;
  ASSERT_OK(DB::Open(options, db_name, &db));

  CreateMTD(db, odd_keys);
  db->Flush(FlushOptions());
  //dbfull(db)->TEST_WaitForFlushMemTable();
 
  CreateMTD(db, odd_keys);
  db->Flush(FlushOptions());
  //dbfull(db)->TEST_WaitForFlushMemTable();
 
  CreateMTD(db, odd_keys);
  db->Flush(FlushOptions());
  //dbfull(db)->TEST_WaitForFlushMemTable();
 
  CreateMTD(db, odd_keys);
  db->Flush(FlushOptions());
  //dbfull(db)->TEST_WaitForFlushMemTable();

  /////
  CreateMTD(db, odd_big_keys);
  db->Flush(foptions);
  
  CreateMTD(db, odd_big_keys);
  db->Flush(foptions);
 
  CreateMTD(db, odd_big_keys);
  db->Flush(foptions);
 



  // pair : {TestName, key}
  std::vector<std::pair<std::string, uint32_t>> GetTestKey 
    = {{"Parent Node search (odd key)", kNumKeys + 1},
       {"Parent Node search (odd key)", kNumKeys + kNumKeys/2 + 1},
       {"Parent Node search (odd key)", kNumKeys - kNumKeys/2 + 1},
       {"Parent search (odd key)", kNumKeys + 1},
       {"Child Node search (even key)", kNumKeys/4},
       {"Child Node search (even key)", kNumKeys + kNumKeys/2},
       {"Child Node search (even key)", kNumKeys - 2},
       {"Child Node search (even key)", kNumKeys},
       {"Child Node search (even key)", kNumKeys + 2}};
  
  for (auto p: GetTestKey) {
    std::ostringstream ss;
    ss << std::setw(8) << std::setfill('0') << p.second;
    std::string key = ss.str();
    std::string test_name = p.first; 
    std::string value; 
    Status s; 

    s = db->Get(ReadOptions(), key, &value);
   
    fprintf(stdout, "[SplitTest] [%s] Key [%s] ", test_name.c_str(), key.c_str());

    if (s.IsNotFound())
      fprintf(stdout, "Not Found...\n");
    if (s.ok())
      fprintf(stdout, "==> %s\n", value.c_str());
  }

  std::cout << "After compaction: sst count : " << GetSstFileCount(db->GetName()) << std::endl;

  dbfull(db)->DestroyLogicalColumnFamilies();
  delete db;
} 

/*
// Parent(default) has one L1, which is composed of odd keys
// Child(default0) has one memtable, which is composed of even keys
// We verify all the keys we put can be acquired by db->Get() call
TEST_F(SstFileSplitTest, GetParentOneL1ChildOneMem) {
  std::vector<std::string> odd_keys;
  std::vector<std::string> even_keys;
  for (uint64_t i = 0; i < kNumKeys; i++) {
    odd_keys.emplace_back(EncodeAsString(2*i+1));
    even_keys.emplace_back(EncodeAsString(2*i));
  }

  Options options;
  options.create_if_missing = true;
  options.allow_column_family_split = true;
  std::string db_name = test::PerThreadDBPath("test_db");
  DB* db;
  ASSERT_OK(DB::Open(options, db_name, &db));

  CreateMTD(db, odd_keys);
  db->Flush(FlushOptions());
  dbfull(db)->TEST_WaitForFlushMemTable();
 
  CreateMTD(db, odd_keys);
  db->Flush(FlushOptions());
  dbfull(db)->TEST_WaitForFlushMemTable();
 
  CreateMTD(db, odd_keys);
  db->Flush(FlushOptions());
  dbfull(db)->TEST_WaitForFlushMemTable();
 
  CreateMTD(db, odd_keys);
  db->Flush(FlushOptions());
  dbfull(db)->TEST_WaitForFlushMemTable();
 
  CreateMTD(db, even_keys);


  // pair : {TestName, key}
  std::vector<std::pair<std::string, uint32_t>> GetTestKey 
    = {{"Parent Node search (odd key)", kNumKeys + 1},
       {"Parent Node search (odd key)", kNumKeys + kNumKeys/2 + 1},
       {"Parent Node search (odd key)", kNumKeys - kNumKeys/2 + 1},
       {"Parent search (odd key)", kNumKeys + 1},
       {"Child Node search (even key)", kNumKeys/4},
       {"Child Node search (even key)", kNumKeys + kNumKeys/2},
       {"Child Node search (even key)", kNumKeys - 2},
       {"Child Node search (even key)", kNumKeys},
       {"Child Node search (even key)", kNumKeys + 2}};
  
  for (auto p: GetTestKey) {
    std::ostringstream ss;
    ss << std::setw(8) << std::setfill('0') << p.second;
    std::string key = ss.str();
    std::string test_name = p.first; 
    std::string value; 
    Status s; 

    s = db->Get(ReadOptions(), key, &value);
   
    fprintf(stdout, "[SplitTest] [%s] Key [%s] ", test_name.c_str(), key.c_str());

    if (s.IsNotFound())
      fprintf(stdout, "Not Found...\n");
    if (s.ok())
      fprintf(stdout, "==> %s\n", value.c_str());
  }

  std::cout << "After compaction: sst count : " << GetSstFileCount(db->GetName()) << std::endl;

  dbfull(db)->DestroyLogicalColumnFamilies();
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
