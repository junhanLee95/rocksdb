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

class LCFIteratorTest : public testing::Test {
 public:
  LCFIteratorTest() {
    options_.merge_operator = MergeOperators::CreateUInt64AddOperator();
    sst_name_ = test::PerThreadDBPath("sst_file");
  }

  ~LCFIteratorTest() {
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
/*
TEST_F(LCFIteratorTest, SimpleSeek) {
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

  std::vector<SplitFileInfo> infos;
  FileMetaData* f1 = new FileMetaData;
  std::string s1 = "c";
  std::string l1 = "f";
  f1->smallest = InternalKey(Slice(s1), 0, kTypeValue);
  f1->largest = InternalKey(Slice(l1), 0, kTypeValue);
  infos.push_back(SplitFileInfo(f1, cfd));

  dbfull(db)->SplitColumnFamilyFromSstFiles(infos);
  dbfull(db)->TEST_WaitForSplit();
  size_t numItem = 10;
  std::vector<std::string> keys;
  std::vector<std::string> values;

  for(size_t i = 0; i < numItem; i++) {
    std::string k(1, 'a' + i);
    std::string v = "v" + k;
    keys.push_back(k); 
    values.push_back(v); 
  }

  for(size_t i = 0; i < numItem; i++) {
    db->Put(WriteOptions(), cfh, Slice(keys[i]), Slice(values[i]));
  }

  std::unique_ptr<Iterator> iterator(dbfull(db)->NewIterator(ReadOptions(), cfh));
  for(size_t i = 0; i < numItem; i++) {
    iterator->Seek(keys[i]);
    //fprintf(stdout, "value : %s\n", iterator->value().data());
    //fprintf(stdout, "value size : %ld\n", iterator->value().size());
    ASSERT_EQ(iterator->value().ToString(), values[i]);
  }
  fprintf(stdout, "Value test is success\n"); 
  dbfull(db)->DestroyLogicalColumnFamilies();
  delete db;
}
*/
/*
TEST_F(LCFIteratorTest, DoubleSeek) {
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

  std::vector<SplitFileInfo> infos;
  FileMetaData* f1 = new FileMetaData;
  std::string s1 = "c";
  std::string l1 = "d";
  f1->smallest = InternalKey(Slice(s1), 0, kTypeValue);
  f1->largest = InternalKey(Slice(l1), 0, kTypeValue);
  FileMetaData* f2 = new FileMetaData;
  std::string s2 = "e";
  std::string l2 = "f";
  f2->smallest = InternalKey(Slice(s2), 0, kTypeValue);
  f2->largest = InternalKey(Slice(l2), 0, kTypeValue);

  infos.push_back(SplitFileInfo(f1, cfd));
  infos.push_back(SplitFileInfo(f2, cfd));

  dbfull(db)->SplitColumnFamilyFromSstFiles(infos);
  dbfull(db)->TEST_WaitForSplit();
  size_t numItem = 10;
  std::vector<std::string> keys;
  std::vector<std::string> values;

  for(size_t i = 0; i < numItem; i++) {
    std::string k(1, 'a' + i);
    std::string v = "v" + k;
    keys.push_back(k); 
    values.push_back(v); 
  }

  for(size_t i = 0; i < numItem; i++) {
    db->Put(WriteOptions(), cfh, Slice(keys[i]), Slice(values[i]));
  }

  std::unique_ptr<Iterator> iterator(dbfull(db)->NewIterator(ReadOptions(), cfh));
  for(size_t i = 0; i < numItem; i++) {
    iterator->Seek(keys[i]);
    //fprintf(stdout, "value : %s\n", iterator->value().data());
    //fprintf(stdout, "value size : %ld\n", iterator->value().size());
    ASSERT_EQ(iterator->value().ToString(), values[i]);
  }
  fprintf(stdout, "Value test is success\n");
 
  dbfull(db)->DestroyLogicalColumnFamilies();
  delete db;
}
*/
/*
TEST_F(LCFIteratorTest, ThreeLevelSimpleSeek) {
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

  std::vector<SplitFileInfo> infos;
  FileMetaData* f1 = new FileMetaData;
  std::string s1 = "c";
  std::string l1 = "f";
  f1->smallest = InternalKey(Slice(s1), 0, kTypeValue);
  f1->largest = InternalKey(Slice(l1), 0, kTypeValue);
  infos.push_back(SplitFileInfo(f1, cfd));
  dbfull(db)->SplitColumnFamilyFromSstFiles(infos);
  dbfull(db)->TEST_WaitForSplit();

  ColumnFamilyData* cfd1 = cfd->GetColumnFamilySet()->GetColumnFamily(1);
  infos.clear();
  FileMetaData* f2 = new FileMetaData;
  std::string s2 = "d";
  std::string l2 = "e";
  f2->smallest = InternalKey(Slice(s2), 0, kTypeValue);
  f2->largest = InternalKey(Slice(l2), 0, kTypeValue);
  infos.push_back(SplitFileInfo(f2, cfd1));

  dbfull(db)->SplitColumnFamilyFromSstFiles(infos);
  dbfull(db)->TEST_WaitForSplit();
  size_t numItem = 10;
  std::vector<std::string> keys;
  std::vector<std::string> values;

  for(size_t i = 0; i < numItem; i++) {
    std::string k(1, 'a' + i);
    std::string v = "v" + k;
    keys.push_back(k); 
    values.push_back(v); 
  }

  for(size_t i = 0; i < numItem; i++) {
    db->Put(WriteOptions(), cfh, Slice(keys[i]), Slice(values[i]));
  }

  std::unique_ptr<Iterator> iterator(dbfull(db)->NewIterator(ReadOptions(), cfh));
  for(size_t i = 6; i < numItem; i++) {
    iterator->Seek(keys[i]);
    //fprintf(stdout, "value : %s\n", iterator->value().data());
    //fprintf(stdout, "value size : %ld\n", iterator->value().size());
    ASSERT_EQ(iterator->value().ToString(), values[i]);
  }
  fprintf(stdout, "Value test is success\n");
 
  dbfull(db)->DestroyLogicalColumnFamilies();
  delete db;
}
*/
/*
TEST_F(LCFIteratorTest, NewestSimpleSeek) {
  size_t numItem = 10;
  std::vector<std::string> keys;
  std::vector<std::string> values;
  std::vector<std::string> values2;

  for(size_t i = 0; i < numItem; i++) {
    std::string k(1, 'a' + i);
    std::string v = "v" + k;
    std::string w = "w" + k;
    keys.push_back(k); 
    values.push_back(v); 
    values2.push_back(w); 
  }

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
  for(size_t i = 0; i < numItem; i++) {
    db->Put(WriteOptions(), cfh, Slice(keys[i]), Slice(values[i]));
  }
  db->Flush(FlushOptions(), cfh);
  dbfull(db)->TEST_WaitForFlushMemTable(cfh);

  std::vector<SplitFileInfo> infos;
  FileMetaData* f1 = new FileMetaData;
  std::string s1 = "c";
  std::string l1 = "f";
  f1->smallest = InternalKey(Slice(s1), 0, kTypeValue);
  f1->largest = InternalKey(Slice(l1), 0, kTypeValue);
  infos.push_back(SplitFileInfo(f1, cfd));

  dbfull(db)->SplitColumnFamilyFromSstFiles(infos);
  dbfull(db)->TEST_WaitForSplit();
  for(size_t i = 0; i < numItem; i++) {
    db->Put(WriteOptions(), cfh, Slice(keys[i]), Slice(values2[i]));
  }

  std::unique_ptr<Iterator> iterator(dbfull(db)->NewIterator(ReadOptions(), cfh));
  for(size_t i = 0; i < numItem; i++) {
    iterator->Seek(keys[i]);
    ASSERT_EQ(iterator->value().ToString(), values2[i]);
  }
  fprintf(stdout, "Value test is success\n");
   
  //dbfull(db)->DestroyLogicalColumnFamilies();
  delete db;
}*/
/*
TEST_F(LCFIteratorTest, SimpleNext) {
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

  std::vector<SplitFileInfo> infos;
  FileMetaData* f1 = new FileMetaData;
  std::string s1 = "c";
  std::string l1 = "f";
  f1->smallest = InternalKey(Slice(s1), 0, kTypeValue);
  f1->largest = InternalKey(Slice(l1), 0, kTypeValue);
  infos.push_back(SplitFileInfo(f1, cfd));

  dbfull(db)->SplitColumnFamilyFromSstFiles(infos);
  dbfull(db)->TEST_WaitForSplit();
  size_t numItem = 10;
  std::vector<std::string> keys;
  std::vector<std::string> values;

  for(size_t i = 0; i < numItem; i++) {
    std::string k(1, 'a' + i);
    std::string v = "v" + k;
    keys.push_back(k); 
    values.push_back(v); 
  }

  for(size_t i = 0; i < numItem; i++) {
    db->Put(WriteOptions(), cfh, Slice(keys[i]), Slice(values[i]));
  }

  std::unique_ptr<Iterator> iterator(dbfull(db)->NewIterator(ReadOptions(), cfh));
  iterator->SeekToFirst();
  fprintf(stdout, "$$$$$$$$$$$smallest is %s\n",iterator->value().data());
  for(size_t i = 0; i < numItem; i++) {
    //fprintf(stdout, "value : %s\n", iterator->value().data());
    //fprintf(stdout, "value size : %ld\n", iterator->value().size());
    ASSERT_EQ(iterator->value().ToString(), values[i]);
	iterator->Next();
  }
  fprintf(stdout, "Value test is success\n");
 
  dbfull(db)->DestroyLogicalColumnFamilies();
  delete db;
}
*/
/*
TEST_F(LCFIteratorTest, DoubleNext) {
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

  std::vector<SplitFileInfo> infos;
  FileMetaData* f1 = new FileMetaData;
  std::string s1 = "c";
  std::string l1 = "d";
  f1->smallest = InternalKey(Slice(s1), 0, kTypeValue);
  f1->largest = InternalKey(Slice(l1), 0, kTypeValue);
  FileMetaData* f2 = new FileMetaData;
  std::string s2 = "e";
  std::string l2 = "f";
  f2->smallest = InternalKey(Slice(s2), 0, kTypeValue);
  f2->largest = InternalKey(Slice(l2), 0, kTypeValue);

  infos.push_back(SplitFileInfo(f1, cfd));
  infos.push_back(SplitFileInfo(f2, cfd));

  dbfull(db)->SplitColumnFamilyFromSstFiles(infos);
  dbfull(db)->TEST_WaitForSplit();
  size_t numItem = 10;
  std::vector<std::string> keys;
  std::vector<std::string> values;

  for(size_t i = 0; i < numItem; i++) {
    std::string k(1, 'a' + i);
    std::string v = "v" + k;
    keys.push_back(k); 
    values.push_back(v); 
  }

  for(size_t i = 0; i < numItem; i++) {
    db->Put(WriteOptions(), cfh, Slice(keys[i]), Slice(values[i]));
  }

  std::unique_ptr<Iterator> iterator(dbfull(db)->NewIterator(ReadOptions(), cfh));
  iterator->SeekToFirst();
  for(size_t i = 0; i < numItem; i++) {
    //fprintf(stdout, "value : %s\n", iterator->value().data());
    //fprintf(stdout, "value size : %ld\n", iterator->value().size());
    ASSERT_EQ(iterator->value().ToString(), values[i]);
	iterator->Next();
  }
  fprintf(stdout, "Value test is success\n");
 
  dbfull(db)->DestroyLogicalColumnFamilies();
  delete db;
}
*/

TEST_F(LCFIteratorTest, ThreeLevelSimpleNext) {
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

  std::vector<SplitFileInfo> infos;
  FileMetaData* f1 = new FileMetaData;
  std::string s1 = "c";
  std::string l1 = "f";
  f1->smallest = InternalKey(Slice(s1), 0, kTypeValue);
  f1->largest = InternalKey(Slice(l1), 0, kTypeValue);
  infos.push_back(SplitFileInfo(f1, cfd));
  dbfull(db)->SplitColumnFamilyFromSstFiles(infos);
  dbfull(db)->TEST_WaitForSplit();

  ColumnFamilyData* cfd1 = cfd->GetColumnFamilySet()->GetColumnFamily(1);
  infos.clear();
  FileMetaData* f2 = new FileMetaData;
  std::string s2 = "d";
  std::string l2 = "e";
  f2->smallest = InternalKey(Slice(s2), 0, kTypeValue);
  f2->largest = InternalKey(Slice(l2), 0, kTypeValue);
  infos.push_back(SplitFileInfo(f2, cfd1));

  dbfull(db)->SplitColumnFamilyFromSstFiles(infos);
  dbfull(db)->TEST_WaitForSplit();
  size_t numItem = 10;
  std::vector<std::string> keys;
  std::vector<std::string> values;

  for(size_t i = 0; i < numItem; i++) {
    std::string k(1, 'a' + i);
    std::string v = "v" + k;
    keys.push_back(k); 
    values.push_back(v); 
  }

  for(size_t i = 0; i < numItem; i++) {
    db->Put(WriteOptions(), cfh, Slice(keys[i]), Slice(values[i]));
	//fprintf(stdout,"\n");
  }
  //fprintf(stdout, "Put task is finished\n");
  //std::unique_ptr<Iterator> iterator(dbfull(db)->NewIterator(ReadOptions(), cfh));
  auto *iterator=dbfull(db)->NewIterator(ReadOptions(), cfh);
  iterator->SeekToFirst();
  for(size_t i = 0; i < numItem; i++) {
    //fprintf(stdout, "value : %s\n", iterator->value().data());
    //fprintf(stdout, "value size : %ld\n", iterator->value().size());
    ASSERT_EQ(iterator->value().ToString(), values[i]);
	iterator->Next();
	//fprintf(stdout,"\n");
  }
  fprintf(stdout, "Value test is success\n");
  delete iterator; 
  //dbfull(db)->DestroyLogicalColumnFamilies();
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
