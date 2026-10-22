/* OpenXR head tracking composes with the stock camera. No controllers. */
#ifndef OPENXR_DISPLAY_H
#define OPENXR_DISPLAY_H

#ifdef USE_OPENXR
void			   OXR_Init (void);
VkResult		   OXR_CreateVulkanInstance (const VkInstanceCreateInfo *info, VkInstance *instance, PFN_vkGetInstanceProcAddr get_proc);
VkPhysicalDevice   OXR_PhysicalDevice (VkInstance instance);
VkResult		   OXR_CreateVulkanDevice (const VkDeviceCreateInfo *info, VkDevice *device);
void			   OXR_InitSession (VkInstance instance, VkPhysicalDevice physical_device);
void			   OXR_Shutdown (void);
void			   OXR_GetSize (int *width, int *height);
VkFormat		   OXR_Format (void);
uint32_t		   OXR_ImageCount (void);
const VkImageView *OXR_ImageViews (void);
uint32_t		   OXR_ImageIndex (void);
VkImage			   OXR_Image (void);
qboolean		   OXR_BeginFrame (void);
void			   OXR_EndFrame (qboolean rendered);
void			   OXR_SetEye (int eye);
int				   OXR_Eye (void);
void			   OXR_ApplyView (const vec3_t center, const vec3_t base_angles, vec3_t origin, vec3_t angles);
void			   OXR_Projection (float matrix[16]);
void			   OXR_Fov (float *fovx, float *fovy);
qboolean		   OXR_HideSteamVRDashboard (void);
void			   OXR_SetAim (const vec3_t origin, const vec3_t angles, const vec3_t target);
void			   OXR_UIMatrix (float matrix[16], const VkViewport *viewport);
void			   OXR_ViewProjection (float matrix[16], const vec3_t origin, const vec3_t angles);
qboolean		   OXR_IsSRGB (void);
typedef enum
{
	OXR_RECENTER_IDLE,
	OXR_RECENTER_READY,
	OXR_RECENTER_APPLYING,
	OXR_RECENTER_CONFIRM
} oxr_recenter_prompt_t;
oxr_recenter_prompt_t OXR_RecenterPrompt (void);
qboolean			  OXR_RecenterMouse (int button, qboolean down);
#else
#define OXR_Eye() (-1)
#endif

#endif
