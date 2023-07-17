// Author: Dohyun Kim (ehgus421210@kaist.ac.kr)
// Note: Partition tree for logical column family.
// Modified by : Junhan (junhanlee2020@gmail.com)
// Note: add rw mutex to partition tree for the synchronization.

#pragma once 

#include <string>
#include <vector>
#include <atomic>
#include <iostream>

#include "db/column_family.h"
#include "port/port.h"
#include "util/mutexlock.h"

#define get_lmost_key(n) ((n)->cfd_->GetSmallestKey())
#define get_rmost_key(n) ((n)->cfd_->GetLargestKey())

namespace rocksdb {

class ColumnFamilyData;

class PartitionTreeNode {
 public:
  
  ColumnFamilyData *cfd_ = nullptr;
  int depth_=0;

  std::vector<PartitionTreeNode*> lower_level_nodes_;
  PartitionTreeNode *parent_node_ = nullptr;
  
  PartitionTreeNode() {};
  PartitionTreeNode(ColumnFamilyData*); 
  ~PartitionTreeNode();

  void SetColumnFamily(ColumnFamilyData*);
  PartitionTreeNode *SearchNextNode(const Slice &key);

  PartitionTreeNode *GetParentNode(void);
  std::vector<PartitionTreeNode *> GetChildrenNodes(void);

  void Print(std::string TreeID, bool recursive);
  int GetDepth(void);
};

class PartitionTree {
 public:

  PartitionTreeNode* root_ = nullptr;
  std::unordered_map<uint32_t, PartitionTreeNode*> partition_nodes_;
  mutable port::RWMutex rwlock_;

  PartitionTree() {};
  PartitionTree(ColumnFamilyData *cfd);
  ~PartitionTree();

  void SetRootColumnFamily(ColumnFamilyData*);
  Status InsertSplittedColumnFamily (ColumnFamilyData *base_cfd, const std::vector<ColumnFamilyData*> &new_cfds);
  ColumnFamilyData *SearchColumnFamily (const Slice &key); 
  std::vector<ColumnFamilyData*> SearchAllColumnFamilies (const Slice &key); 
  
  void PrintAll();
};

}






