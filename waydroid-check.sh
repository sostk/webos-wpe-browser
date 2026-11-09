#!/bin/sh
# waydroid-check.sh — read-only feasibility probe 
# for Waydroid on a rooted LG webOS TV. Changes 
# nothing on the TV. Run as root: sh 
# /tmp/waydroid-check.sh > /tmp/wd-report.txt 2>&1 
# POSIX sh / busybox compatible.
PASS=0; WARN=0; FAIL=0 ok() { echo " [PASS] $*"; 
PASS=$((PASS+1)); } warn() { echo " [WARN] $*"; 
WARN=$((WARN+1)); } bad() { echo " [FAIL] $*"; 
FAIL=$((FAIL+1)); } hdr() { echo; echo "== $* =="; } 
have() { command -v "$1" >/dev/null 2>&1; } hdr 
"Device" uname -a KREL=$(uname -r) KMAJ=$(echo 
"$KREL" | cut -d. -f1); KMIN=$(echo "$KREL" | cut -d. 
-f2) echo "kernel release: $KREL" if have luna-send; 
then
  luna-send -n 1 -f 
  luna://com.webos.service.tv.systemproperty/getSystemInfo 
  \
    '{"keys":["modelName","firmwareVersion","sdkVersion","boardType"]}' 
    2>/dev/null | grep -E 
    'modelName|firmwareVersion|sdkVersion|boardType'
fi [ -f /etc/starfish-release ] && cat 
/etc/starfish-release [ "$(uname -m)" = "aarch64" ] 
&& ok "aarch64 kernel" || bad "kernel is $(uname -m), 
Waydroid images need aarch64" file /bin/sh 
2>/dev/null | head -1 hdr "Memory / storage / CPU" 
grep -E 'MemTotal|MemAvailable|SwapTotal' 
/proc/meminfo df -h 2>/dev/null | grep -E 
'Filesystem|/media|/mnt|/var|/tmp|usb' | head -15 
echo "cpus: $(grep -c ^processor /proc/cpuinfo)" grep 
-m1 -E '^Features' /proc/cpuinfo grep -m1 -E 'CPU 
part' /proc/cpuinfo AVAIL_KB=$(grep MemAvailable 
/proc/meminfo | awk '{print $2}') if [ -n "$AVAIL_KB" 
]; then
  [ "$AVAIL_KB" -gt 1200000 ] && ok "MemAvailable 
  ${AVAIL_KB} kB" || warn "MemAvailable only 
  ${AVAIL_KB} kB (Android wants ~1-1.5 GB free)"
fi hdr "Kernel config (/proc/config.gz)" 
KCFG=/tmp/.wd_kcfg.$$ if [ -r /proc/config.gz ]; then
  (zcat /proc/config.gz 2>/dev/null || gunzip -c 
  /proc/config.gz) > "$KCFG" echo "config lines: $(wc 
  -l < "$KCFG")" HAVECFG=1
else warn "/proc/config.gz not available - relying on 
  runtime probes only" HAVECFG=0
