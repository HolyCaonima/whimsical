cmake_minimum_required(VERSION 3.24)
project(Whimsical LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/EHsc>)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
# Multi-config generators take per-configuration flags from the cache, which a stale cache
# or a dependency's CACHE FORCE can leave empty. An unoptimized "Release" silently
# multiplies CPU frame cost, so pin the flags whenever optimization is missing.
if(MSVC)
  foreach(LANG C CXX)
    if(NOT CMAKE_${LANG}_FLAGS_RELEASE MATCHES "/O2")
      set(CMAKE_${LANG}_FLAGS_RELEASE "/O2 /Ob2 /DNDEBUG" CACHE STRING "" FORCE)
    endif()
    if(NOT CMAKE_${LANG}_FLAGS_RELWITHDEBINFO MATCHES "/O2")
      set(CMAKE_${LANG}_FLAGS_RELWITHDEBINFO "/O2 /Ob1 /DNDEBUG /Zi" CACHE STRING "" FORCE)
    endif()
  endforeach()
  if(NOT CMAKE_EXE_LINKER_FLAGS_RELEASE MATCHES "OPT:REF")
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE "/INCREMENTAL:NO /OPT:REF /OPT:ICF" CACHE STRING "" FORCE)
  endif()
endif()
set(DEPS "${CMAKE_SOURCE_DIR}/third_party")
if(NOT EXISTS "${DEPS}/nrd/Include/NRD.h" OR NOT EXISTS "${DEPS}/rtxdi/Include/Rtxdi/DI/Reservoir.hlsli")
  message(FATAL_ERROR "Run node tools/bootstrap.mjs first")
endif()
if(NOT WIN32)
  message(FATAL_ERROR "This initial platform layer targets Windows x64")
