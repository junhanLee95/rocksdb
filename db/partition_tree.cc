// Author: Dohyun Kim (ehgus421210@kaist.ac.kr)
// Note: Partition tree for logical column family.
//

#include "db/partition_tree.h"

#define get_lmost_key(n) ((n)->cfd_->GetSmallestKey())
#define get_rmost_key(n) ((n)->cfd_->GetLargestKey())

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
      if (get_lmost_key(nodes).empty() || key.compare(get_lmost_key(nodes)) >= 0)
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
  //fprintf(stdout,"ID is %d\n",cfd_->GetID());
  ROCKS_LOG_INFO(cfd_->ioptions()->info_log,
                 "%-6s LCF[%d] %s => [%s, %s]\n", 
                 TreeID.c_str(), cfd_->GetID(), cfd_->GetName().c_str(), 
                 cfd_->GetSmallestKey().c_str(), cfd_->GetLargestKey().c_str());

/*  ROCKS_LOG_INFO(cfd_->ioptions()->info_log,
                 "%-6s LCF[%d] %s => [%d]\n", 
                 TreeID.c_str(), cfd_->GetID(), cfd_->GetName().c_str(), 
                 cfd_->IsMade());*/

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


std::vector<PartitionTreeNode *> PartitionTreeNode::Traversal() {
	std::vector<PartitionTreeNode*> nodes;
	this->TraversalImpl(&nodes);
	return nodes;
}
void PartitionTreeNode::TraversalImpl(
		std::vector<PartitionTreeNode*> *nodes) {
  nodes->push_back(this);
//  fprintf(stdout,"ID is %d\n",this->cfd_->GetID());

  for (auto cnodes: lower_level_nodes_)
    cnodes->TraversalImpl(nodes);

}

