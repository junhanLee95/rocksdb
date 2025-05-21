import sys
import re
#import pandas as pd
import glob
import os
import csv
import time

#mypath = sys.argv[1] #ex : ./zc/single_level_kr_16_99/run/cf_0/trim_sst_dump/
mypath = sys.argv[1]
filei_pattern = os.path.join(mypath, "*_dump.txt") 
fileis = glob.glob(filei_pattern)
fileo_path =  mypath+"/compaction_cnt.txt"
#df_compaction_cnt = pd.DataFrame(columns=['key', 'flush_count', 'compaction_count', 'count','l0_compaction_cnt','l1_compaction_cnt','l2_compaction_cnt','l3_compaction_cnt'])
file_cnt = len(fileis)
mydict={}
fid = 0

start_time=time.time()
for filei_path in fileis:
  fid = fid+1
  file_name = os.path.basename(filei_path)
  print("{} / {} stage".format(fid, file_cnt))
  print(file_name + " start")
  lid=0
  with open(filei_path, "r") as file:
  #with open(filei_path, "r", encoding='latin1') as file:
    lines = file.readlines()
    for line in lines:
      lid=lid+1
      records_f_cnt = re.search("flush_cnt:\d*", line)
      records_c_cnt = re.search("compaction_cnt:\d*", line)
      if ( records_f_cnt is None or records_c_cnt is None):
        continue
      usr_key = re.split(" ", line)[0][1:-1]
      
      f_cnt = (int)(re.split(":", records_f_cnt[0])[1])
      c_cnt = (int)(re.split(":", records_c_cnt[0])[1])
      c1_cnt = c_cnt&0xffff;
      c2_cnt = (c_cnt>>16)&0xffff;
      c3_cnt = (c_cnt>>32)&0xffff;
      c0_cnt = (c_cnt>>48)&0xffff;
      if usr_key in mydict:
        mydict[usr_key][0] += f_cnt 
        mydict[usr_key][1] += (c1_cnt+c2_cnt+c3_cnt+c0_cnt) 
        mydict[usr_key][2] += 1 
        mydict[usr_key][3] += c0_cnt
        mydict[usr_key][4] += c1_cnt
        mydict[usr_key][5] += c2_cnt
        mydict[usr_key][6] += c3_cnt
      else:
        mydict[usr_key] = [f_cnt, c1_cnt+c2_cnt+c3_cnt+c0_cnt, 1, c0_cnt, c1_cnt, c2_cnt, c3_cnt]

      #  df_compaction_cnt.loc[df_compaction_cnt['key'] == usr_key] += put_cnt
      #else:
      #  new_row = {'key': usr_key, 'compaction_count': put_cnt}
      #  df_compaction_cnt = df_compaction_cnt.append(new_row, ignore_index=True)
#df_compaction_cnt.to_csv(fileo_path)
with open(fileo_path, mode='w', newline='') as file:
  writer=csv.writer(file)
  writer.writerow(['key_id', 'flush_cnt', 'compaction_cnt', 'cnt','l0_compaction_cnt','l1_compaction_cnt','l2_compaction_cnt','l3_compaction_cnt'])
  for key, value in  sorted(mydict.items(), key=lambda x:x[0]):
    writer.writerow([key,value[0],value[1],value[2],value[3],value[4],value[5],value[6]])
end_time=time.time()
elapsed_time=end_time-start_time
#print(f"Reading the file took {elapsed_time:.4f} seconds")
