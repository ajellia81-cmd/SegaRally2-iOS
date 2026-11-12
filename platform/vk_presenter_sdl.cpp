// iOS port of vk_presenter.cpp: an SDL3 window instead of a Win32 one. On
// Apple platforms the Vulkan surface is a CAMetalLayer (MoltenVK, linked
// statically); elsewhere (the Linux build that checks the port compiles) SDL
// makes the surface. Keys, touches, the mouse and the game controllers'
// arrivals come in as SDL events and go where the Win32 messages went.
#include "vk_presenter.h"

#include <SDL3/SDL.h>
#ifdef __APPLE__
#include <SDL3/SDL_metal.h>
#else
#include <SDL3/SDL_vulkan.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "dlss.h"

#include <atomic>
extern std::atomic<int> g_sr2_debug_view;   // r3d_renderer.cpp
#include "sdl_events.h"

#ifdef __APPLE__
#include <dlfcn.h>
#endif

namespace {

double seconds() { return double(SDL_GetTicksNS()) * 1e-9; }

bool check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) {
        fprintf(stderr, "[vulkan] %s failed (%d)\n", what, int(r));
        return false;
    }
    return true;
}

bool has_extension(const std::vector<VkExtensionProperties>& avail, const char* name) {
    for (auto& a : avail)
        if (!strcmp(a.extensionName, name)) return true;
    return false;
}

// Game controller arrivals and departures, for Gamepads::poll (gamepad.cpp)
std::mutex g_device_mutex;
std::deque<SDL_Event> g_device_events;

// SDL key -> Windows virtual-key code (a keyboard on an iPad or a Mac)
int to_vk(SDL_Keycode k) {
    if (k >= 'a' && k <= 'z') return int(k - 'a' + 'A');
    if (k >= '0' && k <= '9') return int(k);
    switch (k) {
    case SDLK_LEFT: return VK_LEFT;
    case SDLK_RIGHT: return VK_RIGHT;
    case SDLK_UP: return VK_UP;
    case SDLK_DOWN: return VK_DOWN;
    case SDLK_RETURN: case SDLK_KP_ENTER: return VK_RETURN;
    case SDLK_ESCAPE: return VK_ESCAPE;
    case SDLK_SPACE: return VK_SPACE;
    case SDLK_BACKSPACE: return VK_BACK;
    case SDLK_TAB: return VK_TAB;
    case SDLK_PAGEUP: return VK_PRIOR;
    case SDLK_PAGEDOWN: return VK_NEXT;
    case SDLK_HOME: return VK_HOME;
    case SDLK_END: return VK_END;
    case SDLK_DELETE: return VK_DELETE;
    case SDLK_LSHIFT: case SDLK_RSHIFT: return VK_SHIFT;
    case SDLK_LCTRL: case SDLK_RCTRL: return VK_CONTROL;
    case SDLK_F1: return VK_F1;
    case SDLK_F2: return VK_F2;
    case SDLK_F3: return VK_F3;
    case SDLK_F9: return VK_F9;
    case SDLK_F11: return VK_F11;
    case SDLK_F12: return VK_F12;
    default: return 0;
    }
}

}  // namespace

#ifndef __APPLE__
std::atomic<bool> g_sr2_backgrounded{false};
#endif

bool sr2_take_device_event(SDL_Event* e) {
    std::lock_guard<std::mutex> lock(g_device_mutex);
    if (g_device_events.empty()) return false;
    *e = g_device_events.front();
    g_device_events.pop_front();
    return true;
}

void vk_image_barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                      VkImageAspectFlags aspect, uint32_t layers) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, 0, 1, 0, layers};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &b);
}

uint32_t VkPresenter::find_memory(uint32_t type_bits, VkMemoryPropertyFlags flags) const {
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(gpu_, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; i++)
        if ((type_bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & flags) == flags) return i;
    return UINT32_MAX;
}

LRESULT CALLBACK VkPresenter::wndproc(HWND, UINT, WPARAM, LPARAM) { return 0; }   // (Win32 only)

