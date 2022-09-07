//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#pragma once

#include <memory>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

#include "db/compaction.h"
#include "db/version_set.h"
#include "options/cf_options.h"
#include "rocksdb/env.h"
#include "rocksdb/options.h"
#include "rocksdb/status.h"

namespace rocksdb {

class LogBuffer;
class VersionStorageInfo;
struct SplitInputFiles;

class SplitPicker {
 public:
  SplitPicker(const ImmutableCFOptions& ioptions,
              const MutableCFOptions& mutable_cf_options);
  virtual ~SplitPicker();

  bool SetupL0FilesIfNeeded(CompactionInputFiles& l1_files,
                            CompactionInputFiles& l0_files);
                            
  // Pick level and inputs for a new split.
  // Returns nullptr if there is no split to be done.
  // Otherwise returns a pointer to a heap-allocated object that
  // describes the split.  Caller should delete the result.
  Compaction* PickSplit(const std::string& cf_name,
                   VersionStorageInfo* vstorage,
                   std::vector<FileMetaData*> metas,
                   LogBuffer* log_buffer);


  bool HaveOverlappingKeyRanges(FileMetaData* a, FileMetaData* b);

  Compaction* GetSplit(VersionStorageInfo* vstorage);

  bool NeedsSplit(const VersionStorageInfo* vstorage);

  uint32_t GetPathId(int level);

  // Free up the files that participated in a split
  //
  // Requirement: DB mutex held
  void ReleaseSplitFiles(Compaction* c);

  // Returns true if any one of the specified files are being compacted
  bool AreFilesInSplit(const std::vector<FileMetaData*>& files);

  // Register this split in the set of running splits
  void RegisterSplit(Compaction* c);

  // Remove this split from the set of running splits
  void UnregisterSplit(Compaction* c);

  std::unordered_set<Compaction*>* splits_in_progress() {
    return &splits_in_progress_;
  }

  int NumberLevels() const { return ioptions_.num_levels; }

 protected:
  const ImmutableCFOptions& ioptions_;
  const MutableCFOptions& mutable_cf_options_;

  // Keeps track of all splits that are running.
  // Protected by DB mutex
  std::unordered_set<Compaction*> splits_in_progress_;

  std::vector<CompactionInputFiles> inputs_;
};

}  // namespace rocksdb
