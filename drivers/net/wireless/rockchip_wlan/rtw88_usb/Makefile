# USB-only subset of the rtw88 backport (no PCI/SDIO parts)
ccflags-y += -O2 -std=gnu11 -Wno-declaration-after-statement
ccflags-y += -DCONFIG_RTW88_LEDS=1 -DCONFIG_RTW88_DEBUG=1 -DCONFIG_RTW88_DEBUGFS=1 -D__CHECK_ENDIAN__
ccflags-y += -include $(srctree)/$(src)/compat-andr36oid.h
obj-$(CONFIG_ANDR36OID_RTW88_USB) += rtw_core.o
rtw_core-objs += main.o led.o mac80211.o util.o debug.o tx.o rx.o mac.o phy.o coex.o efuse.o fw.o ps.o sec.o bf.o regd.o sar.o wow.o
obj-$(CONFIG_ANDR36OID_RTW88_USB) += rtw_usb.o
rtw_usb-objs := usb.o
obj-$(CONFIG_ANDR36OID_RTW88_USB) += rtw_8822c.o
rtw_8822c-objs := rtw8822c.o rtw8822c_table.o
obj-$(CONFIG_ANDR36OID_RTW88_USB) += rtw_8822cu.o
rtw_8822cu-objs := rtw8822cu.o
ccflags-y += -include $(srctree)/$(src)/namespace-andr36oid.h
