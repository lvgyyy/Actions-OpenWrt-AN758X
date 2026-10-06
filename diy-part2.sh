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
if [ -f target/linux/airoha/dts/an758x-nokia_xg-040g-common.dtsi ]; then
    sed -i 's/macaddr_factory_3e 0/macaddr_factory_3e 2/g' target/linux/airoha/dts/an758x-nokia_xg-040g-common.dtsi
fi

# 5382A
if [ -f target/linux/airoha/dts/an7581-fiberhome-ubi-parts.dtsi ]; then
    sed -i '/reg = <0x2000 0x0006>;/a\#nvmem-cell-cells = <1>;' target/linux/airoha/dts/an7581-fiberhome-ubi-parts.dtsi
fi

if [ -f  target/linux/airoha/dts/an7581-fiberhome-hg5382a.dts ]; then
   sed -i '1,/fiberhome_base_mac>/s/fiberhome_base_mac>/fiberhome_base_mac 0>/' target/linux/airoha/dts/an7581-fiberhome-hg5382a.dts
   sed -i '1,/fiberhome_base_mac>/s/fiberhome_base_mac>/fiberhome_base_mac 1>/' target/linux/airoha/dts/an7581-fiberhome-hg5382a.dts
   sed -i '1,/fiberhome_base_mac>/s/fiberhome_base_mac>/fiberhome_base_mac 2>/' target/linux/airoha/dts/an7581-fiberhome-hg5382a.dts
fi

# 默认开启硬件流卸载（AN7581 PPE / NPU）
mkdir -p files/etc/uci-defaults
cat > files/etc/uci-defaults/99-pon-offload <<'EOF'
uci -q set firewall.@defaults[0].flow_offloading=1
uci -q set firewall.@defaults[0].flow_offloading_hw=1
uci -q commit firewall
exit 0
EOF

# openwrt-sonic-fullcone
sed -i 's/+kmod-nft-fullcone//g' package/network/config/firewall4/Makefile
rm -rf package/network/config/firewall4/patches/001-firewall4-add-support-for-fullcone-nat.patch
rm -rf package/network/utils/fullconenat-nft/
sed -i '272d' feeds/luci/modules/luci-base/root/usr/share/rpcd/ucode/luci
sed -i '58,62d' feeds/luci/applications/luci-app-firewall/htdocs/luci-static/resources/view/firewall/zones.js

sh feeds/fullcone/add_sonic_fullcone.sh

# MVRP
echo "CONFIG_VLAN_8021Q_MVRP=y" >> target/linux/airoha/an7581/config-6.18
echo "CONFIG_MRP=y" >> target/linux/airoha/an7581/config-6.18
echo "CONFIG_BRIDGE_MRP=y" >> target/linux/airoha/an7581/config-6.18

echo "[diy-part2] 完成"
