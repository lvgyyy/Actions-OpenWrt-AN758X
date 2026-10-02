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

# 主机名
if [ -f package/base-files/files/bin/config_generate ]; then
    sed -i 's/ponwrt/ImmortalWrt/g' package/base-files/files/bin/config_generate
    cat package/base-files/files/bin/config_generate
fi

# MAC修改
# 040G
sed -i 's/macaddr_factory_3e 0/macaddr_factory_3e 2/g' target/linux/airoha/dts/an758x-nokia_xg-040g-common.dtsi

# 5382A
sed -i '/reg = <0x2000 0x0006>;/a\#nvmem-cell-cells = <1>;' target/linux/airoha/dts/an7581-fiberhome-ubi-parts.dtsi
sed -i '1,/fiberhome_base_mac>/s/fiberhome_base_mac>/fiberhome_base_mac 0>/' target/linux/airoha/dts/an7581-fiberhome-hg5382a.dts
sed -i '1,/fiberhome_base_mac>/s/fiberhome_base_mac>/fiberhome_base_mac 1>/' target/linux/airoha/dts/an7581-fiberhome-hg5382a.dts
sed -i '1,/fiberhome_base_mac>/s/fiberhome_base_mac>/fiberhome_base_mac 2>/' target/linux/airoha/dts/an7581-fiberhome-hg5382a.dts

# 默认开启硬件流卸载（AN7581 PPE / NPU）
mkdir -p files/etc/uci-defaults
cat > files/etc/uci-defaults/99-pon-offload <<'EOF'
uci -q set firewall.@defaults[0].flow_offloading=1
uci -q set firewall.@defaults[0].flow_offloading_hw=1
uci -q commit firewall
exit 0
EOF

echo "[diy-part2] 完成"
