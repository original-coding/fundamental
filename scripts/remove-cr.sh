#!/bin/bash
# 查看是否包含cr
# cat -v file

# 搜索cr
# find src -type f | xargs -I {} grep -l '\r' {}

#移除目录或文件中的cr
if [ -z "$1" ]; then
    echo "default operation path ."
    operation_path="."
else
    echo "operation path:$1"
    operation_path="$1"
fi

if [ -f "${operation_path}" ]; then
    sed -i 's/\r//g' ${operation_path}
else
    find ${operation_path} -type f | xargs -I {} sed -i 's/\r//g' {}
fi