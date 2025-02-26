/*
 * Copyright (c) 2017-2019, 2021-2025 Arm Limited.
 *
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#define VK_USE_PLATFORM_WAYLAND_KHR 1

#ifndef __STDC_VERSION__
#define __STDC_VERSION__ 0
#endif

#include <wayland-client.h>
#include <linux-dmabuf-unstable-v1-client-protocol.h>

#include <cassert>
#include <cstdlib>
#include <algorithm>
#include <array>
#include <cstring>
#include "surface_properties.hpp"
#include "surface.hpp"
#include "layer/private_data.hpp"
#include "wl_helpers.hpp"
#include "wl_object_owner.hpp"
#include "util/drm/drm_utils.hpp"
#include "util/log.hpp"
#include "util/macros.hpp"
#include "util/helpers.hpp"

namespace wsi
{
namespace wayland
{

void surface_properties::populate_present_mode_compatibilities()
{
   std::array<present_mode_compatibility, 2> compatible_present_modes_list = {
      present_mode_compatibility{ VK_PRESENT_MODE_FIFO_KHR, 1, { VK_PRESENT_MODE_FIFO_KHR } },
      present_mode_compatibility{ VK_PRESENT_MODE_MAILBOX_KHR, 1, { VK_PRESENT_MODE_MAILBOX_KHR } }
   };
   m_compatible_present_modes = compatible_present_modes<2>(compatible_present_modes_list);
}

surface_properties::surface_properties(surface *wsi_surface, const util::allocator &allocator)
   : specific_surface(wsi_surface)
   , supported_formats(allocator)
   , m_supported_modes({ VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_MAILBOX_KHR })
{
   populate_present_mode_compatibilities();
}

surface_properties::surface_properties()
   : surface_properties(nullptr, util::allocator::get_generic())
{
}

surface_properties &surface_properties::get_instance()
{
   static surface_properties instance;
   return instance;
}

VkResult surface_properties::get_surface_capabilities(VkPhysicalDevice physical_device,
                                                      VkSurfaceCapabilitiesKHR *pSurfaceCapabilities)
{

   /* Image count limits */
   get_surface_capabilities_common(physical_device, pSurfaceCapabilities);
   pSurfaceCapabilities->minImageCount = 2;

   /* Composite alpha */
   pSurfaceCapabilities->supportedCompositeAlpha = static_cast<VkCompositeAlphaFlagBitsKHR>(
      VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR | VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR | VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR);
   return VK_SUCCESS;
}

VkResult surface_properties::get_surface_capabilities(VkPhysicalDevice physical_device,
                                                      const VkPhysicalDeviceSurfaceInfo2KHR *pSurfaceInfo,
                                                      VkSurfaceCapabilities2KHR *pSurfaceCapabilities)
{
   TRY(check_surface_present_mode_query_is_supported(pSurfaceInfo, m_supported_modes));

   /* Image count limits */
   get_surface_capabilities(physical_device, &pSurfaceCapabilities->surfaceCapabilities);

   m_compatible_present_modes.get_surface_present_mode_compatibility_common(pSurfaceInfo, pSurfaceCapabilities);

   auto surface_scaling_capabilities = util::find_extension<VkSurfacePresentScalingCapabilitiesEXT>(
      VK_STRUCTURE_TYPE_SURFACE_PRESENT_SCALING_CAPABILITIES_EXT, pSurfaceCapabilities);
   if (surface_scaling_capabilities != nullptr)
   {
      get_surface_present_scaling_and_gravity(surface_scaling_capabilities);
      surface_scaling_capabilities->minScaledImageExtent = pSurfaceCapabilities->surfaceCapabilities.minImageExtent;
      surface_scaling_capabilities->maxScaledImageExtent = pSurfaceCapabilities->surfaceCapabilities.maxImageExtent;
   }

   return VK_SUCCESS;
}

