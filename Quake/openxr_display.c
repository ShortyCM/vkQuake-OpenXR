/*
 * vkQuake-OpenXR display backend. GPL-2.0-or-later.
 * Head tracking composes with the stock mouse-controlled game camera.
 * Deliberately contains no OpenXR action sets or controller bindings.
 */
#include "quakedef.h"
#include "openxr_display.h"
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

static XrInstance							   xr_instance;
static XrSystemId							   xr_system;
static XrSession							   xr_session;
static XrSpace								   xr_view_space;
static XrSpace								   xr_local_space;
static XrVector3f							   xr_reference_position;
static float								   xr_reference_yaw;
static qboolean								   xr_recenter_pending = true;
static qboolean								   xr_presence_supported;
static XrTime								   xr_recenter_time;
static XrSessionState						   xr_state;
static qboolean								   xr_running, xr_frame;
static qboolean								   xr_acquired;
static XrFrameState							   xr_frame_state;
static XrView								   xr_views[2];
static XrSwapchain							   xr_swapchains[2];
static XrSwapchainImageVulkanKHR			  *xr_images[2];
static uint32_t								   xr_counts[2], xr_indices[2];
static VkImageView							  *xr_image_views;
static VkFormat								   xr_format;
static uint32_t								   xr_width, xr_height;
static int									   xr_eye = -1;
static float								   xr_ipd = 0.064f;
static VkPhysicalDevice						   xr_physical_device;
static PFN_vkGetInstanceProcAddr			   xr_vk_get_proc;
static PFN_xrGetVulkanGraphicsRequirements2KHR xr_get_requirements;
static PFN_xrCreateVulkanInstanceKHR		   xr_create_instance;
static PFN_xrGetVulkanGraphicsDevice2KHR	   xr_get_device;
static PFN_xrCreateVulkanDeviceKHR			   xr_create_device;
static XrGraphicsRequirementsVulkanKHR		   xr_requirements;

static cvar_t	xr_units_per_meter = {"xr_units_per_meter", "30.621172", CVAR_ARCHIVE}; // 56 / 1.8288
static cvar_t	xr_hud_distance = {"xr_hud_distance", "2", CVAR_ARCHIVE};
static cvar_t	xr_hud_width = {"xr_hud_width", "1.6", CVAR_ARCHIVE};
static cvar_t	xr_auto_hide_dashboard = {"xr_auto_hide_dashboard", "1", CVAR_ARCHIVE};
static qboolean xr_dashboard_handled;
static vec3_t	xr_aim_center, xr_aim_right, xr_aim_up;
static float	xr_aim_distance;

static qboolean				 xr_presence_known, xr_user_present;
static oxr_recenter_prompt_t xr_recenter_prompt = OXR_RECENTER_READY;
static unsigned int			 xr_recenter_mouse_held;

oxr_recenter_prompt_t OXR_RecenterPrompt (void)
{
	return xr_recenter_prompt;
}
void OXR_SetAim (const vec3_t origin, const vec3_t angles, const vec3_t target)
{
	vec3_t copy, delta, forward;
	VectorCopy (angles, copy);
	AngleVectors (copy, forward, xr_aim_right, xr_aim_up);
	VectorCopy (target, xr_aim_center);
	VectorSubtract (target, origin, delta);
	xr_aim_distance = q_max (1.0f, VectorLength (delta));
}

void OXR_ViewProjection (float matrix[16], const vec3_t origin, const vec3_t angles)
{
	vec3_t copy, forward, right, up;
	float  view[16] = {0};
	VectorCopy (angles, copy);
	AngleVectors (copy, forward, right, up);
	for (int axis = 0; axis < 3; ++axis)
	{
		view[axis * 4] = right[axis];
		view[axis * 4 + 1] = up[axis];
		view[axis * 4 + 2] = -forward[axis];
	}
	view[12] = -DotProduct (right, origin);
	view[13] = -DotProduct (up, origin);
	view[14] = DotProduct (forward, origin);
	view[15] = 1;
	OXR_Projection (matrix);
	MatrixMultiply (matrix, view);
}