void VkPresenter::set_cursor_visible(bool visible) {
    if (visible == show_cursor_) return;
    show_cursor_ = visible;
    if (visible) SDL_ShowCursor();
    else SDL_HideCursor();
}

// The window always fills the screen here
void VkPresenter::toggle_fullscreen() {
    fullscreen_ = true;
    resized_ = true;
}

void VkPresenter::update_size() {
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window_, &w, &h);
    pixel_w_ = w;
    pixel_h_ = h;
    density_ = SDL_GetWindowPixelDensity(window_);
    if (density_ <= 0) density_ = 1;
    SDL_Rect safe{};
    int pw = 0, ph = 0;
    SDL_GetWindowSize(window_, &pw, &ph);
    if (SDL_GetWindowSafeArea(window_, &safe)) {
        safe_.left = int(std::lround(float(safe.x) * density_));
        safe_.top = int(std::lround(float(safe.y) * density_));
        safe_.right = int(std::lround(float(pw - safe.x - safe.w) * density_));
        safe_.bottom = int(std::lround(float(ph - safe.y - safe.h) * density_));
    } else {
        safe_ = {};
    }
    compat_set_window(hwnd_, pixel_w_, pixel_h_);
}

bool VkPresenter::init(const char* title, int width, int height, bool with_ngx) {
    (void)with_ngx;   // no NVIDIA NGX here
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");   // touches arrive as touches only
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    SDL_SetHint(SDL_HINT_IOS_HIDE_HOME_INDICATOR, "2");   // dimmed, and a first swipe does not leave the game
    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        fprintf(stderr, "[video] SDL could not start: %s\n", SDL_GetError());
        return false;
    }
#ifdef __APPLE__
    SDL_WindowFlags flags = SDL_WINDOW_FULLSCREEN | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_METAL |
                            SDL_WINDOW_BORDERLESS;
    window_ = SDL_CreateWindow(title, 0, 0, flags);
    fullscreen_ = true;
#else
    window_ = SDL_CreateWindow(title, width, height, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
#endif
    if (!window_) {
        fprintf(stderr, "[video] no window: %s\n", SDL_GetError());
        return false;
    }
    hwnd_ = reinterpret_cast<HWND>(window_);
    // iOS: the app going to the background is told at once (an event watch,
    // not the queue): GPU work sent after that loses the device for good
    SDL_AddEventWatch(
        [](void*, SDL_Event* e) -> bool {
            if (e->type == SDL_EVENT_WILL_ENTER_BACKGROUND || e->type == SDL_EVENT_DID_ENTER_BACKGROUND ||
                e->type == SDL_EVENT_WINDOW_FOCUS_LOST) {
                compat_set_focus(false);
                g_sr2_backgrounded = true;
            }
            else if (e->type == SDL_EVENT_DID_ENTER_FOREGROUND || e->type == SDL_EVENT_WINDOW_FOCUS_GAINED)
                compat_set_focus(true);
            return true;
        },
        nullptr);
#ifdef __APPLE__
    platform_watch_lifecycle();
#endif
    update_size();
    printf("[video] window %dx%d pixels (x%.2f), safe area insets L%d T%d R%d B%d\n", pixel_w_, pixel_h_, density_,
           safe_.left, safe_.top, safe_.right, safe_.bottom);

    std::vector<std::string> inst_names = {VK_KHR_SURFACE_EXTENSION_NAME};
    VkInstanceCreateFlags inst_flags = 0;
#ifdef __APPLE__
    // MoltenVK: Metal argument buffers carry the texture pack's descriptor array
    setenv("MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS", "1", 0);
    // Metal's fast math assumes no NaN and no infinity; the shaders' guards
    // against them were dropped and some models (the cars of the selection
    // screens, the countdown) came out black
    setenv("MVK_CONFIG_FAST_MATH_ENABLED", "0", 0);
    // Images and buffers in Metal heaps, made resident as a whole: the texture
    // pack's pictures, reached only through the argument buffer, were not
    // always resident and read as black (the cars' paint, the countdown)
    setenv("MVK_CONFIG_USE_MTLHEAP", "1", 0);
    {
        // MoltenVK is a framework inside the app (Frameworks/), loaded here: linked
        // statically, its vkXxx functions took the place of volk's pointers of the same names
        const char* base = SDL_GetBasePath();
        std::string path = std::string(base ? base : "") + "Frameworks/MoltenVK.framework/MoltenVK";
        void* lib = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!lib) lib = dlopen("MoltenVK.framework/MoltenVK", RTLD_NOW | RTLD_LOCAL);
        auto gipa = lib ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr")) : nullptr;
        if (!gipa) {
            fprintf(stderr, "[vulkan] could not load %s: %s\n", path.c_str(), dlerror());
            return false;
        }
        volkInitializeCustom(gipa);
    }
    inst_names.push_back(VK_EXT_METAL_SURFACE_EXTENSION_NAME);
