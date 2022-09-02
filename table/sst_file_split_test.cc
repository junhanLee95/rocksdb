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
/*
TEST_F(SstFileSplitTest, SplitColumnFamilySimple) {
  std::vector<std::string> keys;
  for (uint64_t i = 0; i < kNumKeys; i++) {
    keys.emplace_back(EncodeAsString(i));
  }

  // Generate a SST file.
  CreateFile(sst_name_, keys);

  // Ingest the file into a db, to assign it a global sequence number.
  Options options;
  options.create_if_missing = true;
  std::string db_name = test::PerThreadDBPath("test_db");
  DB* db;
  ASSERT_OK(DB::Open(options, db_name, &db));

  // create column family
  ColumnFamilyHandle* cfh;
  std::string cf_name = "cf0";
  db->CreateColumnFamily(ColumnFamilyOptions(), cf_name, &cfh);
  
  // Bump sequence number.
  for (uint64_t i = 0; i < kNumKeys; i++) {
    std::string val = "foo" + std::to_string(i);
    ASSERT_OK(db->Put(WriteOptions(), cfh, keys[i], val));
  }
  ASSERT_OK(db->Flush(FlushOptions(), cfh));

  std::vector<std::string> live_files;
  std::string sst_file = "";
  uint64_t manifest_file_size;
  db->GetLiveFiles(live_files, &manifest_file_size);

  // check live sst files
  for (auto& live_file : live_files) {
    if (live_file.substr(live_file.size() - 4, std::string::npos) == ".sst") {
      sst_file = live_file;
    }
  }
  ASSERT_TRUE(sst_file.compare(""));

  // Split
  ColumnFamilyHandle *cfh1, *cfh2;
  db->SplitColumnFamily(ColumnFamilyOptions(), &cfh, &cfh1, &cfh2);

  // Check list
  std::vector<std::string> cfnames;
  db->ListColumnFamilies(db->GetDBOptions(), db->GetName(), &cfnames); 

  for(size_t i = 0; i < cfnames.size(); i++){
    std::cout << "cf[" << i << "] : " << cfnames[i] << std::endl;
  }  

  SstFileReader reader0(options_);
  ASSERT_OK(reader0.Open(db->GetName() + sst_file));
  ASSERT_OK(reader0.VerifyChecksum());
  auto properties0 = reader0.GetTableProperties();
  fprintf(stdout,"[INFO] cf0 properties : %s\n", properties0->ToString().c_str());
 
  
  ReadOptions ropts;
  SstFileReader reader1(options_);
  ASSERT_OK(reader1.Open(db->GetName() + "split_sst0.sst"));
  ASSERT_OK(reader1.VerifyChecksum());
  
  auto properties1 = reader1.GetTableProperties();
  fprintf(stdout,"[INFO] cf1 name : %s\n", properties1->column_family_name.c_str());
  fprintf(stdout,"[INFO] cf1 id : %ld\n", properties1->column_family_id);
  fprintf(stdout,"[INFO] cf1 num entries : %ld\n", properties1->num_entries);
  fprintf(stdout,"[INFO] cf1 properties : %s\n", properties1->ToString().c_str());

  SstFileReader reader2(options_);
  ASSERT_OK(reader2.Open(db->GetName() + "split_sst1.sst"));
  ASSERT_OK(reader2.VerifyChecksum());
  auto properties2 = reader2.GetTableProperties();
  fprintf(stdout,"[INFO] cf2 name : %s\n", properties2->column_family_name.c_str());
  fprintf(stdout,"[INFO] cf2 id : %ld\n", properties2->column_family_id);
  fprintf(stdout,"[INFO] cf2 num entries : %ld\n", properties2->num_entries);
  fprintf(stdout,"[INFO] cf2 properties : %s\n", properties2->ToString().c_str());
  

  // Cleanup.
  db->DropColumnFamily(cfh);
  db->DestroyColumnFamilyHandle(cfh);
  db->DropColumnFamily(cfh1);
  db->DestroyColumnFamilyHandle(cfh1);
  db->DropColumnFamily(cfh2);
  db->DestroyColumnFamilyHandle(cfh2);

  //ASSERT_OK(DestroyDB(db_name, options));
  delete db;
}

TEST_F(SstFileSplitTest, SplitColumnFamilyMultiple) {
  std::vector<std::string> keys;
  for (uint64_t i = 0; i < kNumKeys; i++) {
    keys.emplace_back(EncodeAsString(i));
  }

  // Generate a SST file.
  CreateFile(sst_name_, keys);

  // Ingest the file into a db, to assign it a global sequence number.
  Options options;
  options.create_if_missing = true;
  std::string db_name = test::PerThreadDBPath("test_db");
  DB* db;
  ASSERT_OK(DB::Open(options, db_name, &db));

  // create column family
  ColumnFamilyHandle* cfh;
  std::string cf_name = "cf0";
  db->CreateColumnFamily(ColumnFamilyOptions(), cf_name, &cfh);
  
  // Bump sequence number.
  for (uint64_t i = 0; i < kNumKeys; i++) {
    std::string val = "foo" + std::to_string(i);
    ASSERT_OK(db->Put(WriteOptions(), cfh, keys[i], val));
  }
  ASSERT_OK(db->Flush(FlushOptions(), cfh));

  // Let's make two sst files
  for (uint64_t i = 0; i < kNumKeys; i++) {
    std::string val = "foo2" + std::to_string(i);
    ASSERT_OK(db->Put(WriteOptions(), cfh, keys[i]+"2", val));
  }
  ASSERT_OK(db->Flush(FlushOptions(), cfh));

  std::vector<std::string> live_files;
  std::vector<std::string> sst_files;
  uint64_t manifest_file_size;
  db->GetLiveFiles(live_files, &manifest_file_size);

  // check live sst files
  for (auto& live_file : live_files) {
    if (live_file.substr(live_file.size() - 4, std::string::npos) == ".sst") {
      sst_files.push_back(live_file);
    }
  }
  ASSERT_TRUE(sst_files.size() > 0);


  // Split
  ColumnFamilyHandle *cfh1, *cfh2;
  db->SplitColumnFamily(ColumnFamilyOptions(), &cfh, &cfh1, &cfh2);

  // Check list
  std::vector<std::string> cfnames;
  db->ListColumnFamilies(db->GetDBOptions(), db->GetName(), &cfnames); 

  for(size_t i = 0; i < cfnames.size(); i++){
    std::cout << "cf[" << i << "] : " << cfnames[i] << std::endl;
  }  

  for (auto& sst_file: sst_files) {
    SstFileReader reader0(options_);
    ASSERT_OK(reader0.Open(db->GetName() + sst_file));
    ASSERT_OK(reader0.VerifyChecksum());
    auto properties0 = reader0.GetTableProperties();
    fprintf(stdout,"[INFO] cf0 properties : %s\n", properties0->ToString().c_str());
  }
  
  ReadOptions ropts;
  SstFileReader reader1(options_);
  ASSERT_OK(reader1.Open(db->GetName() + "split_sst0.sst"));
  ASSERT_OK(reader1.VerifyChecksum());
  
  auto properties1 = reader1.GetTableProperties();
  fprintf(stdout,"[INFO] cf1 name : %s\n", properties1->column_family_name.c_str());
  fprintf(stdout,"[INFO] cf1 id : %ld\n", properties1->column_family_id);
  fprintf(stdout,"[INFO] cf1 num entries : %ld\n", properties1->num_entries);
  fprintf(stdout,"[INFO] cf1 properties : %s\n", properties1->ToString().c_str());

  SstFileReader reader2(options_);
  ASSERT_OK(reader2.Open(db->GetName() + "split_sst1.sst"));
  ASSERT_OK(reader2.VerifyChecksum());
  auto properties2 = reader2.GetTableProperties();
  fprintf(stdout,"[INFO] cf2 name : %s\n", properties2->column_family_name.c_str());
  fprintf(stdout,"[INFO] cf2 id : %ld\n", properties2->column_family_id);
  fprintf(stdout,"[INFO] cf2 num entries : %ld\n", properties2->num_entries);
  fprintf(stdout,"[INFO] cf2 properties : %s\n", properties2->ToString().c_str());
  

  // Cleanup.
  db->DropColumnFamily(cfh);
  db->DestroyColumnFamilyHandle(cfh);
  db->DropColumnFamily(cfh1);
  db->DestroyColumnFamilyHandle(cfh1);
  db->DropColumnFamily(cfh2);
  db->DestroyColumnFamilyHandle(cfh2);

  //ASSERT_OK(DestroyDB(db_name, options));
  delete db;
}
*/
/*
TEST_F(SstFileSplitTest, SplitColumnFamilyMultipleOverlapped) {
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
  cfo->target_file_size_base = 1024*1024; // 1MB
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
}*/

