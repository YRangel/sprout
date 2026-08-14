/* vkprobe2.c — full render+present loop probe (clear color per frame).
 * Builds on vkprobe success path; adds renderpass/fbo/cmdbuf/submit/fence.
 * Prints phase + frame counter; freeze point surfaces immediately.
 * Build: gcc -O2 -o /root/vkprobe2 vkprobe2.c -lvulkan -lxcb
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#include <xcb/xcb.h>
#include <vulkan/vulkan_xcb.h>

static void phase(const char *s) { fprintf(stderr, "[vkprobe2] %s\n", s); fflush(stderr); }
#define CHECK(r, failstr) do { if ((r) != VK_SUCCESS) { \
    fprintf(stderr, "[vkprobe2] FAIL %s rc=%d\n", failstr, r); exit(9); } } while (0)

int main(void) {
    VkInstance inst;
    const char *iexts[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_XCB_SURFACE_EXTENSION_NAME };
    VkInstanceCreateInfo ic = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &(VkApplicationInfo){
            .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
            .apiVersion = VK_API_VERSION_1_0 },
        .enabledExtensionCount = 2,
        .ppEnabledExtensionNames = iexts,
    };
    phase("vkCreateInstance");
    CHECK(vkCreateInstance(&ic, NULL, &inst), "instance");

    VkPhysicalDevice gpu;
    uint32_t n = 1;
    CHECK(vkEnumeratePhysicalDevices(inst, &n, &gpu), "enum");

    xcb_connection_t *xc = xcb_connect(NULL, NULL);
    const xcb_setup_t *setup = xcb_get_setup(xc);
    xcb_window_t w = xcb_generate_id(xc);
    xcb_screen_t *screen = xcb_setup_roots_iterator(setup).data;
    xcb_create_window(xc, 0, w, screen->root, 100, 100, 320, 240,
                      1, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, 0, NULL);
    xcb_map_window(xc, w);
    xcb_flush(xc);
    VkSurfaceKHR surf;
    VkXcbSurfaceCreateInfoKHR s = {
        .sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR,
        .connection = xc, .window = w };
    phase("vkCreateXcbSurfaceKHR");
    CHECK(vkCreateXcbSurfaceKHR(inst, &s, NULL, &surf), "surface");

    uint32_t nfmt = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surf, &nfmt, NULL);
    VkSurfaceFormatKHR fmt[8];
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surf, &nfmt, fmt);
    VkSurfaceCapabilitiesKHR caps;
    CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, surf, &caps), "caps");

    float qp = 1.f;
    const char *dexts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDevice dev;
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &(VkDeviceQueueCreateInfo){
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &qp },
        .enabledExtensionCount = 1,
        .ppEnabledExtensionNames = dexts };
    phase("vkCreateDevice");
    CHECK(vkCreateDevice(gpu, &dci, NULL, &dev), "device");
    VkQueue q;
    vkGetDeviceQueue(dev, 0, 0, &q);

    VkSwapchainKHR chain;
    VkSwapchainCreateInfoKHR sw = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = surf,
        .minImageCount = 2,
        .imageFormat = fmt[0].format,
        .imageColorSpace = fmt[0].colorSpace,
        .imageExtent = { 320, 240 },
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE };
    phase("vkCreateSwapchainKHR");
    CHECK(vkCreateSwapchainKHR(dev, &sw, NULL, &chain), "swapchain");

    VkImage imgs[8];
    uint32_t nimg = 8;
    vkGetSwapchainImagesKHR(dev, chain, &nimg, imgs);

    VkRenderPass rp;
    VkRenderPassCreateInfo rpci = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &(VkAttachmentDescription){
            .format = fmt[0].format,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR },
        .subpassCount = 1,
        .pSubpasses = &(VkSubpassDescription){
            .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
            .colorAttachmentCount = 1,
            .pColorAttachments = &(VkAttachmentReference){
                .attachment = 0,
                .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL } } };
    phase("vkCreateRenderPass");
    CHECK(vkCreateRenderPass(dev, &rpci, NULL, &rp), "renderpass");

    VkFramebuffer fbos[8];
    for (uint32_t i = 0; i < nimg; i++) {
        VkImageView view;
        VkImageViewCreateInfo vci = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = imgs[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = fmt[0].format,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        CHECK(vkCreateImageView(dev, &vci, NULL, &view), "imageview");
        VkFramebufferCreateInfo fci = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = rp,
            .attachmentCount = 1,
            .pAttachments = &view,
            .width = 320, .height = 240, .layers = 1 };
        CHECK(vkCreateFramebuffer(dev, &fci, NULL, &fbos[i]), "fbo");
    }

    VkCommandPool pool;
    VkCommandPoolCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = 0 };
    phase("vkCreateCommandPool");
    CHECK(vkCreateCommandPool(dev, &pci, NULL, &pool), "pool");

    VkCommandBuffer cmd;
    VkCommandBufferAllocateInfo cai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1 };
    CHECK(vkAllocateCommandBuffers(dev, &cai, &cmd), "cmdbuf");

    VkFence fence;
    VkFenceCreateInfo fenc = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    phase("vkCreateFence");
    CHECK(vkCreateFence(dev, &fenc, NULL, &fence), "fence");

    VkSemaphore sem_img, sem_rdy;
    VkSemaphoreCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    CHECK(vkCreateSemaphore(dev, &sci, NULL, &sem_img), "sem");
    CHECK(vkCreateSemaphore(dev, &sci, NULL, &sem_rdy), "sem");

    for (int frame = 0; frame < 100; frame++) {
        uint32_t idx = 0;
        if (frame < 4) { fprintf(stderr, "[vkprobe2] f%d pre-acquire\n", frame); }
        VkResult r = vkAcquireNextImageKHR(dev, chain, 3000000000ULL,
                                           sem_img, VK_NULL_HANDLE, &idx);
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
            fprintf(stderr, "[vkprobe2] acquire f%d rc=%d (VK_TIMEOUT=1?%d)\n", frame, r, r==VK_TIMEOUT);
            break;
        }
        if (frame < 4) { fprintf(stderr, "[vkprobe2] f%d acquired idx=%u\n", frame, idx); }

        CHECK(vkResetCommandBuffer(cmd, 0), "reset");
        VkCommandBufferBeginInfo bi = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        CHECK(vkBeginCommandBuffer(cmd, &bi), "begin");
        VkClearValue clr = {{{ (frame % 256) / 255.f, 0.2f, 0.3f, 1.f }}};
        VkRenderPassBeginInfo rpbi = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = rp,
            .framebuffer = fbos[idx],
            .renderArea = {{ 0, 0 }, { 320, 240 }},
            .clearValueCount = 1,
            .pClearValues = &clr };
        vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdEndRenderPass(cmd);
        CHECK(vkEndCommandBuffer(cmd), "end");

        CHECK(vkResetFences(dev, 1, &fence), "resetfence");
        VkPipelineStageFlags stage =
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &sem_img,
            .pWaitDstStageMask = &stage,
            .commandBufferCount = 1,
            .pCommandBuffers = &cmd,
            .signalSemaphoreCount = 1,
            .pSignalSemaphores = &sem_rdy };
        if (frame < 4) { fprintf(stderr, "[vkprobe2] f%d pre-submit\n", frame); }
        CHECK(vkQueueSubmit(q, 1, &si, fence), "submit");
        if (frame < 4) { fprintf(stderr, "[vkprobe2] f%d pre-waitfence\n", frame); }
        CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 4000000000ULL), "fence-wait");
        if (frame < 4) { fprintf(stderr, "[vkprobe2] f%d fence done\n", frame); }

        VkPresentInfoKHR pi = {
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &sem_rdy,
            .swapchainCount = 1,
            .pSwapchains = &chain,
            .pImageIndices = &idx };
        if (frame == 0) phase("vkQueuePresentKHR(first)");
        VkResult pr = vkQueuePresentKHR(q, &pi);
        if (pr != VK_SUCCESS && pr != VK_SUBOPTIMAL_KHR) {
            fprintf(stderr, "[vkprobe2] present f%d rc=%d\n", frame, pr);
            break;
        }
        if (frame % 25 == 0) { fprintf(stderr, "[vkprobe2] frame %d ok\n", frame);}
    }
    phase("vkDeviceWaitIdle");
    vkDeviceWaitIdle(dev);
    phase("DONE OK");
    return 0;
}
