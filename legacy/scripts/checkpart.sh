#!/bin/sh
# $1=磁盘镜像 $2=分区LBA $3=分区扇区数
set -e
SZ=$(expr "$3" \* 512)
dd if="$1" of=/tmp/_p.img bs=1M skip=$(expr "$2" \* 512 / 1048576) count=$(expr "$SZ" / 1048576 + 1) conv=sparse status=none 2>/dev/null || true
truncate -s "$SZ" /tmp/_p.img
echo "--- ntfsfix (分区 LBA $2, $3 扇区) ---"
ntfsfix -n /tmp/_p.img 2>&1 | tail -3
rm -f /tmp/_p.img
