include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-poe-manager
PKG_VERSION:=20260921
PKG_RELEASE:=1
PKG_PO_VERSION:=$(PKG_VERSION)-r$(PKG_RELEASE)
PKG_LICENSE:=GPL-2.0-only

LUCI_TITLE:=LuCI control for TMI7604R/TMI7608R and RTL8238B PoE
LUCI_DEPENDS:=@(TARGET_qualcommbe||TARGET_mediatek) +libuci +luci-base
LUCI_DESCRIPTION:=LuCI interface and native PoE control for the Xiaomi BE3600 Pro wired P5 and P8 (TMI7604R/TMI7608R) and for boards built around an RTL8238B PSE.

define Package/luci-app-poe-manager/conffiles
/etc/config/poe-manager
endef

include $(TOPDIR)/feeds/luci/luci.mk

# call BuildPackage - OpenWrt buildroot signature