#else
    if (!check(volkInitialize(), "volkInitialize (is the Vulkan driver missing?)")) return false;
    Uint32 sdl_count = 0;
    const char* const* sdl_ext = SDL_Vulkan_GetInstanceExtensions(&sdl_count);
    for (Uint32 i = 0; i < sdl_count; i++)
        if (std::find(inst_names.begin(), inst_names.end(), sdl_ext[i]) == inst_names.end()) inst_names.push_back(sdl_ext[i]);
#endif
    {
        uint32_t count = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> avail(count);
        vkEnumerateInstanceExtensionProperties(nullptr, &count, avail.data());
        if (has_extension(avail, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
            inst_names.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
            inst_flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        }
    }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Sega Rally 2 Native";
    app.apiVersion = VK_API_VERSION_1_3;
    std::vector<const char*> inst_ext;
    for (auto& e : inst_names) inst_ext.push_back(e.c_str());
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.flags = inst_flags;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = uint32_t(inst_ext.size());
    ici.ppEnabledExtensionNames = inst_ext.data();
    if (!check(vkCreateInstance(&ici, nullptr, &instance_), "vkCreateInstance")) return false;
    volkLoadInstance(instance_);

#ifdef __APPLE__
    metal_view_ = SDL_Metal_CreateView(window_);
    if (!metal_view_) {
        fprintf(stderr, "[video] no Metal view: %s\n", SDL_GetError());
        return false;
    }
    VkMetalSurfaceCreateInfoEXT sci{VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT};
    sci.pLayer = static_cast<const CAMetalLayer*>(SDL_Metal_GetLayer(metal_view_));
    if (!check(vkCreateMetalSurfaceEXT(instance_, &sci, nullptr, &surface_), "vkCreateMetalSurfaceEXT")) return false;
#else
    if (!SDL_Vulkan_CreateSurface(window_, instance_, nullptr, &surface_)) {
        fprintf(stderr, "[vulkan] no surface: %s\n", SDL_GetError());
        return false;
    }
#endif

    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance_, &n, nullptr);
    std::vector<VkPhysicalDevice> gpus(n);
    vkEnumeratePhysicalDevices(instance_, &n, gpus.data());
    int best_score = -1;
    uint32_t api = 0;
    for (auto g : gpus) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(g, &p);
        if (p.apiVersion < VK_API_VERSION_1_3) {
            fprintf(stderr, "[vulkan] %s: Vulkan %u.%u, 1.3 needed\n", p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion),
                    VK_API_VERSION_MINOR(p.apiVersion));
            continue;
        }
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(g, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qs(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(g, &qn, qs.data());
        for (uint32_t q = 0; q < qn; q++) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(g, q, surface_, &present);
            if (!(qs[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !present) continue;
            int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
            if (score > best_score) {
                best_score = score;
                gpu_ = g;
                queue_family_ = q;
                device_name_ = p.deviceName;
                api = p.apiVersion;
            }
            break;
        }
    }
    if (!gpu_) {
        fprintf(stderr, "[vulkan] no GPU with Vulkan 1.3 that can present to the window\n");
        return false;
    }
    printf("[vulkan] %s, Vulkan %u.%u.%u\n", device_name_.c_str(), VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api),
           VK_API_VERSION_PATCH(api));

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queue_family_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    std::vector<VkExtensionProperties> dev_avail;
    {
        uint32_t count = 0;
        vkEnumerateDeviceExtensionProperties(gpu_, nullptr, &count, nullptr);
        dev_avail.resize(count);
        vkEnumerateDeviceExtensionProperties(gpu_, nullptr, &count, dev_avail.data());
    }
    std::vector<std::string> dev_names = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    // A portability implementation (MoltenVK) must have this one enabled
    if (has_extension(dev_avail, "VK_KHR_portability_subset")) dev_names.push_back("VK_KHR_portability_subset");
    // Present ids and waits: MoltenVK has none, but take them where they are
    VkPhysicalDevicePresentWaitFeaturesKHR fwait{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR};
    VkPhysicalDevicePresentIdFeaturesKHR fid{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR};
    fid.pNext = &fwait;
    if (has_extension(dev_avail, VK_KHR_PRESENT_ID_EXTENSION_NAME) &&
        has_extension(dev_avail, VK_KHR_PRESENT_WAIT_EXTENSION_NAME)) {
        VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        query.pNext = &fid;
        vkGetPhysicalDeviceFeatures2(gpu_, &query);
        present_wait_ = fid.presentId && fwait.presentWait;
        if (present_wait_) {
            dev_names.push_back(VK_KHR_PRESENT_ID_EXTENSION_NAME);
            dev_names.push_back(VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
        }
    }
    ray_query_ = false;   // ray traced effects need ray queries, which MoltenVK does not offer

    // Ask for the features the renderer uses, only where the GPU has them
    VkPhysicalDeviceVulkan12Features have12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features have13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    have13.pNext = &have12;
    VkPhysicalDeviceFeatures2 have{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    have.pNext = &have13;
    vkGetPhysicalDeviceFeatures2(gpu_, &have);
    printf("[vulkan] features: dynamic rendering %d, partially bound %d, update unused %d, non-uniform images %d, "
           "anisotropy %d\n",
           have13.dynamicRendering, have12.descriptorBindingPartiallyBound,
           have12.descriptorBindingUpdateUnusedWhilePending, have12.shaderSampledImageArrayNonUniformIndexing,
           have.features.samplerAnisotropy);
    if (!have13.dynamicRendering) {
        fprintf(stderr, "[vulkan] the GPU has no dynamic rendering\n");
        return false;
    }
    std::vector<const char*> dev_ext;
    for (auto& e : dev_names) dev_ext.push_back(e.c_str());
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.descriptorBindingPartiallyBound = have12.descriptorBindingPartiallyBound;
    f12.descriptorBindingUpdateUnusedWhilePending = have12.descriptorBindingUpdateUnusedWhilePending;
    f12.shaderSampledImageArrayNonUniformIndexing = have12.shaderSampledImageArrayNonUniformIndexing;
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.pNext = &f12;
    f13.dynamicRendering = VK_TRUE;
    if (present_wait_) {
        fwait.pNext = f13.pNext;
        f13.pNext = &fid;
    }
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.pNext = &f13;
    features.features.samplerAnisotropy = have.features.samplerAnisotropy;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &features;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = uint32_t(dev_ext.size());
    dci.ppEnabledExtensionNames = dev_ext.data();
    if (!check(vkCreateDevice(gpu_, &dci, nullptr, &device_), "vkCreateDevice")) return false;
    volkLoadDevice(device_);
    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = queue_family_;
    vkCreateCommandPool(device_, &pci, nullptr, &pool_);
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool_;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    vkAllocateCommandBuffers(device_, &cai, &cmd_);
    VkSemaphoreCreateInfo semi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    vkCreateSemaphore(device_, &semi, nullptr, &acquired_);
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vkCreateFence(device_, &fi, nullptr, &fence_);

    return create_swapchain();
}

bool VkPresenter::create_swapchain() {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu_, surface_, &caps);
    extent_ = caps.currentExtent;
    if (extent_.width == 0xFFFFFFFFu || extent_.width == 0) {   // the surface takes the swapchain's size
        update_size();
        extent_.width = uint32_t(std::max(pixel_w_, 1));
        extent_.height = uint32_t(std::max(pixel_h_, 1));
    }
    if (extent_.width == 0 || extent_.height == 0) return false;

    uint32_t fn = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu_, surface_, &fn, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(fn);
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu_, surface_, &fn, formats.data());
    if (formats.empty()) return false;
    VkSurfaceFormatKHR format = formats[0];
    for (auto& f : formats)
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) format = f;
    format_ = format.format;

    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = surface_;
    ci.minImageCount = std::min(std::max(caps.minImageCount, 3u), caps.maxImageCount ? caps.maxImageCount : 8u);
    ci.imageFormat = format.format;
    ci.imageColorSpace = format.colorSpace;
    ci.imageExtent = extent_;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
                          ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                          : caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;   // vsync at the screen's refresh rate
    ci.clipped = VK_TRUE;
    std::unique_lock<std::shared_mutex> lock(swapchain_mutex_);
    swapchain_first_id_ = present_id_ + 1;
    VkSwapchainKHR old = swapchain_;
    ci.oldSwapchain = old;
    if (!check(vkCreateSwapchainKHR(device_, &ci, nullptr, &swapchain_), "vkCreateSwapchainKHR")) return false;
    for (uint32_t i = 0; i < image_count_; i++)
        if (views_[i]) vkDestroyImageView(device_, views_[i], nullptr);
    if (old) vkDestroySwapchainKHR(device_, old, nullptr);

    if (!check(vkGetSwapchainImagesKHR(device_, swapchain_, &image_count_, nullptr), "vkGetSwapchainImagesKHR")) return false;
    images_.assign(image_count_, VK_NULL_HANDLE);
    views_.assign(image_count_, VK_NULL_HANDLE);
    if (!check(vkGetSwapchainImagesKHR(device_, swapchain_, &image_count_, images_.data()), "vkGetSwapchainImagesKHR")) return false;
    if (rendered_.size() < image_count_) rendered_.resize(image_count_, VK_NULL_HANDLE);
    VkSemaphoreCreateInfo semi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (uint32_t i = 0; i < image_count_; i++) {
        if (!rendered_[i]) vkCreateSemaphore(device_, &semi, nullptr, &rendered_[i]);
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = images_[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format_;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(device_, &vi, nullptr, &views_[i]);
    }
    printf("[video] swapchain %ux%u, %u images\n", extent_.width, extent_.height, image_count_);
    return true;
}

