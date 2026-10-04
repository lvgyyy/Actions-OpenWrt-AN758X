#!/bin/bash

mkdir -p package/custom

git clone --depth=1 https://github.com/sirpdboy/luci-app-poweroffdevice.git package/custom/poweroffdevice
git clone --depth=1 https://github.com/isalikai/luci-app-owq-wol.git package/custom/owq-wol
git clone --depth=1 https://github.com/gdy666/luci-app-lucky.git package/custom/lucky
git clone --depth=1 https://github.com/sbwml/luci-app-openlist2.git package/custom/openlist2

git clone --depth=1 https://github.com/stackia/rtp2httpd.git package/custom/rtp2httpd

git clone --depth=1 https://github.com/sirpdboy/luci-app-watchdog.git package/custom/watchdog
git clone --depth=1 https://github.com/sirpdboy/luci-app-taskplan.git package/custom/taskplan
git clone --depth=1 https://github.com/iv7777/luci-app-authshield.git package/custom/authshield
git clone --depth=1 https://github.com/destan19/OpenAppFilter.git package/custom/OpenAppFilter
git clone --depth=1 https://github.com/janvanstiphout/luci-app-accesscontrol.git package/custom/accesscontrol

git clone --depth=1 https://github.com/luanmuc/luci-app-airoha-npu.git package/custom/luci-app-airoha-npu