void OXR_UIMatrix (float matrix[16], const VkViewport *viewport)
{
	// The stock canvas and its viewport form one mouse-aimed physical plane.
	float canvas[16] = {0}, plane[16] = {0}, result[16];
	canvas[0] = viewport->width / vid.width;
	canvas[5] = viewport->height / vid.height;
	canvas[12] = (2 * viewport->x + viewport->width) / vid.width - 1;
	canvas[13] = (2 * viewport->y + viewport->height) / vid.height - 1;
	canvas[15] = 1;
	MatrixMultiply (canvas, matrix);
	float half_width = xr_aim_distance * CLAMP (0.2f, xr_hud_width.value, 5.0f) / (2 * CLAMP (0.5f, xr_hud_distance.value, 10.0f));
	float half_height = half_width * (float)vid.height / vid.width;
	for (int axis = 0; axis < 3; ++axis)
	{
		plane[axis] = xr_aim_right[axis] * half_width;
		plane[4 + axis] = -xr_aim_up[axis] * half_height;
		plane[12 + axis] = xr_aim_center[axis];
	}
	plane[15] = 1;
	MatrixMultiply (plane, canvas);
	memcpy (result, vulkan_globals.view_projection_matrix, sizeof (result));
	MatrixMultiply (result, plane);
	memcpy (matrix, result, sizeof (result));
}

static void OXR_HideDashboard (void)
{
	Con_Printf ("OpenXR: SteamVR dashboard hide %s.\n", OXR_HideSteamVRDashboard () ? "requested" : "unavailable");
}

static void OXR_Check (XrResult result, const char *operation)
{
	if (XR_FAILED (result))
	{
		char message[XR_MAX_RESULT_STRING_SIZE] = "unknown OpenXR error";
		xrResultToString (xr_instance, result, message);
		Con_Printf ("OpenXR error: %s: %s (%d)\n", operation, message, (int)result);
		Sys_Error ("OpenXR: %s: %s (%d)", operation, message, (int)result);
	}
}
#define XR_CHECK(call) OXR_Check ((call), #call)

static void OXR_Recenter (void)
{
	xr_recenter_pending = true;
	xr_recenter_time = 0;
}

static void OXR_ManualRecenter (void)
{
	xr_recenter_prompt = OXR_RECENTER_APPLYING;
	OXR_Recenter ();
}

qboolean OXR_RecenterMouse (int button, qboolean down)
{
	if (button < 0 || button > 1)
		return false;
	unsigned int bit = 1u << button;
	// Always swallow releases/repeats of clicks captured by this workflow.
	if (xr_recenter_mouse_held & bit)
	{
		if (!down)
			xr_recenter_mouse_held &= ~bit;
		return true;
	}
	if (!down || xr_recenter_prompt == OXR_RECENTER_IDLE)
		return false;
	xr_recenter_mouse_held |= bit;
	if (xr_recenter_prompt == OXR_RECENTER_READY && button == 0)
		OXR_ManualRecenter ();
	else if (xr_recenter_prompt == OXR_RECENTER_CONFIRM)
		xr_recenter_prompt = button == 0 ? OXR_RECENTER_IDLE : OXR_RECENTER_READY;
	return true;
}
static XrVector3f OXR_Rotate (XrQuaternionf q, XrVector3f v)
{
	XrVector3f t = {2 * (q.y * v.z - q.z * v.y), 2 * (q.z * v.x - q.x * v.z), 2 * (q.x * v.y - q.y * v.x)};
	return (XrVector3f){v.x + q.w * t.x + q.y * t.z - q.z * t.y, v.y + q.w * t.y + q.z * t.x - q.x * t.z, v.z + q.w * t.z + q.x * t.y - q.y * t.x};
}