endif()
add_library(volk STATIC "${DEPS}/volk/volk.c")
target_include_directories(volk PUBLIC "${DEPS}/volk" "${DEPS}/vulkan/include")
target_compile_definitions(volk PUBLIC VK_USE_PLATFORM_WIN32_KHR WIN32_LEAN_AND_MEAN NOMINMAX VK_NO_PROTOTYPES)
add_library(duktape STATIC "${DEPS}/duktape/src/duktape.c")
target_include_directories(duktape PUBLIC "${DEPS}/duktape/src")
target_compile_options(duktape PRIVATE /utf-8)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
add_subdirectory("${DEPS}/freetype" "third_party/freetype")
add_library(Freetype::Freetype ALIAS freetype)
target_compile_options(freetype PRIVATE /utf-8)
set(RMLUI_SAMPLES OFF CACHE BOOL "" FORCE)
set(RMLUI_FONT_ENGINE freetype CACHE STRING "" FORCE)
add_subdirectory("${DEPS}/rmlui" "third_party/rmlui")
add_library(whimsical_uicore engine/uiCore/UiCore.cpp)
target_include_directories(whimsical_uicore PUBLIC engine)
target_compile_definitions(whimsical_uicore PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(whimsical_uicore PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(whimsical_uicore PUBLIC RmlUi::Core whimsical_assets PRIVATE windowscodecs ole32)
set(SHADERMAKE_FIND_COMPILERS OFF CACHE BOOL "" FORCE)
set(SHADERMAKE_TOOL OFF CACHE BOOL "" FORCE)
set(SHADERMAKE_DXC_VK_PATH "${DEPS}/dxc/bin/x64/dxc.exe" CACHE FILEPATH "" FORCE)
set(NRD_STATIC_LIBRARY ON CACHE BOOL "" FORCE)
set(NRD_EMBEDS_DXBC_SHADERS OFF CACHE BOOL "" FORCE)
set(NRD_EMBEDS_DXIL_SHADERS OFF CACHE BOOL "" FORCE)
set(NRD_NORMAL_ENCODING 4 CACHE STRING "" FORCE)
set(FETCHCONTENT_SOURCE_DIR_MATHLIB "${DEPS}/mathlib" CACHE PATH "" FORCE)
set(FETCHCONTENT_SOURCE_DIR_SHADERMAKE "${DEPS}/shadermake" CACHE PATH "" FORCE)
add_subdirectory("${DEPS}/nrd" "third_party/nrd")
if(NOT EXISTS "${DEPS}/onnxruntime/include/onnxruntime_cxx_api.h")
  message(FATAL_ERROR "Run node tools/bootstrap.mjs to install ONNX Runtime")
endif()
add_library(onnxruntime SHARED IMPORTED)
set_target_properties(onnxruntime PROPERTIES
  IMPORTED_IMPLIB "${DEPS}/onnxruntime/lib/onnxruntime.lib"
  IMPORTED_LOCATION "${DEPS}/onnxruntime/lib/onnxruntime.dll"
  INTERFACE_INCLUDE_DIRECTORIES "${DEPS}/onnxruntime/include")
add_library(whimsical_assets
  engine/assets/Json.cpp engine/assets/Asset.cpp engine/assets/Project.cpp engine/assets/ContentMounts.cpp engine/assets/AssetManager.cpp)
target_include_directories(whimsical_assets PUBLIC engine)
target_compile_definitions(whimsical_assets PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(whimsical_assets PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(whimsical_assets PRIVATE duktape)
add_library(whimsical_animation
  engine/animation/SkinnedMesh.cpp
  engine/animation/Animation.cpp engine/animation/ai4animation/Controller.cpp
  engine/animation/ai4animation/ControllerAsset.cpp engine/animation/ai4animation/OnnxModel.cpp)
target_include_directories(whimsical_animation PUBLIC engine "${DEPS}/glm")
target_compile_definitions(whimsical_animation PUBLIC GLM_FORCE_RADIANS GLM_FORCE_DEPTH_ZERO_TO_ONE
  GLM_FORCE_CTOR_INIT GLM_ENABLE_EXPERIMENTAL NOMINMAX)
target_compile_options(whimsical_animation PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(whimsical_animation PUBLIC whimsical_assets PRIVATE onnxruntime)
function(whimsical_copy_onnx TARGET)
  add_custom_command(TARGET ${TARGET} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:onnxruntime>" "$<TARGET_FILE_DIR:${TARGET}>"
    VERBATIM)
endfunction()
add_library(whimsical_core
  engine/assets/RenderTargetAsset.cpp engine/core/RenderTarget.cpp
  engine/core/ConsoleRegistry.cpp engine/core/EngineSettings.cpp engine/core/GpuProfile.cpp engine/core/CpuProfile.cpp engine/ui/Console.cpp
  engine/assets/StaticMesh.cpp
  engine/assets/ShaderAsset.cpp engine/assets/Material.cpp
  engine/assets/EngineAssets.cpp engine/scene/SceneAsset.cpp engine/scene/ScenePersistence.cpp
  engine/ui/EngineUi.cpp engine/scripting/UiBindings.cpp
  engine/scripting/RuntimeHost.cpp engine/scripting/RuntimeBindings.cpp
  engine/core/World.cpp engine/core/RenderScene.cpp engine/navigation/Navigation.cpp engine/scripting/ScriptRuntime.cpp
  engine/ecs/ComponentCatalog.cpp engine/ecs/BuiltinComponents.cpp
  engine/ecs/TransformSystem.cpp engine/ecs/MotionSystem.cpp engine/ecs/AnimationSystem.cpp engine/ecs/RenderSystem.cpp
  engine/physics/PhysicsScene.cpp engine/physics/DynamicAabbTree.cpp engine/animation/AnimationCollision.cpp
)
target_include_directories(whimsical_core PUBLIC engine "${DEPS}/glm")
target_compile_definitions(whimsical_core PUBLIC GLM_FORCE_RADIANS GLM_FORCE_DEPTH_ZERO_TO_ONE GLM_FORCE_CTOR_INIT
  NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS WHIMSICAL_ROOT="${CMAKE_SOURCE_DIR}")
target_link_libraries(whimsical_core PUBLIC duktape whimsical_animation whimsical_uicore)
add_library(whimsical_shader_runtime engine/render/ShaderCompiler.cpp engine/render/MaterialBindings.cpp)
target_link_libraries(whimsical_shader_runtime PUBLIC whimsical_core)
target_compile_options(whimsical_shader_runtime PRIVATE /W4 /permissive- /utf-8)
target_compile_definitions(whimsical_shader_runtime PRIVATE
  WHIMSICAL_RTXDI_INCLUDES="${DEPS}/rtxdi/Include"
  WHIMSICAL_SHADER_SOURCES="${CMAKE_SOURCE_DIR}/engine/render/shaders"
  WHIMSICAL_GLSLANG="${DEPS}/glslang/bin/glslang.exe"
  WHIMSICAL_SHADERS="${CMAKE_BINARY_DIR}/shaders")
# The registry is the single source of every binding number. This tool emits the GLSL the
# shaders include, so the descriptor layout and the shader declarations are one statement.
add_executable(shader_bindings engine/render/graph/ShaderBindings.cpp engine/render/graph/Registry.cpp
  engine/render/RenderResources.cpp)
target_link_libraries(shader_bindings PRIVATE whimsical_core)
target_include_directories(shader_bindings PRIVATE "${DEPS}/rtxdi/Include")
target_compile_options(shader_bindings PRIVATE /W4 /permissive- /utf-8)
set(GENERATED_GLSL
  "${CMAKE_BINARY_DIR}/shaders/generated/graph.shared.glsl"
  "${CMAKE_BINARY_DIR}/shaders/generated/graph.compute.glsl"
  "${CMAKE_BINARY_DIR}/shaders/generated/graph.rtxdi.glsl")
add_custom_command(OUTPUT ${GENERATED_GLSL}
  COMMAND shader_bindings "${CMAKE_BINARY_DIR}/shaders/generated"
  DEPENDS shader_bindings VERBATIM)
add_custom_target(whimsical_shader_bindings DEPENDS ${GENERATED_GLSL})
add_executable(Whimsical engine/main.cpp engine/platform/Window.cpp engine/render/Renderer.cpp engine/render/VulkanContext.cpp engine/render/NrdDenoiser.cpp engine/render/GpuProfiler.cpp engine/render/UiRenderer.cpp)
target_sources(Whimsical PRIVATE engine/render/RenderAuditWorker.cpp engine/render/GpuScene.cpp
  engine/render/GpuRenderTargets.cpp
  engine/render/RenderPipeline.cpp engine/render/RenderResources.cpp
  engine/render/graph/Registry.cpp engine/render/graph/ResourcePool.cpp engine/render/graph/RenderGraph.cpp
  engine/render/graph/ShaderAccess.cpp)
target_link_libraries(Whimsical PRIVATE whimsical_shader_runtime volk NRD user32 gdi32 winmm comdlg32)
target_include_directories(Whimsical PRIVATE "${DEPS}/rtxdi/Include")
whimsical_copy_onnx(Whimsical)
target_compile_options(Whimsical PRIVATE /W4 /permissive- /utf-8)
target_compile_options(whimsical_core PRIVATE /W4 /permissive- /utf-8)
set(SHADER_FILES gbuffer.vert composite.comp di_confidence.comp di_gradient_filter.comp ui.vert ui.frag overlay.frag overlay_id.frag)
set(SPIRV)
foreach(SHADER IN LISTS SHADER_FILES)
  set(OUTPUT "${CMAKE_BINARY_DIR}/shaders/${SHADER}.spv")
  add_custom_command(OUTPUT "${OUTPUT}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/shaders"
    COMMAND "${DEPS}/glslang/bin/glslang.exe" --target-env vulkan1.2 -V "${CMAKE_SOURCE_DIR}/engine/render/shaders/${SHADER}" -I${CMAKE_SOURCE_DIR}/engine/render/shaders -I${CMAKE_BINARY_DIR}/shaders -o "${OUTPUT}"
    DEPENDS "${CMAKE_SOURCE_DIR}/engine/render/shaders/${SHADER}" "${CMAKE_SOURCE_DIR}/engine/render/shaders/common.glsl"
      "${CMAKE_SOURCE_DIR}/engine/lighting/LightSampling.h" ${GENERATED_GLSL} VERBATIM)
  list(APPEND SPIRV "${OUTPUT}")
endforeach()
add_custom_target(whimsical_shaders DEPENDS ${SPIRV})
add_dependencies(whimsical_shaders whimsical_shader_bindings)
add_dependencies(Whimsical whimsical_shaders)
target_compile_definitions(Whimsical PRIVATE WHIMSICAL_SHADERS="${CMAKE_BINARY_DIR}/shaders")
enable_testing()
add_executable(console_tests tests/ConsoleTests.cpp)
target_link_libraries(console_tests PRIVATE whimsical_core)
target_compile_options(console_tests PRIVATE /W4 /permissive- /utf-8)
whimsical_copy_onnx(console_tests)
add_test(NAME console_system COMMAND console_tests)
add_executable(content_mount_tests tests/ContentMountTests.cpp)
target_link_libraries(content_mount_tests PRIVATE whimsical_core)
target_compile_options(content_mount_tests PRIVATE /W4 /permissive- /utf-8)
whimsical_copy_onnx(content_mount_tests)
add_test(NAME content_mounts COMMAND content_mount_tests)
set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT Whimsical)
set_target_properties(Whimsical PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
