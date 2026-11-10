# Overrides for Buildroot packages. Recipes expand these variables lazily, so appending here works.
# Use the WPEPlatform API (headless platform, SHM buffers) and skip the DRM/Wayland platforms,
# which the browser does not use (it composites WebKit's buffers itself through the kit).
WPEWEBKIT_CONF_OPTS += \
	-DENABLE_WPE_PLATFORM=ON \
	-DENABLE_WPE_PLATFORM_HEADLESS=ON \
	-DENABLE_WPE_PLATFORM_DRM=OFF \
	-DENABLE_WPE_PLATFORM_WAYLAND=OFF

# HTTPS trust: the browser app copies its bundled CA list to this fixed path before starting the engine
# (a path inside the app package is not known at build time). Appended last, so it wins over Buildroot's
# --with-default-trust-store-file=/etc/ssl/certs/ca-certificates.crt.
GNUTLS_CONF_OPTS += --with-default-trust-store-file=/tmp/wpe-browser/ca-certificates.crt

# JavaScriptCore's 32-bit ARM JIT needs the hard-float ABI (PlatformEnable.h: CPU(ARM_THUMB2) && CPU(ARM_HARDFP)),
# and webOS userland is softfp, so WebKit turns the baseline JIT off in its headers while CMake keeps DFG on:
# "DFG and FTL JIT require baseline JIT to be enabled". Buildroot only does this for ARMv5/v6/MIPS
# (https://bugs.webkit.org/show_bug.cgi?id=278559); do the same here: the portable CLoop interpreter.
WPEWEBKIT_CONF_OPTS += \
	-DENABLE_JIT=OFF \
	-DENABLE_DFG_JIT=OFF \
	-DENABLE_FTL_JIT=OFF \
	-DENABLE_C_LOOP=ON \
	-DENABLE_SAMPLING_PROFILER=OFF \
	-DENABLE_WEBASSEMBLY=OFF

# Mesa is bundled for WPEWebProcess only, as the surfaceless (software) EGL platform. Buildroot adds the
# wayland platform whenever BR2_PACKAGE_WAYLAND is on (wpewebkit selects it), which makes libEGL link
# libwayland-client/-server: libraries the TV has its own copies of, and which the kit's packaging check
# refuses in the app. Appended last, so the platform list wins over Buildroot's.
MESA3D_CONF_OPTS += -Dplatforms= -Dlegacy-wayland=
