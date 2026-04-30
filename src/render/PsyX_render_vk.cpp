// PsyX_render_vk.cpp
//
// Vulkan / MoltenVK rendering backend for Psy-X.
// Companion to (and selectable alternative to) the OpenGL backend in
// PsyX_render.cpp. Build is enabled by RENDERER_VK (premake5 --renderer=vulkan).
//
// State of this revision:
//   - Vulkan instance + surface + device + swapchain bring-up via SDL2.
//   - Per-frame BeginScene → render-pass → command-buffer → submit → present.
//   - GR_Shader_Compile compiles GLSL source to SPIR-V at runtime via
//     libshaderc (statically linked).
//   - A single "PSX vertex" graphics pipeline is created at GR_InitialisePSX
//     time, matching the GrVertex layout. GR_UpdateVertexBuffer fills a
//     mappable host-visible vertex buffer. GR_DrawTriangles binds and draws.
//   - GR_SetBlendMode toggles between a small set of pre-baked PSO variants.
//   - VRAM image, paletted texture sampling, full GTE shader port,
//     framebuffer→VRAM blit and PGXP Z-buffer are still TODO and are
//     stubbed out (they no-op cleanly so the rest of the engine runs).
//
// All identifiers exposed to PsyX_main / PsyX_GPU keep the same C++ linkage
// the OpenGL backend uses; the public GR_* surface lives in PsyX_render.h.

#include "PsyX/PsyX_render.h"

#if defined(RENDERER_VK)

#include "PsyX/PsyX_globals.h"
#include "PsyX/PsyX_public.h"
#include "../PsyX_main.h"
#include "../platform.h"

#include <vulkan/vulkan.h>
#include <SDL_vulkan.h>
#include <shaderc/shaderc.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <float.h>
#include <math.h>
#include <stdint.h>

// =======================================================================
// Engine-side globals (parity with GL backend)
// =======================================================================

extern "C" {

extern SDL_Window* g_window;	// owned by PsyX_main.cpp

int g_windowWidth  = 0;
int g_windowHeight = 0;

int g_dbg_wireframeMode  = 0;
int g_dbg_texturelessMode = 0;

int g_cfg_pgxpTextureCorrection = 1;
int g_cfg_pgxpZBuffer           = 1;
int g_cfg_bilinearFiltering     = 0;

TextureID g_whiteTexture = 0;
TextureID g_vramTexture  = 0;

int g_PreviousBlendMode    = BM_NONE;
int g_PreviousDepthMode    = 0;
int g_PreviousStencilMode  = 0;
int g_PreviousScissorState = 0;
int g_PreviousOffscreenState = 0;
ShaderID g_PreviousShader  = (ShaderID)-1;
TextureID g_lastBoundTexture = (TextureID)-1;

int vram_need_update         = 1;
int framebuffer_need_update  = 0;

}

// =======================================================================
// Vulkan backend internals
// =======================================================================

