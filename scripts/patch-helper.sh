#!/bin/bash -e
if [ "$#" -lt 2 ]; then
    echo "$0 <dst_dir> <patches_dir> <patch_level>"
    exit 1
fi

dst_dir=$(realpath  $1) 
patches_dir=$(realpath  $2) 
if [ ! -d ${dst_dir} ];then
    echo "dst_dir:${dst_dir} is not valid"
    exit 2
fi
if [ ! -d ${patches_dir} ];then
    echo "patches_dir:${patches_dir} is not valid"
    exit 3
fi
echo "dst_dir=${dst_dir}"
echo "patches_dir=${patches_dir}"

if [ -z "$3" ]; then
    patch_level="1"
    echo "use default patch level ${patch_level}"
else
    patch_level="$3"
    echo "use patch level ${patch_level}"
fi

#get all available patches
patch_files=$(find ${patches_dir} -maxdepth 1 -type f -regex '.*/[0-9]+-.*\.patch$' -exec bash -c ' 
for file; do
    num=$(basename "$file" | cut -d- -f1)
    echo -e "$num\t$file"
done
' bash {} + | sort -n -k1,1 | cut -f2-)

echo "patch_files=${patch_files}"

echo "${patch_files}" | xargs -I {}  patch -p${patch_level} -d ${dst_dir} -i {}

	 

