################################################################################
#
# ocdm-widevine
#
################################################################################

# Built from this repository (drm/ocdm-widevine): libocdm.so plus the OpenCDM headers WebKit's
# FindThunder looks for (include/WPEFramework/ocdm/open_cdm.h).
OCDM_WIDEVINE_VERSION = 1.0.0
OCDM_WIDEVINE_SITE = $(BR2_EXTERNAL_WEBOS_BROWSER_PATH)/../drm/ocdm-widevine
OCDM_WIDEVINE_SITE_METHOD = local
OCDM_WIDEVINE_INSTALL_STAGING = YES
OCDM_WIDEVINE_DEPENDENCIES = gstreamer1 host-pkgconf
OCDM_WIDEVINE_CONF_OPTS = -DOCDM_WIDEVINE_TESTS=OFF

$(eval $(cmake-package))
