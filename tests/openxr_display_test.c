/* Exercise the production backend against a deterministic OpenXR frame source. */
#define xrPollEvent				test_xrPollEvent
#define xrWaitFrame				test_xrWaitFrame
#define xrBeginFrame			test_xrBeginFrame
#define xrLocateViews			test_xrLocateViews
#define xrLocateSpace			test_xrLocateSpace
#define xrAcquireSwapchainImage test_xrAcquireSwapchainImage
#define xrWaitSwapchainImage	test_xrWaitSwapchainImage
#define xrReleaseSwapchainImage test_xrReleaseSwapchainImage
#define xrEndFrame				test_xrEndFrame
#define xrBeginSession			test_xrBeginSession
#define xrEndSession			test_xrEndSession
#include "../Quake/openxr_display.c"
#include <assert.h>

static int dashboard_requests;
qboolean   OXR_HideSteamVRDashboard (void)
{
	++dashboard_requests;
	return true;
}

vulkanglobals_t			vulkan_globals;
viddef_t				vid;
static XrSessionState	next_state;
static XrBool32			should_render = XR_TRUE;
static XrViewStateFlags view_flags;
static float			test_ipd = 0.064f, head_x, head_y, head_z;
static XrQuaternionf	head_rotation = {0, 0, 0, 1};
static int				acquires, waits, releases, ends, session_begins, session_ends, located;
static qboolean			expect_layer;
static qboolean			head_valid = true;
static XrTime			test_time = 123456789;
static XrTime			reference_change;
static int				presence_event = -1;
static XrFovf			test_fov[2] = {{-0.95f, 0.8f, 0.9f, -0.85f}, {-0.8f, 0.95f, 0.9f, -0.85f}};

static void near_value (float a, float b)
{
	assert (fabsf (a - b) < 0.0001f);
}

FUNC_NORETURN void Sys_Error (const char *format, ...)
{
	va_list args;
	va_start (args, format);
	vfprintf (stderr, format, args);
	va_end (args);
	abort ();
}
void Con_Printf (const char *format, ...)
{
	(void)format;
}
void Cvar_RegisterVariable (cvar_t *var)
{
	(void)var;
}
cmd_function_t *Cmd_AddCommand2 (const char *name, xcommand_t function, cmd_source_t source, qboolean interceptable)
{
	(void)name;
	(void)function;
	(void)source;
	(void)interceptable;
	return NULL;
}
void *Mem_Alloc (const size_t size)
{
	return calloc (1, size);
}
void Mem_Free (void *ptr)
{
	free (ptr);
}
void Host_Quit_f (void)
{
	abort ();
}
size_t q_strlcpy (char *dst, const char *src, size_t size)
{
	size_t length = strlen (src);
	if (size)
	{
		size_t copy = q_min (length, size - 1);
		memcpy (dst, src, copy);
		dst[copy] = 0;
	}
	return length;
}

