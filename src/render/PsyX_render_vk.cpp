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

// One graphics pipeline per PSX blend mode. GR_SetBlendMode picks the
// active one; GR_DrawTriangles re-binds if it differs from what's
// currently bound on the command buffer.
VkPipelineLayout     g_vkPipelineLayout = VK_NULL_HANDLE;
VkPipeline           g_vkPipelines[5]   = {}; // indexed by BlendMode enum
int                  g_vkBoundPipelineIdx = -1;
int                  g_vkActiveBlendMode  = BM_NONE;

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

// Staging buffer used to upload the CPU-side `vram[]` to the GPU image.
VkBuffer             g_vkVramStaging      = VK_NULL_HANDLE;
VkDeviceMemory       g_vkVramStagingMem   = VK_NULL_HANDLE;
void*                g_vkVramStagingMap   = nullptr;

// Descriptor set layout / pool / set bound to the pipeline at slot 0.
VkDescriptorSetLayout g_vkDescSetLayout = VK_NULL_HANDLE;
VkDescriptorPool      g_vkDescPool      = VK_NULL_HANDLE;
VkDescriptorSet       g_vkDescSet       = VK_NULL_HANDLE;

bool                 g_vkVramDirty      = true;

// Vertex buffer (host-visible, persistently mapped). Sized to PsyX's
// MAX_VERTEX_BUFFER_SIZE upper bound.
VkBuffer             g_vkVertexBuffer = VK_NULL_HANDLE;
VkDeviceMemory       g_vkVertexBufferMem = VK_NULL_HANDLE;
void*                g_vkVertexBufferMap = nullptr;
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
        gl_Position = pb.Projection3D * vec4(a_position.xy * a_zw.y, a_zw.x, 1.0);
    } else {
        gl_Position = pb.Projection * vec4(a_position.xy, 0.5, 1.0);
    }

    // GL→Vulkan NDC z remap. The projection matrices were copied verbatim
    // from the OpenGL backend, which emits clip-space z in [-w, w] (NDC
    // z ∈ [-1, 1]). Vulkan's clip volume is z ∈ [0, w] (NDC z ∈ [0, 1]),
    // so without this remap every primitive lands outside the clip volume
    // and gets discarded — leaving the screen black.
    gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;
    v_color = a_color;
    v_texcoord = a_texcoord;

    // PSX texture page → VRAM coords:
    //   page is bits-packed: low 4 bits = X / 64, next 1 bit = Y / 256
    //   the actual texture page format (4/8/16-bit) is in bits 7-8 of the
    //   tpage word, but we receive `page` already split.
    // For a first cut we extract:
    //   page_x = (page & 0x0F) * 64
    //   page_y = ((page >> 4) & 0x01) * 256
    //   tex_format = (page >> 7) & 0x03   (0=4bit, 1=8bit, 2/3=16bit)
    //   clut_x = (clut & 0x3F) * 16
    //   clut_y = (clut >> 6)
    // Pass them along so the fragment shader can sample.
    float pageRaw = a_position.z;
    float clutRaw = a_position.w;
    float pageX = mod(pageRaw, 16.0) * 64.0;
    float pageY = floor(pageRaw / 16.0);
    pageY = mod(pageY, 2.0) * 256.0;
    float texFormat = floor(pageRaw / 128.0);
    texFormat = mod(texFormat, 4.0);
    float clutX = mod(clutRaw, 64.0) * 16.0;
    float clutY = floor(clutRaw / 64.0);
    v_page_clut = vec4(pageX, pageY, clutX, clutY);
    // Stash format in the otherwise-unused dither slot so the fragment
    // shader picks the right decode path.
    v_texcoord.w = texFormat;
}
)GLSL";

const char* kBasicFragSrc = R"GLSL(
#version 450
layout(location = 0) in vec4 v_color;
layout(location = 1) in vec4 v_texcoord;     // u, v, bright, texFormat
layout(location = 2) in vec4 v_page_clut;    // pageX, pageY, clutX, clutY

layout(location = 0) out vec4 out_color;

// VRAM as R8G8 unorm (low byte = R, high byte = G).
layout(set = 0, binding = 0) uniform sampler2D s_vram;

const float VRAM_W = 1024.0;
const float VRAM_H = 512.0;

// Decode a 16-bit packed (low,high) byte pair to the raw u16 value.
float pack_rg_u16(vec2 rg) {
    return floor(rg.x * 255.0 + 0.5) + floor(rg.y * 255.0 + 0.5) * 256.0;
}

