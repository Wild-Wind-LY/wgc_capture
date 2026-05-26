cmake_policy(VERSION 3.15)
cmake_minimum_required(VERSION 3.15)

# 设置 Windows SDK 版本
set(CMAKE_SYSTEM_VERSION "10.0.19041.0" CACHE STRING "Windows SDK version" FORCE)
set(CMAKE_INSTALL_PREFIX "${CMAKE_SOURCE_DIR}/wgc_core" CACHE PATH "Install path" FORCE)

project(wgc_capture LANGUAGES CXX)

if(NOT MSVC)
    message(FATAL_ERROR "This project only supports MSVC. Aborting.")
endif()

# 输出编译的版本信息
message(STATUS "CMake generator: ${CMAKE_GENERATOR}")
message(STATUS "CMake Visual Studio version: ${CMAKE_VS_PLATFORM_TOOLSET}")
message(STATUS "CMake Windows SDK version: ${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}")

if(CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION)
    string(REPLACE "." ";" sdk_parts ${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION})
    list(GET sdk_parts 0 SDK_MAJOR)
    list(GET sdk_parts 1 SDK_MINOR)
    list(GET sdk_parts 2 SDK_BUILD)
    
    add_definitions(-DPROJECT_SDK_MAJOR=${SDK_MAJOR})
    add_definitions(-DPROJECT_SDK_MINOR=${SDK_MINOR})
    add_definitions(-DPROJECT_SDK_BUILD=${SDK_BUILD})

    if(SDK_MAJOR GREATER 10 OR (SDK_MAJOR EQUAL 10 AND SDK_BUILD GREATER_EQUAL 26100))
        add_definitions(-DWGC_ADVANCED_API=1)
    else()
        add_definitions(-DWGC_ADVANCED_API=0)
    endif()
endif()


set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /MANIFEST:NO")


# 启用 WinRT 支持
add_definitions(-DUNICODE -D_UNICODE)
add_compile_definitions(WINAPI_FAMILY=WINAPI_FAMILY_DESKTOP_APP)
add_compile_definitions(WINRT_LEAN_AND_MEAN)
add_compile_definitions(NOMINMAX)


aux_source_directory(${CMAKE_SOURCE_DIR}/src SRC_LIST)
list(REMOVE_ITEM SRC_LIST "${CMAKE_SOURCE_DIR}/src/py_capture.cpp")
set(ALL_SRC ${SRC_LIST})
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

# 配置输出python版本
option(pybind11_build "Enable pybind11 generation" OFF)