namespace {

// -----------------------------------------------------------------------
// Core Vulkan handles
// -----------------------------------------------------------------------
VkInstance         g_vkInstance        = VK_NULL_HANDLE;
VkPhysicalDevice   g_vkPhysicalDevice  = VK_NULL_HANDLE;
VkDevice           g_vkDevice          = VK_NULL_HANDLE;
VkSurfaceKHR       g_vkSurface         = VK_NULL_HANDLE;
VkQueue            g_vkGraphicsQueue   = VK_NULL_HANDLE;
VkQueue            g_vkPresentQueue    = VK_NULL_HANDLE;
uint32_t           g_vkGraphicsQueueFamily = 0;
uint32_t           g_vkPresentQueueFamily  = 0;
VkPhysicalDeviceMemoryProperties g_vkMemProps = {};

VkSwapchainKHR     g_vkSwapchain       = VK_NULL_HANDLE;
VkFormat           g_vkSwapchainFormat = VK_FORMAT_UNDEFINED;
VkExtent2D         g_vkSwapchainExtent = {0, 0};
bool               g_vkSwapchainCanReadback = false;
std::vector<VkImage>     g_vkSwapchainImages;
std::vector<VkImageView> g_vkSwapchainImageViews;
std::vector<VkFramebuffer> g_vkFramebuffers;

// Depth attachment shared by every framebuffer (depth contents are
// cleared each frame so we don't need per-image instances).
VkImage            g_vkDepthImage      = VK_NULL_HANDLE;
VkDeviceMemory     g_vkDepthImageMem   = VK_NULL_HANDLE;
VkImageView        g_vkDepthImageView  = VK_NULL_HANDLE;
const VkFormat     g_vkDepthFormat     = VK_FORMAT_D32_SFLOAT;

VkRenderPass       g_vkClearRenderPass = VK_NULL_HANDLE;

VkCommandPool      g_vkCommandPool     = VK_NULL_HANDLE;
static const int   kMaxFramesInFlight  = 2;
VkCommandBuffer    g_vkCmdBuffers[kMaxFramesInFlight] = {};
VkSemaphore        g_vkImageAvailableSems[kMaxFramesInFlight] = {};
VkSemaphore        g_vkRenderFinishedSems[kMaxFramesInFlight] = {};
VkFence            g_vkInFlightFences[kMaxFramesInFlight] = {};

int                g_vkCurrentFrame    = 0;
uint32_t           g_vkAcquiredImageIdx = 0;
bool               g_vkSceneRecording  = false;

float              g_vkClearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

// -----------------------------------------------------------------------
// Pipeline / drawing state
// -----------------------------------------------------------------------

// libshaderc compiler context — created once at startup, reused for every
// GR_Shader_Compile call.
shaderc_compiler_t   g_shadercCompiler = nullptr;

enum
{
	kVkDepthDisabled = 0,
	kVkDepthEnabled = 1,
	kVkDepthModeCount = 2,
	kVkBlendModeCount = 5
};

VkPipelineLayout     g_vkPipelineLayout = VK_NULL_HANDLE;
VkPipeline           g_vkPipelines[kVkDepthModeCount][kVkBlendModeCount] = {};
int                  g_vkBoundPipelineKey = -1;
int                  g_vkActiveBlendMode  = BM_NONE;
int                  g_vkActiveDepthMode  = kVkDepthEnabled;

static int GetPipelineKey(int depthMode, int blendMode)
{
	return depthMode * kVkBlendModeCount + blendMode;
}

// Default vertex/fragment shader modules used by the solid pipeline.
VkShaderModule       g_vkVertModule = VK_NULL_HANDLE;
VkShaderModule       g_vkFragModule = VK_NULL_HANDLE;

// VRAM image — 1024×512 of 16-bit packed pixels exposed to the shader as
// R8G8_UNORM (low byte = R, high byte = G). Mirrors what the GL backend
// uploads as GL_RG via GL_UNSIGNED_BYTE.
VkImage              g_vkVramImage      = VK_NULL_HANDLE;
VkDeviceMemory       g_vkVramImageMem   = VK_NULL_HANDLE;
VkImageView          g_vkVramImageView  = VK_NULL_HANDLE;
VkSampler            g_vkVramSampler    = VK_NULL_HANDLE;

// Staging buffers used to upload the CPU-side `vram[]` to the GPU image.
VkBuffer             g_vkVramStaging[kMaxFramesInFlight]    = {};
VkDeviceMemory       g_vkVramStagingMem[kMaxFramesInFlight] = {};
void*                g_vkVramStagingMap[kMaxFramesInFlight] = {};

// Descriptor set layout / pool / set bound to the pipeline at slot 0.
VkDescriptorSetLayout g_vkDescSetLayout = VK_NULL_HANDLE;
VkDescriptorPool      g_vkDescPool      = VK_NULL_HANDLE;
VkDescriptorSet       g_vkDescSet       = VK_NULL_HANDLE;

bool                 g_vkVramDirty      = true;

// Vertex buffers (host-visible, persistently mapped). Sized to PsyX's
// MAX_VERTEX_BUFFER_SIZE upper bound.
VkBuffer             g_vkVertexBuffer[kMaxFramesInFlight]    = {};
VkDeviceMemory       g_vkVertexBufferMem[kMaxFramesInFlight] = {};
void*                g_vkVertexBufferMap[kMaxFramesInFlight] = {};
uint32_t             g_vkVertexBufferSize = 0; // in vertices currently uploaded

// Push constants — both projection matrices in one block. Vulkan
// guarantees at least 128 bytes of push constant space (most desktop
// GPUs and MoltenVK expose more). The PGXP vertex path needs both the
// 2D ortho (HUD/menus) and the 3D perspective (world geometry); the
// vertex shader picks one based on a_zw.y > 100.
struct PushBlock {
	float Projection[16];    // 64 B — 2D ortho
	float Projection3D[16];  // 64 B — 3D perspective (PGXP world path)
};
static_assert(sizeof(PushBlock) == 128, "Push constants tuned to the 128 B core minimum");

PushBlock           g_vkPushBlock = {
	// Identity for both projections so the very first frames (before the
	// game calls GR_Ortho2D / GR_Perspective3D) don't collapse every
	// vertex onto the origin via a zero matrix.
	{ 1,0,0,0,  0,1,0,0,  0,0,1,0,  0,0,0,1 },
	{ 1,0,0,0,  0,1,0,0,  0,0,1,0,  0,0,0,1 },
};

// CPU-side VRAM mirror — same layout as the GL backend's `vram[]`.
unsigned short g_vkVramCPU[VRAM_WIDTH * VRAM_HEIGHT] = {};

// =======================================================================
// Helpers
// =======================================================================

#define VK_CHECK(expr)                                                \
	do {                                                              \
		VkResult _r = (expr);                                         \
		if (_r != VK_SUCCESS) {                                       \
			eprinterr("Vulkan call failed: %s -> %d\n", #expr, _r);   \
		}                                                             \
	} while (0)

bool VkDiagEnabled()
{
	static int cached = -1;
	if (cached < 0) {
		const char* value = getenv("REDRIVER2_VK_DIAG");
		cached = (value && atoi(value) != 0) ? 1 : 0;
	}
	return cached != 0;
}

int VkDiagInterval()
{
	static int cached = -1;
	if (cached < 0) {
		const char* value = getenv("REDRIVER2_VK_DIAG_INTERVAL");
		cached = value ? atoi(value) : 60;
		if (cached < 1)
			cached = 1;
	}
	return cached;
}

bool VkDrawLogEnabled()
{
	static int cached = -1;
	if (cached < 0) {
		const char* value = getenv("REDRIVER2_VK_DRAW_LOG");
		cached = (value && atoi(value) != 0) ? 1 : 0;
	}
	return cached != 0;
}

int VkCaptureFrame()
{
	static int cached = -2;
	if (cached == -2) {
		const char* value = getenv("REDRIVER2_VK_CAPTURE_FRAME");
		cached = value ? atoi(value) : -1;
	}
	return cached;
}

const char* VkCapturePath()
{
	const char* path = getenv("REDRIVER2_VK_CAPTURE_PATH");
	return path ? path : "REDRIVER2_VK_CAPTURE.bmp";
}

struct VkDiagFrameStats {
	uint64_t draws;
	uint64_t triangles;
	uint64_t depthEnabledDraws;
	uint64_t depthDisabledDraws;
	uint64_t depthEnabledTriangles;
	uint64_t depthDisabledTriangles;
	uint64_t pgxpTriangles;
	uint64_t flatTriangles;
	uint64_t mixedTriangles;
	uint64_t pgxpWithoutDepth;
	uint64_t flatWithDepth;
	uint64_t rejectLeft;
	uint64_t rejectRight;
	uint64_t rejectTop;
	uint64_t rejectBottom;
	uint64_t rejectNear;
	uint64_t rejectFar;
	uint64_t anyNonPositiveW;
	uint64_t allNonPositiveW;
	float minW;
	float maxW;
	float minZ;
	float maxZ;
	float minNdcZ;
	float maxNdcZ;
};

VkDiagFrameStats g_vkDiagStats = {};
uint64_t g_vkDiagFrame = 0;

void VkDiagResetFrame()
{
	memset(&g_vkDiagStats, 0, sizeof(g_vkDiagStats));
	g_vkDiagStats.minW = FLT_MAX;
	g_vkDiagStats.maxW = -FLT_MAX;
	g_vkDiagStats.minZ = FLT_MAX;
	g_vkDiagStats.maxZ = -FLT_MAX;
	g_vkDiagStats.minNdcZ = FLT_MAX;
	g_vkDiagStats.maxNdcZ = -FLT_MAX;
}

void VkDiagMulMat4Vec4(const float* m, const float* in, float* out)
{
	out[0] = m[0] * in[0] + m[4] * in[1] + m[8]  * in[2] + m[12] * in[3];
	out[1] = m[1] * in[0] + m[5] * in[1] + m[9]  * in[2] + m[13] * in[3];
	out[2] = m[2] * in[0] + m[6] * in[1] + m[10] * in[2] + m[14] * in[3];
	out[3] = m[3] * in[0] + m[7] * in[1] + m[11] * in[2] + m[15] * in[3];
}

bool VkDiagVertexIsPGXP(const GrVertex& v)
{
#if USE_PGXP
	return v.scr_h > 100.0f;
#else
	(void)v;
	return false;
#endif
}

void VkDiagVertexToClip(const GrVertex& v, float* out)
{
#if USE_PGXP
	const bool fmvLike = (v.z == 0.0f && v.scr_h == 0.0f && v.a == 0);
	if (fmvLike) {
		out[0] = v.x;
		out[1] = v.y;
		out[2] = 0.5f;
		out[3] = 1.0f;
	} else if (v.scr_h > 100.0f) {
		const float in[4] = {
			(v.x + 0.5f) * v.scr_h,
			(v.y + 0.5f) * -v.scr_h,
			v.z,
			1.0f
		};
		VkDiagMulMat4Vec4(g_vkPushBlock.Projection3D, in, out);
		out[0] += v.ofsX * out[3];
		out[1] += -v.ofsY * out[3];
	} else {
		const float in[4] = { v.x, v.y, 0.5f, 1.0f };
		VkDiagMulMat4Vec4(g_vkPushBlock.Projection, in, out);
	}
#else
	const float in[4] = { (float)v.x, (float)v.y, 0.5f, 1.0f };
	VkDiagMulMat4Vec4(g_vkPushBlock.Projection, in, out);
#endif
	out[2] = (out[2] + out[3]) * 0.5f;
}

void VkDiagAccumulateDraw(int startVertex, int triangles)
{
	if (!VkDiagEnabled() || triangles <= 0)
		return;

	const GrVertex* vertices = (const GrVertex*)g_vkVertexBufferMap[g_vkCurrentFrame];
	if (!vertices)
		return;

	const int maxTriangles = std::max(0, ((int)g_vkVertexBufferSize - startVertex) / 3);
	triangles = std::min(triangles, maxTriangles);
	if (triangles <= 0)
		return;

	g_vkDiagStats.draws++;
	g_vkDiagStats.triangles += (uint64_t)triangles;

	const int depthIdx = (g_vkActiveDepthMode == kVkDepthEnabled && g_cfg_pgxpZBuffer)
		? kVkDepthEnabled : kVkDepthDisabled;

	if (depthIdx == kVkDepthEnabled) {
		g_vkDiagStats.depthEnabledDraws++;
		g_vkDiagStats.depthEnabledTriangles += (uint64_t)triangles;
	} else {
		g_vkDiagStats.depthDisabledDraws++;
		g_vkDiagStats.depthDisabledTriangles += (uint64_t)triangles;
	}

	for (int i = 0; i < triangles; ++i) {
		const GrVertex* tri = vertices + startVertex + i * 3;
		float clip[3][4];
		int pgxpVerts = 0;
		int nonPositiveW = 0;

		for (int v = 0; v < 3; ++v) {
			if (VkDiagVertexIsPGXP(tri[v]))
				pgxpVerts++;

			VkDiagVertexToClip(tri[v], clip[v]);

			if (clip[v][3] <= 0.0f)
				nonPositiveW++;

			g_vkDiagStats.minW = std::min(g_vkDiagStats.minW, clip[v][3]);
			g_vkDiagStats.maxW = std::max(g_vkDiagStats.maxW, clip[v][3]);
			g_vkDiagStats.minZ = std::min(g_vkDiagStats.minZ, clip[v][2]);
			g_vkDiagStats.maxZ = std::max(g_vkDiagStats.maxZ, clip[v][2]);

			if (fabsf(clip[v][3]) > 0.000001f) {
				const float ndcZ = clip[v][2] / clip[v][3];
				g_vkDiagStats.minNdcZ = std::min(g_vkDiagStats.minNdcZ, ndcZ);
				g_vkDiagStats.maxNdcZ = std::max(g_vkDiagStats.maxNdcZ, ndcZ);
			}
		}

		if (pgxpVerts == 3) {
			g_vkDiagStats.pgxpTriangles++;
			if (depthIdx != kVkDepthEnabled)
				g_vkDiagStats.pgxpWithoutDepth++;
		} else if (pgxpVerts == 0) {
			g_vkDiagStats.flatTriangles++;
			if (depthIdx == kVkDepthEnabled)
				g_vkDiagStats.flatWithDepth++;
		} else {
			g_vkDiagStats.mixedTriangles++;
		}

		if (nonPositiveW > 0)
			g_vkDiagStats.anyNonPositiveW++;
		if (nonPositiveW == 3)
			g_vkDiagStats.allNonPositiveW++;

		if (clip[0][0] < -clip[0][3] && clip[1][0] < -clip[1][3] && clip[2][0] < -clip[2][3])
			g_vkDiagStats.rejectLeft++;
		if (clip[0][0] > clip[0][3] && clip[1][0] > clip[1][3] && clip[2][0] > clip[2][3])
			g_vkDiagStats.rejectRight++;
		if (clip[0][1] < -clip[0][3] && clip[1][1] < -clip[1][3] && clip[2][1] < -clip[2][3])
			g_vkDiagStats.rejectTop++;
		if (clip[0][1] > clip[0][3] && clip[1][1] > clip[1][3] && clip[2][1] > clip[2][3])
			g_vkDiagStats.rejectBottom++;
		if (clip[0][2] < 0.0f && clip[1][2] < 0.0f && clip[2][2] < 0.0f)
			g_vkDiagStats.rejectNear++;
		if (clip[0][2] > clip[0][3] && clip[1][2] > clip[1][3] && clip[2][2] > clip[2][3])
			g_vkDiagStats.rejectFar++;
	}
}

void VkDiagLogFrame()
{
	if (!VkDiagEnabled() || g_vkDiagStats.triangles == 0)
		return;

	if ((g_vkDiagFrame % (uint64_t)VkDiagInterval()) != 0)
		return;

	eprintinfo("[VKDIAG][frame=%llu] draws=%llu tris=%llu depth on/off draws=%llu/%llu tris=%llu/%llu pgxp=%llu flat=%llu mixed=%llu bad pgxpNoDepth/flatDepth=%llu/%llu "
		"clipReject L/R/T/B/N/F=%llu/%llu/%llu/%llu/%llu/%llu w<=0 any/all=%llu/%llu "
		"clipW=[%.3f %.3f] clipZ=[%.3f %.3f] ndcZ=[%.3f %.3f]\n",
		(unsigned long long)g_vkDiagFrame,
		(unsigned long long)g_vkDiagStats.draws,
		(unsigned long long)g_vkDiagStats.triangles,
		(unsigned long long)g_vkDiagStats.depthEnabledDraws,
		(unsigned long long)g_vkDiagStats.depthDisabledDraws,
		(unsigned long long)g_vkDiagStats.depthEnabledTriangles,
		(unsigned long long)g_vkDiagStats.depthDisabledTriangles,
		(unsigned long long)g_vkDiagStats.pgxpTriangles,
		(unsigned long long)g_vkDiagStats.flatTriangles,
		(unsigned long long)g_vkDiagStats.mixedTriangles,
		(unsigned long long)g_vkDiagStats.pgxpWithoutDepth,
		(unsigned long long)g_vkDiagStats.flatWithDepth,
		(unsigned long long)g_vkDiagStats.rejectLeft,
		(unsigned long long)g_vkDiagStats.rejectRight,
		(unsigned long long)g_vkDiagStats.rejectTop,
		(unsigned long long)g_vkDiagStats.rejectBottom,
		(unsigned long long)g_vkDiagStats.rejectNear,
		(unsigned long long)g_vkDiagStats.rejectFar,
		(unsigned long long)g_vkDiagStats.anyNonPositiveW,
		(unsigned long long)g_vkDiagStats.allNonPositiveW,
		(double)g_vkDiagStats.minW,
		(double)g_vkDiagStats.maxW,
		(double)g_vkDiagStats.minZ,
		(double)g_vkDiagStats.maxZ,
		(double)g_vkDiagStats.minNdcZ,
		(double)g_vkDiagStats.maxNdcZ);
}

uint32_t FindMemoryTypeIndex(uint32_t typeBits, VkMemoryPropertyFlags wanted)
{
	for (uint32_t i = 0; i < g_vkMemProps.memoryTypeCount; ++i) {
		if ((typeBits & (1u << i)) &&
			(g_vkMemProps.memoryTypes[i].propertyFlags & wanted) == wanted) {
			return i;
		}
	}
	eprinterr("FindMemoryTypeIndex: no compatible heap (typeBits=0x%x wanted=0x%x)\n",
		typeBits, (unsigned)wanted);
	return 0;
}

bool CreateImage(uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage,
                 VkImage* outImg, VkDeviceMemory* outMem)
{
	VkImageCreateInfo ci = {};
	ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	ci.imageType = VK_IMAGE_TYPE_2D;
	ci.format = format;
	ci.extent = { w, h, 1 };
	ci.mipLevels = 1;
	ci.arrayLayers = 1;
	ci.samples = VK_SAMPLE_COUNT_1_BIT;
	ci.tiling = VK_IMAGE_TILING_OPTIMAL;
	ci.usage = usage;
	ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VK_CHECK(vkCreateImage(g_vkDevice, &ci, nullptr, outImg));

	VkMemoryRequirements mr;
	vkGetImageMemoryRequirements(g_vkDevice, *outImg, &mr);
	VkMemoryAllocateInfo ai = {};
	ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	ai.allocationSize = mr.size;
	ai.memoryTypeIndex = FindMemoryTypeIndex(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	VK_CHECK(vkAllocateMemory(g_vkDevice, &ai, nullptr, outMem));
	VK_CHECK(vkBindImageMemory(g_vkDevice, *outImg, *outMem, 0));
	return true;
}

bool CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                  VkMemoryPropertyFlags memProps,
                  VkBuffer* outBuffer, VkDeviceMemory* outMem)
{
	VkBufferCreateInfo bi = {};
	bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bi.size = size;
	bi.usage = usage;
	bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VK_CHECK(vkCreateBuffer(g_vkDevice, &bi, nullptr, outBuffer));

	VkMemoryRequirements mr;
	vkGetBufferMemoryRequirements(g_vkDevice, *outBuffer, &mr);

	VkMemoryAllocateInfo ai = {};
	ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	ai.allocationSize = mr.size;
	ai.memoryTypeIndex = FindMemoryTypeIndex(mr.memoryTypeBits, memProps);
	VK_CHECK(vkAllocateMemory(g_vkDevice, &ai, nullptr, outMem));

	VK_CHECK(vkBindBufferMemory(g_vkDevice, *outBuffer, *outMem, 0));
	return true;
}

void SwapchainImageBarrier(VkCommandBuffer cb,
                           VkImageLayout oldLayout, VkImageLayout newLayout,
                           VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                           VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage)
{
	VkImageMemoryBarrier b = {};
	b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	b.oldLayout = oldLayout;
	b.newLayout = newLayout;
	b.srcAccessMask = srcAccess;
	b.dstAccessMask = dstAccess;
	b.image = g_vkSwapchainImages[g_vkAcquiredImageIdx];
	b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	b.subresourceRange.levelCount = 1;
	b.subresourceRange.layerCount = 1;
	vkCmdPipelineBarrier(cb, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

bool WriteBgraBmp(const char* path, const unsigned char* pixels, uint32_t width, uint32_t height)
{
	FILE* fp = fopen(path, "wb");
	if (!fp)
		return false;

	const uint32_t pixelBytes = width * height * 4;
	const uint32_t fileBytes = 14 + 40 + pixelBytes;
	const int32_t topDownHeight = -(int32_t)height;
	unsigned char fileHeader[14] = {
		'B', 'M',
		(unsigned char)(fileBytes), (unsigned char)(fileBytes >> 8), (unsigned char)(fileBytes >> 16), (unsigned char)(fileBytes >> 24),
		0, 0, 0, 0,
		54, 0, 0, 0
	};
	unsigned char infoHeader[40] = {};
	infoHeader[0] = 40;
	infoHeader[4] = (unsigned char)(width);
	infoHeader[5] = (unsigned char)(width >> 8);
	infoHeader[6] = (unsigned char)(width >> 16);
	infoHeader[7] = (unsigned char)(width >> 24);
	infoHeader[8] = (unsigned char)(topDownHeight);
	infoHeader[9] = (unsigned char)(topDownHeight >> 8);
	infoHeader[10] = (unsigned char)(topDownHeight >> 16);
	infoHeader[11] = (unsigned char)(topDownHeight >> 24);
	infoHeader[12] = 1;
	infoHeader[14] = 32;

	fwrite(fileHeader, 1, sizeof(fileHeader), fp);
	fwrite(infoHeader, 1, sizeof(infoHeader), fp);
	fwrite(pixels, 1, pixelBytes, fp);
	fclose(fp);
	return true;
}

void ConvertSwapchainPixelsToBgra(unsigned char* pixels, uint32_t width, uint32_t height)
{
	if (g_vkSwapchainFormat == VK_FORMAT_B8G8R8A8_UNORM ||
		g_vkSwapchainFormat == VK_FORMAT_B8G8R8A8_SRGB)
		return;

	if (g_vkSwapchainFormat == VK_FORMAT_R8G8B8A8_UNORM ||
		g_vkSwapchainFormat == VK_FORMAT_R8G8B8A8_SRGB) {
		for (uint32_t i = 0; i < width * height; ++i) {
			unsigned char* p = pixels + i * 4;
			std::swap(p[0], p[2]);
		}
	}
}

VkShaderModule CreateShaderModule(const uint32_t* spirv, size_t bytes)
{
	VkShaderModuleCreateInfo ci = {};
	ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	ci.codeSize = bytes;
	ci.pCode    = spirv;
	VkShaderModule m = VK_NULL_HANDLE;
	VK_CHECK(vkCreateShaderModule(g_vkDevice, &ci, nullptr, &m));
	return m;
}

VkShaderModule CompileGLSL(const char* source, shaderc_shader_kind kind, const char* tag)
{
	if (!g_shadercCompiler) {
		eprinterr("CompileGLSL called before shaderc init\n");
		return VK_NULL_HANDLE;
	}
	shaderc_compile_options_t opts = shaderc_compile_options_initialize();
	shaderc_compile_options_set_optimization_level(opts, shaderc_optimization_level_performance);
	shaderc_compile_options_set_target_env(opts, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_1);
	shaderc_compile_options_set_source_language(opts, shaderc_source_language_glsl);

	shaderc_compilation_result_t res = shaderc_compile_into_spv(
		g_shadercCompiler, source, strlen(source), kind, tag, "main", opts);

	shaderc_compilation_status status = shaderc_result_get_compilation_status(res);
	if (status != shaderc_compilation_status_success) {
		eprinterr("shaderc[%s] failed: %s\n", tag, shaderc_result_get_error_message(res));
		shaderc_result_release(res);
		shaderc_compile_options_release(opts);
		return VK_NULL_HANDLE;
	}

	const uint32_t* code = (const uint32_t*)shaderc_result_get_bytes(res);
	size_t bytes = shaderc_result_get_length(res);
	VkShaderModule mod = CreateShaderModule(code, bytes);

	shaderc_result_release(res);
	shaderc_compile_options_release(opts);
	return mod;
}

// -----------------------------------------------------------------------
// Default shaders (PSX-vertex layout)
// -----------------------------------------------------------------------
//
// GrVertex layout (see PsyX_render.h):
//   #if USE_PGXP -> position is (x,y,page,clut, z,scr_h,ofsX,ofsY) as floats
//   else         -> (x,y,page,clut) as int16
//   then  u,v,bright,dither (u8x4)
//   then  r,g,b,a            (u8x4)
//   then  tcx,tcy,_p0,_p1    (i8x4)
//
// PsyX is built without USE_PGXP for now (the engine uses the int16 path),
// so we declare matching attribute formats below. Texture sampling is
// stubbed — for this revision the fragment shader just outputs vertex
// colour directly. That is enough to start seeing world geometry.

// PSX vertex shader (PGXP variant) and fragment shader.
//
// GrVertex layout with USE_PGXP=1:
//   loc 0 (a_position) : float x, y, page, clut
//   loc 1 (a_zw)       : float z, scr_h, ofsX, ofsY
//   loc 2 (a_texcoord) : u8 raw  u, v, bright, dither   (0..255)
//   loc 3 (a_color)    : u8 norm r, g, b, a             (0..1)
//   loc 4 (a_extra)    : i8 raw  tcx, tcy, _p0, _p1     (-128..127)
//
// The fragment shader implements PSX paletted texture sampling against a
// 1024×512 VRAM image exposed as R8G8_UNORM (low byte in .r, high byte
// in .g — matches the GL backend's GL_RG / GL_UNSIGNED_BYTE upload).
// Texture format is encoded in `clut` (0 = 4-bit, 1 = 8-bit, 2+ = 16-bit
// direct). For a first pass the menu UI tends to be 8-bit.
const char* kBasicVertSrc = R"GLSL(
#version 450
layout(location = 0) in vec4 a_position;
layout(location = 1) in vec4 a_zw;
layout(location = 2) in vec4 a_texcoord;
layout(location = 3) in vec4 a_color;
layout(location = 4) in vec4 a_extra;

layout(push_constant) uniform PB {
    mat4 Projection;
    mat4 Projection3D;
} pb;

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec4 v_texcoord;
layout(location = 2) out vec4 v_page_clut;

void main() {
    bool fmvLike = (a_zw.x == 0.0 && a_zw.y == 0.0 && a_color.a < 0.001);
    if (fmvLike) {
        gl_Position = vec4(a_position.xy, 0.5, 1.0);
    } else if (a_zw.y > 100.0) {
        const vec2 geom_ofs = vec2(0.5, 0.5);
        vec4 p = pb.Projection3D * vec4((a_position.xy + geom_ofs) *
                                          vec2(1.0, -1.0) * a_zw.y,
                                         a_zw.x, 1.0);
        p.x += a_zw.z * p.w;
        p.y += -a_zw.w * p.w;
        gl_Position = p;
    } else {
        gl_Position = pb.Projection * vec4(a_position.xy, 0.5, 1.0);
    }

    gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;

    v_color = a_color;
    v_texcoord = a_texcoord;

    // page → pixel coords, clut → NORMALISED [0, 1] coords.
    float pageRaw = a_position.z;
    float clutRaw = a_position.w;
    v_page_clut.x = fract(pageRaw / 16.0) * 1024.0;
    v_page_clut.y = floor(pageRaw / 16.0) * 256.0;
    v_page_clut.z = fract(clutRaw / 64.0);
    v_page_clut.w = floor(clutRaw / 64.0) / 512.0;
    // [A] Sub-texel bias (matches GL backend's c_UVFudge): nudge clut UV
    // away from a texel boundary so NEAREST sampling can't drift one
    // texel left and pull index-0 (transparency mask) for 4-bit STP=1
    // sprites like the car wheels. zw is normalised, x/y are pixel-space
    // and already have the integer-pixel center implied by R8G8 sampling.
    v_page_clut.zw += vec2(0.00025, 0.00025);
    v_texcoord.w = mod(floor(pageRaw / 128.0), 4.0);
}
)GLSL";

const char* kBasicFragSrc = R"GLSL(
#version 450
// 1:1 port of the GL backend's GPU_FRAGMENT_SAMPLE_SHADER (4/8/16-bit
// variants merged into one branched routine, since we don't have multi-
// pipeline shader switching yet). VRAM is exposed as R8G8_UNORM:
// low byte → .r, high byte → .g (matches GL's GL_RG / GL_UNSIGNED_BYTE
// upload of the engine's `unsigned short vram[]`).
// v_texcoord.w doubles as the texture format (0=4-bit, 1=8-bit, 2=16-bit)
// — the vertex shader writes it there. Frees us from a separate varying
// slot at the cost of overwriting the dither index, which we don't sample
// in this revision anyway.
layout(location = 0) in vec4 v_color;
layout(location = 1) in vec4 v_texcoord;
layout(location = 2) in vec4 v_page_clut;

layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D s_vram;

const vec2  c_VRAMTexel = vec2(1.0 / 1024.0, 1.0 / 512.0);
const float c_PackRange = 255.001;

// Inline the GL helpers verbatim — same pack / decode arithmetic.
float packRG(vec2 rg) {
    return (rg.y * 256.0 + rg.x) * c_PackRange;
}
vec4 decodeRG(float rg) {
    vec4 value = fract(floor(rg / vec4(1.0, 32.0, 1024.0, 32768.0)) / 32.0);
    return vec4(value.xyz, rg == 0.0 ? rg : (1.0 - value.w * 16.0));
}
vec2 VRAM(vec2 uv) { return texture(s_vram, uv).rg; }

float samplePSX(vec2 tc) {
    float fmt = v_texcoord.w;
    if (fmt < 0.5) {
        // 4-bit paletted: 4 pixels per VRAM cell
        vec2 uv = (tc * vec2(0.25, 1.0) + v_page_clut.xy) * c_VRAMTexel;
        vec2 comp = VRAM(uv);
        int  index = int(fract(tc.x / 4.0 + 0.0001) * 4.0);
        // pull the right nibble from the (low, high) bytes
        float v = (index < 2 ? comp.x : comp.y) * (c_PackRange / 16.0);
        float f = floor(v);
        vec2 c = vec2((v - f) * 16.0, f);
        vec2 clut_pos = v_page_clut.zw;
        clut_pos.x += mix(c[0], c[1], mod(float(index), 2.0)) * c_VRAMTexel.x;
        return packRG(VRAM(clut_pos));
    } else if (fmt < 1.5) {
        // 8-bit paletted: 2 pixels per VRAM cell
        vec2 uv = (tc * vec2(0.5, 1.0) + v_page_clut.xy) * c_VRAMTexel;
        vec2 comp = VRAM(uv);
        vec2 clut_pos = v_page_clut.zw;
        int index = int(mod(tc.x, 2.0));
        clut_pos.x += (index == 0 ? comp.x : comp.y) * c_PackRange * c_VRAMTexel.x;
        return packRG(VRAM(clut_pos));
    } else {
        // 16-bit direct
        vec2 uv = (tc + v_page_clut.xy) * c_VRAMTexel;
        return packRG(VRAM(uv));
    }
}

void main() {
    // Untextured fill (engine sets bright=1 for these via
    // MakeTexcoordTriangleZero/QuadZero); flat-shaded vertex colour
    // doubled so PSX-neutral rgba=128 (=0.5 normalised) lands at 1.0.
    if (v_texcoord.z < 1.5) {
        out_color = vec4(min(v_color.rgb * 2.0, vec3(1.0)), 1.0);
        return;
    }

    float color_16 = samplePSX(v_texcoord.xy);
    if (color_16 == 0.0) discard;
    vec4 texel = decodeRG(color_16);

    // Same x2 modulation as the untextured path so neutral PSX vertex
    // colour (rgba=128) doesn't dim the texture by 50 %.
    out_color = vec4(texel.rgb * min(v_color.rgb * 2.0, vec3(1.0)), 1.0);
}
)GLSL";

// -----------------------------------------------------------------------
// Initialisation chain
// -----------------------------------------------------------------------

bool PickPhysicalDeviceAndQueueFamilies()
{
	uint32_t count = 0;
	vkEnumeratePhysicalDevices(g_vkInstance, &count, nullptr);
	if (count == 0) {
		eprinterr("No Vulkan-capable physical devices found\n");
		return false;
	}
	std::vector<VkPhysicalDevice> devices(count);
	vkEnumeratePhysicalDevices(g_vkInstance, &count, devices.data());

	VkPhysicalDevice chosen = VK_NULL_HANDLE;
	for (auto d : devices) {
		VkPhysicalDeviceProperties props;
		vkGetPhysicalDeviceProperties(d, &props);
		if (chosen == VK_NULL_HANDLE) chosen = d;
		if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
			chosen = d;
			break;
		}
	}
	g_vkPhysicalDevice = chosen;

	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(g_vkPhysicalDevice, &props);
	eprintinfo("Vulkan device: %s (API %u.%u.%u)\n",
		props.deviceName,
		VK_VERSION_MAJOR(props.apiVersion),
		VK_VERSION_MINOR(props.apiVersion),
		VK_VERSION_PATCH(props.apiVersion));

	vkGetPhysicalDeviceMemoryProperties(g_vkPhysicalDevice, &g_vkMemProps);

	uint32_t qfCount = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(g_vkPhysicalDevice, &qfCount, nullptr);
	std::vector<VkQueueFamilyProperties> qfProps(qfCount);
	vkGetPhysicalDeviceQueueFamilyProperties(g_vkPhysicalDevice, &qfCount, qfProps.data());

	bool foundGfx = false, foundPresent = false;
	for (uint32_t i = 0; i < qfCount; ++i) {
		if (!foundGfx && (qfProps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
			g_vkGraphicsQueueFamily = i;
			foundGfx = true;
		}
		VkBool32 presentSupport = VK_FALSE;
		vkGetPhysicalDeviceSurfaceSupportKHR(g_vkPhysicalDevice, i, g_vkSurface, &presentSupport);
		if (!foundPresent && presentSupport) {
			g_vkPresentQueueFamily = i;
			foundPresent = true;
		}
	}
	if (!foundGfx || !foundPresent) {
		eprinterr("Vulkan: no suitable queue families (gfx=%d present=%d)\n",
			(int)foundGfx, (int)foundPresent);
		return false;
	}
	return true;
}

bool CreateLogicalDevice()
{
	float qPriority = 1.0f;
	std::vector<VkDeviceQueueCreateInfo> queueInfos;
	std::vector<uint32_t> uniqueFamilies = { g_vkGraphicsQueueFamily };
	if (g_vkPresentQueueFamily != g_vkGraphicsQueueFamily) {
		uniqueFamilies.push_back(g_vkPresentQueueFamily);
	}
	for (uint32_t fam : uniqueFamilies) {
		VkDeviceQueueCreateInfo qi = {};
		qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		qi.queueFamilyIndex = fam;
		qi.queueCount = 1;
		qi.pQueuePriorities = &qPriority;
		queueInfos.push_back(qi);
	}

	std::vector<const char*> deviceExts = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
#if defined(__APPLE__)
	deviceExts.push_back("VK_KHR_portability_subset");
#endif

	VkPhysicalDeviceFeatures features = {};

	VkDeviceCreateInfo ci = {};
	ci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	ci.queueCreateInfoCount = (uint32_t)queueInfos.size();
	ci.pQueueCreateInfos    = queueInfos.data();
	ci.enabledExtensionCount = (uint32_t)deviceExts.size();
	ci.ppEnabledExtensionNames = deviceExts.data();
	ci.pEnabledFeatures = &features;
	VK_CHECK(vkCreateDevice(g_vkPhysicalDevice, &ci, nullptr, &g_vkDevice));
	if (g_vkDevice == VK_NULL_HANDLE) return false;

	vkGetDeviceQueue(g_vkDevice, g_vkGraphicsQueueFamily, 0, &g_vkGraphicsQueue);
	vkGetDeviceQueue(g_vkDevice, g_vkPresentQueueFamily, 0, &g_vkPresentQueue);
	return true;
}

bool CreateSwapchain(int width, int height)
{
	VkSurfaceCapabilitiesKHR caps;
	VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_vkPhysicalDevice, g_vkSurface, &caps));