// PSX 16-bit pixel — bits 0-4 = R, 5-9 = G, 10-14 = B, bit 15 = STP
// (semitransparency / mask). When STP=0 the pixel is opaque (alpha=1),
// when STP=1 it should blend with the active ABR mode (alpha=0.5 so
// VK_BLEND_FACTOR_SRC_ALPHA in the BM_AVERAGE pipeline produces the
// 50/50 mix the PSX expects; the BM_ADD/SUBTRACT pipelines ignore the
// alpha because their factors are ONE/ONE).
vec4 psx16_to_rgba(float v16) {
    float r = mod(v16, 32.0);                v16 = floor(v16 / 32.0);
    float g = mod(v16, 32.0);                v16 = floor(v16 / 32.0);
    float b = mod(v16, 32.0);                v16 = floor(v16 / 32.0);
    float stp = mod(v16, 2.0);  // 0 or 1
    return vec4(r / 31.0, g / 31.0, b / 31.0, stp > 0.5 ? 0.5 : 1.0);
}

// Read the raw 16-bit value at integer VRAM (x,y).
float read_vram_u16(float x, float y) {
    vec2 uv = vec2((x + 0.5) / VRAM_W, (y + 0.5) / VRAM_H);
    return pack_rg_u16(texture(s_vram, uv).rg);
}

