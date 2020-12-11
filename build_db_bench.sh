# !/bin/bash
make clean && DEBUG_LEVEL=2 make db_bench -j 64