	uint32_t fmtCount = 0;
	vkGetPhysicalDeviceSurfaceFormatsKHR(g_vkPhysicalDevice, g_vkSurface, &fmtCount, nullptr);
	std::vector<VkSurfaceFormatKHR> formats(fmtCount);
	vkGetPhysicalDeviceSurfaceFormatsKHR(g_vkPhysicalDevice, g_vkSurface, &fmtCount, formats.data());

	VkSurfaceFormatKHR chosen = formats[0];
	for (auto& f : formats) {
		if (f.format == VK_FORMAT_B8G8R8A8_UNORM &&
			f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
			chosen = f; break;
		}
	}
	g_vkSwapchainFormat = chosen.format;

	VkExtent2D extent;
	if (caps.currentExtent.width != 0xFFFFFFFFu) {
		extent = caps.currentExtent;
	} else {
		extent.width  = std::max(caps.minImageExtent.width,  std::min(caps.maxImageExtent.width,  (uint32_t)width));
		extent.height = std::max(caps.minImageExtent.height, std::min(caps.maxImageExtent.height, (uint32_t)height));
	}
	g_vkSwapchainExtent = extent;

	uint32_t imgCount = caps.minImageCount + 1;
	if (caps.maxImageCount > 0 && imgCount > caps.maxImageCount)
		imgCount = caps.maxImageCount;

