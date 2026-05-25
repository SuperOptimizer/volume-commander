qt_add_executable(volume_commander
  app/main.cpp
  data/volume.cpp
  data/mask.cpp
  render/surface.cpp
  render/renderer.cpp
  utils/c3d_codec.cpp
)
set_target_properties(volume_commander PROPERTIES AUTOMOC ON)

qt_add_qml_module(volume_commander
  URI VolumeCommander
  VERSION 1.0
  QML_FILES app/Main.qml
  SOURCES
    app/state.hpp        app/state.cpp
    app/viewer_item.hpp  app/viewer_item.cpp
)

target_include_directories(volume_commander PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/app)

target_link_libraries(volume_commander PRIVATE
  Qt6::Core Qt6::Gui Qt6::Quick Qt6::Qml
  ZLIB::ZLIB
  Threads::Threads
  PkgConfig::TIFF
  OpenMP::OpenMP_CXX
  c3d
  s3
)
if(BLOSC2_FOUND)
  target_link_libraries(volume_commander PRIVATE PkgConfig::BLOSC2)
endif()
if(nlohmann_json_FOUND)
  target_link_libraries(volume_commander PRIVATE nlohmann_json::nlohmann_json)
endif()
