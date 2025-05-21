import sys
import re
import pandas as pd
import glob
import os
import csv
import time

mypath1 = sys.argv[1] # bg32/20230925_1512 for example
mypath2 = sys.argv[2] # bg32/20230925_1512 for example
mypath3 = sys.argv[3] # bg32/20230925_1512 for example
file_fillrandom_path =  mypath1+"/compaction_cnt.txt"
file_mixgraph_path =  mypath2+"/compaction_cnt.txt"
fileo_path = mypath3+"_diff_compaction_cnt.txt"

mydict={}

start_time=time.time()
# read cold compactions
with open(file_mixgraph_path, "r", encoding='latin1') as file:
  lines = file.readlines()
  for line in lines:
    records = re.split(",|\n", line)
    #usr_key = int(records[0])
    f_cnt = records[1]
    if not f_cnt.isdigit():
      continue
    usr_key = int(records[0])
    c_cnt = records[2]
    if not c_cnt.isdigit():
      continue
    f_cnt = (int)(f_cnt)
    c_cnt = (int)(c_cnt)
    cnt = (int)(records[3])
    l0_c_cnt = (int)(records[4])
    l1_c_cnt = (int)(records[5])
    l2_c_cnt = (int)(records[6])
    l3_c_cnt = (int)(records[7])
    if usr_key in mydict:
      print("no such case")
      mydict[usr_key][1] += f_cnt 
      mydict[usr_key][2] += c_cnt 
      mydict[usr_key][3] += cnt 
      mydict[usr_key][4] += l0_c_cnt 
      mydict[usr_key][5] += l1_c_cnt 
      mydict[usr_key][6] += l2_c_cnt 
      mydict[usr_key][7] += l3_c_cnt 
    else:
      mydict[usr_key] = [usr_key,f_cnt, c_cnt, cnt, l0_c_cnt, l1_c_cnt, l2_c_cnt, l3_c_cnt]

with open(file_fillrandom_path, "r", encoding='latin1') as file:
  lines = file.readlines()
  for line in lines:
    records = re.split(",|\n", line)
    f_cnt = records[1]
    if not f_cnt.isdigit():
      continue
    usr_key = int(records[0])
    c_cnt = records[2]
    if not c_cnt.isdigit():
      continue
    f_cnt = (int)(f_cnt)
    c_cnt = (int)(c_cnt)
    cnt = (int)(records[3])
    l0_c_cnt = (int)(records[4])
    l1_c_cnt = (int)(records[5])
    l2_c_cnt = (int)(records[6])
    l3_c_cnt = (int)(records[7])

    if usr_key in mydict:
      mydict[usr_key][1] -= f_cnt 
      mydict[usr_key][2] -= c_cnt 
      mydict[usr_key][3] -= cnt 
      mydict[usr_key][4] -= l0_c_cnt 
      mydict[usr_key][5] -= l1_c_cnt 
      mydict[usr_key][6] -= l2_c_cnt 
      mydict[usr_key][7] -= l3_c_cnt 
    else:
      print("no such case: "+usr_key)

#df_compaction_cnt.to_csv(fileo_path)
with open(fileo_path, mode='w', newline='') as file:
  writer=csv.writer(file)
  writer.writerow(['key', 'flush_cnt', 'compaction_cnt', 'cnt', 'l0_compaction_cnt', 'l1_compaction_cnt', 'l2_compaction_cnt', 'l3_compaction_cnt'])
  for key, value in sorted(mydict.items(), key=lambda x:x[0]):
    writer.writerow([value[0], value[1], value[2], value[3], value[4], value[5], value[6], value[7]])
end_time=time.time()
elapsed_time=end_time-start_time
print(f"Reading the file took {elapsed_time:.4f} seconds")
