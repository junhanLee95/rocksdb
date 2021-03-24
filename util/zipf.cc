#include "zipf.h"
#include <math.h>
#include <stdlib.h>
#include <time.h>
#include <stdio.h>
#include <climits>
#include <mutex>

// 	*
// 	 * Create a zipfian generator for items between min and max (inclusive) for the specified zipfian constant, using the precomputed value of zeta.
// 	 * 
// 	 * @param min The smallest integer to generate in the sequence.
// 	 * @param max The largest integer to generate in the sequence.
void ZipfGenerator::init_zipf_generator(long min, long max, char c, bool counter, long rcount) {
	items = max-min+1;
	base = min;
	zipfianconstant = 0.99;
	theta = zipfianconstant;
	zeta2theta = zeta(0, 2, 0);
	alpha = 1.0/(1.0-theta);
	zetan = zetastatic(0, max-min+1, 0);
	countforzeta = items;
	eta=(1 - pow(2.0/items,1-theta) )/(1-zeta2theta/zetan);
  prefix = c;
  countergenerator = counter;
  recordcount = rcount;
  setLastValue(-1); // key is taken through nextValue call and it must be zero at first.
}

void ZipfGenerator::init_zipf_generator(long min, long max, char c, bool counter) {
	items = max-min+1;
	base = min;
	zipfianconstant = 0.99;
	theta = zipfianconstant;
	zeta2theta = zeta(0, 2, 0);
	alpha = 1.0/(1.0-theta);
	zetan = zetastatic(0, max-min+1, 0);
	countforzeta = items;
	eta=(1 - pow(2.0/items,1-theta) )/(1-zeta2theta/zetan);
  prefix = c;
  countergenerator = counter;
  recordcount = LONG_MAX;
}


void ZipfGenerator::init_zipf_generator(long min, long max, char c) {
	items = max-min+1;
	base = min;
	zipfianconstant = 0.99;
	theta = zipfianconstant;
	zeta2theta = zeta(0, 2, 0);
	alpha = 1.0/(1.0-theta);
	zetan = zetastatic(0, max-min+1, 0);
	countforzeta = items;
	eta=(1 - pow(2.0/items,1-theta) )/(1-zeta2theta/zetan);
  prefix = c;
  countergenerator = false;
  recordcount = LONG_MAX;

}

void ZipfGenerator::init_zipf_generator(long min, long max) {
	items = max-min+1;
	base = min;
	zipfianconstant = 0.99;
	theta = zipfianconstant;
	zeta2theta = zeta(0, 2, 0);
	alpha = 1.0/(1.0-theta);
	zetan = zetastatic(0, max-min+1, 0);
	countforzeta = items;
	eta=(1 - pow(2.0/items,1-theta) )/(1-zeta2theta/zetan);
  countergenerator = false;
  recordcount = items;

}

char ZipfGenerator::getPrefix(void) {
  return prefix;
}

long ZipfGenerator::getItems(void) {
  return items;
}

double ZipfGenerator::zeta(long st, long n, double initialsum) {
	countforzeta=n;
	return zetastatic(st,n,initialsum);
}

//initialsum is the value of zeta we are computing incrementally from
double ZipfGenerator::zetastatic(long st, long n, double initialsum){
	double sum=initialsum;
	for (long i=st; i<n; i++){
		sum+=1/(pow(i+1,theta));
	}				
	return sum;
}

long ZipfGenerator::nextLong(long itemcount){
  if(countergenerator){
    long ret = (lastVal + 1) % recordcount;
    setLastValue(ret);
    return ret;
  }

	//from "Quickly Generating Billion-Record Synthetic Databases", Jim Gray et al, SIGMOD 1994
	if (itemcount!=countforzeta){
		if (itemcount>countforzeta){
			printf("WARNING: Incrementally recomputing Zipfian distribtion. (itemcount= %ld; countforzeta= %ld)", itemcount, countforzeta);
			//we have added more items. can compute zetan incrementally, which is cheaper
			zetan = zeta(countforzeta,itemcount,zetan);
			eta = ( 1 - pow(2.0/items,1-theta) ) / (1-zeta2theta/zetan);
		} 
	}
	
	double u = (double)rand() / ((double)RAND_MAX);
	double uz=u*zetan;
	if (uz < 1.0){
		return base;
	}
	
	if (uz<1.0 + pow(0.5,theta)) {
		return base + 1;
	}
	long ret = base + (long)((itemcount) * pow(eta*u - eta + 1, alpha));
	setLastValue(ret);
	return ret;
}

long ZipfGenerator::nextValue() {
  std::lock_guard<std::mutex> lock(mutex_);
  long nextVal;
  if(countergenerator){
    nextVal = nextLong(items);
  } else{
    do{
      nextVal = nextLong(items);
    } while(nextVal >= recordcount); 
  }
	return nextVal;
}

long ZipfGenerator::nextLatestValue() {
  long max = lastVal;
  long next = max - nextLong(max);
  setLastValue(next);
  return next;
}

void ZipfGenerator::setLastValue(long val){
	lastVal = val;
}