void PartitionTreeNode::Traversal(std::pair<std::string,std::string> range, std::vector<std::string> keys, std::pair<std::vector<int>,std::pair<std::string,std::string>> *jobs) {
	//std::pair<std::vector<int>,std::pair<std::string,std::string>> jobs;
	this->TraversalImpl(jobs,range,keys);
	//return jobs;
}
void PartitionTreeNode::TraversalImpl(
		std::pair<std::vector<int>,std::pair<std::string,std::string>> *jobs, std::pair<std::string,std::string> range, std::vector<std::string> keys) {
  //check overlap and store id and overlapped key range
  if(cfd_->IsHot()){
	//std::cout << range.first << " " << range.second << std::endl;

	bool empty=true;

	std::vector<std::pair<std::string,std::string>> overlap_range;

	//ID_list is consist of base_cfd,(delete cfd ....)
    std::vector<int> ID_list;
	ID_list.push_back(cfd_->GetID()); // base_cfd
    
	std::vector<int> overlap_status;
	// 0 : non overlap
	// 1 : part overlap
	// 2 : whole overlap
	// 3 : range is in node

	std::string first;
	std::string second;

    for (auto cnodes: lower_level_nodes_){
      if( range.first.compare(get_lmost_key(cnodes)) >= 0 && range.second.compare(get_rmost_key(cnodes)) <= 0 ) {// range is in node
	    //std::cout << "range is in node" << std::endl;
	    //std::cout << range.first << "," << range.second << std::endl;
	    //*jobs= std::make_pair(ID_list,std::make_pair(first,second));

		return;
	  }
      auto ranges = cnodes->overlap(range);
	  //std::cout << "range is not in node" << std::endl;
	  //std::cout << range.first << "," << range.second << std::endl;
	  //std::cout << ranges.first << "," << ranges.second << std::endl;
	  //std::cout << get_lmost_key(cnodes) << "," << get_rmost_key(cnodes) << std::endl;
	  if(!ranges.first.empty()){
		empty=false;
		if( ranges.first.compare(get_lmost_key(cnodes)) == 0 && ranges.second.compare(get_rmost_key(cnodes)) == 0 ) // whole
	      overlap_status.push_back(2);
		else
		  overlap_status.push_back(1);
	  }
	  else
		overlap_status.push_back(0);
      overlap_range.push_back(ranges);
	}

	//r -run -db lcfdb -P workloads/workloade -P lcfdb/lcfdb.properties -p threadcount=1 -p recordcount=1000 -p operationcount=1000



	if(empty){//there is any overlap with childs.
		/*
	  for(auto i: overlap_status)
	    std::cout << i << " " ;
	  std::cout << std::endl;*/
      *jobs=std::make_pair(ID_list,range);

	  //std::cout << range.first << "," << range.second << " >> " << range.first << "," << range.second << std::endl;
	  return;
	}

	//for(auto i : overlap_range)
	  //fprintf(stdout,"%s %s\n", i.first.c_str(),i.second.c_str());

	//get whole overlapped range.
    if(overlap_status.front()==2)
	  first=range.first;

    if(overlap_status.back()==2)
	  second=range.second;

    if(overlap_status.front()==1){
      if(range.first.compare(get_lmost_key(lower_level_nodes_.front())) < 0 ){
	    first=range.first;
      }
	}

    if(overlap_status.back()==1){
      if(range.first.compare(get_lmost_key(lower_level_nodes_.back())) > 0 ){
	    second=range.second;
	  }
	}
/*
	for(auto i : overlap_status)
	  fprintf(stdout,"%d ", i);
	fprintf(stdout,"\n");
*/
	// think more case
    for (int i=0;i<int(lower_level_nodes_.size())-1; i++){

	  // 0 .... 0 2
	  if(overlap_status[i] == 0 && overlap_status[i+1] == 2)
		first=range.first;
	  // 2 0 ...... 0
	  if(overlap_status[i] == 2 && overlap_status[i+1] == 0)
		second=range.second;

	  /*
	  if(overlap_status[i] == 1 && overlap_status[i+1] == 2){
		int index = find(keys.begin(), keys.end(), overlap_range[i].second)-keys.begin()+1;
		first=keys[index];
	  }

	  if(overlap_status[i] == 2 && overlap_status[i+1] == 1){
		int index = find(keys.begin(), keys.end(), overlap_range[i+1].first)-keys.begin()-1;
		second=keys[index];
	  }*/

	  if(overlap_status[i] == 1 && overlap_status[i+1] == 0){
		if(range.first.compare(overlap_range[i].first) < 0 ){
		  int index = find(keys.begin(), keys.end(), overlap_range[i].first)-keys.begin()-1;
		  second=keys[index];
		}
		else{
		  int index = find(keys.begin(), keys.end(), overlap_range[i].second)-keys.begin()+1;
		  first=keys[index];
		  second = range.second;
	    }
	  }

	  if(overlap_status[i] == 0 && overlap_status[i+1] == 1){
		if(range.first.compare(overlap_range[i+1].first) < 0 ){
	      first = range.first;
		  int index = find(keys.begin(), keys.end(), overlap_range[i+1].first)-keys.begin()-1;
		  second=keys[index];
		}
		else{
		  int index = find(keys.begin(), keys.end(), overlap_range[i+1].second)-keys.begin()+1;
		  first=keys[index];
	    }
	  }
    }

	//get ids for whole overlapped nodes.
    for (int i=0;i<int(lower_level_nodes_.size()); i++){
	  if(overlap_status[i]==2){
		auto nodes = lower_level_nodes_[i]->Traversal();
		for(auto node : nodes)
		  ID_list.push_back(node->cfd_->GetID()); 
	  }
	}
/*
	for(auto i: overlap_status)
	  std::cout << i << " " ;
	std::cout << std::endl;
	std::cout << range.first << "," << range.second << " >> " << first << "," << second << std::endl;*/
	*jobs=std::make_pair(ID_list,std::make_pair(first,second));

	//repeat for the part overlap nodes.
	/*
    for (int i=0;i<int(lower_level_nodes_.size()); i++){
      if(overlap_status[i] == 1) 
        cnodes->TraversalImpl(jobs,overlap_range[i],kvpair);
    }*/
  }
  else{
    for (auto cnodes: lower_level_nodes_){
      cnodes->TraversalImpl(jobs,range,keys);
	}
  }
}

