/*
 * Copyright (c) 2017-2019, 2021-2022 Arm Limited.
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

#include <array>
#include <cassert>
#include <cstdlib>
#include <cstring>

#include <vector>
#include <xcb/xcb.h>
#include <X11/Xlib-xcb.h>
#include <vulkan/vk_icd.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_xcb.h>
#include <vulkan/vulkan_xlib.h>
#include <vulkan/vulkan_core.h>

#include <layer/private_data.hpp>

#include "surface_properties.hpp"
#include "surface.hpp"
#include "util/macros.hpp"

namespace wsi
{
namespace x11
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
   , m_supported_modes({ VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_MAILBOX_KHR })
{
   UNUSED(allocator);
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
                                                      VkSurfaceCapabilitiesKHR *surface_capabilities)
{
   /* Image count limits */
   get_surface_capabilities_common(physical_device, surface_capabilities);
   surface_capabilities->minImageCount = 4;

   int depth;
   specific_surface->get_size_and_depth(&surface_capabilities->currentExtent.width,
                                        &surface_capabilities->currentExtent.height, &depth);

   /* Composite alpha */
   surface_capabilities->supportedCompositeAlpha = static_cast<VkCompositeAlphaFlagBitsKHR>(
      VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR | VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR |
      VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR);

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

std::vector<VkFormat> support_formats {
   VK_FORMAT_R8G8B8A8_UNORM,
   VK_FORMAT_B8G8R8A8_SRGB,
   VK_FORMAT_B8G8R8A8_UNORM,
   VK_FORMAT_R8G8B8A8_SRGB,
   VK_FORMAT_R5G6B5_UNORM_PACK16
};

VkResult surface_properties::get_surface_formats(VkPhysicalDevice physical_device, uint32_t *surface_format_count,
                                                 VkSurfaceFormatKHR *surface_formats,
                                                 VkSurfaceFormat2KHR *extended_surface_formats)
{
   UNUSED(physical_device);
   std::vector<surface_format_properties> formats;
   for (auto &format : support_formats)
   {
      if (format != VK_FORMAT_UNDEFINED)
      {
         formats.insert(formats.begin(), (surface_format_properties){ format });
      }
   }
   return surface_properties_formats_helper(formats.begin(), formats.end(), surface_format_count, surface_formats,
                                            extended_surface_formats);
}

VkResult surface_properties::get_surface_present_modes(VkPhysicalDevice physical_device, VkSurfaceKHR surface,
                                                       uint32_t *present_mode_count, VkPresentModeKHR *present_modes)
{
   UNUSED(physical_device);
   UNUSED(surface);
   return get_surface_present_modes_common(present_mode_count, present_modes, m_supported_modes);
}

static const char *required_device_extensions[] = {
   VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
   VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
   VK_KHR_EXTERNAL_FENCE_EXTENSION_NAME,
   VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME,
   VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
   VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
   VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
   VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
   VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
   VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
   VK_KHR_MAINTENANCE1_EXTENSION_NAME,
   VK_KHR_BIND_MEMORY_2_EXTENSION_NAME,
};

VkResult surface_properties::get_required_device_extensions(util::extension_list &extension_list)
{
   return extension_list.add(required_device_extensions,
                             sizeof(required_device_extensions) / sizeof(required_device_extensions[0]));
}

static const char *required_instance_extensions[] = {
   VK_KHR_EXTERNAL_FENCE_CAPABILITIES_EXTENSION_NAME,
   VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
   VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
};

VkResult surface_properties::get_required_instance_extensions(util::extension_list &extension_list)
{
   return extension_list.add(required_instance_extensions,
                             sizeof(required_instance_extensions) / sizeof(required_instance_extensions[0]));
}

VWL_VKAPI_CALL(VkResult)
CreateXcbSurfaceKHR(VkInstance instance, const VkXcbSurfaceCreateInfoKHR *pCreateInfo,
                    const VkAllocationCallbacks *pAllocator, VkSurfaceKHR *pSurface) VWL_API_POST
{

   auto &instance_data = layer::instance_private_data::get(instance);
   util::allocator allocator{ instance_data.get_allocator(), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT, pAllocator };

   auto wsi_surface = surface::make_surface(allocator, pCreateInfo->connection, pCreateInfo->window);
   if (wsi_surface == nullptr)
   {
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   auto surface_base = util::unique_ptr<wsi::surface>(std::move(wsi_surface));
   VkResult res = instance_data.disp.CreateXcbSurfaceKHR(instance, pCreateInfo, pAllocator, pSurface);
   if (res == VK_SUCCESS)
   {
      res = instance_data.add_surface(*pSurface, surface_base);
      if (res != VK_SUCCESS)
      {
         instance_data.disp.DestroySurfaceKHR(instance, *pSurface, pAllocator);
      }
   }
   return res;
}

VWL_VKAPI_CALL(VkResult)
GetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex, VkSurfaceKHR surface,
                                   VkBool32 *pSupported)
{
   UNUSED(physicalDevice);
   UNUSED(queueFamilyIndex);
   UNUSED(surface);
   *pSupported = VK_TRUE;
   return VK_SUCCESS;
}

static bool visual_supported(xcb_visualtype_t *visual)
{
   if (!visual)
      return false;

   return visual->_class == XCB_VISUAL_CLASS_TRUE_COLOR || visual->_class == XCB_VISUAL_CLASS_DIRECT_COLOR;
}