static VkResult surface_format_properties_add_modifier_support(VkPhysicalDevice phys_dev,
                                                               surface_format_properties &format_props,
                                                               const drm_format_pair &drm_format,
                                                               bool add_compression = false)
{
   VkPhysicalDeviceExternalImageFormatInfoKHR external_info = {};
   external_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO_KHR;
   external_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

   VkPhysicalDeviceImageDrmFormatModifierInfoEXT drm_mod_info = {};
   drm_mod_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT;
   drm_mod_info.pNext = &external_info;
   drm_mod_info.drmFormatModifier = drm_format.modifier;
   drm_mod_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

   VkPhysicalDeviceImageFormatInfo2KHR image_info = {};
   image_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2_KHR;
   image_info.pNext = &drm_mod_info;
   image_info.format = format_props.m_surface_format.format;
   image_info.type = VK_IMAGE_TYPE_2D;
   image_info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
   image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

   if (add_compression)
   {
      return format_props.add_device_compression_support(phys_dev, image_info);
   }

   return format_props.check_device_support(phys_dev, image_info);
}

static VkResult surface_format_properties_map_add(VkPhysicalDevice phys_dev, surface_format_properties_map &format_map,
                                                  VkFormat format, const drm_format_pair &drm_format)
{
   surface_format_properties format_props{ format };
   VkResult res = surface_format_properties_add_modifier_support(phys_dev, format_props, drm_format);
   if (res == VK_SUCCESS)
   {
      auto it = format_map.try_insert(std::make_pair(format, format_props));
      if (!it.has_value())
      {
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      }
   }

   if (res == VK_ERROR_FORMAT_NOT_SUPPORTED)
   {
      return VK_SUCCESS;
   }

   return res;
}

static VkResult surface_format_properties_map_init(VkPhysicalDevice phys_dev, surface_format_properties_map &format_map,
                                                   const util::vector<drm_format_pair> &drm_format_list)
{
   for (const auto &drm_format : drm_format_list)
   {
      const VkFormat vk_format = util::drm::drm_to_vk_format(drm_format.fourcc);
      if (vk_format != VK_FORMAT_UNDEFINED && format_map.find(vk_format) == format_map.end())
      {
         TRY_LOG_CALL(surface_format_properties_map_add(phys_dev, format_map, vk_format, drm_format));
      }
      const VkFormat srgb_vk_format = util::drm::drm_to_vk_srgb_format(drm_format.fourcc);
      if (srgb_vk_format != VK_FORMAT_UNDEFINED && format_map.find(srgb_vk_format) == format_map.end())
      {
         TRY_LOG_CALL(surface_format_properties_map_add(phys_dev, format_map, srgb_vk_format, drm_format));
      }
   }

   return VK_SUCCESS;
}

static VkResult surface_format_properties_map_add_compression(VkPhysicalDevice phys_dev,
                                                              surface_format_properties_map &format_map,
                                                              const util::vector<drm_format_pair> &drm_format_list)
{
   for (const auto &drm_format : drm_format_list)
   {
      const VkFormat vk_format = util::drm::drm_to_vk_format(drm_format.fourcc);
      if (vk_format != VK_FORMAT_UNDEFINED)
      {
         auto entry = format_map.find(vk_format);
         if (entry != format_map.end())
         {
            TRY_LOG_CALL(surface_format_properties_add_modifier_support(phys_dev, entry->second, drm_format, true));
         }
      }
      const VkFormat srgb_vk_format = util::drm::drm_to_vk_srgb_format(drm_format.fourcc);
      if (srgb_vk_format != VK_FORMAT_UNDEFINED)
      {
         auto entry = format_map.find(srgb_vk_format);
         if (entry != format_map.end())
         {
            TRY_LOG_CALL(surface_format_properties_add_modifier_support(phys_dev, entry->second, drm_format, true));
         }
      }
   }
   return VK_SUCCESS;
}