TEST_F(SstFileSplitTest, SplitColumnFamilyMultipleOverlapped) {
  std::vector<std::string> keys;
  for (uint64_t i = 0; i < kNumKeys; i++) {
    keys.emplace_back(EncodeAsString(i));
  }
  std::cout << "[SplitTest] keys [" << EncodeAsString(0) << ", " << EncodeAsString(kNumKeys-1) << "]"<<std::endl;
  std::vector<std::string> keys2;
  for (uint64_t i = kNumKeys; i < 2*kNumKeys; i++) {
    keys2.emplace_back(EncodeAsString(i));
  }
  std::cout << "[SplitTest] keys2 [" << EncodeAsString(kNumKeys) << ", " << EncodeAsString(2*kNumKeys-1) << "]"<<std::endl;
  
  std::vector<std::string> keys3;
  for (uint64_t i = kNumKeys*2; i < 3*kNumKeys; i++) {
    keys3.emplace_back(EncodeAsString(i));
  }
  std::cout << "[SplitTest] keys3 [" << EncodeAsString(kNumKeys*2) << ", " << EncodeAsString(3*kNumKeys-1) << "]"<< std::endl;
  std::vector<std::string> keys4;
  for (uint64_t i = 3*kNumKeys; i < 4*kNumKeys; i++) {
    keys4.emplace_back(EncodeAsString(i));
  }
  std::cout << "[SplitTest] keys4 [" << EncodeAsString(kNumKeys*3) << ", " << EncodeAsString(4*kNumKeys-1) << "]"<<std::endl;
  
  std::vector<std::string> keys5;
  for (uint64_t i = kNumKeys/2; i < 4*kNumKeys; i++) {
    keys5.emplace_back(EncodeAsString(i));
  }
  std::cout << "[SplitTest] keys5 [" << EncodeAsString(kNumKeys/2) << ", " << EncodeAsString(4*kNumKeys-1) << "]"<<std::endl;

  // Ingest the file into a db, to assign it a global sequence number.
  Options options;
  options.create_if_missing = true;
  std::string db_name = test::PerThreadDBPath("test_db");
  DB* db;
  ASSERT_OK(DB::Open(options, db_name, &db));

  // create column family
  std::cout << "[SplitTest] create cf : cf0" << std::endl;
  ColumnFamilyHandle* cfh = db->DefaultColumnFamily();
  /*std::string cf_name = "cf0";

  std::unique_ptr<ColumnFamilyOptions> cfo(new ColumnFamilyOptions());
  cfo->compaction_style = kCompactionStyleLevel;
  cfo->num_levels = 2;
  cfo->write_buffer_size = 64 << 20; // 64 MB
  cfo->target_file_size_base = 1024*1024; // 1MB
  cfo->level0_file_num_compaction_trigger = 4; 

  db->CreateColumnFamily(*(cfo.get()), cf_name, &cfh);*/

  // Generate a SST file.
  CreateMT(db, cfh, keys);
  db->Flush(FlushOptions(), cfh);
  dbfull(db)->TEST_WaitForCompact();
  CreateMT(db, cfh, keys2);
  db->Flush(FlushOptions(), cfh);
  dbfull(db)->TEST_WaitForCompact();
  CreateMT(db, cfh, keys3);
  db->Flush(FlushOptions(), cfh);
  dbfull(db)->TEST_WaitForCompact();
  CreateMT(db, cfh, keys4);
  db->Flush(FlushOptions(), cfh);
  dbfull(db)->TEST_WaitForCompact();

  CreateMT(db, cfh, keys5);

  ASSERT_EQ(4, GetSstFileCount(db->GetName()));
  
  // Split
  ColumnFamilyHandle *cfh1, *cfh2;
  std::cout << "[SplitTest] split cf : cf0" << std::endl;
  const char* median_key = (reinterpret_cast<ColumnFamilyHandleImpl*> (cfh))->cfd()->current()->storage_info()->GetMedianKey().ToString().c_str() ;
  std::cout << "[SplitTest] median key : " << median_key << std::endl;

  dbfull(db)->PrintLogicalColumnFamily();
  db->SplitColumnFamily(ColumnFamilyOptions(), &cfh, &cfh1, &cfh2);
 
  fprintf(stdout,"[SplitTest] cf00 : smallest key :  -> %s\n", static_cast<ColumnFamilyHandleImpl*>(cfh1)->cfd()->GetSmallestKey().c_str());
  fprintf(stdout,"[SplitTest] cf00 : largest key :  -> %s\n", static_cast<ColumnFamilyHandleImpl*>(cfh1)->cfd()->GetLargestKey().c_str());
 
  fprintf(stdout,"[SplitTest] cf01 : smallest key :  -> %s\n", static_cast<ColumnFamilyHandleImpl*>(cfh2)->cfd()->GetSmallestKey().c_str());
  fprintf(stdout,"[SplitTest] cf01 : largest key :  -> %s\n", static_cast<ColumnFamilyHandleImpl*>(cfh2)->cfd()->GetLargestKey().c_str());


  fprintf(stdout,"[SplitTest] wait split\n"); 
  dbfull(db)->TEST_WaitForSplit();
  fprintf(stdout,"[SplitTest] wait split done\n");
  
  for (auto c: static_cast<ColumnFamilyHandleImpl*>(cfh)->cfd()->children_cfds) {
    fprintf(stdout,"[childrencfd] %s : smallest key :  %s\n", c->GetName().c_str(), c->GetSmallestKey().c_str());
    fprintf(stdout,"[childrencfd] %s : largest key : %s\n", c->GetName().c_str(), c->GetLargestKey().c_str());
  }


  std::string value1, value2, value3;
  std::string value12, value22, value32;
  std::string value13, value23, value33;
  db->Get(ReadOptions(), cfh, "00001999", &value1);
  db->Get(ReadOptions(), cfh1, "00001999",&value2);
  db->Get(ReadOptions(), cfh2, "00001999", &value3);
  db->Get(ReadOptions(), cfh, "00002001", &value12);
  db->Get(ReadOptions(), cfh1, "00002001",&value22);
  db->Get(ReadOptions(), cfh2, "00002001", &value32);
  db->Get(ReadOptions(), cfh, "00002000", &value13);
  db->Get(ReadOptions(), cfh1, "00002000",&value23);
  db->Get(ReadOptions(), cfh2, "00002000", &value33);
  
  fprintf(stdout,"[SplitTest] cf0 : smallest key :  -> %s\n", static_cast<ColumnFamilyHandleImpl*>(cfh)->cfd()->GetSmallestKey().c_str());
  fprintf(stdout,"[SplitTest] cf0 : largest key :  -> %s\n", static_cast<ColumnFamilyHandleImpl*>(cfh)->cfd()->GetLargestKey().c_str());
 
  fprintf(stdout,"[SplitTest] cf00 : smallest key :  -> %s\n", static_cast<ColumnFamilyHandleImpl*>(cfh1)->cfd()->GetSmallestKey().c_str());
  fprintf(stdout,"[SplitTest] cf00 : largest key :  -> %s\n", static_cast<ColumnFamilyHandleImpl*>(cfh1)->cfd()->GetLargestKey().c_str());
 
  fprintf(stdout,"[SplitTest] cf01 : smallest key :  -> %s\n", static_cast<ColumnFamilyHandleImpl*>(cfh2)->cfd()->GetSmallestKey().c_str());
  fprintf(stdout,"[SplitTest] cf01 : largest key :  -> %s\n", static_cast<ColumnFamilyHandleImpl*>(cfh2)->cfd()->GetLargestKey().c_str());




  fprintf(stdout,"[SplitTest] cf0 : key less than median(00001999) -> %s\n", value1.c_str());
  fprintf(stdout,"[SplitTest] cf00 : key less than median(00001999) -> %s\n", value2.c_str());
  fprintf(stdout,"[SplitTest] cf01 : key less than median(00001999) -> %s\n", value3.c_str());
   
  fprintf(stdout,"[SplitTest] cf0 : key greater than median(00002001) -> %s\n", value12.c_str());
  fprintf(stdout,"[SplitTest] cf00 : key greater than median(00002001) -> %s\n", value22.c_str());
  fprintf(stdout,"[SplitTest] cf01 : key greater than median(00002001) -> %s\n", value32.c_str());
  
  fprintf(stdout,"[SplitTest] cf0 : key median -> %s\n", value13.c_str());
  fprintf(stdout,"[SplitTest] cf00 : key median -> %s\n", value23.c_str());
  fprintf(stdout,"[SplitTest] cf01 : key median -> %s\n", value33.c_str());
 
  dbfull(db)->PrintLogicalColumnFamily();
  /*auto lcf = dbfull(db)->GetLogicalColumnFamily();
  for (auto l: lcf) {
    fprintf(stdout, "[SplitTest] LCF[%d] %s => [%s, %s)\n", l->GetID(), l->GetName().c_str(), l->GetSmallestKey().c_str(), l->GetLargestKey().c_str());
  }*/
  //db->DropColumnFamily(cfh);
  db->DestroyColumnFamilyHandle(cfh1);
  db->DestroyColumnFamilyHandle(cfh2);
  // db->DestroyColumnFamilyHandle(cfh); // Default column family automatically removed when the dbimpl is removed. 

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