XRAPI_ATTR XrResult XRAPI_CALL test_xrPollEvent (XrInstance instance, XrEventDataBuffer *event)
{
	assert (instance == xr_instance);
	if (presence_event >= 0)
	{
		XrEventDataUserPresenceChangedEXT *presence = (XrEventDataUserPresenceChangedEXT *)event;
		presence->type = XR_TYPE_EVENT_DATA_USER_PRESENCE_CHANGED_EXT;
		presence->session = xr_session;
		presence->isUserPresent = presence_event;
		presence_event = -1;
		return XR_SUCCESS;
	}
	if (reference_change)
	{
		XrEventDataReferenceSpaceChangePending *change = (XrEventDataReferenceSpaceChangePending *)event;
		change->type = XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING;
		change->referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
		change->changeTime = reference_change;
		reference_change = 0;
		return XR_SUCCESS;
	}
	if (!next_state)
		return XR_EVENT_UNAVAILABLE;
	XrEventDataSessionStateChanged *changed = (XrEventDataSessionStateChanged *)event;
	changed->type = XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED;
	changed->session = xr_session;
	changed->state = next_state;
	next_state = XR_SESSION_STATE_UNKNOWN;
	return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL test_xrBeginSession (XrSession session, const XrSessionBeginInfo *info)
{
	assert (session == xr_session && info->primaryViewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO);
	session_begins++;
	return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL test_xrEndSession (XrSession session)
{
	assert (session == xr_session);
	session_ends++;
	return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL test_xrWaitFrame (XrSession session, const XrFrameWaitInfo *info, XrFrameState *state)
{
	assert (session == xr_session && info->type == XR_TYPE_FRAME_WAIT_INFO);
	state->predictedDisplayTime = test_time;
	state->shouldRender = should_render;
	return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL test_xrBeginFrame (XrSession session, const XrFrameBeginInfo *info)
{
	assert (session == xr_session && info->type == XR_TYPE_FRAME_BEGIN_INFO);
	return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL
test_xrLocateViews (XrSession session, const XrViewLocateInfo *info, XrViewState *state, uint32_t capacity, uint32_t *count, XrView *views)
{
	assert (session == xr_session && info->space == xr_local_space && capacity == 2);
	assert (info->displayTime == test_time);
	state->viewStateFlags = view_flags;
	*count = 2;
	located++;
	for (int i = 0; i < 2; ++i)
	{
		XrVector3f offset = OXR_Rotate (head_rotation, (XrVector3f){(i ? 0.5f : -0.5f) * test_ipd, 0, 0});
		views[i].pose.position = (XrVector3f){head_x + offset.x, head_y + offset.y, head_z + offset.z};
		views[i].pose.orientation = head_rotation;
		views[i].fov = test_fov[i];
	}
	return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL test_xrLocateSpace (XrSpace space, XrSpace base, XrTime time, XrSpaceLocation *location)
{
	assert (space == xr_view_space && base == xr_local_space && time == test_time);
	location->locationFlags = head_valid ? XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT : 0;
	location->pose.position = (XrVector3f){head_x, head_y, head_z};
	location->pose.orientation = head_rotation;
	return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL test_xrAcquireSwapchainImage (XrSwapchain swap, const XrSwapchainImageAcquireInfo *info, uint32_t *index)
{
	assert ((swap == xr_swapchains[0] || swap == xr_swapchains[1]) && info->type == XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO);
	*index = 0;
	acquires++;
	return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL test_xrWaitSwapchainImage (XrSwapchain swap, const XrSwapchainImageWaitInfo *info)
{
	assert ((swap == xr_swapchains[0] || swap == xr_swapchains[1]) && info->timeout == XR_INFINITE_DURATION);
	waits++;
	return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL test_xrReleaseSwapchainImage (XrSwapchain swap, const XrSwapchainImageReleaseInfo *info)
{
	assert ((swap == xr_swapchains[0] || swap == xr_swapchains[1]) && info->type == XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO);
	releases++;
	return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL test_xrEndFrame (XrSession session, const XrFrameEndInfo *info)
{
	assert (session == xr_session && info->displayTime == test_time);
	assert (info->environmentBlendMode == XR_ENVIRONMENT_BLEND_MODE_OPAQUE);
	assert (info->layerCount == (expect_layer ? 1u : 0u));
	assert (acquires == releases && acquires == waits);
	if (expect_layer)
	{
		const XrCompositionLayerProjection *layer = (const XrCompositionLayerProjection *)info->layers[0];
		assert (layer->space == xr_local_space && layer->viewCount == 2);
		for (int eye = 0; eye < 2; ++eye)
		{
			const XrCompositionLayerProjectionView *view = &layer->views[eye];
			assert (!memcmp (&view->pose, &xr_views[eye].pose, sizeof (XrPosef)));
			assert (!memcmp (&view->fov, &test_fov[eye], sizeof (XrFovf)));
			assert (view->subImage.imageRect.extent.width == 2160);
			assert (view->subImage.imageRect.extent.height == 2160);
		}
	}
	ends++;
	return XR_SUCCESS;
}

static void check_projection (int eye)
{
	float matrix[16], fx, fy;
	OXR_SetEye (eye);
	OXR_Projection (matrix);
	const XrFovf *f = &test_fov[eye];
	near_value (matrix[0] * tanf (f->angleLeft) - matrix[8], -1);
	near_value (matrix[0] * tanf (f->angleRight) - matrix[8], 1);
	near_value (matrix[5] * tanf (f->angleUp) - matrix[9], -1);
	near_value (matrix[5] * tanf (f->angleDown) - matrix[9], 1);
	near_value (matrix[14] / 0.5f, 1); // Near maps to 1, infinity to 0.
	OXR_Fov (&fx, &fy);
	assert (DEG2RAD (fx / 2) >= fabsf (f->angleLeft) - 0.0001f);
	assert (DEG2RAD (fx / 2) >= fabsf (f->angleRight) - 0.0001f);
	assert (DEG2RAD (fy / 2) >= fabsf (f->angleUp) - 0.0001f);
	assert (DEG2RAD (fy / 2) >= fabsf (f->angleDown) - 0.0001f);
}

int main (void)
{
	_set_error_mode (_OUT_TO_STDERR);
	const int64_t steamvr_formats[] = {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R16G16B16A16_SFLOAT};
	assert (OXR_SelectFormat (steamvr_formats, countof (steamvr_formats)) == VK_FORMAT_R8G8B8A8_SRGB);
	const int64_t mixed_formats[] = {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_UNORM};
	assert (OXR_SelectFormat (mixed_formats, countof (mixed_formats)) == VK_FORMAT_B8G8R8A8_UNORM);
	const int64_t unsupported_formats[] = {VK_FORMAT_R16G16B16A16_SFLOAT};
	assert (OXR_SelectFormat (unsupported_formats, 1) == VK_FORMAT_UNDEFINED);
	xr_format = VK_FORMAT_B8G8R8A8_SRGB;
	assert (OXR_IsSRGB ());
	xr_format = VK_FORMAT_R8G8B8A8_UNORM;
	assert (!OXR_IsSRGB ());
	view_flags = XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
	xr_instance = (XrInstance)(uintptr_t)1;
	xr_session = (XrSession)(uintptr_t)2;
	xr_view_space = (XrSpace)(uintptr_t)3;
	xr_local_space = (XrSpace)(uintptr_t)6;
	xr_swapchains[0] = (XrSwapchain)(uintptr_t)4;
	xr_swapchains[1] = (XrSwapchain)(uintptr_t)5;
	xr_width = xr_height = vid.width = vid.height = 2160;
	// A desktop resize must not shrink rendering into the eye image's corner.
	const int desktop_sizes[][2] = {{1280, 720}, {1920, 1080}, {800, 600}};
	for (size_t i = 0; i < countof (desktop_sizes); ++i)
	{
		vid.width = desktop_sizes[i][0];
		vid.height = desktop_sizes[i][1];
		OXR_GetSize (&vid.width, &vid.height);
		assert (vid.width == 2160 && vid.height == 2160);
	}
	xr_units_per_meter.value = 56.0f / 1.8288f;
	xr_hud_distance.value = 2;
	xr_hud_width.value = 1.6f;
	next_state = XR_SESSION_STATE_READY;
	expect_layer = true;
	assert (OXR_BeginFrame ());
	assert (session_begins == 1 && acquires == 2 && waits == 2);
	assert (!xr_presence_supported && OXR_RecenterPrompt () == OXR_RECENTER_READY);
	check_projection (0);
	check_projection (1);
	vec3_t center = {10, 20, 30}, angles = {0, 0, 0}, left, right, tracked_angles;
	OXR_SetEye (0);
	OXR_ApplyView (center, angles, left, tracked_angles);
	OXR_SetEye (1);
	OXR_ApplyView (center, angles, right, tracked_angles);
	near_value (tracked_angles[PITCH], 0);
	near_value (tracked_angles[YAW], 0);
	near_value (tracked_angles[ROLL], 0);
	near_value (left[0], center[0]);
	near_value (left[2], center[2]);
	near_value (left[1] - right[1], test_ipd * xr_units_per_meter.value);
	near_value ((left[1] + right[1]) / 2, center[1]);
	float before[16];
	OXR_Projection (before);
	OXR_EndFrame (true);
	assert (OXR_Eye () == -1);
	near_value (xr_units_per_meter.value * 1.8288f, 56);

	// HUD center and reticle remain on the same mouse aim target under head motion.
	vec3_t aim_origin = {0, 0, 0}, aim_angles = {0, 0, 0}, aim_target = {100, 0, 0};
	OXR_SetAim (aim_origin, aim_angles, aim_target);
	VkViewport hud = {0, 0, 2160, 2160, 0, 1};
	for (int sample = 0; sample < 4; ++sample)
	{
		float  yaw = sample * 0.15f;
		float *vp = vulkan_globals.view_projection_matrix;
		memset (vp, 0, 16 * sizeof (float));
		vp[0] = -sinf (yaw);
		vp[4] = -cosf (yaw);
		vp[9] = -1;
		vp[3] = cosf (yaw);
		vp[7] = -sinf (yaw);
		vp[12] = sample - 1.5f; // Different eye positions and head lean.
		vp[13] = sample * 2.0f;
		float matrix[16] = {0};
		matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1;
		OXR_UIMatrix (matrix, &hud);
		float w = vp[3] * 100;
		near_value (matrix[12] / matrix[15], (vp[0] * 100 + vp[12]) / w);
		near_value (matrix[13] / matrix[15], vp[13] / w);
		assert (matrix[0] > 0 && matrix[5] > 0);
	}
	// Focus is unrelated to wearing the headset and must never recenter it.
	head_x = 1;
	next_state = XR_SESSION_STATE_FOCUSED;
	assert (OXR_BeginFrame ());
	near_value (xr_reference_position.x, 0);
	OXR_EndFrame (true);
	// Wearing opens the manual workflow; elapsed time never recenters again.
	xr_presence_supported = true;
	presence_event = 1;
	assert (OXR_BeginFrame ());
	assert (OXR_RecenterPrompt () == OXR_RECENTER_READY);
	near_value (xr_reference_position.x, 1);
	OXR_EndFrame (true);
	head_x = 2;
	test_time += 10000000000LL;
	assert (OXR_BeginFrame ());
	assert (OXR_RecenterPrompt () == OXR_RECENTER_READY);
	near_value (xr_reference_position.x, 1);
	OXR_EndFrame (true);
	assert (OXR_RecenterMouse (0, true));
	assert (OXR_RecenterPrompt () == OXR_RECENTER_APPLYING);
	assert (OXR_BeginFrame ());
	assert (!xr_recenter_pending);
	assert (OXR_RecenterPrompt () == OXR_RECENTER_CONFIRM);
	near_value (xr_reference_position.x, 2);
	assert (OXR_RecenterMouse (0, false));
	assert (OXR_RecenterMouse (0, true));
	assert (OXR_RecenterPrompt () == OXR_RECENTER_IDLE);
	assert (OXR_RecenterMouse (0, false));
	OXR_SetEye (0);
	OXR_ApplyView (center, angles, left, tracked_angles);
	OXR_SetEye (1);
	OXR_ApplyView (center, angles, right, tracked_angles);
	for (int i = 0; i < 3; ++i)
		near_value ((left[i] + right[i]) / 2, center[i]);
	OXR_EndFrame (true);
	head_x = 0;
	OXR_Recenter ();
	assert (OXR_BeginFrame ());
	OXR_EndFrame (true);
	head_x = 0.2f;
	next_state = XR_SESSION_STATE_VISIBLE;
	assert (OXR_BeginFrame ());
	OXR_EndFrame (true);
	next_state = XR_SESSION_STATE_FOCUSED;
	assert (OXR_BeginFrame ());
	near_value (xr_reference_position.x, 0); // Later dashboard focus changes must not snap the view.
	OXR_EndFrame (true);

	// Head translation maps meters to Quake units without changing mouse input.
	head_x = 3;
	head_y = -7;
	head_z = 18;
	assert (OXR_BeginFrame ());
	float  after[16];
	vec3_t moved;
	OXR_SetEye (1);
	OXR_Projection (after);
	assert (!memcmp (before, after, sizeof (before)));
	OXR_ApplyView (center, angles, moved, tracked_angles);
	near_value (moved[0], right[0] - 18 * xr_units_per_meter.value);
	near_value (moved[1], right[1] - 3 * xr_units_per_meter.value);
	near_value (moved[2], right[2] - 7 * xr_units_per_meter.value);
	near_value (angles[YAW], 0);
	near_value (center[0], 10);
	OXR_EndFrame (true);

	// A 90-degree head turn rotates stereo separation without shrinking IPD.
	head_rotation = (XrQuaternionf){0, sinf (M_PI / 4), 0, cosf (M_PI / 4)};
	test_ipd = 0.07f;
	assert (OXR_BeginFrame ());
	near_value (xr_ipd, test_ipd);
	OXR_SetEye (0);
	OXR_ApplyView (center, angles, left, tracked_angles);
	OXR_SetEye (1);
	OXR_ApplyView (center, angles, right, tracked_angles);
	near_value (right[0] - left[0], 0.07f * xr_units_per_meter.value);
	near_value (tracked_angles[YAW], 90);
	near_value (tracked_angles[PITCH], 0);
	vec3_t mouse_angles = {0, 30, 0};
	OXR_ApplyView (center, mouse_angles, moved, tracked_angles);
	near_value (tracked_angles[YAW], 120);
	near_value (mouse_angles[YAW], 30);
	vec3_t tilted_mouse = {20, 30, 10}, base_forward, base_right, base_up, composed_forward, composed_right, composed_up;
	AngleVectors (tilted_mouse, base_forward, base_right, base_up);
	OXR_ApplyView (center, tilted_mouse, moved, tracked_angles);
	AngleVectors (tracked_angles, composed_forward, composed_right, composed_up);
	for (int i = 0; i < 3; ++i)
	{
		near_value (composed_forward[i], -base_right[i]);
		near_value (composed_up[i], base_up[i]);
	}
	near_value (tilted_mouse[PITCH], 20);
	near_value (tilted_mouse[YAW], 30);
	near_value (tilted_mouse[ROLL], 10);
	OXR_EndFrame (true);

	// Recenter establishes seated position and heading without moving the game camera.
	OXR_Recenter ();
	assert (OXR_BeginFrame ());
	OXR_SetEye (0);
	OXR_ApplyView (center, mouse_angles, left, tracked_angles);
	OXR_SetEye (1);
	OXR_ApplyView (center, mouse_angles, right, tracked_angles);
	for (int i = 0; i < 3; ++i)
		near_value ((left[i] + right[i]) / 2, center[i]);
	near_value (tracked_angles[YAW], 30);
	OXR_EndFrame (true);

	// Head pitch and roll survive recenter; composition uses rotations, not Euler addition.
	head_x = head_y = head_z = 0;
	head_rotation = (XrQuaternionf){0, 0, 0, 1};
	OXR_Recenter ();
	assert (OXR_BeginFrame ());
	OXR_EndFrame (true);
	head_rotation = (XrQuaternionf){sinf (M_PI / 12), 0, 0, cosf (M_PI / 12)};
	assert (OXR_BeginFrame ());
	OXR_SetEye (0);
	OXR_ApplyView (center, angles, moved, tracked_angles);
	near_value (tracked_angles[PITCH], -30);
	near_value (tracked_angles[YAW], 0);
	OXR_EndFrame (true);
	head_rotation = (XrQuaternionf){0, 0, sinf (M_PI / 12), cosf (M_PI / 12)};
	assert (OXR_BeginFrame ());
	OXR_SetEye (0);
	OXR_ApplyView (center, angles, moved, tracked_angles);
	near_value (tracked_angles[ROLL], -30);
	OXR_EndFrame (true);

	// Runtime reference-space changes take effect at their advertised display time.
	head_x = 4;
	reference_change = test_time + 10;
	assert (OXR_BeginFrame ());
	assert (xr_recenter_pending);
	near_value (xr_reference_position.x, 0);
	OXR_EndFrame (true);
	test_time += 10;
	assert (OXR_BeginFrame ());
	assert (!xr_recenter_pending);
	near_value (xr_reference_position.x, 4);
	OXR_EndFrame (true);

	// Invalid tracking submits an empty frame and never consumes invalid poses.
	expect_layer = false;
	view_flags = 0;
	test_ipd = 10;
	assert (!OXR_BeginFrame ());
	near_value (xr_ipd, 0.07f);
	view_flags = XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
	head_valid = false;
	assert (!OXR_BeginFrame ());
	head_valid = true;

	// GPU/render setup failure: release both images and submit no layer.
	expect_layer = false;
	assert (OXR_BeginFrame ());
	OXR_EndFrame (false);
	int acquired_before = acquires, located_before = located;
	should_render = XR_FALSE;
	assert (!OXR_BeginFrame ());
	assert (acquires == acquired_before && located == located_before);
	assert (!xr_frame && !xr_acquired);

	next_state = XR_SESSION_STATE_STOPPING;
	assert (!OXR_BeginFrame ());
	assert (session_ends == 1 && !xr_running);
	assert (ends == 20 && acquires == releases);
	// Dismiss the startup dashboard once, without fighting later user toggles.
	xr_running = true;
	xr_dashboard_handled = false;
	xr_auto_hide_dashboard.value = 1;
	should_render = XR_TRUE;
	expect_layer = true;
	next_state = XR_SESSION_STATE_VISIBLE;
	assert (OXR_BeginFrame ());
	OXR_EndFrame (true);
	assert (dashboard_requests == 1);
	assert (OXR_BeginFrame ());
	OXR_EndFrame (true);
	assert (dashboard_requests == 1);
	// Duplicate presence reports must not reopen an already confirmed workflow.
	head_x += 1;
	float reference_before = xr_reference_position.x;
	presence_event = 1;
	assert (OXR_BeginFrame ());
	assert (OXR_RecenterPrompt () == OXR_RECENTER_IDLE);
	near_value (xr_reference_position.x, reference_before);
	OXR_EndFrame (true);
	presence_event = 0;
	assert (OXR_BeginFrame ());
	OXR_EndFrame (true);
	presence_event = 1;
	assert (OXR_BeginFrame ());
	assert (OXR_RecenterPrompt () == OXR_RECENTER_READY);
	OXR_EndFrame (true);
	assert (!OXR_RecenterMouse (0, false)); // Release of an earlier gameplay click passes through.
	head_x += 1;
	assert (OXR_RecenterMouse (0, true));
	assert (OXR_RecenterPrompt () == OXR_RECENTER_APPLYING);
	assert (OXR_RecenterMouse (0, true)); // Repeats cannot confirm.
	head_valid = false;
	expect_layer = false;
	assert (!OXR_BeginFrame ());
	assert (OXR_RecenterPrompt () == OXR_RECENTER_APPLYING);
	head_valid = true;
	expect_layer = true;
	assert (OXR_BeginFrame ());
	near_value (xr_reference_position.x, head_x);
	assert (OXR_RecenterPrompt () == OXR_RECENTER_CONFIRM);
	OXR_EndFrame (true);
	assert (OXR_RecenterMouse (0, true)); // Holding the first click still cannot confirm.
	assert (OXR_RecenterPrompt () == OXR_RECENTER_CONFIRM);
	assert (OXR_RecenterMouse (0, false));
	assert (OXR_RecenterMouse (1, true));
	assert (OXR_RecenterPrompt () == OXR_RECENTER_READY);
	assert (OXR_RecenterMouse (1, false));
	reference_before = xr_reference_position.x;
	head_x += 1;
	test_time += 10000000000LL;
	assert (OXR_BeginFrame ());
	near_value (xr_reference_position.x, reference_before); // Retry waits for Mouse1.
	OXR_EndFrame (true);
	assert (OXR_RecenterMouse (0, true));
	assert (OXR_RecenterMouse (0, false));
	assert (OXR_BeginFrame ());
	near_value (xr_reference_position.x, head_x);
	assert (OXR_RecenterPrompt () == OXR_RECENTER_CONFIRM);
	OXR_EndFrame (true);
	assert (OXR_RecenterMouse (0, true));
	assert (OXR_RecenterPrompt () == OXR_RECENTER_IDLE);
	assert (OXR_RecenterMouse (0, true)); // Consume confirmation repeats and release too.
	assert (OXR_RecenterMouse (0, false));
	assert (!OXR_RecenterMouse (0, true));
	assert (!OXR_RecenterMouse (0, false));
	assert (!OXR_RecenterMouse (1, true));
	assert (!OXR_RecenterMouse (1, false));
	// Presence is optional: startup/manual prompts survive a not-worn event.
	presence_event = 0;
	assert (OXR_BeginFrame ());
	OXR_EndFrame (true);
	presence_event = 1;
	assert (OXR_BeginFrame ());
	assert (OXR_RecenterPrompt () == OXR_RECENTER_READY);
	OXR_EndFrame (true);
	presence_event = 0;
	assert (OXR_BeginFrame ());
	assert (OXR_RecenterPrompt () == OXR_RECENTER_READY);
	OXR_EndFrame (true);
	xr_presence_supported = false;
	assert (OXR_RecenterMouse (0, true));
	assert (OXR_RecenterMouse (0, false));
	assert (OXR_BeginFrame ());
	assert (OXR_RecenterPrompt () == OXR_RECENTER_CONFIRM);
	OXR_EndFrame (true);
	assert (OXR_RecenterMouse (0, true));
	assert (OXR_RecenterMouse (0, false));
	assert (OXR_RecenterPrompt () == OXR_RECENTER_IDLE);

	// Menus without a loaded world still get a valid asymmetric eye matrix.
	vec3_t menu_origin = {0, 0, 0}, menu_angles = {0, 0, 0};
	float  menu_matrix[16], projection[16];
	OXR_SetEye (0);
	OXR_Projection (projection);
	OXR_ViewProjection (menu_matrix, menu_origin, menu_angles);
	near_value ((menu_matrix[0] * 100 + menu_matrix[12]) / (menu_matrix[3] * 100 + menu_matrix[15]), -projection[8]);
	near_value ((menu_matrix[1] * 100 + menu_matrix[13]) / (menu_matrix[3] * 100 + menu_matrix[15]), -projection[9]);
	puts ("OpenXR: projection, scale, IPD, head/mouse composition, manual recenter/confirm/retry, menus, resize, release and session checks passed.");
	return 0;
}
