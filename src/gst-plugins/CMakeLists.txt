cmake_minimum_required(VERSION 2.8)

include(GLibHelpers)

add_subdirectory(dtls-plugins)

set(KMS_ELEMENTS_SOURCES
  kmselements.c
  kmshttpendpoint.c
  kmshttpgetendpoint.c
  kmshttppostendpoint.c
  kmsplayerendpoint.c
  kmsrecorderendpoint.c
  kmsrtpconnection.c
  kmsrtpendpoint.c
  kmswebrtcconnection.c
  kmswebrtcendpoint.c
  kmsselectablemixer.c
  kmsdispatcher.c
  kmsdispatcheronetomany.c
  kmscompositemixer.c
  kmsalphablending.c
  kmsmuxingpipeline.c
)

set(KMS_ELEMENTS_HEADERS
  kmshttpendpoint.h
  kmshttpendpointmethod.h
  kmshttpgetendpoint.h
  kmshttppostendpoint.h
  kmsplayerendpoint.h
  kmsrecorderendpoint.h
  kmsrtpendpoint.h
  kmswebrtcendpoint.h
  kmsselectablemixer.h
  kmsdispatcher.h
  kmsdispatcheronetomany.h
  kmscompositemixer.h
  kmsalphablending.h
  kmsmuxingpipeline.h
)

set(ENUM_HEADERS
  kmshttpendpointmethod.h
  kmsencodingrules.h
)

add_glib_marshal(KMS_ELEMENTS_SOURCES KMS_ELEMENTS_HEADERS kms-elements-marshal __kms_elements_marshal)
add_glib_enumtypes(KMS_ELEMENTS_SOURCES KMS_ELEMENTS_HEADERS kms-elements-enumtypes KMS ${ENUM_HEADERS})

add_library(${LIBRARY_NAME}plugins MODULE ${KMS_ELEMENTS_SOURCES} ${KMS_ELEMENTS_HEADERS})

add_dependencies(${LIBRARY_NAME}plugins kmsdtls rtcpdemux)

include_directories(
  ${KmsGstCommons_INCLUDE_DIRS}
  ${gstreamer-1.0_INCLUDE_DIRS}
  ${gstreamer-base-1.0_SOURCE_DIRS}
  ${gstreamer-sdp-1.0_SOURCE_DIRS}
  ${gstreamer-pbutils-1.0_SOURCE_DIRS}
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_BINARY_DIR}
  ${libsoup-2.4_INCLUDE_DIRS}
  ${nice_INCLUDE_DIRS}
)

target_link_libraries(${LIBRARY_NAME}plugins
  ${KmsGstCommons_LIBRARIES}
  ${gstreamer-1.0_LIBRARIES}
  ${gstreamer-base-1.0_LIBRARIES}
  ${gstreamer-sdp-1.0_LIBRARIES}
  ${gstreamer-pbutils-1.0_LIBRARIES}
  ${libsoup-2.4_LIBRARIES}
  ${nice_LIBRARIES}
)

install(
  TARGETS ${LIBRARY_NAME}plugins
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_GST_PLUGINS_DIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