std::pair<std::string,std::string> PartitionTreeNode::overlap(std::pair<std::string,std::string> range){
  if(get_lmost_key(this).empty() || get_rmost_key(this).empty()) //root
	return range;

  std::string first;
  std::string second;
  if (range.second.compare(get_lmost_key(this)) < 0 || range.first.compare(get_rmost_key(this)) > 0) //non-overlap
    return make_pair(first,second);

  if (range.second.compare(get_rmost_key(this)) <= 0 && range.first.compare(get_lmost_key(this)) >= 0) //range is in node
    return make_pair(first,second);
  
  first = (range.first.compare(get_lmost_key(this)) >= 0) ? range.first : get_lmost_key(this);
  second = (range.second.compare(get_rmost_key(this)) <= 0) ? range.second : get_rmost_key(this);

  std::pair<std::string,std::string> overlap_range = std::make_pair(first,second);
  return overlap_range;
}
// PartitionTree function.

PartitionTree::PartitionTree( 
    ColumnFamilyData* column_family_data) {
  root_ = new PartitionTreeNode(column_family_data);
  if (column_family_data != nullptr) {
      column_family_data->SetPartitionTreeNode(root_);  
  }
  //fprintf(stdout, "[PartitionTree] Insert New CFD %d\n", column_family_data->GetID());

  partition_nodes_.insert({column_family_data->GetID(), root_}); 
}

void PartitionTree::SetRootColumnFamily (
    ColumnFamilyData* column_family_data) {
  root_->SetColumnFamily(column_family_data);

  //fprintf(stdout, "[PartitionTree] Insert New CFD %d\n", column_family_data->GetID());

  partition_nodes_.insert({column_family_data->GetID(), root_});
}