static XrVector3f OXR_RelativeDirection (XrVector3f v)
{
	float c = cosf (xr_reference_yaw), s = sinf (xr_reference_yaw);
	return (XrVector3f){c * v.x - s * v.z, v.y, s * v.x + c * v.z};
}

void OXR_Init (void)
{
	const char *extensions[2] = {XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
	uint32_t	extension_count;
	XR_CHECK (xrEnumerateInstanceExtensionProperties (NULL, 0, &extension_count, NULL));
	XrExtensionProperties *properties = Mem_Alloc (extension_count * sizeof (*properties));
	memset (properties, 0, extension_count * sizeof (*properties));
	for (uint32_t i = 0; i < extension_count; ++i)
		properties[i].type = XR_TYPE_EXTENSION_PROPERTIES;
	XR_CHECK (xrEnumerateInstanceExtensionProperties (NULL, extension_count, &extension_count, properties));
	xr_presence_supported = false;
	for (uint32_t i = 0; i < extension_count; ++i)
		if (!strcmp (properties[i].extensionName, XR_EXT_USER_PRESENCE_EXTENSION_NAME))
			xr_presence_supported = true;
	Mem_Free (properties);
	if (xr_presence_supported)
		extensions[1] = XR_EXT_USER_PRESENCE_EXTENSION_NAME;
	XrInstanceCreateInfo info = {XR_TYPE_INSTANCE_CREATE_INFO};
	q_strlcpy (info.applicationInfo.applicationName, "vkQuake-OpenXR", sizeof (info.applicationInfo.applicationName));
	q_strlcpy (info.applicationInfo.engineName, "vkQuake", sizeof (info.applicationInfo.engineName));
	info.applicationInfo.apiVersion = XR_MAKE_VERSION (1, 0, 0);
	info.enabledExtensionCount = xr_presence_supported ? 2 : 1;
	info.enabledExtensionNames = extensions;
	XrResult result = xrCreateInstance (&info, &xr_instance);
	if (XR_FAILED (result))
		Sys_Error ("OpenXR initialization failed (%d). Enable SteamVR as the active OpenXR runtime and connect the headset.", (int)result);
	XrSystemGetInfo system_info = {XR_TYPE_SYSTEM_GET_INFO};
	system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	XR_CHECK (xrGetSystem (xr_instance, &system_info, &xr_system));
	if (xr_presence_supported)
	{
		XrSystemUserPresencePropertiesEXT presence = {XR_TYPE_SYSTEM_USER_PRESENCE_PROPERTIES_EXT};
		XrSystemProperties				  system = {XR_TYPE_SYSTEM_PROPERTIES};
		system.next = &presence;
		XR_CHECK (xrGetSystemProperties (xr_instance, xr_system, &system));
		xr_presence_supported = presence.supportsUserPresence;
	}
	Con_Printf ("OpenXR: headset presence detection %s\n", xr_presence_supported ? "supported" : "unavailable; use xr_recenter");
#define XR_PROC(name, target) XR_CHECK (xrGetInstanceProcAddr (xr_instance, name, (PFN_xrVoidFunction *)&target))
	XR_PROC ("xrGetVulkanGraphicsRequirements2KHR", xr_get_requirements);
	XR_PROC ("xrCreateVulkanInstanceKHR", xr_create_instance);
	XR_PROC ("xrGetVulkanGraphicsDevice2KHR", xr_get_device);
	XR_PROC ("xrCreateVulkanDeviceKHR", xr_create_device);
#undef XR_PROC
	xr_requirements.type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR;
	XR_CHECK (xr_get_requirements (xr_instance, xr_system, &xr_requirements));
	Cvar_RegisterVariable (&xr_units_per_meter);
	Cvar_RegisterVariable (&xr_hud_distance);
	Cvar_RegisterVariable (&xr_hud_width);
	Cmd_AddCommand ("xr_recenter", OXR_ManualRecenter);
	Cvar_RegisterVariable (&xr_auto_hide_dashboard);
	Cmd_AddCommand ("xr_hide_dashboard", OXR_HideDashboard);
	Con_Printf ("OpenXR: head tracking with stock mouse aiming; controllers disabled\n");
}

VkResult OXR_CreateVulkanInstance (const VkInstanceCreateInfo *info, VkInstance *instance, PFN_vkGetInstanceProcAddr get_proc)
{
	xr_vk_get_proc = get_proc;
	VkApplicationInfo application = *info->pApplicationInfo;
	XrVersion		  requested = XR_MAKE_VERSION (VK_VERSION_MAJOR (application.apiVersion), VK_VERSION_MINOR (application.apiVersion), 0);
	if (requested < xr_requirements.minApiVersionSupported || requested > xr_requirements.maxApiVersionSupported)
		Sys_Error ("OpenXR runtime does not support vkQuake's Vulkan API version");
	VkInstanceCreateInfo vk_info = *info;
	application.pApplicationName = "vkQuake-OpenXR";
	vk_info.pApplicationInfo = &application;
	XrVulkanInstanceCreateInfoKHR create = {XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
	create.systemId = xr_system;
	create.pfnGetInstanceProcAddr = get_proc;
	create.vulkanCreateInfo = &vk_info;
	VkResult result;
	XR_CHECK (xr_create_instance (xr_instance, &create, instance, &result));
	return result;
}

VkPhysicalDevice OXR_PhysicalDevice (VkInstance instance)
{
	XrVulkanGraphicsDeviceGetInfoKHR info = {XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
	info.systemId = xr_system;
	info.vulkanInstance = instance;
	XR_CHECK (xr_get_device (xr_instance, &info, &xr_physical_device));
	return xr_physical_device;
}

VkResult OXR_CreateVulkanDevice (const VkDeviceCreateInfo *info, VkDevice *device)
{
	XrVulkanDeviceCreateInfoKHR create = {XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
	create.systemId = xr_system;
	create.pfnGetInstanceProcAddr = xr_vk_get_proc;
	create.vulkanPhysicalDevice = xr_physical_device;
	create.vulkanCreateInfo = info;
	VkResult result;
	XR_CHECK (xr_create_device (xr_instance, &create, device, &result));
	return result;
}

static VkFormat OXR_SelectFormat (const int64_t *formats, uint32_t count)
{
	const VkFormat preferred[] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB};
	for (uint32_t preference = 0; preference < countof (preferred); ++preference)
		for (uint32_t i = 0; i < count; ++i)
			if (formats[i] == preferred[preference])
				return preferred[preference];
	return VK_FORMAT_UNDEFINED;
}

void OXR_InitSession (VkInstance instance, VkPhysicalDevice physical_device)
{
	XrGraphicsBindingVulkanKHR binding = {XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
	binding.instance = instance;
	binding.physicalDevice = physical_device;
	binding.device = vulkan_globals.device;
	binding.queueFamilyIndex = vulkan_globals.gfx_queue_family_index;
	XrSessionCreateInfo info = {XR_TYPE_SESSION_CREATE_INFO};
	info.next = &binding;
	info.systemId = xr_system;
	XR_CHECK (xrCreateSession (xr_instance, &info, &xr_session));
	XrReferenceSpaceCreateInfo space = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
	space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
	space.poseInReferenceSpace.orientation.w = 1;
	XR_CHECK (xrCreateReferenceSpace (xr_session, &space, &xr_view_space));
	space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	XR_CHECK (xrCreateReferenceSpace (xr_session, &space, &xr_local_space));
	uint32_t count;
	XR_CHECK (xrEnumerateViewConfigurationViews (xr_instance, xr_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &count, NULL));
	if (count != 2)
		Sys_Error ("OpenXR requires a primary stereo view configuration");
	XrViewConfigurationView views[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
	XR_CHECK (xrEnumerateViewConfigurationViews (xr_instance, xr_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &count, views));
	xr_width = q_max (views[0].recommendedImageRectWidth, views[1].recommendedImageRectWidth);
	xr_height = q_max (views[0].recommendedImageRectHeight, views[1].recommendedImageRectHeight);
	xr_width = q_min (xr_width, q_min (views[0].maxImageRectWidth, views[1].maxImageRectWidth));
	xr_height = q_min (xr_height, q_min (views[0].maxImageRectHeight, views[1].maxImageRectHeight));
	XR_CHECK (xrEnumerateSwapchainFormats (xr_session, 0, &count, NULL));
	int64_t *formats = Mem_Alloc (count * sizeof (*formats));
	XR_CHECK (xrEnumerateSwapchainFormats (xr_session, count, &count, formats));
	// SteamVR may expose only sRGB formats. Postprocess compensates for the
	// attachment's automatic encoding so Quake's gamma/contrast stay unchanged.
	xr_format = OXR_SelectFormat (formats, count);
	Mem_Free (formats);
	if (!xr_format)
		Sys_Error ("OpenXR runtime has no supported RGBA/BGRA swapchain format");
	Con_Printf ("OpenXR eye format: %d (%s)\n", (int)xr_format, OXR_IsSRGB () ? "sRGB" : "UNORM");
	for (int eye = 0; eye < 2; ++eye)
	{
		XrSwapchainCreateInfo swap = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
		swap.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT;
		swap.format = xr_format;
		swap.sampleCount = 1;
		swap.width = xr_width;
		swap.height = xr_height;
		swap.faceCount = swap.arraySize = swap.mipCount = 1;
		XR_CHECK (xrCreateSwapchain (xr_session, &swap, &xr_swapchains[eye]));
		XR_CHECK (xrEnumerateSwapchainImages (xr_swapchains[eye], 0, &xr_counts[eye], NULL));
		xr_images[eye] = Mem_Alloc (xr_counts[eye] * sizeof (*xr_images[eye]));
		for (uint32_t i = 0; i < xr_counts[eye]; ++i)
			xr_images[eye][i].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
		XR_CHECK (xrEnumerateSwapchainImages (xr_swapchains[eye], xr_counts[eye], &xr_counts[eye], (XrSwapchainImageBaseHeader *)xr_images[eye]));
	}
	xr_image_views = Mem_Alloc (OXR_ImageCount () * sizeof (*xr_image_views));
	for (int eye = 0; eye < 2; ++eye)
		for (uint32_t i = 0; i < xr_counts[eye]; ++i)
		{
			VkImageViewCreateInfo view = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
			view.image = xr_images[eye][i].image;
			view.viewType = VK_IMAGE_VIEW_TYPE_2D;
			view.format = xr_format;
			view.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
			if (vkCreateImageView (vulkan_globals.device, &view, NULL, &xr_image_views[(eye ? xr_counts[0] : 0) + i]) != VK_SUCCESS)
				Sys_Error ("OpenXR: failed to create eye image view");
		}
	Con_Printf ("OpenXR eye resolution: %u x %u\n", xr_width, xr_height);
}

void OXR_GetSize (int *width, int *height)
{
	if (xr_session)
	{
		*width = (int)xr_width;
		*height = (int)xr_height;
	}
}
VkFormat OXR_Format (void)
{
	return xr_format;
}
qboolean OXR_IsSRGB (void)
{
	return xr_format == VK_FORMAT_R8G8B8A8_SRGB || xr_format == VK_FORMAT_B8G8R8A8_SRGB;
}
uint32_t OXR_ImageCount (void)
{
	return xr_counts[0] + xr_counts[1];
}
const VkImageView *OXR_ImageViews (void)
{
	return xr_image_views;
}
uint32_t OXR_ImageIndex (void)
{
	return (xr_eye ? xr_counts[0] : 0) + xr_indices[xr_eye];
}
VkImage OXR_Image (void)
{
	return xr_images[xr_eye][xr_indices[xr_eye]].image;
}
void OXR_SetEye (int eye)
{
	xr_eye = eye;
}
int OXR_Eye (void)
{
	return xr_eye;
}

qboolean OXR_BeginFrame (void)
{
	XrEventDataBuffer event = {XR_TYPE_EVENT_DATA_BUFFER};
	XrResult		  poll;
	while ((poll = xrPollEvent (xr_instance, &event)) == XR_SUCCESS)
	{
		if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING)
			Sys_Error ("OpenXR runtime was lost. Restart vkQuake-OpenXR.");
		if (event.type == XR_TYPE_EVENT_DATA_USER_PRESENCE_CHANGED_EXT && xr_presence_supported)
		{
			const XrEventDataUserPresenceChangedEXT *presence = (const XrEventDataUserPresenceChangedEXT *)&event;
			if (presence->session == xr_session && (!xr_presence_known || xr_user_present != (qboolean)presence->isUserPresent))
			{
				xr_presence_known = true;
				xr_user_present = presence->isUserPresent;
				if (presence->isUserPresent)
				{
					xr_recenter_prompt = OXR_RECENTER_READY;
					OXR_Recenter ();
				}
				Con_Printf ("OpenXR: headset %s\n", presence->isUserPresent ? "worn; press Mouse1 to recenter when ready" : "removed");
			}
		}
		if (event.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING)
		{
			const XrEventDataReferenceSpaceChangePending *change = (const XrEventDataReferenceSpaceChangePending *)&event;
			if (change->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL)
			{
				xr_recenter_pending = true;
				xr_recenter_time = change->changeTime;
			}
		}
		if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
		{
			xr_state = ((XrEventDataSessionStateChanged *)&event)->state;
			if (xr_state == XR_SESSION_STATE_READY)
			{
				XrSessionBeginInfo begin = {XR_TYPE_SESSION_BEGIN_INFO};
				begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
				XR_CHECK (xrBeginSession (xr_session, &begin));
				xr_running = true;
				xr_dashboard_handled = false;
				xr_recenter_prompt = OXR_RECENTER_READY;
				xr_presence_known = false;
				OXR_Recenter ();
			}
			else if (xr_state == XR_SESSION_STATE_STOPPING)
			{
				XR_CHECK (xrEndSession (xr_session));
				xr_running = false;
				xr_recenter_prompt = OXR_RECENTER_IDLE;
			}
			else if (xr_state == XR_SESSION_STATE_EXITING || xr_state == XR_SESSION_STATE_LOSS_PENDING)
			{
				xr_running = false;
				Host_Quit_f ();
				return false;
			}
		}
		event = (XrEventDataBuffer){XR_TYPE_EVENT_DATA_BUFFER};
	}
	XR_CHECK (poll);
	if (!xr_running)
		return false;
	xr_frame_state = (XrFrameState){XR_TYPE_FRAME_STATE};
	XrFrameWaitInfo wait = {XR_TYPE_FRAME_WAIT_INFO};
	XR_CHECK (xrWaitFrame (xr_session, &wait, &xr_frame_state));
	XrFrameBeginInfo begin = {XR_TYPE_FRAME_BEGIN_INFO};
	XR_CHECK (xrBeginFrame (xr_session, &begin));
	xr_frame = true;
	if (!xr_frame_state.shouldRender)
	{
		OXR_EndFrame (false);
		return false;
	}
	XrViewLocateInfo locate = {XR_TYPE_VIEW_LOCATE_INFO};
	locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	locate.displayTime = xr_frame_state.predictedDisplayTime;
	locate.space = xr_local_space;
	XrViewState state = {XR_TYPE_VIEW_STATE};
	uint32_t	count;
	xr_views[0] = (XrView){XR_TYPE_VIEW};
	xr_views[1] = (XrView){XR_TYPE_VIEW};
	XR_CHECK (xrLocateViews (xr_session, &locate, &state, 2, &count, xr_views));
	if (count != 2 || !(state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) || !(state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT))
	{
		OXR_EndFrame (false);
		return false;
	}
	XrSpaceLocation head = {XR_TYPE_SPACE_LOCATION};
	XR_CHECK (xrLocateSpace (xr_view_space, xr_local_space, xr_frame_state.predictedDisplayTime, &head));
	if (!(head.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) || !(head.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
	{
		OXR_EndFrame (false);
		return false;
	}
	if (xr_recenter_pending && xr_frame_state.predictedDisplayTime >= xr_recenter_time)
	{
		XrVector3f forward = OXR_Rotate (head.pose.orientation, (XrVector3f){0, 0, -1});
		// Reset position and heading, preserving physical up and natural pitch/roll.
		if (forward.x * forward.x + forward.z * forward.z > 0.0001f)
			xr_reference_yaw = atan2f (-forward.x, -forward.z);
		xr_reference_position = head.pose.position;
		xr_recenter_pending = false;
		if (xr_recenter_prompt == OXR_RECENTER_APPLYING)
			xr_recenter_prompt = OXR_RECENTER_CONFIRM;
		Con_Printf ("OpenXR tracking origin recentered\n");
	}
	// Eye distance is rotation invariant. X separation alone shrinks on head turns.
	static qboolean logged_geometry;
	if (!logged_geometry)
	{
		for (int eye = 0; eye < 2; ++eye)
		{
			const XrView *v = &xr_views[eye];
			Con_Printf (
				"OpenXR eye %d: pos %.6f %.6f %.6f, orientation %.6f %.6f %.6f %.6f, FOV %.6f %.6f %.6f %.6f\n", eye, v->pose.position.x, v->pose.position.y,
				v->pose.position.z, v->pose.orientation.x, v->pose.orientation.y, v->pose.orientation.z, v->pose.orientation.w, v->fov.angleLeft,
				v->fov.angleRight, v->fov.angleUp, v->fov.angleDown);
		}
		logged_geometry = true;
	}
	float dx = xr_views[1].pose.position.x - xr_views[0].pose.position.x;
	float dy = xr_views[1].pose.position.y - xr_views[0].pose.position.y;
	float dz = xr_views[1].pose.position.z - xr_views[0].pose.position.z;
	float ipd = sqrtf (dx * dx + dy * dy + dz * dz);
	if ((state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) && isfinite (ipd) && ipd > 0.03f && ipd < 0.09f)
		xr_ipd = ipd;
	for (int eye = 0; eye < 2; ++eye)
	{
		XrSwapchainImageAcquireInfo acquire = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
		XR_CHECK (xrAcquireSwapchainImage (xr_swapchains[eye], &acquire, &xr_indices[eye]));
		XrSwapchainImageWaitInfo image_wait = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
		image_wait.timeout = XR_INFINITE_DURATION;
		XR_CHECK (xrWaitSwapchainImage (xr_swapchains[eye], &image_wait));
	}
	xr_acquired = true;
	return true;
}

void OXR_EndFrame (qboolean rendered)
{
	if (!xr_frame)
		return;
	rendered = rendered && xr_acquired;
	XrCompositionLayerProjectionView views[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
	if (xr_acquired)
		for (int eye = 0; eye < 2; ++eye)
		{
			XrSwapchainImageReleaseInfo release = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
			XR_CHECK (xrReleaseSwapchainImage (xr_swapchains[eye], &release));
			views[eye].pose = xr_views[eye].pose;
			views[eye].fov = xr_views[eye].fov;
			views[eye].subImage.swapchain = xr_swapchains[eye];
			views[eye].subImage.imageRect.extent = (XrExtent2Di){(int32_t)xr_width, (int32_t)xr_height};
		}
	XrCompositionLayerProjection layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
	layer.space = xr_local_space;
	layer.viewCount = 2;
	layer.views = views;
	const XrCompositionLayerBaseHeader *layers[] = {(const XrCompositionLayerBaseHeader *)&layer};
	XrFrameEndInfo						end = {XR_TYPE_FRAME_END_INFO};
	end.displayTime = xr_frame_state.predictedDisplayTime;
	end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	end.layerCount = rendered ? 1 : 0;
	end.layers = rendered ? layers : NULL;
	XR_CHECK (xrEndFrame (xr_session, &end));
	if (rendered && !xr_dashboard_handled && (xr_state == XR_SESSION_STATE_VISIBLE || xr_state == XR_SESSION_STATE_FOCUSED))
	{
		xr_dashboard_handled = true;
		if (xr_state == XR_SESSION_STATE_VISIBLE && xr_auto_hide_dashboard.value)
			OXR_HideDashboard ();
	}
	xr_frame = false;
	xr_acquired = false;
	xr_eye = -1;
}

void OXR_ApplyView (const vec3_t center, const vec3_t base_angles, vec3_t origin, vec3_t angles)
{
	vec3_t forward, right, up;
	vec3_t game_angles;
	VectorCopy (base_angles, game_angles);
	AngleVectors (game_angles, forward, right, up);
	const XrPosef *eye = &xr_views[xr_eye].pose;
	XrVector3f	   offset = {eye->position.x - xr_reference_position.x, eye->position.y - xr_reference_position.y, eye->position.z - xr_reference_position.z};
	offset = OXR_RelativeDirection (offset);
	XrVector3f look = OXR_RelativeDirection (OXR_Rotate (eye->orientation, (XrVector3f){0, 0, -1}));
	XrVector3f head_up = OXR_RelativeDirection (OXR_Rotate (eye->orientation, (XrVector3f){0, 1, 0}));
	float	   units = CLAMP (1.0f, xr_units_per_meter.value, 1000.0f);
	vec3_t	   world_forward, world_up;
	for (int i = 0; i < 3; ++i)
	{
		origin[i] = center[i] + units * (right[i] * offset.x + up[i] * offset.y - forward[i] * offset.z);
		world_forward[i] = right[i] * look.x + up[i] * look.y - forward[i] * look.z;
		world_up[i] = right[i] * head_up.x + up[i] * head_up.y - forward[i] * head_up.z;
	}
	VectorAngles (world_forward, world_up, angles);
}

void OXR_Projection (float matrix[16])
{
	const XrFovf *fov = &xr_views[xr_eye].fov;
	float		  l = tanf (fov->angleLeft), r = tanf (fov->angleRight);
	float		  d = tanf (fov->angleDown), u = tanf (fov->angleUp);
	memset (matrix, 0, 16 * sizeof (*matrix));
	matrix[0] = 2 / (r - l);
	matrix[5] = -2 / (u - d);
	matrix[8] = (r + l) / (r - l);
	matrix[9] = -(u + d) / (u - d);
	matrix[11] = -1;
	matrix[14] = 0.5f; // Reverse Z, infinite far plane; close geometry remains visible.
}

void OXR_Fov (float *fovx, float *fovy)
{
	const XrFovf *fov = &xr_views[xr_eye].fov;
	// Symmetric conservative bounds enclose the asymmetric projection for culling.
	*fovx = 2 * q_max (fabsf (fov->angleLeft), fabsf (fov->angleRight)) * 180 / M_PI;
	*fovy = 2 * q_max (fabsf (fov->angleDown), fabsf (fov->angleUp)) * 180 / M_PI;
}

void OXR_Shutdown (void)
{
	if (xr_session)
	{
		for (uint32_t i = 0; i < OXR_ImageCount (); ++i)
			vkDestroyImageView (vulkan_globals.device, xr_image_views[i], NULL);
		Mem_Free (xr_image_views);
		for (int eye = 0; eye < 2; ++eye)
		{
			xrDestroySwapchain (xr_swapchains[eye]);
			Mem_Free (xr_images[eye]);
		}
		xrDestroySpace (xr_view_space);
		xrDestroySpace (xr_local_space);
		xrDestroySession (xr_session);
		xr_session = XR_NULL_HANDLE;
	}
	if (xr_instance)
		xrDestroyInstance (xr_instance);
	xr_instance = XR_NULL_HANDLE;
}
