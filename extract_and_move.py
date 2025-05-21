import os
import re
import shutil
from collections import defaultdict

def extract_sstable_numbers(mani_file):
    """mani 파일에서 sstable 번호를 추출하는 함수"""
    level_sstables = defaultdict(list)
    current_level = None

    with open(mani_file, 'r') as f:
        for line in f:
            level_match = re.match(r"--- level (\d+) ---", line)
            sstable_match = re.match(r"^\s*(\d+):", line)

            if level_match:
                current_level = int(level_match.group(1))
            elif sstable_match and current_level is not None:
                sstable_number = int(sstable_match.group(1))
                level_sstables[current_level].append(sstable_number)

    # 모든 Level의 sstable 번호를 하나의 set으로 반환
    all_sstables = set(num for sstables in level_sstables.values() for num in sstables)
    return all_sstables

def move_matching_sstables(mani_file, dump_dir, target_dir):
    """mani에 있는 sstable만 dump 폴더에서 찾아 target 폴더로 이동"""
    if not os.path.exists(target_dir):
        os.makedirs(target_dir)

    # mani 파일에서 sstable 번호 추출
    mani_sstables = extract_sstable_numbers(mani_file)

    # dump 폴더에서 파일명 검색 (예: 003579_dump.txt)
    dump_files = [f for f in os.listdir(dump_dir) if re.match(r"^\d{6}_dump\.txt$", f)]

    for file in dump_files:
        sstable_number = int(file.split('_')[0])  # 파일명에서 숫자 부분 추출
        if sstable_number in mani_sstables:
            src_path = os.path.join(dump_dir, file)
            dst_path = os.path.join(target_dir, file)
            shutil.move(src_path, dst_path)  # 파일 이동
            print(f"Moved: {file} -> {target_dir}")

# 경로 설정 (사용자 환경에 맞게 수정)
mani_file_path = "mani"
dump_dir_path = "/home/ceph/Line_data/Project/Line/data/put_cnt_stat/hash25_zipfcomp_keyrange_num/rocksdb_4bg_250305/zc/try1/level_kr_4096_150_bg_4/trim_sst_dump"
target_dir_path = "/home/ceph/Line_data/Project/Line/data/put_cnt_stat/hash25_zipfcomp_keyrange_num/rocksdb_4bg_250305/zc/try1/level_kr_4096_150_bg_4/matching_sst_dump"

# 절대 경로로 변환
mani_file_path = os.path.expanduser(mani_file_path)
dump_dir_path = os.path.expanduser(dump_dir_path)
target_dir_path = os.path.expanduser(target_dir_path)

# 실행
move_matching_sstables(mani_file_path, dump_dir_path, target_dir_path)

