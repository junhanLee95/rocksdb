#include <mutex>

#pragma once
class ZipfGenerator {
  public:
    // copy constructor
    ZipfGenerator(const ZipfGenerator &zc) {}
    // constructor
    ZipfGenerator(void) {}
    // destructor
    ~ZipfGenerator(void) {}

    void init_zipf_generator(long min, long max, char c, bool counter, long recordcount);
    void init_zipf_generator(long min, long max, char c, bool counter);
    void init_zipf_generator(long min, long max, char c);
    void init_zipf_generator(long min, long max);
    long nextValue();
    long nextLatestValue();
    char getPrefix();
    long getItems();
  private:
    long nextLong(long itemcount);
    void setLastValue(long val);
    double zeta(long st, long n, double initialsum);
    double zetastatic(long st, long n, double initialsum);

    long items; //initialized in init_zipf_generator
    long base; //initialized in init_zipf_generator
    double zipfianconstant; //initialized in init_zipf_generator
    double alpha; //initialized in init_zipf_generator
    double zetan; //initialized in init_zipf_generator
    double eta; //initialized in init_zipf_generator
    double theta; //initialized in init_zipf_generator
    double zeta2theta; //initialized in init_zipf_generator
    long countforzeta; //initialized in init_zipf_generator
    char prefix; //initialized in init_zipf_generator
    long lastVal; //initialized in setLastValue
    bool countergenerator; //initialized in init_zipf_generator
    long recordcount; //initialized in init_zipf_generator
    std::mutex mutex_;
};