fi
# cfg OPTION LEVEL(req|rec|bld) NOTE
cfg() { [ "$HAVECFG" = 1 ] || return 0 v=$(grep -E 
  "^CONFIG_$1=" "$KCFG" | cut -d= -f2-) if [ "$v" = 
  "y" ] || [ "$v" = "m" ]; then ok "CONFIG_$1=$v" 
  else
    case "$2" in req) bad "CONFIG_$1 not set - $3" ;; 
      bld) warn "CONFIG_$1 not set - $3" ;; *) warn 
      "CONFIG_$1 not set - $3" ;;
    esac fi
}
if [ "$HAVECFG" = 1 ]; then echo "-- Android core" 
  cfg ANDROID_BINDER_IPC bld "must be built as 
  out-of-tree module (binder_linux)" cfg 
  ANDROID_BINDERFS bld "build with binder module 
  (needs kernel >= 5.0)" cfg MEMFD_CREATE req "needed 
  for Android 13 images (ashmem replacement)" cfg 
  ASHMEM rec "only needed for older (Android 10/11) 
  images" cfg PSI rec "recommended; lmkd misbehaves 
  without it" echo "-- Namespaces / cgroups (LXC)" 
  for o in NAMESPACES UTS_NS IPC_NS PID_NS NET_NS 
  CGROUPS CGROUP_DEVICE CGROUP_FREEZER CGROUP_PIDS 
  MEMCG CPUSETS; do
    cfg $o req "LXC container cannot start properly" 
  done cfg USER_NS rec "not strictly needed for a 
  privileged container" cfg SECCOMP_FILTER rec "LXC 
  can run without seccomp" echo "-- Filesystems / 
  block" cfg BLK_DEV_LOOP req "Waydroid mounts 
  system/vendor images via loop" cfg EXT4_FS req 
  "Waydroid images are ext4" cfg FUSE_FS rec "Android 
  storage emulation" cfg OVERLAY_FS rec "Waydroid 
  overlay feature" cfg TMPFS req "" echo "-- 
  Networking" cfg VETH req "container networking" cfg 
  BRIDGE req "waydroid0 bridge" cfg NETFILTER req "" 
  cfg NF_NAT req "NAT for container" cfg 
  IP_NF_TARGET_MASQUERADE rec "need this OR nft 
  masquerade" cfg NETFILTER_XT_TARGET_MASQUERADE rec 
  "need this OR nft masquerade" cfg NF_TABLES rec 
  "nftables path" cfg NFT_MASQ rec "nftables 
  masquerade" cfg IP_NF_IPTABLES rec "iptables path" 
  cfg NF_CT_NETLINK rec "" cfg IPV6 req "Waydroid 
  networking breaks without IPv6 support" cfg TUN rec 
  "" echo "-- Module loading" cfg MODULES req "cannot 
  load binder module" cfg MODULE_UNLOAD rec "" 
  v=$(grep -E '^CONFIG_MODULE_SIG_FORCE=' "$KCFG" | 
  cut -d= -f2) [ "$v" = "y" ] && bad 
  "CONFIG_MODULE_SIG_FORCE=y - unsigned modules 
  rejected" || ok "module signature not forced" grep 
  -E '^CONFIG_MODULE_SIG=' "$KCFG" >/dev/null && echo 
  " (info) CONFIG_MODULE_SIG is set; check 
  sig_enforce below" cfg KALLSYMS_ALL rec "helps 
  out-of-tree binder resolve unexported symbols" grep 
  -E '^CONFIG_LOCALVERSION=' "$KCFG"
fi hdr "Runtime probes" [ "$KMAJ" -ge 5 ] && ok 
"kernel $KMAJ.$KMIN (binderfs possible)" || warn 
"kernel $KMAJ.$KMIN < 5.0: no binderfs, use legacy 
binder devices; no PSI" grep -qw binder 
/proc/filesystems && ok "binder fs registered" || 
echo " (info) binder fs not registered (expected 
until module is built)" ls -l /dev/binder* 
/dev/hwbinder /dev/vndbinder 2>/dev/null [ -d 
/proc/pressure ] && ok "/proc/pressure present (PSI 
active)" || warn "/proc/pressure missing (PSI off or 
unsupported)" echo " namespaces: $(ls /proc/self/ns 
2>/dev/null | tr '\n' ' ')" for n in uts ipc pid net 
mnt; do [ -e /proc/self/ns/$n ] || bad "namespace $n 
missing"; done [ -e /proc/self/ns/user ] && ok "user 
ns present" || warn "user ns missing" echo " cgroup 
controllers:"; grep -v '^#' /proc/cgroups 2>/dev/null 
| awk '{printf " %s enabled=%s\n",$1,$4}'
mount | grep -E 'cgroup' | head -10 ls /dev/loop* 
>/dev/null 2>&1 && ok "loop devices present ($(ls 
/dev/loop* | wc -l))" || warn "no /dev/loop* nodes" [ 
-e /dev/fuse ] && ok "/dev/fuse present" || warn 
"/dev/fuse missing" [ -e /dev/net/tun ] && ok 
"/dev/net/tun present" || warn "/dev/net/tun missing" 
grep -qw overlay /proc/filesystems && ok "overlayfs 
registered" || warn "overlayfs not registered" if [ 
-r /sys/module/module/parameters/sig_enforce ]; then
  se=$(cat /sys/module/module/parameters/sig_enforce) 
  [ "$se" = "Y" ] && bad "module sig_enforce=Y" || ok 
  "module sig_enforce=$se"
