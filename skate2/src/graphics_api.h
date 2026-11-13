// Graphics API choice: Direct3D 12 or Vulkan.

#pragma once

#include <rex/system/interfaces/graphics.h>

#include <memory>
#include <string_view>

// Loads the GPU plugin with the API selected by skate2_graphics_api:
// auto (Vulkan under Proton / Wine, Direct3D 12 on Windows), d3d12 or vulkan.
// Vulkan falls back to Direct3D 12 when no Vulkan GPU or backend is present.
std::unique_ptr<rex::system::IGraphicsSystem> LoadSkate2GraphicsPlugin(std::string_view plugin);

// "d3d12" or "vulkan" once the plugin is loaded.
std::string_view Skate2GraphicsApi();
