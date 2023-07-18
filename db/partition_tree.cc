// Author: Dohyun Kim (ehgus421210@kaist.ac.kr)
// Note: Partition tree for logical column family.
// Modified by : Junhan (junhanlee2020@gmail.com)
// Note: add rw mutex to partition tree for the synchronization.

#include "db/partition_tree.h"
#include <assert.h>

namespace rocksdb {

PartitionTreeNode::PartitionTreeNode (
    ColumnFamilyData *column_family_data) 
    : cfd_(column_family_data) {

  if (cfd_ == nullptr) { 
    // TODO: error raise should be added. 
    return; 
  }

  lower_level_nodes_ = {};
}

PartitionTreeNode::~PartitionTreeNode() {
  // IMPLEMENT THIS
  //fprintf(stdout, "[PartitionTreeNode] delete partition tree node\n");
  for (auto& n: lower_level_nodes_) {
    delete n;
  }
}

void PartitionTreeNode::SetColumnFamily (
    ColumnFamilyData* column_family_data) {
  cfd_ = column_family_data;
}

PartitionTreeNode *PartitionTreeNode::SearchNextNode (
    const Slice &key) {
    
  if (lower_level_nodes_.empty()) 
    return nullptr;

  std::string key_str = key.ToString();
  // Linear Search
  for (auto nodes: lower_level_nodes_) {
    // Right most key is "" or key is less than or equal to right most key. 
    if (get_rmost_key(nodes).empty() || key_str.compare(get_rmost_key(nodes)) <= 0) { 
    // Left most key is "" or key is greater than or equal to left most key. 
      if (get_lmost_key(nodes).empty() || key_str.compare(get_lmost_key(nodes)) >= 0)
        return nodes;
      else 
        return nullptr;
    }
  }
  return nullptr;
}

PartitionTreeNode* PartitionTreeNode::GetParentNode(void) {
  return parent_node_;
}

std::vector<PartitionTreeNode*> PartitionTreeNode::GetChildrenNodes(void) {
  return lower_level_nodes_; 
}

void PartitionTreeNode::Print(
    std::string TreeID, 
    bool recursive) {
  ROCKS_LOG_INFO(cfd_->ioptions()->info_log,
                 "%-6s LCF[%d] %s => [%s, %s]\n", 
                 TreeID.c_str(), cfd_->GetID(), cfd_->GetName().c_str(), 
                 cfd_->GetSmallestKey().c_str(), cfd_->GetLargestKey().c_str());
  /*
  fprintf(stdout, "%-6s LCF[%d] %s => [%s, %s]\n", 
    TreeID.c_str(), cfd_->GetID(), cfd_->GetName().c_str(), 
    cfd_->GetSmallestKey().c_str(), cfd_->GetLargestKey().c_str());*/

  if (!recursive) 
    return;

  uint32_t i = 0; 
  for (auto nodes: lower_level_nodes_) 
    nodes->Print(TreeID + std::to_string(i++), true);
}

int PartitionTreeNode::GetDepth(void) {
  return depth_; 
}

// PartitionTree function.

PartitionTree::PartitionTree( 
    ColumnFamilyData* column_family_data) {
  WriteLock wl(&rwlock_);
  root_ = new PartitionTreeNode(column_family_data);
  if (column_family_data != nullptr) {
      column_family_data->SetPartitionTreeNode(root_);  
  }
  //fprintf(stdout, "[PartitionTree] Insert New CFD %d\n", column_family_data->GetID());

  partition_nodes_.insert({column_family_data->GetID(), root_}); 
}

PartitionTree::~PartitionTree() {
  //fprintf(stdout, "[PartitionTree] delete partition tree node\n");
  delete root_;
  partition_nodes_.clear();
}

void PartitionTree::SetRootColumnFamily (
    ColumnFamilyData* column_family_data) {
  WriteLock wl(&rwlock_);
  root_->SetColumnFamily(column_family_data);

  //fprintf(stdout, "[PartitionTree] Insert New CFD %d\n", column_family_data->GetID());

  partition_nodes_.insert({column_family_data->GetID(), root_});
}

Status PartitionTree::InsertSplittedColumnFamily (
    ColumnFamilyData *base_cfd, 
    const std::vector<ColumnFamilyData*> &new_cfds) {
  WriteLock wl(&rwlock_);

  /*fprintf(stdout, "[InsertSplittedColumnFamily] base CFD[%d] %s - [%s, %s]\n",  
            base_cfd->GetID(),
            base_cfd->GetName().c_str(),
            base_cfd->GetSmallestKey().c_str(),
            base_cfd->GetLargestKey().c_str());*/
  
  if (new_cfds.empty()) {
    //fprintf(stdout, "[InsertSplittedColumnFamily] new_cfds are empty\n");
    return Status::OK(); 
  }

  auto base_node_iter = partition_nodes_.find(base_cfd->GetID());
  auto base_node = base_node_iter->second;

  // TODO: check violation 1. 
  // Violation 1. The key range of splitted nodes under the base should be 
  // included in the key range of base column family. 

  // Note: We assume that the vector new_cfds already sorted 
  // with respect to its key range. 
  /*
  for (auto lnode: base_node->lower_level_nodes_) {
    ColumnFamilyData* l_cfd = lnode->cfd_;
    fprintf(stdout, "[InsertSplittedColumnFamily] before: child CFD[%d] %s - [%s, %s]\n",  
            l_cfd->GetID(),
            l_cfd->GetName().c_str(),
            l_cfd->GetSmallestKey().c_str(),
            l_cfd->GetLargestKey().c_str());
  }*/

  for (auto new_cfd: new_cfds) {
    

    auto node = new PartitionTreeNode(new_cfd);
    node->depth_ = base_node->depth_+1;
    assert (new_cfd != nullptr);
    new_cfd->SetPartitionTreeNode(node);  

    std::string n_smallest = new_cfd->GetSmallestKey();
    std::string n_largest = new_cfd->GetLargestKey();
    assert(!n_smallest.empty());
    assert(!n_largest.empty());
    // boundary check and let's debug it!
    std::string b_smallest = get_lmost_key(base_node);
    std::string b_largest = get_rmost_key(base_node);
    if (!b_smallest.empty() && !b_largest.empty()) {
      if (!(b_smallest.compare(n_smallest) <= 0 &&
            n_largest.compare(b_largest) <= 0)) {
        fprintf(stderr, "InsertSplittedColumnFamily error(1) p[%s, %s] c[%s, %s]\n",
            b_smallest.c_str(), b_largest.c_str(),
            n_smallest.c_str(), n_largest.c_str());
        exit(1);    
      }
    }
    /*
    fprintf(stdout, "[PartitionTree] Insert New CFD[%d] %s - [%s, %s]\n", 
            new_cfd->GetID(), 
            new_cfd->GetName().c_str(), 
            n_smallest.c_str(), 
            n_largest.c_str()
            );*/
    int l, r, m;
    l = 0;
    r = base_node->lower_level_nodes_.size() - 1;
    m = (l + r) / 2;
    while (l <= r) { // binary search
      m = (l + r) / 2;
      std::string m_smallest = base_node->lower_level_nodes_[m]->cfd_->GetSmallestKey();
      std::string m_largest = base_node->lower_level_nodes_[m]->cfd_->GetLargestKey();

      if (m_smallest.compare(n_largest) >= 0) {
        // n is smaller than m
        r = m - 1;
      } else if (m_largest.compare(n_smallest) <= 0) {
        // n is larger than m
        l = m + 1;
      } else {
        // n and m overlaps! assert error
        /*fprintf(stdout, "[PartitionTree] CFD[%d] %s [%s, %s] and CFD[%d] %s [%s, %s] overlaps!\n",
            new_cfd->GetID(),
            new_cfd->GetName().c_str(),
            n_smallest.c_str(),
            n_largest.c_str(),
            base_node->lower_level_nodes_[m]->cfd_->GetID(),
            base_node->lower_level_nodes_[m]->cfd_->GetName().c_str(),
            m_smallest.c_str(),
            m_largest.c_str()
        );*/
        return Status::Corruption();
      }
    }
    // now l is the target to insert to
    /*fprintf(stdout, "[PartitionTree] Insert New CFD[%d] %s to : %d\n", 
            new_cfd->GetID(), 
            new_cfd->GetName().c_str(), 
            l
            );*/
    base_node->lower_level_nodes_.insert(base_node->lower_level_nodes_.begin() + l,
                                         node);
    //JH: Set parent node
    node->parent_node_ = base_node;
    //JH: Add child node to partition_nodes_ map
    partition_nodes_.insert({new_cfd->GetID(), node});
  }

  for (size_t i = 0; i < base_node->lower_level_nodes_.size(); i++) {
    PartitionTreeNode* lnode = base_node->lower_level_nodes_[i];
    ColumnFamilyData* l_cfd = lnode->cfd_;
    ROCKS_LOG_INFO(l_cfd->ioptions()->info_log,
                 "InsertSplittedColumnFamily: child %ld - %s(%d)\n", 
                 i,
                 l_cfd->GetName().c_str(),
                 l_cfd->GetID());
    LogFlush(l_cfd->ioptions()->info_log);
    /*fprintf(stdout, "[InsertSplittedColumnFamily] after: child CFD[%d] %s - [%s, %s]\n",  
            l_cfd->GetID(),
            l_cfd->GetName().c_str(),
            l_cfd->GetSmallestKey().c_str(),
            l_cfd->GetLargestKey().c_str());*/

    if (i != 0) { // boundary overlap check
      PartitionTreeNode* pnode = base_node->lower_level_nodes_[i-1];
      ColumnFamilyData* p_cfd = pnode->cfd_;
      std::string p_largest = p_cfd->GetLargestKey();
      std::string l_smallest = l_cfd->GetSmallestKey();
      assert(p_largest.compare(l_smallest) < 0);
      if(p_largest.compare(l_smallest) >= 0) {
        //exit and let's debug it!
        fprintf(stderr, "InsertSplittedColumnFamily error(2) %s %s\n",
                p_largest.c_str(), l_smallest.c_str());
        exit(1);
      }
    }
  }

  return Status::OK();
}

ColumnFamilyData* PartitionTree::SearchColumnFamily (const Slice &key) {
  ReadLock rl(&rwlock_);
  PartitionTreeNode *cnode = root_; // current node
  PartitionTreeNode *nnode = nullptr; // next node

  while (true) {
    /*ROCKS_LOG_INFO(cnode->cfd_->ioptions()->info_log,
                 "SearchColumnFamily : %s(%d)\n", 
                 cnode->cfd_->GetName().c_str(),
                 cnode->cfd_->GetID());*/

    nnode = cnode->SearchNextNode(key);

    if (nnode == nullptr) 
      break; 

    cnode = nnode; 
  }

  if(cnode->cfd_->GetName() != "default") {
    assert (key.ToString().compare(get_lmost_key(cnode) >= 0));
    assert (key.ToString().compare(get_rmost_key(cnode) <= 0));
    if(key.ToString().compare(get_lmost_key(cnode)) < 0 || 
       key.ToString().compare(get_rmost_key(cnode)) > 0) {
      fprintf(stderr, "SearchColumnFamily error(1) %s [%s, %s]\n",
          key.ToString().c_str(),
          get_lmost_key(cnode).c_str(),
          get_rmost_key(cnode).c_str());
    }
  }
  /*ROCKS_LOG_INFO(cnode->cfd_->ioptions()->info_log,
                 "CFD[%s] Search... %s < [%s] < %s", 
                 cnode->cfd_->GetName().c_str(),
                 get_lmost_key(cnode).c_str(),
                 key.ToString(),
                 get_rmost_key(cnode).c_str());*/

  
  /*fprintf(stdout, "CFD[%s] Search... %s < [%s] < %s\n", 
    cnode->cfd_->GetName().c_str(), get_lmost_key(cnode).c_str(), key.data(), get_rmost_key(cnode).c_str());*/
  
  return cnode->cfd_;
}

std::vector<ColumnFamilyData*> PartitionTree::SearchAllColumnFamilies (const Slice &key) {
  ReadLock rl(&rwlock_);
  std::vector<ColumnFamilyData*> search_cfds; // cfds to return
  PartitionTreeNode *cnode = root_; // current node
  PartitionTreeNode *nnode = nullptr; // next node

  search_cfds.push_back(cnode->cfd_);
  while (true) {
    nnode = cnode->SearchNextNode(key);

    if (nnode == nullptr) 
      break; 

    cnode = nnode; 
    search_cfds.push_back(cnode->cfd_);
  }

  /*
  fprintf(stdout, "CFD[%s] Search... %s < [%s] < %s\n", 
    cnode->cfd_->GetName().c_str(), get_lmost_key(cnode).c_str(), key.data(), get_rmost_key(cnode).c_str());*/
  
  return search_cfds;
}

std::vector<PartitionTreeNode*> PartitionTree::GetChildrenNodes(PartitionTreeNode* node) {
  ReadLock rl(&rwlock_);
  return node->GetChildrenNodes();
}
  
void PartitionTree::PrintAll() {
  ReadLock rl(&rwlock_);
  root_->Print("0", true);
}
 
}




