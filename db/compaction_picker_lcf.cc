//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/compaction_picker_lcf.h"
#ifndef ROCKSDB_LITE

#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif

#include <inttypes.h>
#include <limits>
#include <queue>
#include <string>
#include <utility>
#include "db/column_family.h"
#include "monitoring/statistics.h"
#include "util/filename.h"
#include "util/log_buffer.h"
#include "util/random.h"
#include "util/string_util.h"
#include "util/sync_point.h"

namespace rocksdb {

bool LCFCompactionPicker::NeedsCompaction(
    const VersionStorageInfo* vstorage) const {
  (void)vstorage;
  return false;
}

InterCFCompaction* LCFCompactionPicker::PickInterCFCompaction(
    const std::string& cf_name, const MutableCFOptions& mutable_cf_options,
    VersionStorageInfo* vstorage, VersionStorageInfo* parent_vstorage, LogBuffer* log_buffer,
    CompactionPicker* parent_picker) {
  (void)cf_name;
  (void)mutable_cf_options;
  (void)vstorage;
  (void)parent_vstorage;
  (void)log_buffer;
  (void)parent_picker;

  return nullptr;
}

// LCF style of compaction. Pick files that are contiguous in
// time-range to compact.
Compaction* LCFCompactionPicker::PickCompaction(
    const std::string& cf_name, const MutableCFOptions& mutable_cf_options,
    VersionStorageInfo* vstorage, LogBuffer* log_buffer) {
  (void)cf_name;
  (void)mutable_cf_options;
  (void)vstorage;
  (void)log_buffer;

  return nullptr;
}

uint32_t LCFCompactionPicker::GetPathId(
    const ImmutableCFOptions& ioptions,
    const MutableCFOptions& mutable_cf_options, uint64_t file_size) {
  (void)ioptions;
  (void)mutable_cf_options;
  (void)file_size;

  return 0;
}
}  // namespace rocksdb

#endif  // !ROCKSDB_LITE