void main() {
    // bright (a_texcoord.z, raw u8 0..255) is the engine's untextured
    // marker: PsyX_GPU::MakeTexcoordTriangleZero/QuadZero set bright=1
    // for untextured fills, MakeTexcoordTriangle/Quad sets bright=2 for
    // textured ones. So bright<=1.5 → no texture sampling, just vertex
    // colour. Avoids the "page=0+uv=0 happens to be a real texel"
    // ambiguity that previously made the player character grey.
    if (v_texcoord.z < 1.5) {
        out_color = vec4(min(v_color.rgb * 2.0, vec3(1.0)), 1.0);
        return;
    }

    // FLOOR the interpolated UV to integer texel coordinates BEFORE doing
    // any palette-index math. Otherwise sub-texel fractions leak into
    // mod(u,2)/mod(u,4)/pow(16,sub) and produce wrong palette indices,
    // which manifests as the high-frequency noise pattern across the whole
    // textured surface.
    float u = floor(v_texcoord.x + 0.0001);   // 0..255 integer texel
    float v = floor(v_texcoord.y + 0.0001);
    float fmt = v_texcoord.w;      // 0=4bit, 1=8bit, 2/3=16bit
    vec2 page = v_page_clut.xy;
    vec2 clut = v_page_clut.zw;

    vec4 texel;
    if (fmt < 0.5) {
        // 4-bit paletted: 4 pixels per VRAM cell
        float cellX = page.x + floor(u / 4.0);
        float cellY = page.y + v;
        float cell  = read_vram_u16(cellX, cellY);
        float subX  = mod(u, 4.0);
        float nibble = floor(cell / pow(16.0, subX));
        nibble = mod(nibble, 16.0);
        float c16 = read_vram_u16(clut.x + nibble, clut.y);
        if (c16 == 0.0) discard;
        texel = psx16_to_rgba(c16);
    } else if (fmt < 1.5) {
        // 8-bit paletted: 2 pixels per VRAM cell
        float cellX = page.x + floor(u / 2.0);
        float cellY = page.y + v;
        float cell  = read_vram_u16(cellX, cellY);
        float idx   = (mod(u, 2.0) < 0.5) ? mod(cell, 256.0) : floor(cell / 256.0);
        float c16   = read_vram_u16(clut.x + idx, clut.y);
        if (c16 == 0.0) discard;
        texel = psx16_to_rgba(c16);
    } else {
        // 16-bit direct
        float c16 = read_vram_u16(page.x + u, page.y + v);
        if (c16 == 0.0) discard;
        texel = psx16_to_rgba(c16);
    }

    // PSX modulates texel by 2× the vertex colour (rgba=128 → 1.0).
    out_color = vec4(texel.rgb * min(v_color.rgb * 2.0, vec3(1.0)), texel.a);
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
	si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
	si.unnormalizedCoordinates = VK_FALSE;
	si.compareEnable = VK_FALSE;
	VK_CHECK(vkCreateSampler(g_vkDevice, &si, nullptr, &g_vkVramSampler));

	// Staging buffer (host-visible, persistently mapped) for VRAM uploads.
	const VkDeviceSize vramBytes = (VkDeviceSize)VRAM_WIDTH * VRAM_HEIGHT * sizeof(uint16_t);
	if (!CreateBuffer(vramBytes,
		VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		&g_vkVramStaging, &g_vkVramStagingMem)) {
		return false;
	}
	VK_CHECK(vkMapMemory(g_vkDevice, g_vkVramStagingMem, 0, VK_WHOLE_SIZE, 0, &g_vkVramStagingMap));
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
	vkCmdCopyBufferToImage(cb, g_vkVramStaging, g_vkVramImage,
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

	VkPipelineColorBlendAttachmentState cbaArr[5];
	VkPipelineColorBlendStateCreateInfo cbArr[5] = {};
	for (int i = 0; i < 5; ++i) {
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

	// Build one PSO per blend mode — same vertex/fragment, the colour-
	// blend state and depth-write change. GR_SetBlendMode picks the
	// right index, GR_DrawTriangles re-binds when it changes.
	bool ok = true;
	for (int i = 0; i < 5; ++i) {
		gpci.pColorBlendState = &cbArr[i];
		gpci.pDepthStencilState = (i == BM_NONE) ? &dsOpaque : &dsBlend;
		VK_CHECK(vkCreateGraphicsPipelines(g_vkDevice, VK_NULL_HANDLE, 1, &gpci, nullptr, &g_vkPipelines[i]));
		if (g_vkPipelines[i] == VK_NULL_HANDLE) ok = false;
	}
	return ok;
}

bool CreateVertexBuffer()
{
	const VkDeviceSize totalBytes = (VkDeviceSize)sizeof(GrVertex) * MAX_VERTEX_BUFFER_SIZE;
	if (!CreateBuffer(totalBytes,
		VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		&g_vkVertexBuffer, &g_vkVertexBufferMem)) {
		return false;
	}
	VK_CHECK(vkMapMemory(g_vkDevice, g_vkVertexBufferMem, 0, VK_WHOLE_SIZE, 0, &g_vkVertexBufferMap));
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

	if (g_vkVertexBufferMap)  { vkUnmapMemory(g_vkDevice, g_vkVertexBufferMem); g_vkVertexBufferMap = nullptr; }
	if (g_vkVertexBuffer)     { vkDestroyBuffer(g_vkDevice, g_vkVertexBuffer, nullptr);     g_vkVertexBuffer = VK_NULL_HANDLE; }
	if (g_vkVertexBufferMem)  { vkFreeMemory(g_vkDevice, g_vkVertexBufferMem, nullptr);     g_vkVertexBufferMem = VK_NULL_HANDLE; }

	if (g_vkDepthImageView)   { vkDestroyImageView(g_vkDevice, g_vkDepthImageView, nullptr); g_vkDepthImageView = VK_NULL_HANDLE; }
	if (g_vkDepthImage)       { vkDestroyImage(g_vkDevice, g_vkDepthImage, nullptr); g_vkDepthImage = VK_NULL_HANDLE; }
	if (g_vkDepthImageMem)    { vkFreeMemory(g_vkDevice, g_vkDepthImageMem, nullptr); g_vkDepthImageMem = VK_NULL_HANDLE; }

	if (g_vkVramStagingMap)   { vkUnmapMemory(g_vkDevice, g_vkVramStagingMem); g_vkVramStagingMap = nullptr; }
	if (g_vkVramStaging)      { vkDestroyBuffer(g_vkDevice, g_vkVramStaging, nullptr); g_vkVramStaging = VK_NULL_HANDLE; }
	if (g_vkVramStagingMem)   { vkFreeMemory(g_vkDevice, g_vkVramStagingMem, nullptr); g_vkVramStagingMem = VK_NULL_HANDLE; }
	if (g_vkVramSampler)      { vkDestroySampler(g_vkDevice, g_vkVramSampler, nullptr); g_vkVramSampler = VK_NULL_HANDLE; }
	if (g_vkVramImageView)    { vkDestroyImageView(g_vkDevice, g_vkVramImageView, nullptr); g_vkVramImageView = VK_NULL_HANDLE; }
	if (g_vkVramImage)        { vkDestroyImage(g_vkDevice, g_vkVramImage, nullptr); g_vkVramImage = VK_NULL_HANDLE; }
	if (g_vkVramImageMem)     { vkFreeMemory(g_vkDevice, g_vkVramImageMem, nullptr); g_vkVramImageMem = VK_NULL_HANDLE; }

	if (g_vkDescPool)         { vkDestroyDescriptorPool(g_vkDevice, g_vkDescPool, nullptr); g_vkDescPool = VK_NULL_HANDLE; }
	if (g_vkDescSetLayout)    { vkDestroyDescriptorSetLayout(g_vkDevice, g_vkDescSetLayout, nullptr); g_vkDescSetLayout = VK_NULL_HANDLE; }

	for (int i = 0; i < 5; ++i) {
		if (g_vkPipelines[i]) { vkDestroyPipeline(g_vkDevice, g_vkPipelines[i], nullptr); g_vkPipelines[i] = VK_NULL_HANDLE; }
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
		memcpy(g_vkVramStagingMap, g_vkVramCPU, sizeof(g_vkVramCPU));

		// SHADER_READ_ONLY → TRANSFER_DST → copy → SHADER_READ_ONLY
		VramImageBarrier(cb,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

		VkBufferImageCopy r = {};
		r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		r.imageSubresource.layerCount = 1;
		r.imageExtent = { VRAM_WIDTH, VRAM_HEIGHT, 1 };
		vkCmdCopyBufferToImage(cb, g_vkVramStaging, g_vkVramImage,
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

	// Default viewport + scissor cover the whole swapchain. The Y axis is
	// flipped (negative height) so vertices coming out of GR_Ortho2D /
	// GR_Perspective3D — which are written for the GL backend's clip
	// space (Y-up) — still rasterise the right way up under Vulkan's
	// Y-down clip space. Requires VK_KHR_maintenance1, in core since 1.1.
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
	g_vkBoundPipelineIdx = BM_NONE;
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_vkPipelines[BM_NONE]);
	VkDeviceSize zero = 0;
	vkCmdBindVertexBuffers(cb, 0, 1, &g_vkVertexBuffer, &zero);
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

	VkCommandBuffer cb = g_vkCmdBuffers[g_vkCurrentFrame];
	vkCmdEndRenderPass(cb);
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
	if (!vertices || count <= 0 || !g_vkVertexBufferMap) return;
	if (count > MAX_VERTEX_BUFFER_SIZE) count = MAX_VERTEX_BUFFER_SIZE;
	memcpy(g_vkVertexBufferMap, vertices, sizeof(GrVertex) * (size_t)count);
	g_vkVertexBufferSize = (uint32_t)count;

	// DEBUG: dump first 3 vertices once per ~120 frames — but only when
	// the buffer has more than 30 vertices (skips the FMV's 6-vertex
	// underlay quad so we get real engine geometry samples).
	static int s_dumpTick = 0;
	if (++s_dumpTick >= 120 && count >= 30) {
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
	VkCommandBuffer cb = g_vkCmdBuffers[g_vkCurrentFrame];

	// Switch pipeline if blend mode changed since last draw.
	if (g_vkActiveBlendMode != g_vkBoundPipelineIdx) {
		const int idx = (g_vkActiveBlendMode >= 0 && g_vkActiveBlendMode < 5)
			? g_vkActiveBlendMode : 0;
		vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_vkPipelines[idx]);
		g_vkBoundPipelineIdx = idx;
	}

	vkCmdPushConstants(cb, g_vkPipelineLayout,
		VK_SHADER_STAGE_VERTEX_BIT,
		0, sizeof(PushBlock), &g_vkPushBlock);

	vkCmdDraw(cb, (uint32_t)(triangles * 3), 1, (uint32_t)start_vertex, 0);

	if (++s_logTickFrame >= 60) {
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

	// DEBUG: count uploads + sample one pixel so we can see if data flows.
	static int s_uploads = 0;
	static int s_logTick = 0;
	s_uploads++;
	if (++s_logTick >= 20) {
		s_logTick = 0;
		// pick the centre of the just-copied region
		int cx = dst_x + w/2;
		int cy = dst_y + h/2;
		unsigned short v = g_vkVramCPU[cy * VRAM_WIDTH + cx];
		eprintinfo("[VK][VRAM] upload #%d → (%d,%d) %dx%d  centre[%d,%d]=0x%04x\n",
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
void GR_EnableDepth(int v)       { g_PreviousDepthMode = v; }
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
