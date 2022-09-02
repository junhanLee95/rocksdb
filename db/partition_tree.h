// Author: Dohyun Kim (ehgus421210@kaist.ac.kr)
// Note: Partition tree for logical column family.
// 

#pragma once 

#include <string>
#include <vector>
#include <atomic>

#include "db/column_family.h"

namespace rocksdb {

class ColumnFamilyData;

class PartitionTreeNode {
 public:
  
  ColumnFamilyData *cfd_ = nullptr;

  std::vector<PartitionTreeNode*> lower_level_nodes_;
  
  PartitionTreeNode() {};
  PartitionTreeNode(ColumnFamilyData*); 

  void SetColumnFamily(ColumnFamilyData*);
  PartitionTreeNode *SearchNextNode(const Slice &key);
};

class PartitionTree {
 public:

  PartitionTreeNode root_;
  std::unordered_map<uint32_t, PartitionTreeNode*> partition_nodes_;

  PartitionTree() {};
  PartitionTree(ColumnFamilyData *cfd) : root_(cfd) {};
  ~PartitionTree() {};

  void SetRootColumnFamily(ColumnFamilyData*);
  void InsertSplittedColumnFamily (ColumnFamilyData *base_cfd, const std::vector<ColumnFamilyData*> &new_cfds);
  ColumnFamilyData *SearchColumnFamily (const Slice &key); 
  
};

}