# 自动下载 CPM.cmake
set(CPM_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CPM.cmake")
if(NOT EXISTS ${CPM_PATH})
  file(DOWNLOAD  https://github.com/cpm-cmake/CPM.cmake/releases/latest/download/CPM.cmake ${CPM_PATH})
endif()
include(${CPM_PATH})



CPMAddPackage(
  NAME Microsoft.Windows.CppWinRT
  VERSION 2.0.250303.1
  SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/cppwinrt-2.0.250303.1"
  OPTIONS 
  "BUILD_TESTING OFF"
  "CPPWINRT_BUILD_VERSION 2.0.250303.1"
  "CPPWINRT_BUILD_PROJECTION OFF"
)
message(STATUS "Searching for cppwinrt.exe in: ${Microsoft.Windows.CppWinRT_BINARY_DIR}")
file(GLOB CPPWINRT_RELEASE "${Microsoft.Windows.CppWinRT_BINARY_DIR}/Release/cppwinrt.exe")
file(GLOB CPPWINRT_DEBUG   "${Microsoft.Windows.CppWinRT_BINARY_DIR}/Debug/cppwinrt.exe")
file(GLOB_RECURSE CPPWINRT_OTHER "${Microsoft.Windows.CppWinRT_BINARY_DIR}/*/cppwinrt.exe")
if(CPPWINRT_RELEASE)
    set(CPPWINRT_EXE "${CPPWINRT_RELEASE}")
elseif(CPPWINRT_DEBUG)
    set(CPPWINRT_EXE "${CPPWINRT_DEBUG}")
elseif(CPPWINRT_OTHER)
    list(GET CPPWINRT_OTHER 0 CPPWINRT_EXE)
else()
    message(WARNING "cppwinrt.exe not found under ${Microsoft.Windows.CppWinRT_BINARY_DIR}")
    message(WARNING "You may need to build CppWinRT first, e.g.:")
    message(WARNING "  cmake --build ${Microsoft.Windows.CppWinRT_BINARY_DIR} --config Release")
    set(CPPWINRT_EXE "${Microsoft.Windows.CppWinRT_BINARY_DIR}/Release/cppwinrt.exe")
endif()
message(STATUS "Using cppwinrt.exe: ${CPPWINRT_EXE}")

# set(WINMD_PATH "D:/Windows Kits/10/UnionMetadata/10.0.26100.0/Windows.winmd") 
set(WINMD_PATH "D:/Windows Kits/10/UnionMetadata/10.0.19041.0/Windows.winmd") 
set(CPPWINRT_OUTPUT_DIR "${CMAKE_BINARY_DIR}/generated")
add_custom_command(
    OUTPUT ${CPPWINRT_OUTPUT_DIR}/winrt/base.h  # 任意一个生成后的文件作为标志
    COMMAND "${CPPWINRT_EXE}"
        -input "${WINMD_PATH}"
        -output "${CPPWINRT_OUTPUT_DIR}"
    DEPENDS "${WINMD_PATH}"
    COMMENT "Generating WinRT projection from ${WINMD_PATH}"
    VERBATIM
)
add_custom_target(generate_winrt ALL
    DEPENDS ${CPPWINRT_OUTPUT_DIR}/winrt/base.h
)


CPMAddPackage(
  NAME qoi
  SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/qoi"       
)
add_library(qoi INTERFACE)
target_include_directories(qoi INTERFACE
  $<BUILD_INTERFACE:${qoi_SOURCE_DIR}>
  $<INSTALL_INTERFACE:include/qoi>
)

# 把共享源码放进一个静态库
add_library(wgc_core STATIC ${ALL_SRC})
set_target_properties(wgc_core PROPERTIES DEBUG_POSTFIX d RELEASE_POSTFIX "")
target_link_libraries(wgc_core PRIVATE qoi windowsapp)
target_include_directories(wgc_core PUBLIC
    $<BUILD_INTERFACE:${CPPWINRT_OUTPUT_DIR}>
    $<INSTALL_INTERFACE:include/winrt>
)
target_compile_options(wgc_core PRIVATE /source-charset:utf-8 /execution-charset:utf-8)

add_dependencies(wgc_core generate_winrt)

# add_executable(MFTtest test/MFTtest.cpp)
# target_link_libraries(MFTtest PRIVATE  windowsapp)
# target_compile_definitions(MFTtest PRIVATE NOMINMAX )
# target_compile_options(MFTtest PRIVATE 
#   "$<$<CXX_COMPILER_ID:MSVC>:/source-charset:utf-8>" "$<$<CXX_COMPILER_ID:MSVC>:/execution-charset:utf-8>")
  
add_executable(wgc_capture main.cpp)
target_link_libraries(wgc_capture PRIVATE wgc_core)
target_compile_options(wgc_capture PRIVATE /source-charset:utf-8 /execution-charset:utf-8)


if(pybind11_build)
  set(PYBIND11_FINDPYTHON ON)
  set(Python_EXECUTABLE "D:/Program Files/Miniconda/Miniconda3/envs/conan/python.exe")
  set(pybind11_DIR "D:/Program Files/Miniconda/Miniconda3/envs/conan/Lib/site-packages/pybind11/share/cmake/pybind11")
  find_package(pybind11 REQUIRED)
  
  # pybind11_add_module(py_capture src/py_capture.cpp ${ALL_SRC})
  pybind11_add_module(py_capture src/py_capture.cpp) 
  # target_include_directories(py_capture PRIVATE  ${DEPS_INSTALL_DIR}/include ${CppWinRT_SOURCE_DIR}/winrt) 
  # target_link_directories(py_capture PRIVATE ${DEPS_INSTALL_DIR}/lib)
  target_link_libraries(py_capture PRIVATE wgc_core )

  target_compile_definitions(py_capture PRIVATE BUILD_PYBIND11=1)
  target_compile_options(py_capture PRIVATE /source-charset:utf-8 /execution-charset:utf-8)

  add_custom_command(TARGET py_capture POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_FILE:py_capture> ${CMAKE_CURRENT_BINARY_DIR}/py_capture.pyd
    COMMAND ${CMAKE_COMMAND} -E env
      PYTHONPATH=${CMAKE_CURRENT_BINARY_DIR}
      ${Python_EXECUTABLE} -m pybind11_stubgen py_capture
      -o ${CMAKE_CURRENT_BINARY_DIR}/stubs
    COMMENT "Generating Python stub (.pyi) for py_capture"
  )
endif()


message(STATUS "Generator: ${CMAKE_GENERATOR}")
# set(OpenCV_DIR_DEBUG   "D:/ProjectFiles/CppProject/opencv/opencv-4.11.0/Debug/x64/vc17/staticlib")
# set(OpenCV_DIR_RELEASE "D:/ProjectFiles/CppProject/opencv/opencv-4.11.0/Release/x64/vc17/staticlib")

# set(OpenCV_DIR "$<$<CONFIG:Debug>:${OpenCV_DIR_DEBUG}>$<$<NOT:$<CONFIG:Debug>>:${OpenCV_DIR_RELEASE}>")
# set(OpenCV_DIR "D:/ProjectFiles/CppProject/opencv/opencv-4.11.0/Debug/x64/vc17/staticlib")
# set(OpenCV_DIR "D:/ProjectFiles/CppProject/opencv/opencv-4.11.0/opencv-4.11.0/x64/vc17/staticlib")
# find_package(OpenCV REQUIRED)

# if(OpenCV_FOUND)
#     message(STATUS "Using system OpenCV: ${OpenCV_VERSION}")
#     message(STATUS "OpenCV include dirs: ${OpenCV_INCLUDE_DIRS}")
#     message(STATUS "OpenCV libraries: ${OpenCV_LIBS}")
# endif()

# 安装库（目标）
install(TARGETS wgc_core qoi
        EXPORT wgc_coreTargets 
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib
        RUNTIME DESTINATION bin
        INCLUDES DESTINATION include)

install(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/include/wgc_core.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lock_free_queue.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/monitor_selector.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/window_selector.hpp
    DESTINATION include)

install(DIRECTORY "${CPPWINRT_OUTPUT_DIR}/winrt" DESTINATION include)
# install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/atomic_queue DESTINATION include)

install(EXPORT wgc_coreTargets
        NAMESPACE wgc_core::
        DESTINATION lib/cmake/wgc_core)

include(CMakePackageConfigHelpers)        
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/wgc_coreConfigVersion.cmake"
    VERSION 1.0.0
    COMPATIBILITY AnyNewerVersion)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/wgc_coreConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/wgc_coreConfig.cmake"
    INSTALL_DESTINATION lib/cmake/wgc_core
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/wgc_coreConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/wgc_coreConfigVersion.cmake"
    DESTINATION lib/cmake/wgc_core
)



if(pybind11_build)
# 安装 .pyd 文件（Python 模块）
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/py_capture.pyd
        DESTINATION py_capture)
# 安装 .pyi 文件（类型声明）
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/stubs/py_capture.pyi
        DESTINATION py_capture)
endif()


