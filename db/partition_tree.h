// Author: Dohyun Kim (ehgus421210@kaist.ac.kr)
// Note: Partition tree for logical column family.
// 

#pragma once 

#include <string>
#include <vector>
#include <atomic>
#include <iostream>

#include "db/column_family.h"

namespace rocksdb {

class ColumnFamilyData;

class PartitionTreeNode {
 public:
  
  ColumnFamilyData *cfd_ = nullptr;

  std::vector<PartitionTreeNode*> lower_level_nodes_;
  PartitionTreeNode *parent_node_ = nullptr;
  
  PartitionTreeNode() {};
  PartitionTreeNode(ColumnFamilyData*); 

  void SetColumnFamily(ColumnFamilyData*);
  PartitionTreeNode *SearchNextNode(const Slice &key);

  PartitionTreeNode *GetParentNode(void);

  void Print(std::string TreeID, bool recursive);
};

class PartitionTree {
 public:

  PartitionTreeNode* root_ = nullptr;
  std::unordered_map<uint32_t, PartitionTreeNode*> partition_nodes_;

  PartitionTree() {};
  PartitionTree(ColumnFamilyData *cfd);
  ~PartitionTree() {};

  void SetRootColumnFamily(ColumnFamilyData*);
  void InsertSplittedColumnFamily (ColumnFamilyData *base_cfd, const std::vector<ColumnFamilyData*> &new_cfds);
  ColumnFamilyData *SearchColumnFamily (const Slice &key); 
  std::vector<ColumnFamilyData*> SearchAllColumnFamilies (const Slice &key); 
  
  void PrintAll();
};

}






