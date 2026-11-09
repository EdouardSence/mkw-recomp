// Every destruction of the objects a descriptor set refers to (buffers, image and buffer views, samplers) goes through
// vk_destroy_note, for MKW_DRAW_CACHE (vk_exec_dcache.c): sets are kept across frames by their contents, which name
// objects by handle, and a destroyed handle's value can come back for a new object. A kept set naming the handle
// empties the cache (vk_destroy_gen moves). Included by vk.h and by vk_vma.cpp before vk_mem_alloc.h, so VMA's own
// vkDestroyBuffer calls go through it too.
#ifndef VK_DESTROY_COUNT_H
#define VK_DESTROY_COUNT_H
#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif
extern volatile uint64_t vk_destroy_gen;
void vk_destroy_note(uint64_t handle);   // vk_exec_dcache.c
#ifdef __cplusplus
}
#endif

static inline void vk_destroy_counted_buffer(VkDevice d, VkBuffer o, const VkAllocationCallbacks *a) {
    vk_destroy_note((uint64_t)o);
    vkDestroyBuffer(d, o, a);
}
static inline void vk_destroy_counted_image_view(VkDevice d, VkImageView o, const VkAllocationCallbacks *a) {
    vk_destroy_note((uint64_t)o);
    vkDestroyImageView(d, o, a);
}
static inline void vk_destroy_counted_buffer_view(VkDevice d, VkBufferView o, const VkAllocationCallbacks *a) {
    vk_destroy_note((uint64_t)o);
    vkDestroyBufferView(d, o, a);
}
static inline void vk_destroy_counted_sampler(VkDevice d, VkSampler o, const VkAllocationCallbacks *a) {
    vk_destroy_note((uint64_t)o);
    vkDestroySampler(d, o, a);
}
#define vkDestroyBuffer vk_destroy_counted_buffer
#define vkDestroyImageView vk_destroy_counted_image_view
#define vkDestroyBufferView vk_destroy_counted_buffer_view
#define vkDestroySampler vk_destroy_counted_sampler
#endif
