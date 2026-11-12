include $(sort $(wildcard $(BR2_EXTERNAL_WEBOS_BROWSER_PATH)/package/*/*.mk))

# Overrides for Buildroot packages. Recipes expand these variables lazily, so appending here works.
# Use the WPEPlatform API (headless platform, SHM buffers) and skip the DRM/Wayland platforms,
# which the browser does not use (it composites WebKit's buffers itself through the kit).
WPEWEBKIT_CONF_OPTS += \
	-DENABLE_WPE_PLATFORM=ON \
	-DENABLE_WPE_PLATFORM_HEADLESS=ON \
	-DENABLE_WPE_PLATFORM_DRM=OFF \
	-DENABLE_WPE_PLATFORM_WAYLAND=OFF

# HTTPS trust: the GnuTLS runtime patch accepts WPE_BROWSER_CA_FILE, set to the
# immutable bundled CA list by EngineClient. The configured path is only a fallback
# for older launchers; normal app startup no longer writes a shared /tmp file.
GNUTLS_CONF_OPTS += --with-default-trust-store-file=/tmp/wpe-browser/ca-certificates.crt

# JavaScriptCore's 32-bit ARM JIT needs the hard-float ABI (PlatformEnable.h: CPU(ARM_THUMB2) && CPU(ARM_HARDFP)),
# and webOS userland is softfp, so WebKit turns the baseline JIT off in its headers while CMake keeps DFG on:
# "DFG and FTL JIT require baseline JIT to be enabled". Buildroot only does this for ARMv5/v6/MIPS
# (https://bugs.webkit.org/show_bug.cgi?id=278559); do the same here: the portable CLoop interpreter.
# The hard-float build (webos_browser_hf_defconfig, private glibc) keeps WebKit's defaults: baseline + DFG JIT, WebAssembly.
#
# softfp + JIT (`make WPEB_SOFTFP_JIT=y ...`, tools/build-softfp-jit.sh): uses patches/wpewebkit/0004-jsc-arm-softfp-jit.patch
# (tested on the TV 2026-11-11). JSC's ARMv7 JIT emits Thumb-2 and detects CPU(ARM_THUMB2) from __thumb2__, so WebKit
# alone is compiled as Thumb-2; it interworks with the ARM-mode libraries of the rest of the tree. The toolchain
# wrapper puts its -marm first, so this later -mthumb wins. CMAKE_*_FLAGS must repeat TARGET_*FLAGS (toolchainfile.cmake).
ifeq ($(WPEB_SOFTFP_JIT),y)
WPEWEBKIT_CONF_OPTS += \
	-DCMAKE_C_FLAGS="$(TARGET_CFLAGS) -mthumb" \
	-DCMAKE_CXX_FLAGS="$(TARGET_CXXFLAGS) -mthumb"
else ifneq ($(BR2_ARM_EABIHF),y)
WPEWEBKIT_CONF_OPTS += \
	-DENABLE_JIT=OFF \
	-DENABLE_DFG_JIT=OFF \
	-DENABLE_FTL_JIT=OFF \
	-DENABLE_C_LOOP=ON \
	-DENABLE_SAMPLING_PROFILER=OFF \
	-DENABLE_WEBASSEMBLY=OFF
endif

# Mesa is bundled for WPEWebProcess only, as the surfaceless (software) EGL platform. Buildroot adds the
# wayland platform whenever BR2_PACKAGE_WAYLAND is on (wpewebkit selects it), which makes libEGL link
# libwayland-client/-server: libraries the TV has its own copies of, and which the kit's packaging check
# refuses in the app. Appended last, so the platform list wins over Buildroot's.
MESA3D_CONF_OPTS += -Dplatforms= -Dlegacy-wayland=

# Preserve WebKit's complete in-process media stack (URL playback + MSE). A URL-only
# external holepunch engine cannot implement SourceBuffer and must not replace it.
ifeq ($(BR2_PACKAGE_WPEWEBKIT_MULTIMEDIA),y)
# Buildroot's wpewebkit-reconfigure target only follows explicit dependencies;
# Kconfig selects alone do not build runtime demuxers, parsers or audio sinks.
WPEWEBKIT_DEPENDENCIES += gst1-plugins-good gst1-plugins-bad
# Package infrastructure already expanded configure prerequisites before external.mk
# was included; add the order-only edge explicitly for incremental package targets.
$(WPEWEBKIT_TARGET_CONFIGURE): | gst1-plugins-good gst1-plugins-bad
WPEWEBKIT_CONF_OPTS += -DENABLE_MEDIA_SOURCE=ON -DENABLE_ENCRYPTED_MEDIA=ON -DUSE_EXTERNAL_HOLEPUNCH=OFF
# Widevine EME through WebKit's Thunder backend, served by libocdm (package/ocdm-widevine), which drives the
# TV's own CDM in-process: no Thunder framework. Without it, EME has ClearKey only.
ifeq ($(BR2_PACKAGE_OCDM_WIDEVINE),y)
WPEWEBKIT_DEPENDENCIES += ocdm-widevine
$(WPEWEBKIT_TARGET_CONFIGURE): | ocdm-widevine
WPEWEBKIT_CONF_OPTS += -DENABLE_THUNDER=ON
else
WPEWEBKIT_CONF_OPTS += -DENABLE_THUNDER=OFF
endif
endif
