cmake_minimum_required(VERSION 3.24)
project(Afterlight LANGUAGES C CXX)
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
add_library(uiCore engine/uiCore/UiCore.cpp)
target_include_directories(uiCore PUBLIC engine)
target_compile_definitions(uiCore PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN AFTERLIGHT_ROOT="${CMAKE_SOURCE_DIR}")
target_compile_options(uiCore PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(uiCore PUBLIC RmlUi::Core PRIVATE windowscodecs ole32)
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
add_library(afterlight_assets
  engine/assets/Json.cpp engine/assets/Asset.cpp engine/assets/Project.cpp engine/assets/AssetManager.cpp)
target_include_directories(afterlight_assets PUBLIC engine)
target_compile_definitions(afterlight_assets PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(afterlight_assets PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(afterlight_assets PRIVATE duktape)
add_library(afterlight_animation
  engine/animation/SkinnedMesh.cpp
  engine/animation/Animation.cpp engine/animation/ai4animation/Controller.cpp
  engine/animation/ai4animation/ControllerAsset.cpp engine/animation/ai4animation/OnnxModel.cpp)
target_include_directories(afterlight_animation PUBLIC engine "${DEPS}/glm")
target_compile_definitions(afterlight_animation PUBLIC GLM_FORCE_RADIANS GLM_FORCE_DEPTH_ZERO_TO_ONE
  GLM_FORCE_CTOR_INIT GLM_ENABLE_EXPERIMENTAL NOMINMAX)
target_compile_options(afterlight_animation PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(afterlight_animation PUBLIC afterlight_assets PRIVATE onnxruntime)
function(afterlight_copy_onnx TARGET)
  add_custom_command(TARGET ${TARGET} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:onnxruntime>" "$<TARGET_FILE_DIR:${TARGET}>"
    VERBATIM)
endfunction()
add_library(afterlight_core
  engine/core/ConsoleRegistry.cpp engine/core/EngineSettings.cpp engine/core/GpuProfile.cpp engine/core/CpuProfile.cpp engine/ui/Console.cpp
  engine/assets/StaticMesh.cpp
  engine/assets/ShaderAsset.cpp engine/assets/Material.cpp
  engine/assets/EngineAssets.cpp engine/scene/SceneAsset.cpp engine/scene/ScenePersistence.cpp
  engine/ui/EngineUi.cpp engine/scripting/UiBindings.cpp
  engine/core/World.cpp engine/core/RenderScene.cpp engine/navigation/Navigation.cpp engine/scripting/ScriptRuntime.cpp
  engine/ecs/ComponentCatalog.cpp engine/ecs/BuiltinComponents.cpp
  engine/ecs/TransformSystem.cpp engine/ecs/MotionSystem.cpp engine/ecs/AnimationSystem.cpp engine/ecs/RenderSystem.cpp
  engine/physics/PhysicsScene.cpp engine/physics/DynamicAabbTree.cpp engine/animation/AnimationCollision.cpp
)
target_include_directories(afterlight_core PUBLIC engine "${DEPS}/glm")
target_compile_definitions(afterlight_core PUBLIC GLM_FORCE_RADIANS GLM_FORCE_DEPTH_ZERO_TO_ONE GLM_FORCE_CTOR_INIT
  NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS AFTERLIGHT_ROOT="${CMAKE_SOURCE_DIR}")
target_link_libraries(afterlight_core PUBLIC duktape afterlight_animation uiCore)
add_library(afterlight_shader_runtime engine/render/ShaderCompiler.cpp engine/render/MaterialBindings.cpp)
target_link_libraries(afterlight_shader_runtime PUBLIC afterlight_core)
target_compile_options(afterlight_shader_runtime PRIVATE /W4 /permissive- /utf-8)
target_compile_definitions(afterlight_shader_runtime PRIVATE
  AFTERLIGHT_RTXDI_INCLUDES="${DEPS}/rtxdi/Include"
  AFTERLIGHT_SHADER_SOURCES="${CMAKE_SOURCE_DIR}/engine/render/shaders"
  AFTERLIGHT_GLSLANG="${DEPS}/glslang/bin/glslang.exe"
  AFTERLIGHT_SHADERS="${CMAKE_BINARY_DIR}/shaders")
# The registry is the single source of every binding number. This tool emits the GLSL the
# shaders include, so the descriptor layout and the shader declarations are one statement.
add_executable(shader_bindings engine/render/graph/ShaderBindings.cpp engine/render/graph/Registry.cpp
  engine/render/RenderResources.cpp)
target_link_libraries(shader_bindings PRIVATE afterlight_core)
target_include_directories(shader_bindings PRIVATE "${DEPS}/rtxdi/Include")
target_compile_options(shader_bindings PRIVATE /W4 /permissive- /utf-8)
set(GENERATED_GLSL
  "${CMAKE_BINARY_DIR}/shaders/generated/graph.shared.glsl"
  "${CMAKE_BINARY_DIR}/shaders/generated/graph.compute.glsl"
  "${CMAKE_BINARY_DIR}/shaders/generated/graph.rtxdi.glsl")
add_custom_command(OUTPUT ${GENERATED_GLSL}
  COMMAND shader_bindings "${CMAKE_BINARY_DIR}/shaders/generated"
  DEPENDS shader_bindings VERBATIM)
add_custom_target(afterlight_shader_bindings DEPENDS ${GENERATED_GLSL})
add_executable(Afterlight engine/main.cpp engine/platform/Window.cpp engine/render/Renderer.cpp engine/render/VulkanContext.cpp engine/render/NrdDenoiser.cpp engine/render/GpuProfiler.cpp engine/render/UiRenderer.cpp)
target_sources(Afterlight PRIVATE engine/render/RenderAuditWorker.cpp engine/render/GpuScene.cpp
  engine/render/RenderPipeline.cpp engine/render/RenderResources.cpp
  engine/render/graph/Registry.cpp engine/render/graph/ResourcePool.cpp engine/render/graph/RenderGraph.cpp
  engine/render/graph/ShaderAccess.cpp)
target_link_libraries(Afterlight PRIVATE afterlight_shader_runtime volk NRD user32 gdi32 winmm)
target_include_directories(Afterlight PRIVATE "${DEPS}/rtxdi/Include")
afterlight_copy_onnx(Afterlight)
target_compile_options(Afterlight PRIVATE /W4 /permissive- /utf-8)
target_compile_options(afterlight_core PRIVATE /W4 /permissive- /utf-8)
set(SHADER_FILES gbuffer.vert composite.comp di_confidence.comp di_gradient_filter.comp ui.vert ui.frag)
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
add_custom_target(afterlight_shaders DEPENDS ${SPIRV})
add_dependencies(afterlight_shaders afterlight_shader_bindings)
add_dependencies(Afterlight afterlight_shaders)
target_compile_definitions(Afterlight PRIVATE AFTERLIGHT_SHADERS="${CMAKE_BINARY_DIR}/shaders")
enable_testing()
add_executable(light_tests tests/LightTests.cpp)
target_link_libraries(light_tests PRIVATE afterlight_core)
target_compile_options(light_tests PRIVATE /W4 /permissive- /utf-8)
afterlight_copy_onnx(light_tests)
add_test(NAME physical_lights COMMAND light_tests)
add_executable(ecs_tests tests/EcsTests.cpp)
target_link_libraries(ecs_tests PRIVATE afterlight_core)
target_compile_options(ecs_tests PRIVATE /W4 /permissive- /utf-8)
afterlight_copy_onnx(ecs_tests)
add_test(NAME ecs_lifecycle COMMAND ecs_tests)
add_executable(render_audit_tests tests/RenderAuditTests.cpp engine/render/RenderAuditWorker.cpp)
target_link_libraries(render_audit_tests PRIVATE afterlight_core)
target_compile_options(render_audit_tests PRIVATE /W4 /permissive- /utf-8)
afterlight_copy_onnx(render_audit_tests)
add_test(NAME render_audit COMMAND render_audit_tests)
# Compiling a frame is a pure function of the declarations, so the resource contract —
# dependencies, culling, validation, liveness and storage reuse — is checked without a GPU.
add_executable(render_graph_tests tests/RenderGraphTests.cpp
  engine/render/graph/RenderGraph.cpp engine/render/graph/Registry.cpp
  engine/render/graph/ResourcePool.cpp engine/render/VulkanContext.cpp engine/render/GpuProfiler.cpp)
target_link_libraries(render_graph_tests PRIVATE afterlight_core volk user32 gdi32)
target_compile_options(render_graph_tests PRIVATE /W4 /permissive- /utf-8)
afterlight_copy_onnx(render_graph_tests)
add_test(NAME render_graph_contract COMMAND render_graph_tests)
# A pass's resource declarations are read out of the SPIR-V it will run, so this checks the
# real linked modules against the registry the graph compiles.
add_executable(shader_tests tests/ShaderTests.cpp engine/render/graph/ShaderAccess.cpp
  engine/render/graph/Registry.cpp engine/render/RenderResources.cpp)
target_link_libraries(shader_tests PRIVATE afterlight_shader_runtime volk)
target_include_directories(shader_tests PRIVATE "${DEPS}/rtxdi/Include")
add_dependencies(shader_tests afterlight_shader_bindings)
target_compile_options(shader_tests PRIVATE /W4 /permissive- /utf-8)
afterlight_copy_onnx(shader_tests)
add_test(NAME shader_assets_and_compilation COMMAND shader_tests)
add_executable(ui_tests tests/UiTests.cpp)
target_link_libraries(ui_tests PRIVATE afterlight_core)
target_compile_options(ui_tests PRIVATE /W4 /permissive- /utf-8)
afterlight_copy_onnx(ui_tests)
add_test(NAME ui_core_js COMMAND ui_tests)
add_executable(console_tests tests/ConsoleTests.cpp)
target_link_libraries(console_tests PRIVATE afterlight_core)
target_compile_options(console_tests PRIVATE /W4 /permissive- /utf-8)
afterlight_copy_onnx(console_tests)
add_test(NAME console_system COMMAND console_tests)
add_executable(asset_scene_tests tests/AssetSceneTests.cpp)
target_link_libraries(asset_scene_tests PRIVATE afterlight_core)
target_compile_options(asset_scene_tests PRIVATE /W4 /permissive- /utf-8)
afterlight_copy_onnx(asset_scene_tests)
add_test(NAME project_assets_scene COMMAND asset_scene_tests)
add_executable(afterlight_tests tests/CoreTests.cpp)
target_link_libraries(afterlight_tests PRIVATE afterlight_core)
afterlight_copy_onnx(afterlight_tests)
add_test(NAME core_gameplay COMMAND afterlight_tests)
add_executable(physics_tests tests/PhysicsTests.cpp)
target_link_libraries(physics_tests PRIVATE afterlight_core)
afterlight_copy_onnx(physics_tests)
target_compile_options(physics_tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME physics_scene COMMAND physics_tests)
add_executable(kinematic_tests tests/KinematicTests.cpp)
target_link_libraries(kinematic_tests PRIVATE afterlight_core)
afterlight_copy_onnx(kinematic_tests)
target_compile_options(kinematic_tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME kinematic_shapes_transforms COMMAND kinematic_tests)
add_executable(physics_bvh_tests tests/PhysicsBvhTests.cpp)
target_link_libraries(physics_bvh_tests PRIVATE afterlight_core)
afterlight_copy_onnx(physics_bvh_tests)
target_compile_options(physics_bvh_tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME physics_bvh COMMAND physics_bvh_tests)
add_executable(gpu_profile_tests tests/GpuProfileTests.cpp)
target_link_libraries(gpu_profile_tests PRIVATE afterlight_core)
afterlight_copy_onnx(gpu_profile_tests)
target_compile_options(gpu_profile_tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME gpu_profile COMMAND gpu_profile_tests)
add_executable(cpu_profile_tests tests/CpuProfileTests.cpp)
target_link_libraries(cpu_profile_tests PRIVATE afterlight_core)
afterlight_copy_onnx(cpu_profile_tests)
target_compile_options(cpu_profile_tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME cpu_profile COMMAND cpu_profile_tests)
add_executable(animation_tests tests/AnimationTests.cpp)
target_link_libraries(animation_tests PRIVATE afterlight_core)
target_compile_options(animation_tests PRIVATE /W4 /permissive- /utf-8)
afterlight_copy_onnx(animation_tests)
add_test(NAME animation_runtime COMMAND animation_tests)
add_executable(companion_tests tests/CompanionTests.cpp)
target_link_libraries(companion_tests PRIVATE afterlight_core)
target_compile_options(companion_tests PRIVATE /W4 /permissive- /utf-8)
afterlight_copy_onnx(companion_tests)
add_test(NAME companion_follow COMMAND companion_tests)
add_executable(animation_attribute_tests tests/AnimationAttributeTests.cpp)
target_link_libraries(animation_attribute_tests PRIVATE afterlight_core)
target_compile_options(animation_attribute_tests PRIVATE /W4 /permissive- /utf-8)
afterlight_copy_onnx(animation_attribute_tests)
add_test(NAME animation_attributes COMMAND animation_attribute_tests)
set_tests_properties(core_gameplay PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT Afterlight)
set_target_properties(Afterlight PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
