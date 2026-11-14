// Graphics API choice: Direct3D 12 or Vulkan.

#include "graphics_api.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/gpu_plugin.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

REXCVAR_DEFINE_STRING(skate2_graphics_api, "auto", "Skate2",
                      "Graphics API: auto (Vulkan under Proton / Wine, Direct3D 12 on Windows), "
                      "d3d12 or vulkan")
    .allowed({"auto", "d3d12", "vulkan"})
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_STRING(skate2_perf_csv, "", "Skate2",
                      "Debug: write the per-frame counter CSV to this file (settable from "
                      "skate2.toml, unlike perf_log_csv); empty disables");

REXCVAR_DEFINE_STRING(skate2_dump_image, "", "Skate2",
                      "Debug: write the loaded guest image to this file; empty disables");

namespace {

std::string_view g_active_api = "d3d12";

bool RunningUnderWine() {
#if defined(_WIN32)
  const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  return ntdll && GetProcAddress(ntdll, "wine_get_version");
#else
  return false;
#endif
}

// Just enough of the Vulkan API to ask the system loader for a GPU, so the exe
// needs no Vulkan headers or import library.
namespace vk {
using Result = int32_t;
using Instance = void*;
using PhysicalDevice = void*;
using VoidFunction = void (*)();
using GetInstanceProcAddr = VoidFunction (*)(Instance, const char*);

struct ApplicationInfo {
  int32_t type = 0;  // VK_STRUCTURE_TYPE_APPLICATION_INFO
  const void* next = nullptr;
  const char* application_name = nullptr;
  uint32_t application_version = 0;
  const char* engine_name = nullptr;
  uint32_t engine_version = 0;
  uint32_t api_version = (1u << 22) | (1u << 12);  // 1.1
};

struct InstanceCreateInfo {
  int32_t type = 1;  // VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO
  const void* next = nullptr;
  uint32_t flags = 0;
  const ApplicationInfo* application_info = nullptr;
  uint32_t layer_count = 0;
  const char* const* layers = nullptr;
  uint32_t extension_count = 0;
  const char* const* extensions = nullptr;
};

using CreateInstance = Result (*)(const InstanceCreateInfo*, const void*, Instance*);
using DestroyInstance = void (*)(Instance, const void*);
using EnumeratePhysicalDevices = Result (*)(Instance, uint32_t*, PhysicalDevice*);
// VkPhysicalDeviceProperties is 824 bytes; only its stable prefix is read.
using GetPhysicalDeviceProperties = void (*)(PhysicalDevice, void*);
constexpr size_t kPropertiesSize = 824;
constexpr size_t kDeviceTypeOffset = 16;
constexpr size_t kDeviceNameOffset = 20;
constexpr int32_t kDeviceTypeCpu = 4;
}  // namespace vk

// Names of the Vulkan GPUs (software rasterizers left out).
std::vector<std::string> VulkanGpus() {
  std::vector<std::string> gpus;
#if defined(_WIN32)
  const HMODULE loader = LoadLibraryW(L"vulkan-1.dll");
  if (!loader) return gpus;
  const auto get_proc =
      reinterpret_cast<vk::GetInstanceProcAddr>(GetProcAddress(loader, "vkGetInstanceProcAddr"));
  const auto create =
      get_proc ? reinterpret_cast<vk::CreateInstance>(get_proc(nullptr, "vkCreateInstance"))
               : nullptr;
  vk::Instance instance = nullptr;
  vk::ApplicationInfo app;
  app.application_name = "skate2";
  vk::InstanceCreateInfo info;
  info.application_info = &app;
  if (!create || create(&info, nullptr, &instance) != 0 || !instance) {
    FreeLibrary(loader);
    return gpus;
  }
  const auto destroy =
      reinterpret_cast<vk::DestroyInstance>(get_proc(instance, "vkDestroyInstance"));
  const auto enumerate = reinterpret_cast<vk::EnumeratePhysicalDevices>(
      get_proc(instance, "vkEnumeratePhysicalDevices"));
  const auto properties = reinterpret_cast<vk::GetPhysicalDeviceProperties>(
      get_proc(instance, "vkGetPhysicalDeviceProperties"));
  uint32_t count = 0;
  if (enumerate && properties && enumerate(instance, &count, nullptr) == 0 && count) {
    std::vector<vk::PhysicalDevice> handles(count);
    if (enumerate(instance, &count, handles.data()) >= 0) {
      for (uint32_t i = 0; i < count; ++i) {
        alignas(8) unsigned char props[vk::kPropertiesSize] = {};
        properties(handles[i], props);
        int32_t type = 0;
        std::memcpy(&type, props + vk::kDeviceTypeOffset, sizeof(type));
        if (type != vk::kDeviceTypeCpu) {
          gpus.emplace_back(reinterpret_cast<const char*>(props + vk::kDeviceNameOffset));
        }
      }
    }
  }
  if (destroy) destroy(instance, nullptr);
  FreeLibrary(loader);
#endif
  return gpus;
}

}  // namespace

std::unique_ptr<rex::system::IGraphicsSystem> LoadSkate2GraphicsPlugin(std::string_view plugin) {
  const std::string requested = REXCVAR_GET(skate2_graphics_api);
  std::string api = requested;
  if (api == "auto") api = RunningUnderWine() ? "vulkan" : "d3d12";
  std::unique_ptr<rex::system::IGraphicsSystem> graphics;
  if (api == "vulkan") {
    const auto gpus = VulkanGpus();
    if (gpus.empty()) {
      REXLOG_WARN("Graphics: no Vulkan driver or GPU found, using Direct3D 12");
    } else {
      REXLOG_INFO("Graphics: {} Vulkan GPU(s), first: {}", gpus.size(), gpus.front());
      graphics = rex::system::LoadGpuPlugin(plugin, "vulkan");
      if (!graphics) REXLOG_WARN("Graphics: the GPU plugin has no Vulkan backend, using Direct3D 12");
    }
  }
  if (graphics) {
    g_active_api = "vulkan";
  } else {
    graphics = rex::system::LoadGpuPlugin(plugin, "d3d12");
    g_active_api = "d3d12";
  }
  if (graphics) REXLOG_INFO("Graphics: {} ({} requested)", g_active_api, requested);
  return graphics;
}

std::string_view Skate2GraphicsApi() {
  return g_active_api;
}
