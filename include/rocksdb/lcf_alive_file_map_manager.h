//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#pragma once

#include <memory>
#include <string>
#include <iostream>
#include <unordered_map>
#include <vector>
#include "util/mutexlock.h"
#include "util/logging.h"
#include "port/port.h"

namespace rocksdb {

// LCF: tracking alive flies across multiple column families
class LCFAliveFileMapManager {
  public:
    LCFAliveFileMapManager() {
      std::cout << "Init LCFAliveFileMapManager\n";
    };

    void PrintAliveFiles(const std::shared_ptr<Logger>& log, int job_id, std::string msg) {
      ReadLock rl(&lcf_alive_file_mutex_);
      for (auto& files: lcf_alive_file_map_) {
        int fnum= (int)files.first;
        ROCKS_LOG_INFO(log, "JOB[%d] [%s] file#%d : %d", job_id, msg.c_str(), fnum, files.second);
      }
    }

    void Increment(const std::shared_ptr<Logger>& log, int job_id, uint64_t file_num) {
      //(void)log;
      //(void)job_id;
      //std::cout <<"[Increment]" << file_num << std::endl;
      WriteLock wl(&lcf_alive_file_mutex_);
      int& count = lcf_alive_file_map_[file_num];
      ++count;
      //ROCKS_LOG_INFO(log, "JOB[%d] JH Increment file#%lu -> %d (manager: %p)", job_id, file_num, count, static_cast<void*>(this));
    }

    // JH: return 0 if only there is a last reference and erase the element.
    int Decrement(const std::shared_ptr<Logger>& log, int job_id, uint64_t file_num) { 
      //(void)log;
      //(void)job_id;
      //std::cout <<"[Decrement]" << file_num << std::endl;
      WriteLock wl(&lcf_alive_file_mutex_);
      auto it = lcf_alive_file_map_.find(file_num);
      if (it == lcf_alive_file_map_.end()) {
        return -1;
      }
      //ROCKS_LOG_INFO(log, "JOB[%d] JH Decrement file#%lu -> %d (manager: %p)", job_id, file_num, it->second-1, static_cast<void*>(this));
      if (it->second == 1) {
        lcf_alive_file_map_.erase(it);
        return 0;
      }
      else {
        it->second --;
        return it->second;
      }
    }

    int Get(uint64_t file_num) {
      ReadLock wl(&lcf_alive_file_mutex_);
      auto it = lcf_alive_file_map_.find(file_num);
      return (it != lcf_alive_file_map_.end()) ? it->second : 0;
    }

    std::unordered_map<uint64_t, int> lcf_alive_file_map_;
    port::RWMutex lcf_alive_file_mutex_;
};
}  // namespace rocksdb
