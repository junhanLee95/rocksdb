//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/compaction_picker.h"
#include "db/split_picker.h"

#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif

#include <inttypes.h>
#include <limits>
#include <queue>
#include <string>
#include <utility>
#include <vector>
#include "db/column_family.h"
#include "monitoring/statistics.h"
#include "util/filename.h"
#include "util/log_buffer.h"
#include "util/random.h"
#include "util/string_util.h"
#include "util/sync_point.h"

namespace rocksdb {

SplitPicker::SplitPicker(const ImmutableCFOptions& ioptions,
                         const MutableCFOptions& mutable_cf_options)
    : ioptions_(ioptions), mutable_cf_options_(mutable_cf_options) {}

SplitPicker::~SplitPicker() {}

Compaction* SplitPicker::PickSplit(const std::string& cf_name,
                              VersionStorageInfo* vstorage,
                              LogBuffer* log_buffer) {

  CompactionInputFiles input_files;
  for (int i=0; i< NumberLevels(); i++) {
    const std::vector<FileMetaData*>& level_files =
        vstorage->LevelFiles(i);
    input_files.level = i;
    int c = 0;
    for (auto f: level_files) {
      assert(!f->being_compacted);
      input_files.files.push_back(f);
      c++;
    }

    if (!input_files.empty()){
      ROCKS_LOG_BUFFER(log_buffer, "SplitPicker::PickSplit input_files[%d] : %d", i, c);
      inputs_.push_back(input_files); 
    }

    input_files.clear();
  }

  Compaction* c = GetSplit(vstorage);
  ROCKS_LOG_BUFFER(log_buffer, "SplitPicker::PickSplit %s", cf_name.c_str());
  TEST_SYNC_POINT_CALLBACK("SplitPicker::PickSplit:Return", c);

  return c;
}

Compaction* SplitPicker::GetSplit(VersionStorageInfo* vstorage) {
  auto c = new Compaction(
      vstorage, ioptions_, mutable_cf_options_, std::move(inputs_),
      1 /* output level */,
      67108864 /* max_file_size, 64MB */,
      mutable_cf_options_.max_compaction_bytes,
      GetPathId(0) /* output path id */,
      mutable_cf_options_.compression,
      ioptions_.compression_opts,
      1 /*max_subcompaction*/, {} /* grandparents */, false /*is_manual*/,
      0 /*score*/, false /* deletion_compaction */, CompactionReason::kSplit
          );
  RegisterSplit(c);
  vstorage->ComputeCompactionScore(ioptions_, mutable_cf_options_);
  return c;
}

bool SplitPicker::NeedsSplit(const VersionStorageInfo* vstorage) {
  if (!vstorage->FilesMarkedForSplit().empty()) { // already split in progress
    ROCKS_LOG_INFO(ioptions_.info_log,
                     "NeedsSplit: already split in progress");


    return false;
  }
  assert(vstorage->num_levels()==2);
  ROCKS_LOG_INFO(ioptions_.info_log,
                     "NeedsSplit: level0 file count : %d", (int)vstorage->LevelFiles(0).size());
  ROCKS_LOG_INFO(ioptions_.info_log,
                     "NeedsSplit: level1 file count : %d", (int)vstorage->LevelFiles(1).size());

  assert(vstorage->LevelFiles(1).size() >=4);

  if (vstorage->LevelFiles(1).size() >= 4) {
    // For test, we assume the cf is splitted if l0 count >= 4,
    // instead of L0->L1 compaction
    return true;
  }
  return false;
}

/*
 * Find the optimal path to place a file
 * Given a level, finds the path where levels up to it will fit in levels
 * up to and including this path
 */
uint32_t SplitPicker::GetPathId(int level) {
  uint32_t p = 0;
  assert(!ioptions_.cf_paths.empty());

  // size remaining in the most recent path
  uint64_t current_path_size = ioptions_.cf_paths[0].target_size;

  uint64_t level_size;
  int cur_level = 0;

  // max_bytes_for_level_base denotes L1 size.
  // We estimate L0 size to be the same as L1.
  level_size = mutable_cf_options_.max_bytes_for_level_base;

  // Last path is the fallback
  while (p < ioptions_.cf_paths.size() - 1) {
    if (level_size <= current_path_size) {
      if (cur_level == level) {
        // Does desired level fit in this path?
        return p;
      } else {
        current_path_size -= level_size;
        if (cur_level > 0) {
          if (ioptions_.level_compaction_dynamic_level_bytes) {
            // Currently, level_compaction_dynamic_level_bytes is ignored when
            // multiple db paths are specified. https://github.com/facebook/
            // rocksdb/blob/master/db/column_family.cc.
            // Still, adding this check to avoid accidentally using
            // max_bytes_for_level_multiplier_additional
            level_size = static_cast<uint64_t>(
                level_size * mutable_cf_options_.max_bytes_for_level_multiplier);
          } else {
            level_size = static_cast<uint64_t>(
                level_size * mutable_cf_options_.max_bytes_for_level_multiplier *
                mutable_cf_options_.MaxBytesMultiplerAdditional(cur_level));
          }
        }
        cur_level++;
        continue;
      }
    }
    p++;
    current_path_size = ioptions_.cf_paths[p].target_size;
  }
  return p;
}

// Delete this compaction from the list of running compactions.
void SplitPicker::ReleaseSplitFiles(Compaction* c) {
  UnregisterSplit(c);
}

// Returns true if any one of specified files are being compacted
bool SplitPicker::AreFilesInSplit(
    const std::vector<FileMetaData*>& files) {
  for (size_t i = 0; i < files.size(); i++) {
    if (files[i]->being_splitted) {
      return true;
    }
  }
  return false;
}

void SplitPicker::RegisterSplit(Compaction* c) {
  if (c == nullptr) {
    return;
  }
  splits_in_progress_.insert(c);
}

void SplitPicker::UnregisterSplit(Compaction* c) {
  if (c == nullptr) {
    return;
  }
  splits_in_progress_.erase(c);
}

}  // namespace rocksdb