void VkPresenter::destroy_swapchain() {
    std::unique_lock<std::shared_mutex> lock(swapchain_mutex_);
    for (uint32_t i = 0; i < image_count_; i++)
        if (views_[i]) vkDestroyImageView(device_, views_[i], nullptr);
    if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
}

void VkPresenter::handle_event(const SDL_Event& e) {
    auto mouse_lp = [&](float x, float y) { return MAKELPARAM(int(std::lround(x)), int(std::lround(y))); };
    switch (e.type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_TERMINATING:
        quit_ = true;
        break;
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_SAFE_AREA_CHANGED:
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
        update_size();
        resized_ = true;
        break;
    case SDL_EVENT_WILL_ENTER_BACKGROUND:
        compat_set_focus(false);
        compat_release_keys();
        break;
    case SDL_EVENT_DID_ENTER_FOREGROUND:
        compat_set_focus(true);
        update_size();
        resized_ = true;
        break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        int vk = to_vk(e.key.key);
        if (!vk) break;
        bool down = e.type == SDL_EVENT_KEY_DOWN;
        compat_set_key(vk, down);
        if (down && input_hook) input_hook(WM_KEYDOWN, WPARAM(vk), 0);   // (Esc does not close the game here)
        break;
    }
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_MOTION:
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_CANCELED: {
        float x = e.tfinger.x * float(pixel_w_), y = e.tfinger.y * float(pixel_h_);
        if (e.type == SDL_EVENT_FINGER_DOWN) {   // debugging aid: three fingers cycle the 3D debug views
            int n = 0;
            if (SDL_Finger** f = SDL_GetTouchFingers(e.tfinger.touchID, &n)) SDL_free(f);
            // (once per gesture: the three fingers' touches can each see all three)
            static bool gesture = false;
            if (n < 3) gesture = false;
            if (n == 3 && !gesture) {
                gesture = true;
                int v = (g_sr2_debug_view.load() + 1) % 19;
                g_sr2_debug_view = v;
                printf("[debug] 3D view %d\n", v);
                break;
            }
        }
        if (touch_hook && touch_hook(e, x, y)) break;
        // Otherwise a tap is the menus' click, a drag moves their highlight
        if (!input_hook) break;
        if (e.type == SDL_EVENT_FINGER_DOWN) {
            input_hook(WM_MOUSEMOVE, 0, mouse_lp(x, y));
            input_hook(WM_LBUTTONDOWN, 0, mouse_lp(x, y));
        } else if (e.type == SDL_EVENT_FINGER_MOTION) {
            input_hook(WM_MOUSEMOVE, 0, mouse_lp(x, y));
        }
        break;
    }
    case SDL_EVENT_MOUSE_MOTION:
        if (e.motion.which == SDL_TOUCH_MOUSEID) break;
        if (input_hook) input_hook(WM_MOUSEMOVE, 0, mouse_lp(e.motion.x * density_, e.motion.y * density_));
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        if (e.button.which == SDL_TOUCH_MOUSEID) break;
        float x = e.button.x * density_, y = e.button.y * density_;
        if (touch_hook && touch_hook(e, x, y)) break;
        if (!input_hook) break;
        if (e.button.button == SDL_BUTTON_LEFT) input_hook(WM_LBUTTONDOWN, 0, mouse_lp(x, y));
        else if (e.button.button == SDL_BUTTON_RIGHT) input_hook(WM_RBUTTONDOWN, 0, mouse_lp(x, y));
        break;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        if (input_hook) {
            float mx = 0, my = 0;
            SDL_GetMouseState(&mx, &my);
            int delta = int(std::lround(e.wheel.y * WHEEL_DELTA));
            input_hook(WM_MOUSEWHEEL, MAKEWPARAM(0, uint16_t(int16_t(delta))), mouse_lp(mx * density_, my * density_));
        }
        break;
    case SDL_EVENT_GAMEPAD_ADDED:
    case SDL_EVENT_GAMEPAD_REMOVED:
    case SDL_EVENT_JOYSTICK_ADDED:
    case SDL_EVENT_JOYSTICK_REMOVED: {
        std::lock_guard<std::mutex> lock(g_device_mutex);
        g_device_events.push_back(e);
        break;
    }
    default: break;
    }
}

