cmake_minimum_required(VERSION 3.20)
project(RollingSamplerReconstruction LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static")
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -static")

set(CORE_SOURCES
    src/core/RollingAudioBuffer.h
    src/core/RollingAudioBuffer.cpp
    src/core/SettingsManager.h
    src/core/SettingsManager.cpp
)

set(AUDIO_SOURCES
    src/audio/WasapiAudioCapture.h
    src/audio/WasapiAudioCapture.cpp
)

set(UI_SOURCES
    src/ui/WaveformEditor.h
    src/ui/WaveformEditor.cpp
)

set(PLUGIN_LINK_LIBRARIES
    ole32
    oleaut32
    uuid
    gdi32
    user32
    comctl32
    msimg32
    gdiplus
    shlwapi
    shell32
    winmm
    avrt
    dwmapi
)

# Automated Unit Tests Executable
add_executable(test_buffer
    tests/test_buffer.cpp
    ${CORE_SOURCES}
)

# Automated Plugin Integration Tests (CLAP & VST3 Host Test)
add_executable(test_plugins
    tests/test_plugins.cpp
    third_party/vst3_pluginterfaces/base/coreiids.cpp
    third_party/vst3_pluginterfaces/base/funknown.cpp
)

target_include_directories(test_plugins PRIVATE
    third_party/clap/include
    third_party/vst3_include
)

target_compile_options(test_plugins PRIVATE -Wno-pragma-pack)

# Main Application Executable (BeagleSampler Standalone)
add_executable(rolling_sampler_app WIN32
    src/app/main.cpp
    src/app/beaglesampler.rc
    ${CORE_SOURCES}
    ${AUDIO_SOURCES}
)

set_target_properties(rolling_sampler_app PROPERTIES
    OUTPUT_NAME "BeagleSampler"
)

target_link_libraries(rolling_sampler_app PRIVATE
    ${PLUGIN_LINK_LIBRARIES}
    winhttp
)

target_link_options(rolling_sampler_app PRIVATE
    -static
)

# Milestone 7: CLAP Plugin Target (BeagleSampler.clap)
add_library(beaglesampler_clap MODULE
    src/plugin/BeagleSamplerClap.cpp
    src/app/beaglesampler.rc
    ${CORE_SOURCES}
    ${UI_SOURCES}
    ${AUDIO_SOURCES}
)

set_target_properties(beaglesampler_clap PROPERTIES
    OUTPUT_NAME "BeagleSampler"
    PREFIX ""
    SUFFIX ".clap"
)

target_include_directories(beaglesampler_clap PRIVATE
    third_party/clap/include
)

target_link_libraries(beaglesampler_clap PRIVATE
    ${PLUGIN_LINK_LIBRARIES}
)

target_link_options(beaglesampler_clap PRIVATE
    -static
)

# Milestone 7: VST3 Plugin Target (BeagleSampler.vst3)
add_library(beaglesampler_vst3 MODULE
    src/plugin/BeagleSamplerVst3.cpp
    src/app/beaglesampler.rc
    third_party/vst3_pluginterfaces/base/coreiids.cpp
    third_party/vst3_pluginterfaces/base/funknown.cpp
    ${CORE_SOURCES}
    ${UI_SOURCES}
    ${AUDIO_SOURCES}
)

set_target_properties(beaglesampler_vst3 PROPERTIES
    OUTPUT_NAME "BeagleSampler"
    PREFIX ""
    SUFFIX ".vst3"
)

target_compile_options(beaglesampler_vst3 PRIVATE -Wno-pragma-pack)

target_include_directories(beaglesampler_vst3 PRIVATE
    third_party/vst3_include
)

target_link_libraries(beaglesampler_vst3 PRIVATE
    ${PLUGIN_LINK_LIBRARIES}
)

target_link_options(beaglesampler_vst3 PRIVATE
    -static
)

# Milestone 7: Self-Contained Installer Executable (BeagleSampler_Setup.exe)
add_executable(beaglesampler_installer WIN32
    src/installer/main.cpp
    src/installer/installer.rc
)

set_target_properties(beaglesampler_installer PROPERTIES
    OUTPUT_NAME "BeagleSampler_Setup"
)

target_link_libraries(beaglesampler_installer PRIVATE
    comctl32
    gdiplus
    dwmapi
    shlwapi
    ole32
    uuid
    gdi32
    user32
    shell32
)

target_link_options(beaglesampler_installer PRIVATE
    -static
)
