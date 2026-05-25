# Sources accumulate here as the port progresses.
set(VC_SOURCES
  app/main.cpp
)

qt_add_executable(volume_commander ${VC_SOURCES})
set_target_properties(volume_commander PROPERTIES AUTOMOC ON)

target_include_directories(volume_commander PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(volume_commander PRIVATE
  Qt6::Core Qt6::Gui Qt6::Widgets
  ZLIB::ZLIB
  Threads::Threads
  c3d
  s3
)
if(BLOSC2_FOUND)
  target_link_libraries(volume_commander PRIVATE PkgConfig::BLOSC2)
endif()
if(nlohmann_json_FOUND)
  target_link_libraries(volume_commander PRIVATE nlohmann_json::nlohmann_json)
endif()
