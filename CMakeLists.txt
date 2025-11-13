# Dual-mode CMake: works standalone AND as an ESP-IDF component
# Standalone mode (no ESP-IDF): build a static library
if(NOT ESP_PLATFORM)
	cmake_minimum_required(VERSION 3.10)
	project(imgui-sw LANGUAGES CXX)

	add_library(imgui-sw STATIC sw_imgui.cpp)

	# In standalone builds, we keep parent include to find imgui headers if the repo layout expects it.
	target_include_directories(imgui-sw PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
	target_compile_features(imgui-sw PUBLIC cxx_std_11)
	return()
endif()

# ESP-IDF component mode
idf_component_register(
	SRCS "sw_imgui.cpp"
	INCLUDE_DIRS "."  # export this component's public headers
	REQUIRES imgui-core     # depends on Dear ImGui core (provided by imgui-core component)
)

# Use at least C++11 (IDF default is C++17 in recent versions; this keeps compatibility)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_11)

# Note: Dear ImGui headers/sources are expected to be provided by the application (as another component)