VkResult surface_properties::get_surface_formats(VkPhysicalDevice physical_device, uint32_t *surfaceFormatCount,
                                                 VkSurfaceFormatKHR *surfaceFormats,
                                                 VkSurfaceFormat2KHR *extended_surface_formats)
{
   assert(specific_surface);
   if (!supported_formats.size())
   {
      TRY_LOG_CALL(
         surface_format_properties_map_init(physical_device, supported_formats, specific_surface->get_formats()));
      if (layer::instance_private_data::get(physical_device).has_image_compression_support(physical_device))
      {
         TRY_LOG_CALL(surface_format_properties_map_add_compression(physical_device, supported_formats,
                                                                    specific_surface->get_formats()));
      }
   }

   return surface_properties_formats_helper(supported_formats.begin(), supported_formats.end(), surfaceFormatCount,
                                            surfaceFormats, extended_surface_formats);
}

VkResult surface_properties::get_surface_present_modes(VkPhysicalDevice physical_device, VkSurfaceKHR surface,
                                                       uint32_t *pPresentModeCount, VkPresentModeKHR *pPresentModes)
{
   UNUSED(physical_device);
   UNUSED(surface);

   return get_surface_present_modes_common(pPresentModeCount, pPresentModes, m_supported_modes);
}

VkResult surface_properties::get_required_device_extensions(util::extension_list &extension_list)
{
   const std::array required_device_extensions{
      VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
      VK_KHR_BIND_MEMORY_2_EXTENSION_NAME,
      VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME,
      VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
      VK_KHR_MAINTENANCE1_EXTENSION_NAME,
      VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
      VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
      VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
      VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
      VK_KHR_EXTERNAL_FENCE_EXTENSION_NAME,
      VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME,
   };
   return extension_list.add(required_device_extensions.data(), required_device_extensions.size());
}

VkResult surface_properties::get_required_instance_extensions(util::extension_list &extension_list)
{
   const std::array required_instance_extensions{
      VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
      VK_KHR_EXTERNAL_FENCE_CAPABILITIES_EXTENSION_NAME,
      VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME,
      VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
   };
   return extension_list.add(required_instance_extensions.data(), required_instance_extensions.size());
}

struct required_properties
{
   bool dmabuf;
   bool explicit_sync;
};

VWL_CAPI_CALL(void)
check_required_protocols(void *data, struct wl_registry *registry, uint32_t name, const char *interface,
                         uint32_t version) VWL_API_POST
{
   UNUSED(registry);
   UNUSED(name);
   auto supported = static_cast<required_properties *>(data);

   if (!strcmp(interface, zwp_linux_dmabuf_v1_interface.name) && version >= ZWP_LINUX_DMABUF_V1_MODIFIER_SINCE_VERSION)
   {
      supported->dmabuf = true;
   }
   else if (!strcmp(interface, zwp_linux_explicit_synchronization_v1_interface.name))
   {
      supported->explicit_sync = true;
   }
}

static const wl_registry_listener registry_listener = { check_required_protocols, nullptr };

static bool check_wl_protocols(struct wl_display *display)
{
   required_properties supported = {};

   auto protocol_queue = wayland_owner<wl_event_queue>{ wl_display_create_queue(display) };
   if (protocol_queue.get() == nullptr)
   {
      WSI_LOG_ERROR("Failed to create wl surface queue.");
      return false;
   }
   auto display_proxy = make_proxy_with_queue(display, protocol_queue.get());
   if (display_proxy == nullptr)
   {
      WSI_LOG_ERROR("Failed to create wl display proxy.");
      return false;
   };
   auto registry = wayland_owner<wl_registry>{ wl_display_get_registry(display_proxy.get()) };
   if (registry.get() == nullptr)
   {
      WSI_LOG_ERROR("Failed to get wl display registry.");
      return false;
   }

   int res = wl_registry_add_listener(registry.get(), &registry_listener, &supported);
   if (res < 0)
   {
      WSI_LOG_ERROR("Failed to add registry listener.");
      return false;
   }

   res = wl_display_roundtrip_queue(display, protocol_queue.get());
   if (res < 0)
   {
      WSI_LOG_ERROR("Roundtrip failed.");
      return false;
   }

   return (supported.dmabuf /* && supported.explicit_sync */);
}

