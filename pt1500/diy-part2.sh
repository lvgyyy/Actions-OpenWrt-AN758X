#!/bin/bash
# diy-part2.sh —— 在 .config 载入之后、make defconfig 之前执行
# 只做与机型无关的通用调整，机型与 PON 组件的开关全部由 configs/*.config 决定。
set -e

CFG=".config"

# 时区
if [ -f package/base-files/files/etc/config/system ]; then
    sed -i "s#option timezone 'UTC'#option timezone 'CST-8'#" package/base-files/files/etc/config/system
    sed -i "s#option zonename 'UTC'#option zonename 'Asia/Shanghai'#" package/base-files/files/etc/config/system
fi

# 默认开启硬件流卸载（RTL9617C PPE）
mkdir -p files/etc/uci-defaults
cat > files/etc/uci-defaults/99-hw-offload <<'EOF'
uci -q set firewall.@defaults[0].flow_offloading=1
uci -q set firewall.@defaults[0].flow_offloading_hw=1
uci -q commit firewall
exit 0
EOF

rm -rfv package/libs/libnftnl/patches/001-libnftnl-add-fullcone-expression-support.patch
rm -rfv package/network/utils/nftables/patches/004-nftables-add-fullcone-expression-support.patch
rm -rfv target/linux/generic/hack-6.18/952-net-conntrack-events-support-multiple-registrant.patch
rm -rfv package/network/config/firewall4/patches/130-firewall4-add-support-for-fullcone-nat.patch
# openwrt-sonic-fullcone
sh feeds/fullcone/add_sonic_fullcone.sh

echo "[diy-part2] 完成"
