// iOS port: MoltenVK is linked statically, so volk is handed its
// vkGetInstanceProcAddr instead of loading a Vulkan library. This file is
// compiled with the Vulkan prototypes (no volk), so the name is the real one.
#undef VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

PFN_vkGetInstanceProcAddr sr2_moltenvk_get_instance_proc_addr(void) { return vkGetInstanceProcAddr; }