VWL_VKAPI_CALL(VkBool32)
GetPhysicalDeviceWaylandPresentationSupportKHR(VkPhysicalDevice physical_device, uint32_t queue_index,
                                               struct wl_display *display)
{
   UNUSED(queue_index);
   bool dev_supports_sync =
      sync_fd_fence_sync::is_supported(layer::instance_private_data::get(physical_device), physical_device);
   if (!dev_supports_sync)
   {
      return VK_FALSE;
   }

   if (!check_wl_protocols(display))
   {
      return VK_FALSE;
   }

   return VK_TRUE;
}

VWL_VKAPI_CALL(VkResult)
CreateWaylandSurfaceKHR(VkInstance instance, const VkWaylandSurfaceCreateInfoKHR *pCreateInfo,
                        const VkAllocationCallbacks *pAllocator, VkSurfaceKHR *pSurface) VWL_API_POST
{
   auto &instance_data = layer::instance_private_data::get(instance);
   util::allocator allocator{ instance_data.get_allocator(), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT, pAllocator };
   auto wsi_surface = surface::make_surface(allocator, pCreateInfo->display, pCreateInfo->surface);
   if (wsi_surface == nullptr)
   {
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   VkResult res = instance_data.disp.CreateWaylandSurfaceKHR(instance, pCreateInfo, pAllocator, pSurface);
   if (res == VK_SUCCESS)
   {
      auto surface_base = util::unique_ptr<wsi::surface>(std::move(wsi_surface));
      res = instance_data.add_surface(*pSurface, surface_base);
      if (res != VK_SUCCESS)
      {
         instance_data.disp.DestroySurfaceKHR(instance, *pSurface, pAllocator);
      }
   }
   return res;
}

PFN_vkVoidFunction surface_properties::get_proc_addr(const char *name)
{
   if (strcmp(name, "vkGetPhysicalDeviceWaylandPresentationSupportKHR") == 0)
   {
      return reinterpret_cast<PFN_vkVoidFunction>(GetPhysicalDeviceWaylandPresentationSupportKHR);
   }
   else if (strcmp(name, "vkCreateWaylandSurfaceKHR") == 0)
   {
      return reinterpret_cast<PFN_vkVoidFunction>(CreateWaylandSurfaceKHR);
   }
   return nullptr;
}

bool surface_properties::is_surface_extension_enabled(const layer::instance_private_data &instance_data)
{
   return instance_data.is_instance_extension_enabled(VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME);
}

void surface_properties::get_surface_present_scaling_and_gravity(
   VkSurfacePresentScalingCapabilitiesEXT *scaling_capabilities)
{
   scaling_capabilities->supportedPresentScaling = VK_PRESENT_SCALING_ONE_TO_ONE_BIT_EXT;
   scaling_capabilities->supportedPresentGravityX = VK_PRESENT_GRAVITY_MIN_BIT_EXT;
   scaling_capabilities->supportedPresentGravityY = VK_PRESENT_GRAVITY_MIN_BIT_EXT;
}

bool surface_properties::is_compatible_present_modes(VkPresentModeKHR present_mode_a, VkPresentModeKHR present_mode_b)
{
   return m_compatible_present_modes.is_compatible_present_modes(present_mode_a, present_mode_b);
}

#if VULKAN_WSI_LAYER_EXPERIMENTAL
void surface_properties::get_present_timing_surface_caps(
   VkPresentTimingSurfaceCapabilitiesEXT *present_timing_surface_caps)
{
   present_timing_surface_caps->presentTimingSupported = VK_TRUE;
   present_timing_surface_caps->presentAtAbsoluteTimeSupported = VK_FALSE;
   present_timing_surface_caps->presentAtRelativeTimeSupported = VK_FALSE;
   present_timing_surface_caps->presentStageQueries = VK_PRESENT_STAGE_QUEUE_OPERATIONS_END_BIT_EXT;
   present_timing_surface_caps->presentStageTargets = 0;
}
#endif

} // namespace wayland
} // namespace wsi