	VkSwapchainCreateInfoKHR ci = {};
	ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	ci.surface = g_vkSurface;
	ci.minImageCount = imgCount;
	ci.imageFormat = chosen.format;
	ci.imageColorSpace = chosen.colorSpace;
	ci.imageExtent = extent;
	ci.imageArrayLayers = 1;
	ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	g_vkSwapchainCanReadback = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
	if (g_vkSwapchainCanReadback)
		ci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

	uint32_t qf[2] = { g_vkGraphicsQueueFamily, g_vkPresentQueueFamily };
	if (g_vkGraphicsQueueFamily != g_vkPresentQueueFamily) {
		ci.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
		ci.queueFamilyIndexCount = 2;
		ci.pQueueFamilyIndices = qf;
	} else {
		ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	}

	ci.preTransform = caps.currentTransform;
	ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
	ci.clipped = VK_TRUE;
	VK_CHECK(vkCreateSwapchainKHR(g_vkDevice, &ci, nullptr, &g_vkSwapchain));
	if (g_vkSwapchain == VK_NULL_HANDLE) return false;

	uint32_t actualCount = 0;
	vkGetSwapchainImagesKHR(g_vkDevice, g_vkSwapchain, &actualCount, nullptr);
	g_vkSwapchainImages.resize(actualCount);
	vkGetSwapchainImagesKHR(g_vkDevice, g_vkSwapchain, &actualCount, g_vkSwapchainImages.data());

	g_vkSwapchainImageViews.resize(actualCount);
	for (uint32_t i = 0; i < actualCount; ++i) {
		VkImageViewCreateInfo vi = {};
		vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		vi.image = g_vkSwapchainImages[i];
		vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vi.format = g_vkSwapchainFormat;
		vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		vi.subresourceRange.levelCount = 1;
		vi.subresourceRange.layerCount = 1;
		VK_CHECK(vkCreateImageView(g_vkDevice, &vi, nullptr, &g_vkSwapchainImageViews[i]));
	}
	return true;
}

