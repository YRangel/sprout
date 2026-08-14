/* vkprobe.c — minimal llvmpipe xcb visible-surface probe.
 * Prints per-phase so the exact failing call is visible when it hangs.
 * Build: gcc -O2 -o /tmp/vkprobe vkprobe.c -lvulkan -lxcb
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#include <xcb/xcb.h>
#include <vulkan/vulkan_xcb.h>

static void phase(const char *s) { fprintf(stderr, "[vkprobe] %s\n", s); fflush(stderr); }

int main(void) {
    uint32_t n = 0;

    const char *iexts[8];
    int niext = 0;
    iexts[niext++] = VK_KHR_SURFACE_EXTENSION_NAME;
    iexts[niext++] = VK_KHR_XCB_SURFACE_EXTENSION_NAME;
    VkInstanceCreateInfo ic = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &(VkApplicationInfo){
            .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
            .apiVersion = VK_API_VERSION_1_0 },
        .enabledExtensionCount = (uint32_t)niext,
        .ppEnabledExtensionNames = iexts,
    };
    VkInstance inst;
    phase("vkCreateInstance");
    if (vkCreateInstance(&ic, NULL, &inst) != VK_SUCCESS) { phase("FAIL instance"); return 2; }

    VkPhysicalDevice gpu;
    phase("enumerate devices");
    n = 1;
    vkEnumeratePhysicalDevices(inst, &n, &gpu);
    phase("gpu enumerated");

    xcb_connection_t *xc = xcb_connect(NULL, NULL);
    phase("xcb_connect");
    if (xcb_connection_has_error(xc)) { phase("FAIL xcb"); return 3; }

    const xcb_setup_t *setup = xcb_get_setup(xc);
    xcb_screen_iterator_t si = xcb_setup_roots_iterator(setup);
    xcb_screen_t *screen = si.data;
    xcb_window_t w = xcb_generate_id(xc);
    xcb_create_window(xc, 0, w, screen->root, 100, 100, 320, 240,
                      1, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, 0, NULL);
    xcb_map_window(xc, w);
    xcb_flush(xc);
    phase("window created+mapped");

    VkXcbSurfaceCreateInfoKHR sc = {
        .sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR,
        .connection = xc,
        .window = w,
    };
    VkSurfaceKHR surf;
    phase("vkCreateXcbSurfaceKHR");
    VkResult r = vkCreateXcbSurfaceKHR(inst, &sc, NULL, &surf);
    fprintf(stderr, "[vkprobe] surface rc=%d\n", r);
    if (r != VK_SUCCESS) return 4;

    n = 0;
    phase("vkGetPhysicalDeviceSurfaceSupportKHR");
    VkBool32 sup = 0;
    vkGetPhysicalDeviceSurfaceSupportKHR(gpu, 0, surf, &sup);
    fprintf(stderr, "[vkprobe] surface support=%u\n", sup);

    uint32_t sf = 0;
    phase("surface formats");
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surf, &sf, NULL);
    fprintf(stderr, "[vkprobe] formats=%u\n", sf);
    VkSurfaceFormatKHR fmts[16];
    uint32_t sfc = sf < 16 ? sf : 16;
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surf, &sfc, fmts);

    uint32_t pm = 0;
    phase("present modes");
    vkGetPhysicalDeviceSurfacePresentModesKHR(gpu, surf, &pm, NULL);
    fprintf(stderr, "[vkprobe] present modes=%u\n", pm);

    VkSurfaceCapabilitiesKHR caps;
    phase("surface caps");
    r = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, surf, &caps);
    fprintf(stderr, "[vkprobe] caps rc=%d\n", r);

    float qprio = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &qprio };
    const char *exts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
        .enabledExtensionCount = 1, .ppEnabledExtensionNames = exts };
    VkDevice dev;
    phase("vkCreateDevice");
    r = vkCreateDevice(gpu, &dci, NULL, &dev);
    fprintf(stderr, "[vkprobe] device rc=%d\n", r);
    if (r != VK_SUCCESS) return 5;
    VkQueue q;
    vkGetDeviceQueue(dev, 0, 0, &q);

    VkSwapchainCreateInfoKHR swap = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = surf,
        .minImageCount = 2,
        .imageFormat = fmts[0].format,
        .imageColorSpace = fmts[0].colorSpace,
        .imageExtent = { 320, 240 },
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE,
    };
    VkSwapchainKHR chain;
    phase("vkCreateSwapchainKHR (expect the hang here if WSI-side)");
    r = vkCreateSwapchainKHR(dev, &swap, NULL, &chain);
    fprintf(stderr, "[vkprobe] swapchain rc=%d\n", r);
    if (r != VK_SUCCESS) return 6;

    n = 0;
    phase("vkGetSwapchainImagesKHR");
    vkGetSwapchainImagesKHR(dev, chain, &n, NULL);
    fprintf(stderr, "[vkprobe] images=%u\n", n);

    phase("vkAcquireNextImageKHR (timeout 2s)");
    r = vkAcquireNextImageKHR(dev, chain, 2000000000ULL, VK_NULL_HANDLE, VK_NULL_HANDLE, &n);
    fprintf(stderr, "[vkprobe] acquire rc=%d\n", r);

    VkPresentInfoKHR pi = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .swapchainCount = 1,
        .pSwapchains = &chain,
        .pImageIndices = &n };
    if (r != VK_SUCCESS) { phase("skip present; acquire failed"); return 0; }
    phase("vkQueuePresentKHR");
    r = vkQueuePresentKHR(q, &pi);
    fprintf(stderr, "[vkprobe] present rc=%d\n", r);

    phase("vkDeviceWaitIdle");
    vkDeviceWaitIdle(dev);
    phase("DONE OK");
    return 0;
}