static xcb_visualtype_t *screen_get_visualtype(xcb_screen_t *screen, xcb_visualid_t visual_id, unsigned *depth)
{
   xcb_depth_iterator_t depth_iter = xcb_screen_allowed_depths_iterator(screen);

   for (; depth_iter.rem; xcb_depth_next(&depth_iter))
   {
      xcb_visualtype_iterator_t visual_iter = xcb_depth_visuals_iterator(depth_iter.data);

      for (; visual_iter.rem; xcb_visualtype_next(&visual_iter))
      {
         if (visual_iter.data->visual_id == visual_id)
         {
            if (depth)
               *depth = depth_iter.data->depth;
            return visual_iter.data;
         }
      }
   }

   return NULL;
}

static xcb_visualtype_t *connection_get_visualtype(xcb_connection_t *conn, xcb_visualid_t visual_id)
{
   xcb_screen_iterator_t screen_iter = xcb_setup_roots_iterator(xcb_get_setup(conn));

   /* For this we have to iterate over all of the screens which is rather
    * annoying.  Fortunately, there is probably only 1.
    */
   for (; screen_iter.rem; xcb_screen_next(&screen_iter))
   {
      xcb_visualtype_t *visual = screen_get_visualtype(screen_iter.data, visual_id, NULL);
      if (visual)
         return visual;
   }

   return NULL;
}

VWL_VKAPI_CALL(VkBool32)
GetPhysicalDeviceXcbPresentationSupportKHR(VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex,
                                           xcb_connection_t *connection, xcb_visualid_t visual_id)
{
   UNUSED(queueFamilyIndex);
   bool dev_supports_sync =
      sync_fd_fence_sync::is_supported(layer::instance_private_data::get(physicalDevice), physicalDevice);
   if (!dev_supports_sync)
   {
      return VK_FALSE;
   }

   if (!visual_supported(connection_get_visualtype(connection, visual_id)))
      return false;

   return VK_TRUE;
}

VWL_VKAPI_CALL(VkResult)
CreateXlibSurfaceKHR(VkInstance instance, const VkXlibSurfaceCreateInfoKHR *pCreateInfo,
                     const VkAllocationCallbacks *pAllocator, VkSurfaceKHR *pSurface)
{
   const VkXcbSurfaceCreateInfoKHR CreateInfo = {
      .sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR,
      .pNext = NULL,
      .flags = 0,
      .connection = XGetXCBConnection(pCreateInfo->dpy),
      .window = static_cast<xcb_window_t>(pCreateInfo->window),
   };
   return CreateXcbSurfaceKHR(instance, &CreateInfo, pAllocator, pSurface);
}

VWL_VKAPI_CALL(VkBool32)
GetPhysicalDeviceXlibPresentationSupportKHR(VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex, Display *dpy,
                                            VisualID visualID)
{
   return GetPhysicalDeviceXcbPresentationSupportKHR(physicalDevice, queueFamilyIndex, XGetXCBConnection(dpy),
                                                     visualID);
}

PFN_vkVoidFunction surface_properties::get_proc_addr(const char *name)
{
   if (strcmp(name, "vkCreateXcbSurfaceKHR") == 0)
   {
      return reinterpret_cast<PFN_vkVoidFunction>(CreateXcbSurfaceKHR);
   }
   if (strcmp(name, "vkCreateXlibSurfaceKHR") == 0)
   {
      return reinterpret_cast<PFN_vkVoidFunction>(CreateXlibSurfaceKHR);
   }
   if (strcmp(name, "vkGetPhysicalDeviceSurfaceSupportKHR") == 0)
   {
      return reinterpret_cast<PFN_vkVoidFunction>(GetPhysicalDeviceSurfaceSupportKHR);
   }
   if (strcmp(name, "vkGetPhysicalDeviceXcbPresentationSupportKHR") == 0)
   {
      return reinterpret_cast<PFN_vkVoidFunction>(GetPhysicalDeviceXcbPresentationSupportKHR);
   }
   if (strcmp(name, "vkGetPhysicalDeviceXlibPresentationSupportKHR") == 0)
   {
      return reinterpret_cast<PFN_vkVoidFunction>(GetPhysicalDeviceXlibPresentationSupportKHR);
   }
   return nullptr;
}

bool surface_properties::is_surface_extension_enabled(const layer::instance_private_data &instance_data)
{
   return instance_data.is_instance_extension_enabled(VK_KHR_XCB_SURFACE_EXTENSION_NAME) ||
          instance_data.is_instance_extension_enabled(VK_KHR_XLIB_SURFACE_EXTENSION_NAME);
}

void surface_properties::get_surface_present_scaling_and_gravity(
   VkSurfacePresentScalingCapabilitiesEXT *scaling_capabilities)
{
   scaling_capabilities->supportedPresentScaling = 0;
   scaling_capabilities->supportedPresentGravityX = 0;
   scaling_capabilities->supportedPresentGravityY = 0;
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
   present_timing_surface_caps->presentAtAbsoluteTimeSupported = VK_TRUE;
   present_timing_surface_caps->presentAtRelativeTimeSupported = VK_TRUE;
   present_timing_surface_caps->presentStageQueries =
      VK_PRESENT_STAGE_QUEUE_OPERATIONS_END_BIT_EXT | VK_PRESENT_STAGE_IMAGE_LATCHED_BIT_EXT |
      VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT | VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_VISIBLE_BIT_EXT;
   present_timing_surface_caps->presentStageTargets = VK_PRESENT_STAGE_IMAGE_LATCHED_BIT_EXT |
                                                      VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT |
                                                      VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_VISIBLE_BIT_EXT;
}
#endif

} /* namespace x11 */
} /* namespace wsi */