bool CreateClearRenderPass()
{
	VkAttachmentDescription atts[2] = {};
	// 0: colour
	atts[0].format = g_vkSwapchainFormat;
	atts[0].samples = VK_SAMPLE_COUNT_1_BIT;
	atts[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	atts[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	atts[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	atts[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	atts[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	atts[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	// 1: depth
	atts[1].format = g_vkDepthFormat;
	atts[1].samples = VK_SAMPLE_COUNT_1_BIT;
	atts[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	atts[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	atts[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	atts[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	atts[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	atts[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	VkAttachmentReference colorRef = {};
	colorRef.attachment = 0;
	colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	VkAttachmentReference depthRef = {};
	depthRef.attachment = 1;
	depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	VkSubpassDescription subpass = {};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &colorRef;
	subpass.pDepthStencilAttachment = &depthRef;

	VkSubpassDependency dep = {};
	dep.srcSubpass = VK_SUBPASS_EXTERNAL;
	dep.dstSubpass = 0;
	dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
	                   VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
	dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
	                   VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
	dep.srcAccessMask = 0;
	dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
	                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

	VkRenderPassCreateInfo rp = {};
	rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	rp.attachmentCount = 2;
	rp.pAttachments = atts;
	rp.subpassCount = 1;
	rp.pSubpasses = &subpass;
	rp.dependencyCount = 1;
	rp.pDependencies = &dep;
	VK_CHECK(vkCreateRenderPass(g_vkDevice, &rp, nullptr, &g_vkClearRenderPass));
	return g_vkClearRenderPass != VK_NULL_HANDLE;
}

bool CreateDepthBuffer()
{
	if (!CreateImage(g_vkSwapchainExtent.width, g_vkSwapchainExtent.height,
		g_vkDepthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
		&g_vkDepthImage, &g_vkDepthImageMem)) {
		return false;
	}
	VkImageViewCreateInfo vi = {};
	vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	vi.image = g_vkDepthImage;
	vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vi.format = g_vkDepthFormat;
	vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
	vi.subresourceRange.levelCount = 1;
	vi.subresourceRange.layerCount = 1;
	VK_CHECK(vkCreateImageView(g_vkDevice, &vi, nullptr, &g_vkDepthImageView));
	return true;
}

bool CreateFramebuffers()
{
	g_vkFramebuffers.resize(g_vkSwapchainImageViews.size());
	for (size_t i = 0; i < g_vkSwapchainImageViews.size(); ++i) {
		VkImageView views[2] = { g_vkSwapchainImageViews[i], g_vkDepthImageView };
		VkFramebufferCreateInfo ci = {};
		ci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
		ci.renderPass = g_vkClearRenderPass;
		ci.attachmentCount = 2;
		ci.pAttachments = views;
		ci.width  = g_vkSwapchainExtent.width;
		ci.height = g_vkSwapchainExtent.height;
		ci.layers = 1;
		VK_CHECK(vkCreateFramebuffer(g_vkDevice, &ci, nullptr, &g_vkFramebuffers[i]));
	}
	return true;
}

bool CreateCommandPoolAndBuffers()
{
	VkCommandPoolCreateInfo pi = {};
	pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pi.queueFamilyIndex = g_vkGraphicsQueueFamily;
	VK_CHECK(vkCreateCommandPool(g_vkDevice, &pi, nullptr, &g_vkCommandPool));

	VkCommandBufferAllocateInfo ai = {};
	ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	ai.commandPool = g_vkCommandPool;
	ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	ai.commandBufferCount = kMaxFramesInFlight;
	VK_CHECK(vkAllocateCommandBuffers(g_vkDevice, &ai, g_vkCmdBuffers));
	return true;
}

bool CreateSyncObjects()
{
	VkSemaphoreCreateInfo si = {};
	si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	VkFenceCreateInfo fi = {};
	fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
	for (int i = 0; i < kMaxFramesInFlight; ++i) {
		VK_CHECK(vkCreateSemaphore(g_vkDevice, &si, nullptr, &g_vkImageAvailableSems[i]));
		VK_CHECK(vkCreateSemaphore(g_vkDevice, &si, nullptr, &g_vkRenderFinishedSems[i]));
		VK_CHECK(vkCreateFence(g_vkDevice, &fi, nullptr, &g_vkInFlightFences[i]));
	}
	return true;
}

// -----------------------------------------------------------------------
// VRAM image + sampler + descriptor set
// -----------------------------------------------------------------------

bool CreateVramImage()
{
	if (!CreateImage(VRAM_WIDTH, VRAM_HEIGHT, VK_FORMAT_R8G8_UNORM,
		VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
		&g_vkVramImage, &g_vkVramImageMem)) {
		return false;
	}

	VkImageViewCreateInfo vi = {};
	vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	vi.image = g_vkVramImage;
	vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vi.format = VK_FORMAT_R8G8_UNORM;
	vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	vi.subresourceRange.levelCount = 1;
	vi.subresourceRange.layerCount = 1;
	VK_CHECK(vkCreateImageView(g_vkDevice, &vi, nullptr, &g_vkVramImageView));

	VkSamplerCreateInfo si = {};
	si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	si.magFilter = VK_FILTER_NEAREST;
	si.minFilter = VK_FILTER_NEAREST;
	si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	// [A] REPEAT, NOT CLAMP. The GL backend's tpage→UV math (`floor(pageRaw/16)*256`
	// for page Y, `fract(pageRaw/16)*1024` for X) lets bits 5+ of tpage —
	// which encode ABR / TP / dither — leak into the address. GL silently
	// folds them back via the default GL_REPEAT wrap. With CLAMP_TO_EDGE
	// every textured primitive whose tpage has TP=1 (8-bit) or ABR>0 ends
	// up sampling row 511 of VRAM (the edge), so wheels/particles/anything
	// that uses semitransparency reads garbage and renders as solid blocks.
	si.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	si.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
	si.unnormalizedCoordinates = VK_FALSE;
	si.compareEnable = VK_FALSE;
	VK_CHECK(vkCreateSampler(g_vkDevice, &si, nullptr, &g_vkVramSampler));

	// One host-visible staging buffer per frame in flight. The CPU can start
	// preparing frame N+1 before frame N's transfer has consumed its source.
	const VkDeviceSize vramBytes = (VkDeviceSize)VRAM_WIDTH * VRAM_HEIGHT * sizeof(uint16_t);
	for (int i = 0; i < kMaxFramesInFlight; ++i) {
		if (!CreateBuffer(vramBytes,
			VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			&g_vkVramStaging[i], &g_vkVramStagingMem[i])) {
			return false;
		}
		VK_CHECK(vkMapMemory(g_vkDevice, g_vkVramStagingMem[i], 0, VK_WHOLE_SIZE, 0, &g_vkVramStagingMap[i]));
	}
	return true;
}

bool CreateDescriptorSet()
{
	VkDescriptorSetLayoutBinding b = {};
	b.binding = 0;
	b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	b.descriptorCount = 1;
	b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

	VkDescriptorSetLayoutCreateInfo lci = {};
	lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	lci.bindingCount = 1;
	lci.pBindings = &b;
	VK_CHECK(vkCreateDescriptorSetLayout(g_vkDevice, &lci, nullptr, &g_vkDescSetLayout));

	VkDescriptorPoolSize ps = {};
	ps.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	ps.descriptorCount = 1;
	VkDescriptorPoolCreateInfo pci = {};
	pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pci.maxSets = 1;
	pci.poolSizeCount = 1;
	pci.pPoolSizes = &ps;
	VK_CHECK(vkCreateDescriptorPool(g_vkDevice, &pci, nullptr, &g_vkDescPool));

	VkDescriptorSetAllocateInfo ai = {};
	ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	ai.descriptorPool = g_vkDescPool;
	ai.descriptorSetCount = 1;
	ai.pSetLayouts = &g_vkDescSetLayout;
	VK_CHECK(vkAllocateDescriptorSets(g_vkDevice, &ai, &g_vkDescSet));

	VkDescriptorImageInfo ii = {};
	ii.imageView = g_vkVramImageView;
	ii.sampler = g_vkVramSampler;
	ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

	VkWriteDescriptorSet w = {};
	w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	w.dstSet = g_vkDescSet;
	w.dstBinding = 0;
	w.descriptorCount = 1;
	w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	w.pImageInfo = &ii;
	vkUpdateDescriptorSets(g_vkDevice, 1, &w, 0, nullptr);
	return true;
}

// Helper: transitions the VRAM image between layouts inside the given
// command buffer. Used during initial transition to SHADER_READ_ONLY and
// when we copy from staging buffer.
void VramImageBarrier(VkCommandBuffer cb,
                      VkImageLayout oldL, VkImageLayout newL,
                      VkAccessFlags srcA, VkAccessFlags dstA,
                      VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage)
{
	VkImageMemoryBarrier b = {};
	b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	b.oldLayout = oldL;
	b.newLayout = newL;
	b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = g_vkVramImage;
	b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	b.subresourceRange.levelCount = 1;
	b.subresourceRange.layerCount = 1;
	b.srcAccessMask = srcA;
	b.dstAccessMask = dstA;
	vkCmdPipelineBarrier(cb, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

void TransitionVramToShaderRead(VkCommandBuffer cb)
{
	// Copy staging buffer -> VRAM image, leaving image in SHADER_READ_ONLY_OPTIMAL.
	VramImageBarrier(cb,
		VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		0, VK_ACCESS_TRANSFER_WRITE_BIT,
		VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

	VkBufferImageCopy r = {};
	r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	r.imageSubresource.layerCount = 1;
	r.imageExtent = { VRAM_WIDTH, VRAM_HEIGHT, 1 };
	vkCmdCopyBufferToImage(cb, g_vkVramStaging[g_vkCurrentFrame], g_vkVramImage,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);

	VramImageBarrier(cb,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
}

bool CreateBasicGraphicsPipeline()
{
	g_vkVertModule = CompileGLSL(kBasicVertSrc, shaderc_vertex_shader,   "psx_basic.vert");
	g_vkFragModule = CompileGLSL(kBasicFragSrc, shaderc_fragment_shader, "psx_basic.frag");
	if (!g_vkVertModule || !g_vkFragModule) {
		eprinterr("Failed to compile basic GLSL shaders\n");
		return false;
	}

	// Pipeline layout — push constants for projection + descriptor set 0
	// for the VRAM combined-image-sampler used by the fragment shader.
	VkPushConstantRange pcRange = {};
	pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	pcRange.offset = 0;
	pcRange.size = sizeof(PushBlock);

	VkPipelineLayoutCreateInfo plci = {};
	plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	plci.setLayoutCount = 1;
	plci.pSetLayouts = &g_vkDescSetLayout;
	plci.pushConstantRangeCount = 1;
	plci.pPushConstantRanges = &pcRange;
	VK_CHECK(vkCreatePipelineLayout(g_vkDevice, &plci, nullptr, &g_vkPipelineLayout));

	// Vertex input — match GrVertex with PGXP (USE_PGXP=1):
	//   loc 0 (a_position) : 4×float  (x, y, page, clut)
	//   loc 1 (a_zw)       : 4×float  (z, scr_h, ofsX, ofsY)
	//   loc 2 (a_texcoord) : 4×u8 raw 0..255 (u, v, bright, dither)
	//   loc 3 (a_color)    : 4×u8 normalised (r, g, b, a)
	//   loc 4 (a_extra)    : 4×i8 raw -128..127 (tcx, tcy, _p0, _p1)
	VkVertexInputBindingDescription vibind = {};
	vibind.binding = 0;
	vibind.stride = sizeof(GrVertex);
	vibind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

	VkVertexInputAttributeDescription viattr[5] = {};
	viattr[0].location = 0; viattr[0].binding = 0;
	viattr[0].format = VK_FORMAT_R32G32B32A32_SFLOAT;
	viattr[0].offset = offsetof(GrVertex, x);
	viattr[1].location = 1; viattr[1].binding = 0;
	viattr[1].format = VK_FORMAT_R32G32B32A32_SFLOAT;
	viattr[1].offset = offsetof(GrVertex, z);
	viattr[2].location = 2; viattr[2].binding = 0;
	viattr[2].format = VK_FORMAT_R8G8B8A8_USCALED;	// raw 0..255 as float
	viattr[2].offset = offsetof(GrVertex, u);
	viattr[3].location = 3; viattr[3].binding = 0;
	viattr[3].format = VK_FORMAT_R8G8B8A8_UNORM;	// 0..1
	viattr[3].offset = offsetof(GrVertex, r);
	viattr[4].location = 4; viattr[4].binding = 0;
	viattr[4].format = VK_FORMAT_R8G8B8A8_SSCALED;	// raw -128..127 as float
	viattr[4].offset = offsetof(GrVertex, tcx);

	VkPipelineVertexInputStateCreateInfo vi = {};
	vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vi.vertexBindingDescriptionCount = 1;
	vi.pVertexBindingDescriptions = &vibind;
	vi.vertexAttributeDescriptionCount = 5;
	vi.pVertexAttributeDescriptions = viattr;

	VkPipelineInputAssemblyStateCreateInfo ia = {};
	ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	VkPipelineViewportStateCreateInfo vp = {};
	vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	vp.viewportCount = 1;
	vp.scissorCount  = 1;

	VkPipelineRasterizationStateCreateInfo rs = {};
	rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rs.polygonMode = VK_POLYGON_MODE_FILL;
	rs.cullMode = VK_CULL_MODE_NONE;
	rs.lineWidth = 1.0f;

	VkPipelineMultisampleStateCreateInfo ms = {};
	ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	// Two depth-stencil states: opaque (BM_NONE) writes depth, every blend
	// mode reads depth but does NOT write it (mirrors GR_EnableDepth(0)
	// that the GL backend issues whenever the blend mode is non-NONE —
	// keeps semi-trans polygons from punching holes in opaque ones in
	// the depth buffer).
	VkPipelineDepthStencilStateCreateInfo dsOpaque = {};
	dsOpaque.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	dsOpaque.depthTestEnable = VK_TRUE;
	dsOpaque.depthWriteEnable = VK_TRUE;
	dsOpaque.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

		VkPipelineDepthStencilStateCreateInfo dsBlend = dsOpaque;
		dsBlend.depthWriteEnable = VK_FALSE;

		VkPipelineDepthStencilStateCreateInfo dsDisabled = dsOpaque;
		dsDisabled.depthTestEnable = VK_FALSE;
		dsDisabled.depthWriteEnable = VK_FALSE;

	// PSX blend modes — see GP0 ABR field. Mirror exactly what the GL
	// backend does in GR_SetBlendMode (PsyX_render.cpp) so semi-trans
	// polys with alpha=1.0 stay opaque (no transparency) and only the
	// STP-bit texels (output alpha=0.5 from the fragment) actually blend.
	auto blendForMode = [](int mode) {
		VkPipelineColorBlendAttachmentState a = {};
		a.colorWriteMask = VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|
		                   VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
		switch (mode) {
		case BM_NONE:
			a.blendEnable = VK_FALSE;
			break;
		case BM_AVERAGE:	// glBlendFunc(SRC_ALPHA, ONE_MINUS_SRC_ALPHA)
			a.blendEnable = VK_TRUE;
			a.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
			a.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
			a.colorBlendOp = VK_BLEND_OP_ADD;
			a.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
			a.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
			a.alphaBlendOp = VK_BLEND_OP_ADD;
			break;
		case BM_ADD:		// glBlendFunc(ONE, ONE)
			a.blendEnable = VK_TRUE;
			a.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
			a.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
			a.colorBlendOp = VK_BLEND_OP_ADD;
			a.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
			a.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
			a.alphaBlendOp = VK_BLEND_OP_ADD;
			break;
		case BM_SUBTRACT:	// glBlendEq REVERSE_SUBTRACT, glBlendFunc(ONE, ONE)
			a.blendEnable = VK_TRUE;
			a.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
			a.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
			a.colorBlendOp = VK_BLEND_OP_REVERSE_SUBTRACT;
			a.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
			a.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
			a.alphaBlendOp = VK_BLEND_OP_ADD;
			break;
		case BM_ADD_QUATER_SOURCE:	// glBlendFunc(CONSTANT_ALPHA=0.25, ONE)
			a.blendEnable = VK_TRUE;
			a.srcColorBlendFactor = VK_BLEND_FACTOR_CONSTANT_ALPHA;
			a.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
			a.colorBlendOp = VK_BLEND_OP_ADD;
			a.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
			a.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
			a.alphaBlendOp = VK_BLEND_OP_ADD;
			break;
		}
		return a;
	};

		VkPipelineColorBlendAttachmentState cbaArr[kVkBlendModeCount];
		VkPipelineColorBlendStateCreateInfo cbArr[kVkBlendModeCount] = {};
		for (int i = 0; i < kVkBlendModeCount; ++i) {
			cbaArr[i] = blendForMode(i);
			cbArr[i].sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
			cbArr[i].attachmentCount = 1;
			cbArr[i].pAttachments = &cbaArr[i];
	}

	VkDynamicState dynStates[] = {
		VK_DYNAMIC_STATE_VIEWPORT,
		VK_DYNAMIC_STATE_SCISSOR,
		VK_DYNAMIC_STATE_BLEND_CONSTANTS,
	};
	VkPipelineDynamicStateCreateInfo dyn = {};
	dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dyn.dynamicStateCount = 3;
	dyn.pDynamicStates = dynStates;

	VkPipelineShaderStageCreateInfo stages[2] = {};
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = g_vkVertModule;
	stages[0].pName = "main";
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = g_vkFragModule;
	stages[1].pName = "main";

	VkGraphicsPipelineCreateInfo gpci = {};
	gpci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	gpci.stageCount = 2;
	gpci.pStages = stages;
	gpci.pVertexInputState = &vi;
	gpci.pInputAssemblyState = &ia;
	gpci.pViewportState = &vp;
	gpci.pRasterizationState = &rs;
	gpci.pMultisampleState = &ms;
	gpci.pDynamicState = &dyn;
	gpci.layout = g_vkPipelineLayout;
	gpci.renderPass = g_vkClearRenderPass;
	gpci.subpass = 0;

	// Build PSO variants for blend mode and depth state.
	bool ok = true;
	for (int depthMode = 0; depthMode < kVkDepthModeCount; ++depthMode) {
		for (int i = 0; i < kVkBlendModeCount; ++i) {
			gpci.pColorBlendState = &cbArr[i];
			gpci.pDepthStencilState = (depthMode == kVkDepthEnabled)
				? ((i == BM_NONE) ? &dsOpaque : &dsBlend)
				: &dsDisabled;
			VK_CHECK(vkCreateGraphicsPipelines(g_vkDevice, VK_NULL_HANDLE, 1, &gpci, nullptr, &g_vkPipelines[depthMode][i]));
			if (g_vkPipelines[depthMode][i] == VK_NULL_HANDLE) ok = false;
		}
	}
	return ok;
}

bool CreateVertexBuffer()
{
	const VkDeviceSize totalBytes = (VkDeviceSize)sizeof(GrVertex) * MAX_VERTEX_BUFFER_SIZE;
	for (int i = 0; i < kMaxFramesInFlight; ++i) {
		if (!CreateBuffer(totalBytes,
			VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			&g_vkVertexBuffer[i], &g_vkVertexBufferMem[i])) {
			return false;
		}
		VK_CHECK(vkMapMemory(g_vkDevice, g_vkVertexBufferMem[i], 0, VK_WHOLE_SIZE, 0, &g_vkVertexBufferMap[i]));
	}
	return true;
}

void DestroySwapchainObjects()
{
	if (g_vkDevice == VK_NULL_HANDLE) return;
	vkDeviceWaitIdle(g_vkDevice);
	for (auto fb : g_vkFramebuffers)
		if (fb) vkDestroyFramebuffer(g_vkDevice, fb, nullptr);
	g_vkFramebuffers.clear();
	for (auto v : g_vkSwapchainImageViews)
		if (v) vkDestroyImageView(g_vkDevice, v, nullptr);
	g_vkSwapchainImageViews.clear();
	g_vkSwapchainImages.clear();
	if (g_vkSwapchain) {
		vkDestroySwapchainKHR(g_vkDevice, g_vkSwapchain, nullptr);
		g_vkSwapchain = VK_NULL_HANDLE;
	}
}

} // anonymous namespace

// =======================================================================
// Public GR_* API
// =======================================================================

int GR_InitialiseRender(char* windowName, int width, int height, int fullscreen)
{
	g_windowWidth  = width;
	g_windowHeight = height;

	int windowFlags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE;
	if (fullscreen)
		windowFlags |= SDL_WINDOW_FULLSCREEN;

	g_window = SDL_CreateWindow(windowName,
		SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
		width, height, windowFlags);
	if (!g_window) {
		eprinterr("SDL_CreateWindow (Vulkan) failed: %s\n", SDL_GetError());
		return 0;
	}

	uint32_t extCount = 0;
	if (!SDL_Vulkan_GetInstanceExtensions(g_window, &extCount, nullptr)) {
		eprinterr("SDL_Vulkan_GetInstanceExtensions: %s\n", SDL_GetError());
		return 0;
	}
	std::vector<const char*> exts(extCount);
	SDL_Vulkan_GetInstanceExtensions(g_window, &extCount, exts.data());
#if defined(__APPLE__)
	exts.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
	exts.push_back("VK_KHR_get_physical_device_properties2");
#endif

	VkApplicationInfo app = {};
	app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	app.pApplicationName = windowName ? windowName : "PsyX";
	app.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
	app.pEngineName = "PsyX-Vulkan";
	app.engineVersion = VK_MAKE_VERSION(1, 0, 0);
	app.apiVersion = VK_API_VERSION_1_1;

	VkInstanceCreateInfo ici = {};
	ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	ici.pApplicationInfo = &app;
	ici.enabledExtensionCount = (uint32_t)exts.size();
	ici.ppEnabledExtensionNames = exts.data();
#if defined(__APPLE__)
	ici.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
#endif
	VK_CHECK(vkCreateInstance(&ici, nullptr, &g_vkInstance));
	if (g_vkInstance == VK_NULL_HANDLE) {
		eprinterr("vkCreateInstance failed (MoltenVK ICD missing or broken?)\n");
		return 0;
	}

	if (!SDL_Vulkan_CreateSurface(g_window, g_vkInstance, &g_vkSurface)) {
		eprinterr("SDL_Vulkan_CreateSurface: %s\n", SDL_GetError());
		return 0;
	}

	if (!PickPhysicalDeviceAndQueueFamilies()) return 0;
	if (!CreateLogicalDevice())                return 0;
	if (!CreateSwapchain(width, height))       return 0;
	if (!CreateDepthBuffer())                  return 0;
	if (!CreateClearRenderPass())              return 0;
	if (!CreateFramebuffers())                 return 0;
	if (!CreateCommandPoolAndBuffers())        return 0;
	if (!CreateSyncObjects())                  return 0;

	g_shadercCompiler = shaderc_compiler_initialize();
	if (!g_shadercCompiler) {
		eprinterr("shaderc_compiler_initialize failed\n");
		return 0;
	}

	// VRAM image + sampler + descriptor set must exist BEFORE the
	// pipeline layout (which references the descriptor set layout).
	if (!CreateVramImage())             return 0;
	if (!CreateDescriptorSet())         return 0;
	if (!CreateBasicGraphicsPipeline()) return 0;
	if (!CreateVertexBuffer())          return 0;

	// First-time transition: take the brand-new VRAM image (UNDEFINED)
	// to SHADER_READ_ONLY so the very first frame has a valid layout
	// even before any upload happens.
	{
		VkCommandBufferAllocateInfo ai = {};
		ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		ai.commandPool = g_vkCommandPool;
		ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		ai.commandBufferCount = 1;
		VkCommandBuffer cb;
		vkAllocateCommandBuffers(g_vkDevice, &ai, &cb);
		VkCommandBufferBeginInfo bi = {};
		bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vkBeginCommandBuffer(cb, &bi);
		VramImageBarrier(cb,
			VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			0, VK_ACCESS_SHADER_READ_BIT,
			VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
		vkEndCommandBuffer(cb);
		VkSubmitInfo si = {};
		si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		si.commandBufferCount = 1;
		si.pCommandBuffers = &cb;
		vkQueueSubmit(g_vkGraphicsQueue, 1, &si, VK_NULL_HANDLE);
		vkQueueWaitIdle(g_vkGraphicsQueue);
		vkFreeCommandBuffers(g_vkDevice, g_vkCommandPool, 1, &cb);
	}

	eprintinfo("Vulkan/MoltenVK backend initialised: %dx%d, swap images=%zu\n",
		(int)g_vkSwapchainExtent.width, (int)g_vkSwapchainExtent.height,
		g_vkSwapchainImages.size());
	return 1;
}

int GR_InitialisePSX()
{
	// Equivalent of the GL backend's PSX-shader / VRAM-texture init.
	// Most of this is folded into GR_InitialiseRender for now.
	return 1;
}

void GR_Shutdown()
{
	if (g_vkDevice != VK_NULL_HANDLE)
		vkDeviceWaitIdle(g_vkDevice);

	if (g_shadercCompiler) {
		shaderc_compiler_release(g_shadercCompiler);
		g_shadercCompiler = nullptr;
	}

	for (int i = 0; i < kMaxFramesInFlight; ++i) {
		if (g_vkVertexBufferMap[i]) { vkUnmapMemory(g_vkDevice, g_vkVertexBufferMem[i]); g_vkVertexBufferMap[i] = nullptr; }
		if (g_vkVertexBuffer[i])    { vkDestroyBuffer(g_vkDevice, g_vkVertexBuffer[i], nullptr); g_vkVertexBuffer[i] = VK_NULL_HANDLE; }
		if (g_vkVertexBufferMem[i]) { vkFreeMemory(g_vkDevice, g_vkVertexBufferMem[i], nullptr); g_vkVertexBufferMem[i] = VK_NULL_HANDLE; }
	}

	if (g_vkDepthImageView)   { vkDestroyImageView(g_vkDevice, g_vkDepthImageView, nullptr); g_vkDepthImageView = VK_NULL_HANDLE; }
	if (g_vkDepthImage)       { vkDestroyImage(g_vkDevice, g_vkDepthImage, nullptr); g_vkDepthImage = VK_NULL_HANDLE; }
	if (g_vkDepthImageMem)    { vkFreeMemory(g_vkDevice, g_vkDepthImageMem, nullptr); g_vkDepthImageMem = VK_NULL_HANDLE; }

	for (int i = 0; i < kMaxFramesInFlight; ++i) {
		if (g_vkVramStagingMap[i]) { vkUnmapMemory(g_vkDevice, g_vkVramStagingMem[i]); g_vkVramStagingMap[i] = nullptr; }
		if (g_vkVramStaging[i])    { vkDestroyBuffer(g_vkDevice, g_vkVramStaging[i], nullptr); g_vkVramStaging[i] = VK_NULL_HANDLE; }
		if (g_vkVramStagingMem[i]) { vkFreeMemory(g_vkDevice, g_vkVramStagingMem[i], nullptr); g_vkVramStagingMem[i] = VK_NULL_HANDLE; }
	}
	if (g_vkVramSampler)      { vkDestroySampler(g_vkDevice, g_vkVramSampler, nullptr); g_vkVramSampler = VK_NULL_HANDLE; }
	if (g_vkVramImageView)    { vkDestroyImageView(g_vkDevice, g_vkVramImageView, nullptr); g_vkVramImageView = VK_NULL_HANDLE; }
	if (g_vkVramImage)        { vkDestroyImage(g_vkDevice, g_vkVramImage, nullptr); g_vkVramImage = VK_NULL_HANDLE; }
	if (g_vkVramImageMem)     { vkFreeMemory(g_vkDevice, g_vkVramImageMem, nullptr); g_vkVramImageMem = VK_NULL_HANDLE; }

	if (g_vkDescPool)         { vkDestroyDescriptorPool(g_vkDevice, g_vkDescPool, nullptr); g_vkDescPool = VK_NULL_HANDLE; }
	if (g_vkDescSetLayout)    { vkDestroyDescriptorSetLayout(g_vkDevice, g_vkDescSetLayout, nullptr); g_vkDescSetLayout = VK_NULL_HANDLE; }

	for (int depthMode = 0; depthMode < kVkDepthModeCount; ++depthMode) {
		for (int i = 0; i < kVkBlendModeCount; ++i) {
			if (g_vkPipelines[depthMode][i]) { vkDestroyPipeline(g_vkDevice, g_vkPipelines[depthMode][i], nullptr); g_vkPipelines[depthMode][i] = VK_NULL_HANDLE; }
		}
	}
	if (g_vkPipelineLayout)   { vkDestroyPipelineLayout(g_vkDevice, g_vkPipelineLayout, nullptr); g_vkPipelineLayout = VK_NULL_HANDLE; }
	if (g_vkVertModule)       { vkDestroyShaderModule(g_vkDevice, g_vkVertModule, nullptr); g_vkVertModule = VK_NULL_HANDLE; }
	if (g_vkFragModule)       { vkDestroyShaderModule(g_vkDevice, g_vkFragModule, nullptr); g_vkFragModule = VK_NULL_HANDLE; }

	for (int i = 0; i < kMaxFramesInFlight; ++i) {
		if (g_vkImageAvailableSems[i]) vkDestroySemaphore(g_vkDevice, g_vkImageAvailableSems[i], nullptr);
		if (g_vkRenderFinishedSems[i]) vkDestroySemaphore(g_vkDevice, g_vkRenderFinishedSems[i], nullptr);
		if (g_vkInFlightFences[i])     vkDestroyFence(g_vkDevice, g_vkInFlightFences[i], nullptr);
	}
	if (g_vkCommandPool)     vkDestroyCommandPool(g_vkDevice, g_vkCommandPool, nullptr);
	DestroySwapchainObjects();
	if (g_vkClearRenderPass) vkDestroyRenderPass(g_vkDevice, g_vkClearRenderPass, nullptr);
	if (g_vkDevice)          vkDestroyDevice(g_vkDevice, nullptr);
	if (g_vkSurface)         vkDestroySurfaceKHR(g_vkInstance, g_vkSurface, nullptr);
	if (g_vkInstance)        vkDestroyInstance(g_vkInstance, nullptr);

	g_vkInstance = VK_NULL_HANDLE;
	g_vkDevice = VK_NULL_HANDLE;
	g_vkSurface = VK_NULL_HANDLE;
	g_vkClearRenderPass = VK_NULL_HANDLE;
	g_vkCommandPool = VK_NULL_HANDLE;
}

void GR_ResetDevice() {}
void GR_UpdateSwapIntervalState(int /*swapInterval*/) {}

// -----------------------------------------------------------------------
// Per-frame: BeginScene records, EndScene submits + presents.
// -----------------------------------------------------------------------

void GR_BeginScene()
{
	if (g_vkSceneRecording) return;

	g_vkDiagFrame++;
	VkDiagResetFrame();

	vkWaitForFences(g_vkDevice, 1, &g_vkInFlightFences[g_vkCurrentFrame], VK_TRUE, UINT64_MAX);

	VkResult ar = vkAcquireNextImageKHR(g_vkDevice, g_vkSwapchain, UINT64_MAX,
		g_vkImageAvailableSems[g_vkCurrentFrame], VK_NULL_HANDLE,
		&g_vkAcquiredImageIdx);
	if (ar == VK_ERROR_OUT_OF_DATE_KHR) {
		DestroySwapchainObjects();
		CreateSwapchain(g_windowWidth, g_windowHeight);
		CreateFramebuffers();
		return;
	}

	vkResetFences(g_vkDevice, 1, &g_vkInFlightFences[g_vkCurrentFrame]);

	VkCommandBuffer cb = g_vkCmdBuffers[g_vkCurrentFrame];
	vkResetCommandBuffer(cb, 0);

	VkCommandBufferBeginInfo bi = {};
	bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	vkBeginCommandBuffer(cb, &bi);

	// Upload CPU-side VRAM mirror to the GPU image if dirty. Done OUTSIDE
	// the render pass — vkCmdCopyBufferToImage and image-layout barriers
	// are not allowed inside one.
	if (g_vkVramDirty) {
		// Refresh the staging buffer from the CPU mirror.
		memcpy(g_vkVramStagingMap[g_vkCurrentFrame], g_vkVramCPU, sizeof(g_vkVramCPU));

		// SHADER_READ_ONLY → TRANSFER_DST → copy → SHADER_READ_ONLY
		VramImageBarrier(cb,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

		VkBufferImageCopy r = {};
		r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		r.imageSubresource.layerCount = 1;
		r.imageExtent = { VRAM_WIDTH, VRAM_HEIGHT, 1 };
		vkCmdCopyBufferToImage(cb, g_vkVramStaging[g_vkCurrentFrame], g_vkVramImage,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);

		VramImageBarrier(cb,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
			VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

		g_vkVramDirty = false;
	}

	VkClearValue cv[2] = {};
	cv[0].color = { { g_vkClearColor[0], g_vkClearColor[1], g_vkClearColor[2], g_vkClearColor[3] } };
	cv[1].depthStencil = { 1.0f, 0 };

	VkRenderPassBeginInfo rpb = {};
	rpb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	rpb.renderPass = g_vkClearRenderPass;
	rpb.framebuffer = g_vkFramebuffers[g_vkAcquiredImageIdx];
	rpb.renderArea.offset = { 0, 0 };
	rpb.renderArea.extent = g_vkSwapchainExtent;
	rpb.clearValueCount = 2;
	rpb.pClearValues = cv;
	vkCmdBeginRenderPass(cb, &rpb, VK_SUBPASS_CONTENTS_INLINE);

	// Y-flip via negative-height viewport (VK_KHR_maintenance1 / VK 1.1
	// core). The 2D ortho matrix and the 3D shader's `* vec2(1, -1)`
	// both produce GL-style Y-up clip space; this viewport flip brings
	// it back to VK's Y-down screen.
	VkViewport vp = {};
	vp.x = 0.0f;
	vp.y = (float)g_vkSwapchainExtent.height;
	vp.width  =  (float)g_vkSwapchainExtent.width;
	vp.height = -(float)g_vkSwapchainExtent.height;
	vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
	vkCmdSetViewport(cb, 0, 1, &vp);

	VkRect2D sc = { {0,0}, g_vkSwapchainExtent };
	vkCmdSetScissor(cb, 0, 1, &sc);

	// Bind initial blend-mode pipeline + vertex buffer + descriptor set.
	// GR_DrawTriangles will rebind a different pipeline if the engine
	// switches blend modes mid-frame.
		g_vkBoundPipelineKey = GetPipelineKey(kVkDepthEnabled, BM_NONE);
		vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_vkPipelines[kVkDepthEnabled][BM_NONE]);
	VkDeviceSize zero = 0;
	VkBuffer vertexBuffer = g_vkVertexBuffer[g_vkCurrentFrame];
	vkCmdBindVertexBuffers(cb, 0, 1, &vertexBuffer, &zero);
	vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
		g_vkPipelineLayout, 0, 1, &g_vkDescSet, 0, nullptr);

	// PSX BM_AVERAGE = 0.5*src + 0.5*dst. Set Vulkan blend constant to
	// 0.5 so the BM_AVERAGE pipeline (CONSTANT_COLOR factor) gets it.
	float blendConsts[4] = { 0.5f, 0.5f, 0.5f, 0.25f };
	vkCmdSetBlendConstants(cb, blendConsts);

	g_vkSceneRecording = true;
}

void GR_EndScene()
{
	if (!g_vkSceneRecording) return;

	VkDiagLogFrame();

	VkCommandBuffer cb = g_vkCmdBuffers[g_vkCurrentFrame];
	vkCmdEndRenderPass(cb);

	const VkDeviceSize captureBytes = (VkDeviceSize)g_vkSwapchainExtent.width * g_vkSwapchainExtent.height * 4;
	VkBuffer captureBuffer = VK_NULL_HANDLE;
	VkDeviceMemory captureMemory = VK_NULL_HANDLE;
	const bool captureThisFrame =
		g_vkSwapchainCanReadback &&
		VkCaptureFrame() >= 0 &&
		(int)g_vkDiagFrame == VkCaptureFrame() &&
		CreateBuffer(captureBytes,
			VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			&captureBuffer, &captureMemory);

	if (captureThisFrame) {
		SwapchainImageBarrier(cb,
			VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

		VkBufferImageCopy copy = {};
		copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		copy.imageSubresource.layerCount = 1;
		copy.imageExtent = { g_vkSwapchainExtent.width, g_vkSwapchainExtent.height, 1 };
		vkCmdCopyImageToBuffer(cb, g_vkSwapchainImages[g_vkAcquiredImageIdx],
			VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, captureBuffer, 1, &copy);

		SwapchainImageBarrier(cb,
			VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
			VK_ACCESS_TRANSFER_READ_BIT, 0,
			VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
	}

	vkEndCommandBuffer(cb);

	VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	VkSubmitInfo si = {};
	si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	si.waitSemaphoreCount = 1;
	si.pWaitSemaphores = &g_vkImageAvailableSems[g_vkCurrentFrame];
	si.pWaitDstStageMask = &waitStage;
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cb;
	si.signalSemaphoreCount = 1;
	si.pSignalSemaphores = &g_vkRenderFinishedSems[g_vkCurrentFrame];
	VK_CHECK(vkQueueSubmit(g_vkGraphicsQueue, 1, &si, g_vkInFlightFences[g_vkCurrentFrame]));

	if (captureThisFrame) {
		VK_CHECK(vkWaitForFences(g_vkDevice, 1, &g_vkInFlightFences[g_vkCurrentFrame], VK_TRUE, UINT64_MAX));
		void* mapped = nullptr;
		VK_CHECK(vkMapMemory(g_vkDevice, captureMemory, 0, captureBytes, 0, &mapped));
		std::vector<unsigned char> pixels((size_t)captureBytes);
		memcpy(pixels.data(), mapped, (size_t)captureBytes);
		vkUnmapMemory(g_vkDevice, captureMemory);
		ConvertSwapchainPixelsToBgra(pixels.data(), g_vkSwapchainExtent.width, g_vkSwapchainExtent.height);
		const char* capturePath = VkCapturePath();
		if (WriteBgraBmp(capturePath, pixels.data(), g_vkSwapchainExtent.width, g_vkSwapchainExtent.height))
			eprintinfo("[VK] captured frame %llu to %s\n", (unsigned long long)g_vkDiagFrame, capturePath);
		else
			eprinterr("[VK] failed to capture frame %llu to %s\n", (unsigned long long)g_vkDiagFrame, capturePath);
		vkDestroyBuffer(g_vkDevice, captureBuffer, nullptr);
		vkFreeMemory(g_vkDevice, captureMemory, nullptr);
	}

	VkPresentInfoKHR pi = {};
	pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	pi.waitSemaphoreCount = 1;
	pi.pWaitSemaphores = &g_vkRenderFinishedSems[g_vkCurrentFrame];
	pi.swapchainCount = 1;
	pi.pSwapchains = &g_vkSwapchain;
	pi.pImageIndices = &g_vkAcquiredImageIdx;
	VkResult pr = vkQueuePresentKHR(g_vkPresentQueue, &pi);
	if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) {
		DestroySwapchainObjects();
		CreateSwapchain(g_windowWidth, g_windowHeight);
		CreateFramebuffers();
	}

	g_vkCurrentFrame = (g_vkCurrentFrame + 1) % kMaxFramesInFlight;
	g_vkSceneRecording = false;
}

void GR_SwapWindow() { /* present already happened in GR_EndScene */ }

// -----------------------------------------------------------------------
// Clears
// -----------------------------------------------------------------------

void GR_Clear(int /*x*/, int /*y*/, int /*w*/, int /*h*/,
	unsigned char r, unsigned char g, unsigned char b)
{
	g_vkClearColor[0] = (float)r / 255.0f;
	g_vkClearColor[1] = (float)g / 255.0f;
	g_vkClearColor[2] = (float)b / 255.0f;
	g_vkClearColor[3] = 1.0f;
}

void GR_ClearVRAM(int, int, int, int, unsigned char, unsigned char, unsigned char) {}

// -----------------------------------------------------------------------
// Vertex buffer + draw
// -----------------------------------------------------------------------

void GR_UpdateVertexBuffer(const GrVertex* vertices, int count)
{
	void* vertexBufferMap = g_vkVertexBufferMap[g_vkCurrentFrame];
	if (!vertices || count <= 0 || !vertexBufferMap) return;
	if (count > MAX_VERTEX_BUFFER_SIZE) count = MAX_VERTEX_BUFFER_SIZE;
	memcpy(vertexBufferMap, vertices, sizeof(GrVertex) * (size_t)count);
	g_vkVertexBufferSize = (uint32_t)count;

	static int s_dumpTick = 0;
	if (VkDrawLogEnabled() && ++s_dumpTick >= 120 && count >= 30) {
		s_dumpTick = 0;
		for (int i = 0; i < 3 && i < count; ++i) {
			const GrVertex& v = vertices[i];
#if USE_PGXP
			eprintinfo("[VK][V%d] pos=(%.3f,%.3f) page=%.3f clut=%.3f  "
				"zw=(%.3f,%.3f,%.3f,%.3f)  "
				"uv=(%u,%u) bright=%u dither=%u  "
				"rgba=(%u,%u,%u,%u)  "
				"tcx=(%d,%d)\n",
				i, (double)v.x, (double)v.y, (double)v.page, (double)v.clut,
				(double)v.z, (double)v.scr_h, (double)v.ofsX, (double)v.ofsY,
				(unsigned)v.u, (unsigned)v.v, (unsigned)v.bright, (unsigned)v.dither,
				(unsigned)v.r, (unsigned)v.g, (unsigned)v.b, (unsigned)v.a,
				(int)v.tcx, (int)v.tcy);
#else
			eprintinfo("[VK][V%d] pos=(%d,%d) page=%d clut=%d  "
				"uv=(%u,%u) bright=%u dither=%u  "
				"rgba=(%u,%u,%u,%u)\n",
				i, (int)v.x, (int)v.y, (int)v.page, (int)v.clut,
				(unsigned)v.u, (unsigned)v.v, (unsigned)v.bright, (unsigned)v.dither,
				(unsigned)v.r, (unsigned)v.g, (unsigned)v.b, (unsigned)v.a);
#endif
		}
	}
}

void GR_DrawTriangles(int start_vertex, int triangles)
{
	// DEBUG counter — prints draws-per-second. Lets us tell at a glance
	// whether the engine is actually pumping vkCmdDraw at all.
	static uint64_t s_drawsSinceLog = 0;
	static uint64_t s_trisSinceLog  = 0;
	static int s_logTickFrame = 0;
	s_drawsSinceLog++;
	s_trisSinceLog += (uint64_t)(triangles > 0 ? triangles : 0);

	if (!g_vkSceneRecording || triangles <= 0) return;
	if (g_PreviousOffscreenState) return;

	VkCommandBuffer cb = g_vkCmdBuffers[g_vkCurrentFrame];

	const int blendIdx = (g_vkActiveBlendMode >= 0 && g_vkActiveBlendMode < kVkBlendModeCount)
		? g_vkActiveBlendMode : 0;
	const int depthIdx = (g_vkActiveDepthMode == kVkDepthEnabled && g_cfg_pgxpZBuffer)
		? kVkDepthEnabled : kVkDepthDisabled;
	const int pipelineKey = GetPipelineKey(depthIdx, blendIdx);

	if (pipelineKey != g_vkBoundPipelineKey) {
		vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_vkPipelines[depthIdx][blendIdx]);
		g_vkBoundPipelineKey = pipelineKey;
	}

	vkCmdPushConstants(cb, g_vkPipelineLayout,
		VK_SHADER_STAGE_VERTEX_BIT,
		0, sizeof(PushBlock), &g_vkPushBlock);

	VkDiagAccumulateDraw(start_vertex, triangles);

	vkCmdDraw(cb, (uint32_t)(triangles * 3), 1, (uint32_t)start_vertex, 0);

	if (VkDrawLogEnabled() && ++s_logTickFrame >= 60) {
		eprintinfo("[VK] draws/s=%llu tris/s=%llu vbufVerts=%u\n",
			(unsigned long long)s_drawsSinceLog,
			(unsigned long long)s_trisSinceLog,
			g_vkVertexBufferSize);
		s_drawsSinceLog = s_trisSinceLog = 0;
		s_logTickFrame = 0;
	}
}

// -----------------------------------------------------------------------
// Projection helpers — fill push-constant matrix used by the basic shader.
// PSX coordinate space is Y-down; the engine already feeds the renderer in
// pixel coords mapped via the projection matrix below.
// -----------------------------------------------------------------------

// Match the GL backend's matrices exactly. Y-flip (GL→Vulkan clip-space)
// is done via a negative-height viewport (VK_KHR_maintenance1 / VK 1.1
// core), so the matrices themselves stay identical to the GL ones.

void GR_Ortho2D(float left, float right, float bottom, float top, float znear, float zfar)
{
	float a = 2.0f / (right - left);
	float b = 2.0f / (top - bottom);
	float c = 2.0f / (znear - zfar);
	float x = (left + right) / (left - right);
	float y = (bottom + top) / (bottom - top);
	float z = (znear + zfar) / (znear - zfar);
	float* m = g_vkPushBlock.Projection;
	m[0]  = a; m[1]  = 0; m[2]  = 0; m[3]  = 0;
	m[4]  = 0; m[5]  = b; m[6]  = 0; m[7]  = 0;
	m[8]  = 0; m[9]  = 0; m[10] = c; m[11] = 0;
	m[12] = x; m[13] = y; m[14] = z; m[15] = 1;
}

#include <math.h>
void GR_Perspective3D(const float fov, const float width, const float height,
                      const float zNear, const float zFar)
{
	const float sinF = sinf(0.5f * fov);
	const float cosF = cosf(0.5f * fov);
	const float h = cosF / sinF;
	const float w = (h * height) / width;
	float* m = g_vkPushBlock.Projection3D;
	m[0]  = w; m[1]  = 0; m[2]  = 0;                                m[3]  = 0;
	m[4]  = 0; m[5]  = h; m[6]  = 0;                                m[7]  = 0;
	m[8]  = 0; m[9]  = 0; m[10] = (zFar + zNear) / (zFar - zNear); m[11] = 1;
	m[12] = 0; m[13] = 0; m[14] = -(2*zFar*zNear) / (zFar - zNear); m[15] = 0;
}

// -----------------------------------------------------------------------
// Stubs — to be filled in subsequent revisions
// -----------------------------------------------------------------------

void GR_SaveVRAM(const char*, int, int, int, int, int) {}

void GR_CopyVRAM(unsigned short* src, int x, int y, int w, int h, int dst_x, int dst_y)
{
	g_vkVramDirty = true;
	int stride = w;
	if (!src) {
		src = g_vkVramCPU;
		stride = VRAM_WIDTH;
	}
	src += x + y * stride;
	unsigned short* dst = g_vkVramCPU + dst_x + dst_y * VRAM_WIDTH;
	for (int row = 0; row < h; ++row) {
		memcpy(dst, src, w * sizeof(unsigned short));
		dst += VRAM_WIDTH;
		src += stride;
	}

	static int s_uploads = 0;
	static int s_logTick = 0;
	s_uploads++;
	if (VkDrawLogEnabled() && ++s_logTick >= 20) {
		s_logTick = 0;
		int cx = dst_x + w/2;
		int cy = dst_y + h/2;
		unsigned short v = g_vkVramCPU[cy * VRAM_WIDTH + cx];
		eprintinfo("[VK][VRAM] upload #%d -> (%d,%d) %dx%d center[%d,%d]=0x%04x\n",
			s_uploads, dst_x, dst_y, w, h, cx, cy, (unsigned)v);
	}
}

void GR_ReadVRAM(unsigned short* dst, int x, int y, int dst_w, int dst_h)
{
	const unsigned short* src = g_vkVramCPU + x + y * VRAM_WIDTH;
	for (int row = 0; row < dst_h; ++row) {
		memcpy(dst, src, dst_w * sizeof(unsigned short));
		dst += dst_w;
		src += VRAM_WIDTH;
	}
}

void GR_UpdateVRAM()
{
	// CPU mirror is already up-to-date — just flag the GPU image dirty
	// so BeginScene re-uploads on the next frame.
	g_vkVramDirty = true;
}

void GR_StoreFrameBuffer(int, int, int, int) {
	// Capturing the just-rendered framebuffer back into VRAM (used by
	// some PSX scratch effects) needs an extra image-to-image copy.
	// Stubbed for now — visual artefact: certain post-process effects
	// won't appear, but base rendering proceeds.
}
void GR_ReadFramebufferDataToVRAM() {}

TextureID GR_CreateRGBATexture(int /*w*/, int /*h*/, unsigned char* /*data*/) { return 0; }

// GR_Shader_Compile is meaningful but the engine treats the returned ID as
// opaque; for now we return 0 so the engine's "is there a shader?" checks
// pass. Real shader caching/binding will be added with the GTE port.
ShaderID GR_Shader_Compile(const char* /*src*/) { return 0; }

void GR_SetShader(const ShaderID s) { g_PreviousShader = s; }

void GR_SetBlendMode(BlendMode bm)
{
	g_PreviousBlendMode = bm;
	g_vkActiveBlendMode = (int)bm;
}
void GR_SetPolygonOffset(float) {}
void GR_SetStencilMode(int v)    { g_PreviousStencilMode = v; }
void GR_EnableDepth(int v)
{
	g_PreviousDepthMode = v;
	g_vkActiveDepthMode = (v && g_cfg_pgxpZBuffer) ? kVkDepthEnabled : kVkDepthDisabled;
}
// Engine state owned by PsyX_GPU.cpp — needed by GR_SetupClipMode to
// translate the engine's PSX-space clip rect into a window-space scissor.
extern "C" { extern DISPENV activeDispEnv; }

void GR_SetScissorState(int v)
{
	g_PreviousScissorState = v;
	if (!g_vkSceneRecording) return;
	if (!v) {
		// Disable → full-window scissor (Vulkan can't actually disable
		// scissor like GL can, so we just set it to cover everything).
		VkRect2D sc = { {0, 0}, g_vkSwapchainExtent };
		vkCmdSetScissor(g_vkCmdBuffers[g_vkCurrentFrame], 0, 1, &sc);
	}
}
// Mirror of the GL backend: this is the routine the engine calls before
// every draw split, and where the projection matrices for that split are
// set. Without this the push-constant matrices stay at identity and PSX
// pixel-space vertices land far outside the clip volume → black screen.
void GR_SetOffscreenState(const RECT16* offscreenRect, int enable)
{
	if (enable) {
#if USE_PGXP
		GR_Ortho2D(-0.5f, 0.5f, 0.5f, -0.5f, -1.0f, 1.0f);
#else
		GR_Ortho2D(0, offscreenRect->w, offscreenRect->h, 0, -1.0f, 1.0f);
#endif
	} else {
#if USE_PGXP
		const float perspectiveFOV   = 0.9265f;
		const float perspectiveZNear = 0.25f;
		const float perspectiveZFar  = 1000.0f;
		const float PSX_ASPECT       = (240.0f / 320.0f);
		const float emuScreenAspect  = (float)g_windowWidth / (float)g_windowHeight;
		GR_Ortho2D(-0.5f * emuScreenAspect * PSX_ASPECT,
		            0.5f * emuScreenAspect * PSX_ASPECT,
		            0.5f, -0.5f, -1.0f, 1.0f);
		GR_Perspective3D(perspectiveFOV, 1.0f, 1.0f / (emuScreenAspect * PSX_ASPECT),
		                 perspectiveZNear, perspectiveZFar);
#else
		// Fallback for non-PGXP builds — uses framebuffer (320x240 typical)
		GR_Ortho2D(0, 320, 240, 0, -1.0f, 1.0f);
#endif
	}
	g_PreviousOffscreenState = enable;
}
// Port of the GL backend's GR_SetupClipMode. Translates an engine
// PSX-space RECT16 into a VkRect2D scissor that lines up with the
// widescreen-mapped viewport. Without this the mini-map quad's
// texture leaks outside its frame, and many world primitives that
// the engine *intends* to clip render outside their proper region.
void GR_SetupClipMode(const RECT16* rect, int enable)
{
	if (!g_vkSceneRecording) return;
	VkCommandBuffer cb = g_vkCmdBuffers[g_vkCurrentFrame];

	const bool scissorOn = enable && (
		activeDispEnv.isinter ||
		(rect->x - activeDispEnv.disp.x > 0 ||
		 rect->y - activeDispEnv.disp.y > 0 ||
		 rect->w < activeDispEnv.disp.w - 1 ||
		 rect->h < activeDispEnv.disp.h - 1));

	if (!scissorOn) {
		VkRect2D sc = { {0, 0}, g_vkSwapchainExtent };
		vkCmdSetScissor(cb, 0, 1, &sc);
		return;
	}

	const float PSX_ASPECT = 240.0f / 320.0f;
	const float emuScreenAspect = 1.0f / (PSX_ASPECT *
		(float)g_windowWidth / (float)g_windowHeight);
	const float psxScreenWInv = 1.0f / (float)activeDispEnv.disp.w;
	const float psxScreenHInv = 1.0f / (float)activeDispEnv.disp.h;

	float clipRectX = (float)(rect->x - activeDispEnv.disp.x) * psxScreenWInv;
	float clipRectY = (float)(rect->y - activeDispEnv.disp.y) * psxScreenHInv;
	float clipRectW = (float)(rect->w) * psxScreenWInv;
	float clipRectH = (float)(rect->h) * psxScreenHInv;

	clipRectX -= 0.5f;
	clipRectX *= emuScreenAspect;
	clipRectW *= emuScreenAspect;
	clipRectX += 0.5f;

	int crx = (int)(clipRectX * (float)g_windowWidth);
	int cry = (int)(clipRectY * (float)g_windowHeight);
	int crw = (int)(clipRectW * (float)g_windowWidth);
	int crh = (int)(clipRectH * (float)g_windowHeight);

	// Clamp to swapchain extent — Vulkan rejects scissors that extend
	// beyond the framebuffer with VK_ERROR_VALIDATION_FAILED.
	if (crx < 0) { crw += crx; crx = 0; }
	if (cry < 0) { crh += cry; cry = 0; }
	const int maxW = (int)g_vkSwapchainExtent.width;
	const int maxH = (int)g_vkSwapchainExtent.height;
	if (crx + crw > maxW) crw = maxW - crx;
	if (cry + crh > maxH) crh = maxH - cry;
	if (crw < 0) crw = 0;
	if (crh < 0) crh = 0;

	VkRect2D sc = { {crx, cry}, {(uint32_t)crw, (uint32_t)crh} };
	vkCmdSetScissor(cb, 0, 1, &sc);
}
void GR_SetViewPort(int x, int y, int width, int height)
{
	if (!g_vkSceneRecording) return;
	VkCommandBuffer cb = g_vkCmdBuffers[g_vkCurrentFrame];
	// Same Y-flip trick as the BeginScene default viewport.
	VkViewport vp = {};
	vp.x = (float)x;
	vp.y = (float)(y + height);
	vp.width  =  (float)width;
	vp.height = -(float)height;
	vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
	vkCmdSetViewport(cb, 0, 1, &vp);
}
void GR_SetTexture(TextureID t, TexFormat) { g_lastBoundTexture = t; }
void GR_SetOverrideTextureSize(int, int) {}
void GR_SetWireframe(int v)      { g_dbg_wireframeMode = v; }

void GR_DestroyTexture(TextureID) {}

void GR_PushDebugLabel(const char*) {}
void GR_PopDebugLabel() {}

void PsyX_GetPSXWidescreenMappedViewport(struct _RECT16* /*rect*/) {}

#endif // RENDERER_VK
