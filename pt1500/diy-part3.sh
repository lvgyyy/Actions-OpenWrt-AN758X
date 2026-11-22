#!/bin/bash

mkdir -p package/custom

git clone --depth=1 https://github.com/svenshi/luci-app-oxidns.git package/custom/luci-app-oxidns

wget https://github.com/lvgyyy/oxidns-build/releases/latest/download/oxidns-aarch64-unknown-linux-musl.tar.gz

tar -xzvf oxidns-aarch64-unknown-linux-musl.tar.gz

mkdir -pv target/linux/rtl9617c/base-files/etc/config
mkdir -pv target/linux/rtl9617c/base-files/etc/oxidns
mkdir -pv target/linux/rtl9617c/base-files/usr/bin
mkdir -pv target/linux/rtl9617c/base-files/lib/upgrade/keep.d

cp -arfv config.yaml target/linux/rtl9617c/base-files/etc/oxidns/
cp -arfv oxidns target/linux/rtl9617c/base-files/usr/bin/

cp -arfv package/custom/luci-app-oxidns/root/etc/* target/linux/rtl9617c/base-files/etc/

echo "/etc/oxidns/" > target/linux/rtl9617c/base-files/lib/upgrade/keep.d/oxidns
