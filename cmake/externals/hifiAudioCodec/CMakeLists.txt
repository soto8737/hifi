include(ExternalProject)
include(SelectLibraryConfigurations)

set(EXTERNAL_NAME HiFiAudioCodec)

string(TOUPPER ${EXTERNAL_NAME} EXTERNAL_NAME_UPPER)

ExternalProject_Add(
  ${EXTERNAL_NAME}
  URL https://s3.amazonaws.com/hifi-public/dependencies/codecSDK-1.zip
  URL_MD5 23ec3fe51eaa155ea159a4971856fc13
  CONFIGURE_COMMAND ""
  BUILD_COMMAND ""
  INSTALL_COMMAND ""
  LOG_DOWNLOAD 1
)

# Hide this external target (for ide users)
set_target_properties(${EXTERNAL_NAME} PROPERTIES FOLDER "hidden/externals")

ExternalProject_Get_Property(${EXTERNAL_NAME} SOURCE_DIR)

set(${EXTERNAL_NAME_UPPER}_INCLUDE_DIRS ${SOURCE_DIR}/include CACHE TYPE INTERNAL)

if (WIN32)
    set(${EXTERNAL_NAME_UPPER}_LIBRARIES ${SOURCE_DIR}/Release/audio.lib CACHE TYPE INTERNAL)
elseif(APPLE)
  set(${EXTERNAL_NAME_UPPER}_LIBRARIES ${SOURCE_DIR}/Release/libaudio.a CACHE TYPE INTERNAL)
elseif(NOT ANDROID)
  # FIXME need to account for different architectures 
  #set(${EXTERNAL_NAME_UPPER}_LIBRARIES ${SOURCE_DIR}/lib/linux64/audio.so CACHE TYPE INTERNAL)
endif()

