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

bg2509_modify_platform()
{
	local f=target/linux/mediatek/filogic/base-files/lib/upgrade/platform.sh
	awk '
	/^platform_do_upgrade[[:space:]]*\(\)/ { in_func=1 }
	in_func && !done && /^[[:space:]]*case "\$board" in[[:space:]]*$/ {
		print
		print "\tbeeconx,bg2509xs-p)"
		print "\t\tdefault_do_upgrade \"$1\""
		print "\t\t;;"
		done=1
		next
	}
	{ print }
	' "$f" > "$f.tmp" && mv "$f.tmp" "$f"

	cat $f
}

bg2509_modify_network()
{
	local f=target/linux/mediatek/filogic/base-files/etc/board.d/02_network
	awk '
	/^[[:space:]]*case \$board in[[:space:]]*$/ {
		n++
    		print
    		if (n == 1) {
        		print "\tbeeconx,bg2509xs-p)"
        		print "\t\tucidef_set_interfaces_lan_wan \"eth1\" \"eth0\""
        		print "\t\t;;"
    		} else if (n == 2) {
        		print "\tbeeconx,bg2509xs-p)"
        		print "\t\tlan_mac=$(mtd_get_mac_binary \"art\" 0x0)"
        		print "\t\twan_mac=$(macaddr_add \"$lan_mac\" 1)"
        		print "\t\t;;"
    		}
    		next
	}
	{ print }
	' "$f" > "$f.tmp" && mv "$f.tmp" "$f"

	cat $f
}

bg2509_modify_leds()
{
	local F=target/linux/mediatek/filogic/base-files/etc/board.d/01_leds
	awk '
	/^case [$]board in$/ && !done {
    		print
    		print "beeconx,bg2509xs-p)"
    		print "\tucidef_set_led_netdev \"wan1g\" \"WAN 1G\" \"led_wan_1g\" \"eth0\" \"link_10 link_100 link_1000\""
    		print "\tucidef_set_led_netdev \"wan2p5g\" \"WAN 2P5G\" \"led_wan_2p5g\" \"eth0\" \"link_2500\""
    		print "\t;;"
    		done = 1
    		next
	}
	{ print }
	' "$F" > "$F.tmp" && mv "$F.tmp" "$F"

	cat $F
}

bg2509_modify_filogic_mk()
{
	local F=target/linux/mediatek/image/filogic.mk
	cat >> "$F" <<'EOF'

define Device/beeconx_bg2509xs-p
  DEVICE_VENDOR := BeeconX
  DEVICE_MODEL := BG2509XS-P
  DEVICE_DTS := mt7987b-beeconx-bg2509xs-p
  DEVICE_DTS_DIR := ../dts
  DEVICE_PACKAGES := kmod-phy-airoha-en8811h mt7987-2p5g-phy-firmware kmod-dsa-rtl837x -wpad-openssl luci-app-poe-manager
  KERNEL_SIZE := 6291456
  IMAGE_SIZE  := 32112640
  IMAGES := sysupgrade.bin factory.bin
  IMAGE/factory.bin := append-kernel | pad-to $$(KERNEL_SIZE) | append-rootfs | pad-rootfs | check-size $$(IMAGE_SIZE)
endef
TARGET_DEVICES += beeconx_bg2509xs-p
EOF

	cat $F
}

bg2509_modify_platform
bg2509_modify_network
bg2509_modify_leds
bg2509_modify_filogic_mk

echo "[diy-part2] 完成"