bool VkPresenter::pump() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) handle_event(e);
    return !quit_;
}

void VkPresenter::run_once(const std::function<void(VkCommandBuffer)>& fn) {
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool_;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(device_, &cai, &cmd);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    fn(cmd);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue_);
    vkFreeCommandBuffers(device_, pool_, 1, &cmd);
}

void VkPresenter::frame(const RecordFn& record) {
    if (!device_) return;
    // In the background iOS takes the GPU away: nothing is drawn
    if (!GetForegroundWindow()) {
        SDL_Delay(16);
        return;
    }
    if (device_lost_) return;
    if (resized_) {
        vkDeviceWaitIdle(device_);
        if (!create_swapchain()) {
            SDL_Delay(16);
            return;
        }
        resized_ = false;
    }
    if (!swapchain_) return;

    double t0 = seconds();
    vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);
    double t_fence = seconds();
    uint32_t index = 0;
    VkResult r = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, acquired_, VK_NULL_HANDLE, &index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        resized_ = true;
        return;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return;
    vkResetFences(device_, 1, &fence_);
    double t1 = seconds();

    vkResetCommandBuffer(cmd_, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd_, &bi);
    vk_image_barrier(cmd_, images_[index], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    record(cmd_, views_[index], extent_);
    double t2 = seconds();
    bool capture = !capture_path_.empty();
    if (capture) {
        if (capture_buffer_) {
            vkDestroyBuffer(device_, capture_buffer_, nullptr);
            vkFreeMemory(device_, capture_memory_, nullptr);
        }
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = VkDeviceSize(extent_.width) * extent_.height * 4;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        vkCreateBuffer(device_, &bci, nullptr, &capture_buffer_);
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device_, capture_buffer_, &req);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = find_memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkAllocateMemory(device_, &mai, nullptr, &capture_memory_);
        vkBindBufferMemory(device_, capture_buffer_, capture_memory_, 0);
        vk_image_barrier(cmd_, images_[index], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy c{};
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.imageExtent = {extent_.width, extent_.height, 1};
        vkCmdCopyImageToBuffer(cmd_, images_[index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, capture_buffer_, 1, &c);
        vk_image_barrier(cmd_, images_[index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    } else {
        vk_image_barrier(cmd_, images_[index], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    }
    vkEndCommandBuffer(cmd_);

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &acquired_;
    si.pWaitDstStageMask = &wait_stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd_;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &rendered_[index];
    if (vkQueueSubmit(queue_, 1, &si, fence_) == VK_ERROR_DEVICE_LOST) {
        fprintf(stderr, "[vulkan] device lost: nothing more is drawn\n");
        device_lost_ = true;
        return;
    }

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &rendered_[index];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &index;
    VkPresentIdKHR pid{VK_STRUCTURE_TYPE_PRESENT_ID_KHR};
    uint64_t id = 0;
    if (present_wait_) {
        id = ++present_id_;
        pid.swapchainCount = 1;
        pid.pPresentIds = &id;
        pi.pNext = &pid;
    }
    r = vkQueuePresentKHR(queue_, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) resized_ = true;
    double t3 = seconds();
    timing_.gpu_wait += t1 - t0;
    timing_.record += t2 - t1;
    timing_.present += t3 - t2;
    timing_.frames++;
    last_ = {t_fence - t0, t1 - t_fence, t2 - t1, t3 - t2, id};
    if (capture) {
        vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);
        void* data = nullptr;
        vkMapMemory(device_, capture_memory_, 0, VK_WHOLE_SIZE, 0, &data);
        if (FILE* f = fopen(capture_path_.c_str(), "wb")) {
            BITMAPFILEHEADER fh{};
            BITMAPINFOHEADER ih{};
            fh.bfType = 0x4D42;
            fh.bfOffBits = sizeof(fh) + sizeof(ih);
            fh.bfSize = fh.bfOffBits + extent_.width * extent_.height * 4;
            ih.biSize = sizeof(ih);
            ih.biWidth = LONG(extent_.width);
            ih.biHeight = -LONG(extent_.height);
            ih.biPlanes = 1;
            ih.biBitCount = 32;
            fwrite(&fh, sizeof(fh), 1, f);
            fwrite(&ih, sizeof(ih), 1, f);
            fwrite(data, 4, size_t(extent_.width) * extent_.height, f);
            fclose(f);
        }
        vkUnmapMemory(device_, capture_memory_);
        capture_path_.clear();
    }
}

bool VkPresenter::wait_present(uint64_t id, uint64_t timeout_ns) {
    std::shared_lock<std::shared_mutex> lock(swapchain_mutex_);
    if (!present_wait_ || !swapchain_ || id < swapchain_first_id_) return false;
    VkResult r = vkWaitForPresentKHR(device_, swapchain_, id, timeout_ns);
    return r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR;
}

double VkPresenter::limit_queue(int depth, uint64_t timeout_ns) {
    if (!present_wait_ || !swapchain_ || depth <= 0 || present_id_ < uint64_t(depth)) return 0;
    uint64_t id = present_id_ + 1 - uint64_t(depth);
    if (id < swapchain_first_id_ || vkWaitForPresentKHR(device_, swapchain_, id, timeout_ns) != VK_SUCCESS) return 0;
    return seconds();
}

void VkPresenter::shutdown() {
    if (device_) {
        vkDeviceWaitIdle(device_);
        destroy_swapchain();
        vkDestroyDevice(device_, nullptr);
    }
    if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
    if (instance_) vkDestroyInstance(instance_, nullptr);
#ifdef __APPLE__
    if (metal_view_) SDL_Metal_DestroyView(static_cast<SDL_MetalView>(metal_view_));
#endif
    if (window_) SDL_DestroyWindow(window_);
    window_ = nullptr;
}