Status PartitionTree::InsertMergedColumnFamily (
	ColumnFamilyData* base_cfd,
	ColumnFamilyData* new_cfd){

  if (new_cfd == nullptr) {
    fprintf(stdout, "[InsertMergedColumnFamily] new_cfds are empty\n");
    return Status::OK(); 
  }

 
//Find method to get exact base_cfd.
  if(base_cfd == nullptr){
	std::cout << "root\n" << std::endl;
    auto node = new PartitionTreeNode(new_cfd);
    assert (new_cfd != nullptr);
    new_cfd->SetPartitionTreeNode(node);  

    std::string n_smallest = new_cfd->GetSmallestKey();
    std::string n_largest = new_cfd->GetLargestKey();
    this->SetRootColumnFamily(new_cfd);
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
    fprintf(stdout, "[InsertMergedColumnFamily] before: child CFD[%d] %s - [%s, %s]\n",  
            l_cfd->GetID(),
            l_cfd->GetName().c_str(),
            l_cfd->GetSmallestKey().c_str(),
            l_cfd->GetLargestKey().c_str());
  }
*/
  auto node = new PartitionTreeNode(new_cfd);
  assert (new_cfd != nullptr);
  new_cfd->SetPartitionTreeNode(node);  

  std::string n_smallest = new_cfd->GetSmallestKey();
  std::string n_largest = new_cfd->GetLargestKey();
  assert(!n_smallest.empty());
  assert(!n_largest.empty());
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
      fprintf(stdout, "[PartitionTree] CFD[%d] %s [%s, %s] and CFD[%d] %s [%s, %s] overlaps!\n",
          new_cfd->GetID(),
          new_cfd->GetName().c_str(),
          n_smallest.c_str(),
          n_largest.c_str(),
          base_node->lower_level_nodes_[m]->cfd_->GetID(),
          base_node->lower_level_nodes_[m]->cfd_->GetName().c_str(),
          m_smallest.c_str(),
          m_largest.c_str()
      );
      return Status::Corruption();
    }
  }

  if(int(base_node->lower_level_nodes_.size()) > 0 ){
    auto x_small = base_node->lower_level_nodes_[0]->cfd_->GetSmallestKey();
    auto x_large = base_node->lower_level_nodes_[0]->cfd_->GetLargestKey();

    ROCKS_LOG_INFO(base_node->cfd_->ioptions()->info_log,
				  "Right? : %d %d\n", 
                   x_small.compare(n_smallest) > 0,
                   x_large.compare(n_largest) > 0);
    ROCKS_LOG_INFO(base_node->cfd_->ioptions()->info_log,
				  "Size : %ld %ld\n", 
                   x_small.size(),
                   x_large.size());
    ROCKS_LOG_INFO(base_node->cfd_->ioptions()->info_log,
				  "Size : %ld %ld\n", 
                   n_smallest.size(),
                   n_largest.size());
    ROCKS_LOG_INFO(base_node->cfd_->ioptions()->info_log,
				  "nodes : %d\n", 
                   l);
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
  



  for (size_t i = 0; i < base_node->lower_level_nodes_.size(); i++) {
    PartitionTreeNode* lnode = base_node->lower_level_nodes_[i];
    ColumnFamilyData* l_cfd = lnode->cfd_;
	/*
    fprintf(stdout, "[InsertMergedColumnFamily] after: child CFD[%d] %s - [%s, %s]\n",  
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
    }
  }

  return Status::OK();
}

Status PartitionTree::InsertSplittedColumnFamily (
    ColumnFamilyData *base_cfd, 
    const std::vector<ColumnFamilyData*> &new_cfds) {
/*
  fprintf(stdout, "[InsertSplittedColumnFamily] base CFD[%d] %s - [%s, %s]\n",  
            base_cfd->GetID(),
            base_cfd->GetName().c_str(),
            base_cfd->GetSmallestKey().c_str(),
            base_cfd->GetLargestKey().c_str());
  */
  if (new_cfds.empty()) {
    fprintf(stdout, "[InsertSplittedColumnFamily] new_cfds are empty\n");
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
  }
*/
  for (auto new_cfd: new_cfds) {
    auto node = new PartitionTreeNode(new_cfd);
    assert (new_cfd != nullptr);
    new_cfd->SetPartitionTreeNode(node);  

    std::string n_smallest = new_cfd->GetSmallestKey();
    std::string n_largest = new_cfd->GetLargestKey();
    assert(!n_smallest.empty());
    assert(!n_largest.empty());
/*
    fprintf(stdout, "[PartitionTree] Insert New CFD[%d] %s - [%s, %s]\n", 
            new_cfd->GetID(), 
            new_cfd->GetName().c_str(), 
            n_smallest.c_str(), 
            n_largest.c_str()
            );
			*/
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
	/*
    fprintf(stdout, "[InsertSplittedColumnFamily] after: child CFD[%d] %s - [%s, %s]\n",  
            l_cfd->GetID(),
            l_cfd->GetName().c_str(),
            l_cfd->GetSmallestKey().c_str(),
            l_cfd->GetLargestKey().c_str());
*/
    if (i != 0) { // boundary overlap check
      PartitionTreeNode* pnode = base_node->lower_level_nodes_[i-1];
      ColumnFamilyData* p_cfd = pnode->cfd_;
      std::string p_largest = p_cfd->GetLargestKey();
      std::string l_smallest = l_cfd->GetSmallestKey();
      assert(p_largest.compare(l_smallest) < 0);
    }
  }

  return Status::OK();
}

ColumnFamilyData* PartitionTree::SearchColumnFamily (const Slice &key) {
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

  /*ROCKS_LOG_INFO(cnode->cfd_->ioptions()->info_log,
                 "CFD[%s] Search... %s < [%s] < %s", 
                 cnode->cfd_->GetName().c_str(),
                 get_lmost_key(cnode).c_str(),
                 key.data(),
                 get_rmost_key(cnode).c_str());*/

  
  /*fprintf(stdout, "CFD[%s] Search... %s < [%s] < %s\n", 
    cnode->cfd_->GetName().c_str(), get_lmost_key(cnode).c_str(), key.data(), get_rmost_key(cnode).c_str());*/
  
  return cnode->cfd_;
}

void PartitionTree::DeleteFromTree(uint32_t id){
  auto node = partition_nodes_[id];
  
  node->cfd_=nullptr;
  auto parent = node->parent_node_;
  if( parent != nullptr && parent->cfd_ != nullptr){
    parent->lower_level_nodes_.erase(remove(parent->lower_level_nodes_.begin(), parent->lower_level_nodes_.end(), node), parent->lower_level_nodes_.end());
  }

  partition_nodes_.erase(id);
  
}

std::vector<ColumnFamilyData*> PartitionTree::SearchAllColumnFamilies (const Slice &key) {
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
  
void PartitionTree::PrintAll() {
  root_->Print("0", true);
}
 
}