fi md=$(cat /proc/sys/kernel/modules_disabled 
2>/dev/null) [ "$md" = "1" ] && bad 
"modules_disabled=1" || ok "module loading allowed 
(modules_disabled=${md:-n/a})" grep -qE ' 
(__ksymtab_kallsyms_lookup_name|kallsyms_lookup_name)$' 
/proc/kallsyms 2>/dev/null \
  && echo " (info) kallsyms_lookup_name visible in 
  /proc/kallsyms" \
  || echo " (info) kallsyms_lookup_name not visible 
  || (may need kptr_restrict=0)"
echo " kptr_restrict=$(cat 
/proc/sys/kernel/kptr_restrict 2>/dev/null)" if have 
unshare; then
  unshare -n true 2>/dev/null && ok "unshare -n 
  works" || warn "unshare -n failed"
fi hdr "Existing modules / vermagic (needed to build 
binder that loads)" lsmod 2>/dev/null | head -25 
KO=$(find /lib/modules -name '*.ko' 2>/dev/null | 
head -1) [ -z "$KO" ] && KO=$(find /usr/lib/modules 
/mnt/lg /lib -name '*.ko' 2>/dev/null | grep -v 
'/share/' | head -1) if [ -n "$KO" ]; then
  echo " sample module: $KO" if have modinfo; then 
  modinfo "$KO" 2>/dev/null | grep -E 
  'vermagic|depends' else strings "$KO" 2>/dev/null | 
  grep -m1 vermagic; fi
fi [ -d "/lib/modules/$KREL/build" ] && ok "kernel 
build dir present" || echo " (info) no 
/lib/modules/$KREL/build - build against LG GPL 
source for this exact release" hdr "GPU / display / 
bridge" ls -l /dev/mali* /dev/pvr* /dev/dri/* 
2>/dev/null ls /usr/lib/libEGL* /usr/lib/libGLESv2* 
/usr/lib/libwayland-* 2>/dev/null 
BR=/media/developer/apps/usr/palm/applications/org.webosbrew.bridge-64to32 
[ -d "$BR" ] && ok "64-to-32 bridge installed" || 
warn "64-to-32 bridge not installed" [ -f 
/media/developer/temp/gles_proxy.log ] && { echo " 
last bridge log lines:"; tail -5 
/media/developer/temp/gles_proxy.log; } ls -l 
/tmp/xdg/wayland-* /run/user/*/wayland-* 2>/dev/null 
grep -E 'GL_VERSION|GLES' 
/media/developer/temp/gles_proxy.log 2>/dev/null | 
head -3 hdr "Userland tools present" for t in 
lxc-start python3 iptables nft ip dnsmasq losetup 
mount busybox; do
  have $t && echo " $t: $(command -v $t)" || echo " 
  $t: missing"
done rm -f "$KCFG" hdr "Summary" echo "PASS=$PASS 
WARN=$WARN FAIL=$FAIL" echo "Any FAIL in the 
namespaces/cgroups/loop/networking/module sections is 
a hard blocker" echo "(the signed kernel can't be 
swapped). Binder/binderfs WARNs are expected: that's 
the module you build."
