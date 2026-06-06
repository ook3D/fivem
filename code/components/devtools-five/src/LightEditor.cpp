#include <StdInc.h>
#include <CoreConsole.h>
#include <ConsoleHost.h>
#include <imgui.h>
#include <Hooking.h>
#include <Streaming.h>
#include <Pool.h>
#include <EntitySystem.h>
#include <nutsnbolts.h>
#include <sysAllocator.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <Windows.h>

namespace
{
constexpr float DEG2RAD = 3.14159265358979323846f / 180.0f;
constexpr float RAD2DEG = 180.0f / 3.14159265358979323846f;
constexpr float TWO_PI = 6.28318530717958647692f;

enum class eLightType : uint8_t
{
	Point = 1,
	Spot = 2,
	Capsule = 4,
};

#pragma pack(push, 1)
struct CLightAttr
{
	uintptr_t __vtable;               // 0x00 - runtime vtable, do NOT write
	float     position[3];            // 0x08
	uint32_t  pad0;                   // 0x14
	uint8_t   color[3];               // 0x18 - RGB 0..255
	uint8_t   flashiness;             // 0x1B
	float     intensity;              // 0x1C
	uint32_t  flags;                  // 0x20
	uint16_t  boneId;                 // 0x24
	uint8_t   lightType;              // 0x26 - see eLightType
	uint8_t   groupId;                // 0x27
	uint32_t  timeFlags;              // 0x28 - per-hour on/off bitmask
	float     falloff;                // 0x2C - range / radius
	float     falloffExponent;        // 0x30
	float     cullingPlane[4];        // 0x34 - normal xyz + offset
	uint8_t   shadowBlur;             // 0x44
	uint8_t   unk1;                   // 0x45
	uint16_t  unk2;                   // 0x46
	uint32_t  unk3;                   // 0x48
	float     volumeIntensity;        // 0x4C
	float     volumeSizeScale;        // 0x50
	uint8_t   volumeOuterColor[3];    // 0x54 - RGB 0..255
	uint8_t   lightHash;              // 0x57
	float     volumeOuterIntensity;   // 0x58
	float     coronaSize;             // 0x5C
	float     volumeOuterExponent;    // 0x60
	uint8_t   lightFadeDistance;      // 0x64
	uint8_t   shadowFadeDistance;     // 0x65
	uint8_t   specularFadeDistance;   // 0x66
	uint8_t   volumetricFadeDistance; // 0x67
	float     shadowNearClip;         // 0x68
	float     coronaIntensity;        // 0x6C
	float     coronaZBias;            // 0x70
	float     direction[3];           // 0x74
	float     tangent[3];             // 0x80
	float     coneInnerAngle;         // 0x8C - degrees (spot)
	float     coneOuterAngle;         // 0x90 - degrees (spot)
	float     extents[3];             // 0x94 - capsule extents
	uint32_t  projectedTextureHash;   // 0xA0
	uint32_t  unk4;                   // 0xA4
};                                    // 0xA8
#pragma pack(pop)

static_assert(sizeof(CLightAttr) == 0xA8, "CLightAttr must match the runtime stride");

void* FindDrawable(const char* name)
{
	auto mgr = streaming::Manager::GetInstance();
	if (!mgr)
	{
		return nullptr;
	}

	auto ydrStore = mgr->moduleMgr.GetStreamingModule("ydr");
	if (!ydrStore)
	{
		return nullptr;
	}

	uint32_t slotId = -1;
	if (*ydrStore->FindSlot(&slotId, name) == -1)
	{
		return nullptr;
	}

	return ydrStore->GetPtr(slotId);
}

struct LightArray
{
	CLightAttr* lights;
	uint16_t count;
	uint16_t size;
};

bool IsReadable(const void* p, size_t size)
{
	if (!p)
	{
		return false;
	}

	MEMORY_BASIC_INFORMATION mbi{};
	if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT)
	{
		return false;
	}

	const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY;
	if (!(mbi.Protect & readable) || (mbi.Protect & PAGE_GUARD))
	{
		return false;
	}

	const auto regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
	return reinterpret_cast<uintptr_t>(p) + size <= regionEnd;
}

bool IsLightType(uint8_t t)
{
	return t == 1 || t == 2 || t == 4 || t == 8 || t == 16;
}

bool LooksLikeLightArray(const LightArray& arr)
{
	if (!arr.lights || arr.count == 0 || arr.count > arr.size || arr.size > 4096)
	{
		return false;
	}

	if (!IsReadable(arr.lights, static_cast<size_t>(arr.size) * sizeof(CLightAttr)))
	{
		return false;
	}

	const int sampleCount = arr.count < 4 ? arr.count : 4;
	for (int i = 0; i < sampleCount; ++i)
	{
		const CLightAttr& l = arr.lights[i];
		if (l.__vtable == 0 || !IsReadable(reinterpret_cast<const void*>(l.__vtable), sizeof(void*)))
		{
			return false;
		}
		if (!IsLightType(l.lightType) || l.flashiness > 30)
		{
			return false;
		}
		if (!std::isfinite(l.intensity) || l.intensity < 0.0f || l.intensity > 1.0e6f)
		{
			return false;
		}
		if (!std::isfinite(l.falloff) || l.falloff < 0.0f)
		{
			return false;
		}
	}

	return true;
}

int g_lightArrayOffset = -1;

bool ReadLightArray(void* drawable, LightArray& out)
{
	if (!drawable)
	{
		return false;
	}

	auto* base = reinterpret_cast<uint8_t*>(drawable);

	auto readAt = [&](int offset, LightArray& dst)
	{
		dst.lights = *reinterpret_cast<CLightAttr**>(base + offset);
		dst.count = *reinterpret_cast<uint16_t*>(base + offset + sizeof(void*));
		dst.size = *reinterpret_cast<uint16_t*>(base + offset + sizeof(void*) + sizeof(uint16_t));
	};

	if (g_lightArrayOffset >= 0)
	{
		readAt(g_lightArrayOffset, out);
		return true;
	}

	for (int offset = 0x90; offset <= 0x150; offset += 8)
	{
		LightArray candidate{};
		readAt(offset, candidate);
		if (LooksLikeLightArray(candidate))
		{
			g_lightArrayOffset = offset;
			out = candidate;
			return true;
		}
	}

	return false;
}

int GetLightCount(void* drawable)
{
	LightArray arr{};
	if (!ReadLightArray(drawable, arr) || !arr.lights)
	{
		return 0;
	}
	return arr.count;
}

CLightAttr* GetLight(void* drawable, int index)
{
	if (index < 0)
	{
		return nullptr;
	}

	LightArray arr{};
	if (!ReadLightArray(drawable, arr) || !arr.lights || index >= arr.count)
	{
		return nullptr;
	}

	return &arr.lights[index];
}

bool GetLightArrayFields(void* drawable, CLightAttr*** outLights, uint16_t** outCount, uint16_t** outSize)
{
	if (!drawable || g_lightArrayOffset < 0)
	{
		return false;
	}

	uint8_t* base = reinterpret_cast<uint8_t*>(drawable) + g_lightArrayOffset;
	*outLights = reinterpret_cast<CLightAttr**>(base);
	*outCount = reinterpret_cast<uint16_t*>(base + sizeof(void*));
	*outSize = reinterpret_cast<uint16_t*>(base + sizeof(void*) + sizeof(uint16_t));
	return true;
}

using AddAttachedLightsFn = uint32_t (*)(void* entity);
using RemoveAttachedLightsFn = void (*)(void* entity);
AddAttachedLightsFn g_addAttachedLights = nullptr;
RemoveAttachedLightsFn g_removeAttachedLights = nullptr;

constexpr int kEntityFlagsOffset = 0xC0;
constexpr uint32_t kLightObjectFlag = 0x20;

bool LightRebuildAvailable()
{
	return g_addAttachedLights && g_removeAttachedLights;
}

void SetLightObjectFlag(void* entity, bool on)
{
	auto* flags = reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(entity) + kEntityFlagsOffset);
	if (on)
		*flags |= kLightObjectFlag;
	else
		*flags &= ~kLightObjectFlag;
}

void CollectModelInstances(const char* modelName, std::vector<void*>& out)
{
	if (!modelName || !modelName[0])
	{
		return;
	}

	rage::fwModelId modelId{};
	auto target = rage::fwArchetypeManager::GetArchetypeFromHashKey(HashString(modelName), modelId);
	if (!target)
	{
		return;
	}

	static const char* kPoolNames[] = { "Building", "AnimatedBuilding", "Object", "Dummy Object" };
	for (const char* poolName : kPoolNames)
	{
		auto pool = rage::GetPool<fwEntity>(poolName);
		if (!pool)
		{
			continue;
		}

		const int size = static_cast<int>(pool->GetSize());
		for (int i = 0; i < size; ++i)
		{
			fwEntity* entity = pool->GetAt(i);
			if (entity && entity->GetArchetype() == target)
			{
				out.push_back(entity);
			}
		}
	}
}

void ApplyAddLight(void* drawable, const CLightAttr& src)
{
	CLightAttr** pLights;
	uint16_t* pCount;
	uint16_t* pSize;
	if (!GetLightArrayFields(drawable, &pLights, &pCount, &pSize))
	{
		return;
	}

	CLightAttr* lights = *pLights;
	const uint16_t count = *pCount;
	const uint16_t size = *pSize;

	// Spare capacity: append in place, no reallocation.
	if (lights && count < size)
	{
		lights[count] = src;
		*pCount = count + 1;
		return;
	}

	const int newSize = count + 16;
	if (newSize > 0xFFFF)
	{
		return;
	}

	CLightAttr* nb = static_cast<CLightAttr*>(rage::GetAllocator()->Allocate(sizeof(CLightAttr) * newSize, 16, 0));
	if (!nb)
	{
		return;
	}

	if (lights && count > 0)
	{
		memcpy(nb, lights, sizeof(CLightAttr) * count);
	}
	nb[count] = src;

	*pLights = nb;
	*pSize = static_cast<uint16_t>(newSize);
	*pCount = count + 1;
}

void ApplyRemoveLight(void* drawable, int index)
{
	CLightAttr** pLights;
	uint16_t* pCount;
	uint16_t* pSize;
	if (!GetLightArrayFields(drawable, &pLights, &pCount, &pSize))
	{
		return;
	}

	CLightAttr* lights = *pLights;
	const uint16_t count = *pCount;
	if (!lights || index < 0 || index >= count)
	{
		return;
	}

	for (int i = index; i < count - 1; ++i)
	{
		lights[i] = lights[i + 1];
	}
	*pCount = count - 1;
}

enum class PendingKind
{
	Add,
	Remove,
};

struct PendingOp
{
	PendingKind kind;
	void* drawable;
	int index;            // for Remove
	CLightAttr newLight;  // for Add (full bytes incl. vtable, captured on the UI thread)
	char modelName[128];  // instances to rebuild after the edit
};

std::mutex g_opMutex;
std::vector<PendingOp> g_pendingOps;

void QueueOp(const PendingOp& op)
{
	std::lock_guard<std::mutex> lock(g_opMutex);
	g_pendingOps.push_back(op);
}

void ProcessPendingOps()
{
	std::vector<PendingOp> ops;
	{
		std::lock_guard<std::mutex> lock(g_opMutex);
		ops.swap(g_pendingOps);
	}

	if (ops.empty() || !LightRebuildAvailable())
	{
		return;
	}

	for (const auto& op : ops)
	{
		std::vector<void*> instances;
		CollectModelInstances(op.modelName, instances);

		if (instances.empty())
		{
			trace("[LightEditor] no placed instances of '%s' found - skipping edit (would corrupt)\n", op.modelName);
			continue;
		}

		for (void* entity : instances)
		{
			g_removeAttachedLights(entity);
		}

		// 2. Mutate the drawable's light array.
		if (op.kind == PendingKind::Add)
		{
			ApplyAddLight(op.drawable, op.newLight);
		}
		else
		{
			ApplyRemoveLight(op.drawable, op.index);
		}

		for (void* entity : instances)
		{
			g_addAttachedLights(entity);
			SetLightObjectFlag(entity, true);
		}
	}
}

struct grcViewport
{
	float m_world[16];
	float m_worldView[16];
	float m_worldViewProj[16];
	float m_inverseView[16];   // camera world transform; translation at [12..14]
	float m_view[16];
	float m_projection[16];
};

struct CViewportGame
{
	void* __vtable;
	char pad[8];
	grcViewport viewport;
};

CViewportGame** g_gameViewport = nullptr;

const grcViewport* GetViewport()
{
	return (g_gameViewport && *g_gameViewport) ? &(*g_gameViewport)->viewport : nullptr;
}

void MakeViewProj(const grcViewport* vp, float out[16])
{
	const float* view = vp->m_view;
	const float* proj = vp->m_projection;
	for (int i = 0; i < 4; ++i)
	{
		for (int j = 0; j < 4; ++j)
		{
			float sum = 0.0f;
			for (int k = 0; k < 4; ++k)
			{
				sum += view[i * 4 + k] * proj[k * 4 + j];
			}
			out[i * 4 + j] = sum;
		}
	}
}

struct ModelAnchor
{
	bool valid = false;
	float position[3] = { 0.0f, 0.0f, 0.0f };
	float right[3] = { 1.0f, 0.0f, 0.0f };
	float forward[3] = { 0.0f, 1.0f, 0.0f };
	float up[3] = { 0.0f, 0.0f, 1.0f };
};

ModelAnchor FindModelAnchor(const char* modelName)
{
	ModelAnchor anchor;

	if (!modelName || !modelName[0])
	{
		return anchor;
	}

	rage::fwModelId modelId{};
	auto target = rage::fwArchetypeManager::GetArchetypeFromHashKey(HashString(modelName), modelId);
	if (!target)
	{
		return anchor;
	}

	static const char* kPoolNames[] = { "Building", "AnimatedBuilding", "Object", "Dummy Object" };

	for (const char* poolName : kPoolNames)
	{
		auto pool = rage::GetPool<fwEntity>(poolName);
		if (!pool)
		{
			continue;
		}

		const int size = static_cast<int>(pool->GetSize());
		for (int i = 0; i < size; ++i)
		{
			fwEntity* entity = pool->GetAt(i);
			if (!entity || entity->GetArchetype() != target)
			{
				continue;
			}

			const auto& m = entity->GetTransform(); // DirectX::XMFLOAT4X4, row-major
			const float* f = &m._11;

			anchor.right[0] = f[0];   anchor.right[1] = f[1];   anchor.right[2] = f[2];
			anchor.forward[0] = f[4]; anchor.forward[1] = f[5]; anchor.forward[2] = f[6];
			anchor.up[0] = f[8];      anchor.up[1] = f[9];      anchor.up[2] = f[10];
			anchor.position[0] = f[12]; anchor.position[1] = f[13]; anchor.position[2] = f[14];
			anchor.valid = true;
			return anchor;
		}
	}

	return anchor;
}

void DirectionToEuler(const float dir[3], float outEuler[3])
{
	const float len = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
	if (len < 0.0001f)
	{
		outEuler[0] = outEuler[1] = outEuler[2] = 0.0f;
		return;
	}
	const float nx = dir[0] / len, ny = dir[1] / len, nz = dir[2] / len;
	outEuler[0] = asinf(-nz) * RAD2DEG;     // pitch
	outEuler[1] = 0.0f;                      // roll (not derivable from direction)
	outEuler[2] = atan2f(nx, ny) * RAD2DEG; // heading
}

void EulerToDirection(const float euler[3], float outDir[3], float outTan[3])
{
	const float pitch = euler[0] * DEG2RAD;
	const float heading = euler[2] * DEG2RAD;
	const float roll = euler[1] * DEG2RAD;

	const float cosP = cosf(pitch), sinP = sinf(pitch);
	const float cosH = cosf(heading), sinH = sinf(heading);
	const float cosR = cosf(roll), sinR = sinf(roll);

	outDir[0] = sinH * cosP;
	outDir[1] = cosH * cosP;
	outDir[2] = -sinP;

	outTan[0] = -(cosH * cosR + sinH * sinP * sinR);
	outTan[1] = sinH * cosR - cosH * sinP * sinR;
	outTan[2] = -cosP * sinR;
}

bool ProjectToScreen(const float* vp, const float* world, float w, float h, ImVec2* out)
{
	const float cx = vp[0] * world[0] + vp[4] * world[1] + vp[8] * world[2] + vp[12];
	const float cy = vp[1] * world[0] + vp[5] * world[1] + vp[9] * world[2] + vp[13];
	const float cw = vp[3] * world[0] + vp[7] * world[1] + vp[11] * world[2] + vp[15];
	if (cw <= 0.001f)
	{
		return false; // behind the camera
	}
	out->x = (cx / cw * 0.5f + 0.5f) * w;
	out->y = (1.0f - (cy / cw * 0.5f + 0.5f)) * h;
	return true;
}

void DrawRing(ImDrawList* dl, const float* vp, float w, float h,
              const float* center, const float* axA, const float* axB,
              float radius, ImU32 col, int segments = 48)
{
	ImVec2 prev; bool prevOk = false;
	for (int i = 0; i <= segments; ++i)
	{
		const float a = (float)i / segments * TWO_PI;
		const float ca = cosf(a) * radius, sa = sinf(a) * radius;
		const float p[3] = {
			center[0] + axA[0] * ca + axB[0] * sa,
			center[1] + axA[1] * ca + axB[1] * sa,
			center[2] + axA[2] * ca + axB[2] * sa,
		};
		ImVec2 s; const bool ok = ProjectToScreen(vp, p, w, h, &s);
		if (ok && prevOk) dl->AddLine(prev, s, col, 1.5f);
		prev = s; prevOk = ok;
	}
}

void DrawArc(ImDrawList* dl, const float* vp, float w, float h,
             const float* center, const float* axA, const float* axB,
             float radius, float a0, float a1, ImU32 col, int segments = 24)
{
	ImVec2 prev; bool prevOk = false;
	for (int i = 0; i <= segments; ++i)
	{
		const float a = a0 + (a1 - a0) * ((float)i / segments);
		const float ca = cosf(a) * radius, sa = sinf(a) * radius;
		const float p[3] = {
			center[0] + axA[0] * ca + axB[0] * sa,
			center[1] + axA[1] * ca + axB[1] * sa,
			center[2] + axA[2] * ca + axB[2] * sa,
		};
		ImVec2 s; const bool ok = ProjectToScreen(vp, p, w, h, &s);
		if (ok && prevOk) dl->AddLine(prev, s, col, 1.5f);
		prev = s; prevOk = ok;
	}
}

void DrawSphereGizmo(ImDrawList* dl, const float* vp, float w, float h,
                     const float* pos, float radius, ImU32 col)
{
	const float ux[3] = { 1, 0, 0 }, uy[3] = { 0, 1, 0 }, uz[3] = { 0, 0, 1 };
	DrawRing(dl, vp, w, h, pos, ux, uz, radius, col);
	DrawRing(dl, vp, w, h, pos, ux, uy, radius, col);
	DrawRing(dl, vp, w, h, pos, uy, uz, radius, col);
}

void DrawConeGizmo(ImDrawList* dl, const float* vp, float w, float h,
                   const float* apex, const float* tx, const float* ty, const float* dir,
                   float radius, float height, ImU32 col)
{
	const float baseC[3] = {
		apex[0] + dir[0] * height,
		apex[1] + dir[1] * height,
		apex[2] + dir[2] * height,
	};
	DrawRing(dl, vp, w, h, baseC, tx, ty, radius, col);

	ImVec2 apexS;
	if (ProjectToScreen(vp, apex, w, h, &apexS))
	{
		const int spokes = 4;
		for (int i = 0; i < spokes; ++i)
		{
			const float a = (float)i / spokes * TWO_PI;
			const float ca = cosf(a) * radius, sa = sinf(a) * radius;
			const float p[3] = {
				baseC[0] + tx[0] * ca + ty[0] * sa,
				baseC[1] + tx[1] * ca + ty[1] * sa,
				baseC[2] + tx[2] * ca + ty[2] * sa,
			};
			ImVec2 s;
			if (ProjectToScreen(vp, p, w, h, &s)) dl->AddLine(apexS, s, col, 1.5f);
		}
	}
}

void DrawCapsuleGizmo(ImDrawList* dl, const float* vp, float w, float h,
                      const float* pos, const float* tx, const float* ty, const float* dir,
                      float radius, float halfLen, ImU32 col)
{
	const float cA[3] = { pos[0] + dir[0] * halfLen, pos[1] + dir[1] * halfLen, pos[2] + dir[2] * halfLen };
	const float cB[3] = { pos[0] - dir[0] * halfLen, pos[1] - dir[1] * halfLen, pos[2] - dir[2] * halfLen };

	DrawRing(dl, vp, w, h, cA, tx, ty, radius, col);
	DrawRing(dl, vp, w, h, cB, tx, ty, radius, col);

	const float* axes[2] = { tx, ty };
	for (int axis = 0; axis < 2; ++axis)
	{
		for (int sign = -1; sign <= 1; sign += 2)
		{
			const float off[3] = {
				axes[axis][0] * radius * sign,
				axes[axis][1] * radius * sign,
				axes[axis][2] * radius * sign,
			};
			const float pA[3] = { cA[0] + off[0], cA[1] + off[1], cA[2] + off[2] };
			const float pB[3] = { cB[0] + off[0], cB[1] + off[1], cB[2] + off[2] };
			ImVec2 sA, sB;
			if (ProjectToScreen(vp, pA, w, h, &sA) && ProjectToScreen(vp, pB, w, h, &sB))
				dl->AddLine(sA, sB, col, 1.5f);
		}
	}

	for (int axis = 0; axis < 2; ++axis)
	{
		DrawArc(dl, vp, w, h, cA, axes[axis], dir, radius, -1.5707963f, 1.5707963f, col); // +dir bulge
		DrawArc(dl, vp, w, h, cB, axes[axis], dir, radius, 1.5707963f, 4.7123889f, col);  // -dir bulge
	}
}

void DrawLightFalloffGizmo(const float* viewProj, const CLightAttr* light,
                           const float* pos, const float* wdir, const float* wtan,
                           bool showExponentShell)
{
	ImGuiIO& io = ImGui::GetIO();
	const float w = io.DisplaySize.x, h = io.DisplaySize.y;
	ImDrawList* dl = ImGui::GetForegroundDrawList();

	const ImU32 colWhite = IM_COL32(235, 235, 235, 200);
	const ImU32 colBlue = IM_COL32(80, 140, 255, 220);
	const ImU32 colShell = IM_COL32(255, 190, 70, 130);

	const float falloff = light->falloff;
	float halfR = -1.0f;
	if (showExponentShell && falloff > 0.001f && light->falloffExponent > 0.01f)
		halfR = falloff * (1.0f - powf(0.5f, 1.0f / light->falloffExponent));

	float ty[3] = {
		wdir[1] * wtan[2] - wdir[2] * wtan[1],
		wdir[2] * wtan[0] - wdir[0] * wtan[2],
		wdir[0] * wtan[1] - wdir[1] * wtan[0],
	};
	const float tyLen = sqrtf(ty[0] * ty[0] + ty[1] * ty[1] + ty[2] * ty[2]);
	if (tyLen > 1e-5f) { ty[0] /= tyLen; ty[1] /= tyLen; ty[2] /= tyLen; }

	switch (static_cast<eLightType>(light->lightType))
	{
		case eLightType::Point:
		{
			DrawSphereGizmo(dl, viewProj, w, h, pos, falloff, colWhite);
			if (halfR > 0.0f) DrawSphereGizmo(dl, viewProj, w, h, pos, halfR, colShell);
			break;
		}
		case eLightType::Spot:
		{
			const float inner = (light->coneInnerAngle < light->coneOuterAngle ? light->coneInnerAngle : light->coneOuterAngle) * DEG2RAD;
			const float outer = (light->coneInnerAngle > light->coneOuterAngle ? light->coneInnerAngle : light->coneOuterAngle) * DEG2RAD;
			DrawConeGizmo(dl, viewProj, w, h, pos, wtan, ty, wdir, sinf(outer) * falloff, cosf(outer) * falloff, colBlue);
			DrawConeGizmo(dl, viewProj, w, h, pos, wtan, ty, wdir, sinf(inner) * falloff, cosf(inner) * falloff, colWhite);
			if (halfR > 0.0f)
				DrawConeGizmo(dl, viewProj, w, h, pos, wtan, ty, wdir, sinf(outer) * halfR, cosf(outer) * halfR, colShell);
			break;
		}
		case eLightType::Capsule:
		{
			const float halfLen = light->extents[0] * 0.5f;
			DrawCapsuleGizmo(dl, viewProj, w, h, pos, wtan, ty, wdir, falloff, halfLen, colWhite);
			if (halfR > 0.0f) DrawCapsuleGizmo(dl, viewProj, w, h, pos, wtan, ty, wdir, halfR, halfLen, colShell);
			break;
		}
		default:
			break;
	}
}

namespace gizmo
{
enum Operation { TRANSLATE, ROTATE };

constexpr float PI = 3.14159265358979323846f;
constexpr float AXIS_HIT_THRESHOLD = 12.0f;
constexpr float CIRCLE_HIT_THRESHOLD = 14.0f;
constexpr float GIZMO_SCREEN_FACTOR = 0.08f;
constexpr int CIRCLE_SEGMENTS = 64;

const ImU32 AXIS_COLORS[3] = {
	IM_COL32(220, 50, 50, 255), IM_COL32(50, 180, 50, 255), IM_COL32(50, 100, 220, 255),
};
const ImU32 AXIS_COLORS_HIGHLIGHT[3] = {
	IM_COL32(255, 120, 120, 255), IM_COL32(120, 255, 120, 255), IM_COL32(120, 180, 255, 255),
};
const ImU32 AXIS_COLORS_DIM[3] = {
	IM_COL32(150, 40, 40, 180), IM_COL32(40, 120, 40, 180), IM_COL32(40, 70, 150, 180),
};

struct State
{
	float rectW, rectH;
	bool using_;
	bool over;
	bool mouseOverGui;
	int hoveredAxis;
	int activeAxis;
	float dragStartMouse[2];
	float dragStartValue[3];
	float dragScreenAxis[2];
	float dragScaleFactor;
};

State g_state = {};

float Vec3Dot(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
float Vec3Length(const float* v) { return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }
void Vec3Normalize(float* v)
{
	float len = Vec3Length(v);
	if (len > 1e-6f) { v[0] /= len; v[1] /= len; v[2] /= len; }
}

bool WorldToScreen(const float* worldPos, const float* viewProj, float w, float h, float* sx, float* sy)
{
	ImVec2 out;
	if (!ProjectToScreen(viewProj, worldPos, w, h, &out))
	{
		return false;
	}
	*sx = out.x;
	*sy = out.y;
	return true;
}

float PointSegmentDist2D(float px, float py, float ax, float ay, float bx, float by, float* t)
{
	float dx = bx - ax, dy = by - ay;
	float len2 = dx * dx + dy * dy;
	if (len2 < 1e-6f)
	{
		*t = 0;
		return sqrtf((px - ax) * (px - ax) + (py - ay) * (py - ay));
	}
	*t = ((px - ax) * dx + (py - ay) * dy) / len2;
	*t = std::max(0.0f, std::min(1.0f, *t));
	float cx = ax + (*t) * dx;
	float cy = ay + (*t) * dy;
	return sqrtf((px - cx) * (px - cx) + (py - cy) * (py - cy));
}

void DrawArrowHead(ImDrawList* dl, float tipX, float tipY, float originX, float originY, ImU32 color)
{
	float dx = tipX - originX, dy = tipY - originY;
	float len = sqrtf(dx * dx + dy * dy);
	if (len < 5.0f) return;

	float ndx = dx / len, ndy = dy / len;
	float perpX = -ndy, perpY = ndx;
	float arrowSize = 10.0f;

	ImVec2 tip(tipX, tipY);
	ImVec2 a1(tip.x - ndx * arrowSize + perpX * arrowSize * 0.4f, tip.y - ndy * arrowSize + perpY * arrowSize * 0.4f);
	ImVec2 a2(tip.x - ndx * arrowSize - perpX * arrowSize * 0.4f, tip.y - ndy * arrowSize - perpY * arrowSize * 0.4f);
	dl->AddTriangleFilled(tip, a1, a2, color);
}

void BeginFrame()
{
	if (!g_state.using_)
	{
		g_state.hoveredAxis = -1;
		g_state.over = false;
	}
}

void SetRect(float w, float h) { g_state.rectW = w; g_state.rectH = h; }
void SetMouseOverGui(bool over) { g_state.mouseOverGui = over; }

// position/rotation are in/out. Returns true if modified this frame.
bool Manipulate(const float* viewProj, const float* camPos, Operation operation,
                float* position, float* rotation, const float* snap)
{
	ImGuiIO& io = ImGui::GetIO();
	ImDrawList* drawList = ImGui::GetForegroundDrawList();

	float screenW = g_state.rectW;
	float screenH = g_state.rectH;

	float diff[3] = { position[0] - camPos[0], position[1] - camPos[1], position[2] - camPos[2] };
	float dist = Vec3Length(diff);
	float gizmoScale = dist * GIZMO_SCREEN_FACTOR;
	if (gizmoScale < 0.1f) gizmoScale = 0.1f;

	float screenOrigin[2];
	if (!WorldToScreen(position, viewProj, screenW, screenH, &screenOrigin[0], &screenOrigin[1]))
	{
		g_state.using_ = false;
		return false;
	}

	float mx = io.MousePos.x;
	float my = io.MousePos.y;
	bool changed = false;

	bool canStartInteraction = !g_state.mouseOverGui;

	static const float axes[3][3] = { {1, 0, 0}, {0, 1, 0}, {0, 0, 1} };

	if (operation == TRANSLATE)
	{
		float screenTips[3][2];
		bool axisBehind[3] = {};

		for (int i = 0; i < 3; i++)
		{
			float tip[3] = {
				position[0] + axes[i][0] * gizmoScale,
				position[1] + axes[i][1] * gizmoScale,
				position[2] + axes[i][2] * gizmoScale,
			};
			axisBehind[i] = !WorldToScreen(tip, viewProj, screenW, screenH, &screenTips[i][0], &screenTips[i][1]);
		}

		if (!g_state.using_)
		{
			g_state.hoveredAxis = -1;

			if (canStartInteraction)
			{
				float minDist = AXIS_HIT_THRESHOLD;
				for (int i = 0; i < 3; i++)
				{
					if (axisBehind[i]) continue;
					float t;
					float d = PointSegmentDist2D(mx, my, screenOrigin[0], screenOrigin[1], screenTips[i][0], screenTips[i][1], &t);
					if (d < minDist && t > 0.05f) { minDist = d; g_state.hoveredAxis = i; }
				}
			}

			g_state.over = (g_state.hoveredAxis >= 0);

			if (g_state.hoveredAxis >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
			{
				g_state.using_ = true;
				g_state.activeAxis = g_state.hoveredAxis;
				g_state.dragStartMouse[0] = mx;
				g_state.dragStartMouse[1] = my;
				g_state.dragStartValue[0] = position[0];
				g_state.dragStartValue[1] = position[1];
				g_state.dragStartValue[2] = position[2];

				int a = g_state.activeAxis;
				float sdx = screenTips[a][0] - screenOrigin[0];
				float sdy = screenTips[a][1] - screenOrigin[1];
				float slen = sqrtf(sdx * sdx + sdy * sdy);
				if (slen > 1e-3f)
				{
					g_state.dragScreenAxis[0] = sdx / slen;
					g_state.dragScreenAxis[1] = sdy / slen;
					g_state.dragScaleFactor = gizmoScale / slen;
				}
			}
		}

		if (g_state.using_)
		{
			if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
			{
				float dmx = mx - g_state.dragStartMouse[0];
				float dmy = my - g_state.dragStartMouse[1];
				float projected = dmx * g_state.dragScreenAxis[0] + dmy * g_state.dragScreenAxis[1];

				int a = g_state.activeAxis;
				float worldDelta = projected * g_state.dragScaleFactor;
				float newVal = g_state.dragStartValue[a] + worldDelta;
				if (snap) newVal = roundf(newVal / snap[0]) * snap[0];
				if (position[a] != newVal) { position[a] = newVal; changed = true; }
			}
			else
			{
				g_state.using_ = false;
				g_state.activeAxis = -1;
			}
		}

		for (int i = 0; i < 3; i++)
		{
			if (axisBehind[i]) continue;

			bool isActive = (g_state.using_ && g_state.activeAxis == i);
			bool isHovered = (!g_state.using_ && g_state.hoveredAxis == i);
			ImU32 color = (isActive || isHovered) ? AXIS_COLORS_HIGHLIGHT[i] : AXIS_COLORS[i];
			float thickness = (isActive || isHovered) ? 4.0f : 3.0f;

			if (g_state.using_ && g_state.activeAxis != i) { color = AXIS_COLORS_DIM[i]; thickness = 2.0f; }

			drawList->AddLine(ImVec2(screenOrigin[0], screenOrigin[1]), ImVec2(screenTips[i][0], screenTips[i][1]), color, thickness);
			DrawArrowHead(drawList, screenTips[i][0], screenTips[i][1], screenOrigin[0], screenOrigin[1], color);

			const char* labels[] = { "X", "Y", "Z" };
			drawList->AddText(ImVec2(screenTips[i][0] + 6, screenTips[i][1] - 8), color, labels[i]);
		}

		drawList->AddCircleFilled(ImVec2(screenOrigin[0], screenOrigin[1]), 4.0f, IM_COL32(255, 255, 255, 200));
	}
	else // ROTATE
	{
		float circleRadius = gizmoScale * 0.8f;

		static const float tangents1[3][3] = { {0, 1, 0}, {1, 0, 0}, {1, 0, 0} };
		static const float tangents2[3][3] = { {0, 0, 1}, {0, 0, 1}, {0, 1, 0} };

		if (!g_state.using_)
		{
			g_state.hoveredAxis = -1;

			if (canStartInteraction)
			{
				float minDist = CIRCLE_HIT_THRESHOLD;
				for (int axisIdx = 0; axisIdx < 3; axisIdx++)
				{
					float closestDist = FLT_MAX;
					for (int s = 0; s < CIRCLE_SEGMENTS; s++)
					{
						float angle = (float)s / CIRCLE_SEGMENTS * 2.0f * PI;
						float c = cosf(angle), sn = sinf(angle);
						float wp[3] = {
							position[0] + (tangents1[axisIdx][0] * c + tangents2[axisIdx][0] * sn) * circleRadius,
							position[1] + (tangents1[axisIdx][1] * c + tangents2[axisIdx][1] * sn) * circleRadius,
							position[2] + (tangents1[axisIdx][2] * c + tangents2[axisIdx][2] * sn) * circleRadius,
						};
						float sp[2];
						if (WorldToScreen(wp, viewProj, screenW, screenH, &sp[0], &sp[1]))
						{
							float d = sqrtf((mx - sp[0]) * (mx - sp[0]) + (my - sp[1]) * (my - sp[1]));
							if (d < closestDist) closestDist = d;
						}
					}
					if (closestDist < minDist) { minDist = closestDist; g_state.hoveredAxis = axisIdx; }
				}
			}

			g_state.over = (g_state.hoveredAxis >= 0);

			if (g_state.hoveredAxis >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
			{
				g_state.using_ = true;
				g_state.activeAxis = g_state.hoveredAxis;
				g_state.dragStartMouse[0] = mx;
				g_state.dragStartMouse[1] = my;
				g_state.dragStartValue[0] = rotation[0];
				g_state.dragStartValue[1] = rotation[1];
				g_state.dragStartValue[2] = rotation[2];
			}
		}

		if (g_state.using_)
		{
			if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
			{
				float startDx = g_state.dragStartMouse[0] - screenOrigin[0];
				float startDy = g_state.dragStartMouse[1] - screenOrigin[1];
				float curDx = mx - screenOrigin[0];
				float curDy = my - screenOrigin[1];

				float startAngle = atan2f(startDy, startDx);
				float curAngle = atan2f(curDy, curDx);
				float deltaAngle = (curAngle - startAngle) * RAD2DEG;

				int a = g_state.activeAxis;
				float camDir[3] = { position[0] - camPos[0], position[1] - camPos[1], position[2] - camPos[2] };
				Vec3Normalize(camDir);
				float dotAxis = Vec3Dot(camDir, axes[a]);
				if (dotAxis < 0) deltaAngle = -deltaAngle;

				float newVal = g_state.dragStartValue[a] + deltaAngle;
				while (newVal > 360.0f) newVal -= 360.0f;
				while (newVal < -360.0f) newVal += 360.0f;
				if (snap) newVal = roundf(newVal / snap[0]) * snap[0];
				if (rotation[a] != newVal) { rotation[a] = newVal; changed = true; }
			}
			else
			{
				g_state.using_ = false;
				g_state.activeAxis = -1;
			}
		}

		for (int axisIdx = 0; axisIdx < 3; axisIdx++)
		{
			bool isActive = (g_state.using_ && g_state.activeAxis == axisIdx);
			bool isHovered = (!g_state.using_ && g_state.hoveredAxis == axisIdx);
			ImU32 color = (isActive || isHovered) ? AXIS_COLORS_HIGHLIGHT[axisIdx] : AXIS_COLORS[axisIdx];
			float thickness = (isActive || isHovered) ? 3.5f : 2.5f;

			if (g_state.using_ && g_state.activeAxis != axisIdx) { color = AXIS_COLORS_DIM[axisIdx]; thickness = 1.5f; }

			float prevSp[2] = {};
			bool prevValid = false;
			for (int s = 0; s <= CIRCLE_SEGMENTS; s++)
			{
				float angle = (float)(s % CIRCLE_SEGMENTS) / CIRCLE_SEGMENTS * 2.0f * PI;
				float c = cosf(angle), sn = sinf(angle);
				float wp[3] = {
					position[0] + (tangents1[axisIdx][0] * c + tangents2[axisIdx][0] * sn) * circleRadius,
					position[1] + (tangents1[axisIdx][1] * c + tangents2[axisIdx][1] * sn) * circleRadius,
					position[2] + (tangents1[axisIdx][2] * c + tangents2[axisIdx][2] * sn) * circleRadius,
				};
				float sp[2];
				bool valid = WorldToScreen(wp, viewProj, screenW, screenH, &sp[0], &sp[1]);
				if (valid && prevValid)
					drawList->AddLine(ImVec2(prevSp[0], prevSp[1]), ImVec2(sp[0], sp[1]), color, thickness);
				prevSp[0] = sp[0]; prevSp[1] = sp[1]; prevValid = valid;
			}

			float labelWorldPos[3] = {
				position[0] + tangents2[axisIdx][0] * circleRadius * 1.1f,
				position[1] + tangents2[axisIdx][1] * circleRadius * 1.1f,
				position[2] + tangents2[axisIdx][2] * circleRadius * 1.1f,
			};
			float labelSp[2];
			if (WorldToScreen(labelWorldPos, viewProj, screenW, screenH, &labelSp[0], &labelSp[1]))
			{
				const char* labels[] = { "X", "Y", "Z" };
				drawList->AddText(ImVec2(labelSp[0] + 4, labelSp[1] - 8), color, labels[axisIdx]);
			}
		}

		drawList->AddCircleFilled(ImVec2(screenOrigin[0], screenOrigin[1]), 4.0f, IM_COL32(255, 255, 255, 200));
	}

	return changed;
}
} // namespace gizmo

void BeginProp(const char* label)
{
	ImGui::PushID(label);
	ImGui::Columns(2, nullptr, false);
	ImGui::SetColumnWidth(0, ImGui::GetWindowWidth() * 0.45f);
	ImGui::AlignTextToFramePadding();
	ImGui::Text("%s", label);
	ImGui::NextColumn();
	ImGui::SetNextItemWidth(-1.0f);
}

void EndProp()
{
	ImGui::Columns(1);
	ImGui::PopID();
}

void ColorProperty(const char* label, uint8_t color[3])
{
	BeginProp(label);
	float c[3] = { color[0] / 255.0f, color[1] / 255.0f, color[2] / 255.0f };
	if (ImGui::ColorEdit3("##v", c, ImGuiColorEditFlags_NoInputs))
	{
		color[0] = (uint8_t)(c[0] * 255.0f + 0.5f);
		color[1] = (uint8_t)(c[1] * 255.0f + 0.5f);
		color[2] = (uint8_t)(c[2] * 255.0f + 0.5f);
	}
	EndProp();
}

void FloatProperty(const char* label, float* value, float speed = 0.01f, float min = 0.0f, float max = 0.0f)
{
	BeginProp(label);
	ImGui::DragFloat("##v", value, speed, min, max);
	EndProp();
}

void Float3Property(const char* label, float value[3], float speed = 0.01f)
{
	BeginProp(label);
	ImGui::DragFloat3("##v", value, speed);
	EndProp();
}

void ByteProperty(const char* label, uint8_t* value, int max = 255)
{
	BeginProp(label);
	int v = *value;
	if (ImGui::DragInt("##v", &v, 1.0f, 0, max))
	{
		*value = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
	}
	EndProp();
}

const char* LightTypeName(uint8_t type)
{
	switch (static_cast<eLightType>(type))
	{
		case eLightType::Point: return "Point";
		case eLightType::Spot: return "Spot";
		case eLightType::Capsule: return "Capsule";
		default: return "Unknown";
	}
}

const char* const kLightFlagNames[32] = {
	"Interior Only",              // 0
	"Exterior Only",              // 1
	"Don't Use In Cutscene",      // 2
	"Vehicle",                    // 3
	"FX",                         // 4
	"Texture Projection",         // 5
	"Cast Shadows",               // 6
	"Cast Static Geom Shadows",   // 7
	"Cast Dynamic Geom Shadows",  // 8
	"Calc From Sun",              // 9
	"Enable Buzzing",             // 10
	"Force Buzzing",              // 11
	"Draw Volume",                // 12
	"No Specular",                // 13
	"Both Interior & Exterior",   // 14
	"Corona Only",                // 15
	"Not In Reflection",          // 16
	"Only In Reflection",         // 17
	"Use Cull Plane",             // 18
	"Use Volume Outer Colour",    // 19
	"Cast Higher Res Shadows",    // 20
	"Cast Only Lowres Shadows",   // 21
	"Far LOD Light",              // 22
	"Don't Light Alpha",          // 23
	"Cast Shadows If Possible",   // 24
	"Cutscene",                   // 25
	"Moving Light Source",        // 26
	"Use Vehicle Twin",           // 27
	"Force Medium LOD Light",     // 28
	"Corona Only LOD Light",      // 29
	"Delay Render",               // 30
	"Already Tested For Occlusion", // 31
};

void FlagsPanel(uint32_t* flags)
{
	char header[48];
	snprintf(header, sizeof(header), "Flags (0x%08X)###flags", *flags);

	if (ImGui::CollapsingHeader(header))
	{
		ImGui::Indent();
		if (ImGui::BeginTable("##flaggrid", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings))
		{
			for (int bit = 0; bit < 32; ++bit)
			{
				ImGui::TableNextColumn();
				ImGui::PushID(bit);
				bool on = (*flags & (1u << bit)) != 0;
				if (ImGui::Checkbox(kLightFlagNames[bit], &on))
				{
					if (on)
						*flags |= (1u << bit);
					else
						*flags &= ~(1u << bit);
				}
				ImGui::PopID();
			}
			ImGui::EndTable();
		}
		ImGui::Unindent();
	}
}

void TimeFlagsPanel(uint32_t* timeFlags)
{
	constexpr uint32_t kHourMask = 0x00FFFFFF;

	char header[48];
	snprintf(header, sizeof(header), "Time Flags (0x%08X)###timeflags", *timeFlags);

	if (ImGui::CollapsingHeader(header))
	{
		ImGui::Indent();
		ImGui::TextDisabled("Hours the light is enabled");

		if (ImGui::SmallButton("All")) { *timeFlags |= kHourMask; }
		ImGui::SameLine();
		if (ImGui::SmallButton("None")) { *timeFlags &= ~kHourMask; }
		ImGui::SameLine();
		if (ImGui::SmallButton("Invert")) { *timeFlags ^= kHourMask; }

		if (ImGui::BeginTable("##timegrid", 6, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings))
		{
			for (int hour = 0; hour < 24; ++hour)
			{
				ImGui::TableNextColumn();
				ImGui::PushID(hour);
				bool on = (*timeFlags & (1u << hour)) != 0;
				char label[8];
				snprintf(label, sizeof(label), "%02d", hour);
				if (ImGui::Checkbox(label, &on))
				{
					if (on)
						*timeFlags |= (1u << hour);
					else
						*timeFlags &= ~(1u << hour);
				}
				ImGui::PopID();
			}
			ImGui::EndTable();
		}
		ImGui::Unindent();
	}
}

std::string Fxml(float v)
{
	char b[32];
	snprintf(b, sizeof(b), "%.7g", v);
	return b;
}

void XmlV3(std::string& o, const char* tag, const float* v)
{
	char b[192];
	snprintf(b, sizeof(b), "   <%s x=\"%s\" y=\"%s\" z=\"%s\" />\n", tag, Fxml(v[0]).c_str(), Fxml(v[1]).c_str(), Fxml(v[2]).c_str());
	o += b;
}

void XmlF1(std::string& o, const char* tag, float v)
{
	char b[128];
	snprintf(b, sizeof(b), "   <%s value=\"%s\" />\n", tag, Fxml(v).c_str());
	o += b;
}

void XmlU1(std::string& o, const char* tag, unsigned v)
{
	char b[96];
	snprintf(b, sizeof(b), "   <%s value=\"%u\" />\n", tag, v);
	o += b;
}

void XmlRGB(std::string& o, const char* tag, const uint8_t* c)
{
	char b[96];
	snprintf(b, sizeof(b), "   <%s r=\"%u\" g=\"%u\" b=\"%u\" />\n", tag, c[0], c[1], c[2]);
	o += b;
}

void AppendLightItem(std::string& o, const CLightAttr* l)
{
	o += "  <Item>\n";

	XmlV3(o, "Position", l->position);
	XmlRGB(o, "Colour", l->color);
	XmlU1(o, "Flashiness", l->flashiness);
	XmlF1(o, "Intensity", l->intensity);
	XmlU1(o, "Flags", l->flags);
	XmlU1(o, "BoneId", l->boneId);

	{
		char b[64];
		snprintf(b, sizeof(b), "   <Type>%s</Type>\n", LightTypeName(l->lightType));
		o += b;
	}

	XmlU1(o, "GroupId", l->groupId);
	XmlU1(o, "TimeFlags", l->timeFlags);
	XmlF1(o, "Falloff", l->falloff);
	XmlF1(o, "FalloffExponent", l->falloffExponent);
	XmlV3(o, "CullingPlaneNormal", l->cullingPlane);
	XmlF1(o, "CullingPlaneOffset", l->cullingPlane[3]);
	XmlU1(o, "Unknown45", l->unk1);
	XmlU1(o, "Unknown46", l->unk2);
	XmlF1(o, "VolumeIntensity", l->volumeIntensity);
	XmlF1(o, "VolumeSizeScale", l->volumeSizeScale);
	XmlRGB(o, "VolumeOuterColour", l->volumeOuterColor);
	XmlU1(o, "LightHash", l->lightHash);
	XmlF1(o, "VolumeOuterIntensity", l->volumeOuterIntensity);
	XmlF1(o, "CoronaSize", l->coronaSize);
	XmlF1(o, "VolumeOuterExponent", l->volumeOuterExponent);
	XmlU1(o, "LightFadeDistance", l->lightFadeDistance);
	XmlU1(o, "ShadowBlur", l->shadowBlur);
	XmlU1(o, "ShadowFadeDistance", l->shadowFadeDistance);
	XmlU1(o, "SpecularFadeDistance", l->specularFadeDistance);
	XmlU1(o, "VolumetricFadeDistance", l->volumetricFadeDistance);
	XmlF1(o, "ShadowNearClip", l->shadowNearClip);
	XmlF1(o, "CoronaIntensity", l->coronaIntensity);
	XmlF1(o, "CoronaZBias", l->coronaZBias);
	XmlV3(o, "Direction", l->direction);
	XmlV3(o, "Tangent", l->tangent);
	XmlF1(o, "ConeInnerAngle", l->coneInnerAngle);
	XmlF1(o, "ConeOuterAngle", l->coneOuterAngle);
	XmlV3(o, "Extent", l->extents);

	if (l->projectedTextureHash == 0)
	{
		o += "   <ProjectedTextureHash />\n";
	}
	else
	{
		char b[64];
		snprintf(b, sizeof(b), "   <ProjectedTextureHash>hash_%08x</ProjectedTextureHash>\n", l->projectedTextureHash);
		o += b;
	}

	o += "  </Item>\n";
}

std::string BuildLightsXml(void* drawable, int lightCount)
{
	std::string o;
	o.reserve(2048);
	o += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
	o += "<Lights>\n";
	for (int i = 0; i < lightCount; ++i)
	{
		const CLightAttr* l = GetLight(drawable, i);
		if (l)
		{
			AppendLightItem(o, l);
		}
	}
	o += "</Lights>\n";
	return o;
}

// Writes the XML to %USERPROFILE%\Documents\fivem_lights\<model>.lights.xml.
// On success outResult holds the full path; on failure it holds an error.
bool SaveLightsXml(const std::string& xml, const std::string& modelName, std::string& outResult)
{
	char userProfile[MAX_PATH];
	DWORD n = GetEnvironmentVariableA("USERPROFILE", userProfile, sizeof(userProfile));
	if (n == 0 || n >= sizeof(userProfile))
	{
		outResult = "USERPROFILE not set";
		return false;
	}

	const std::string docs = std::string(userProfile) + "\\Documents";
	const std::string dir = docs + "\\fivem_lights";
	CreateDirectoryA(docs.c_str(), nullptr);
	CreateDirectoryA(dir.c_str(), nullptr);

	std::string safeName = modelName.empty() ? "model" : modelName;
	for (char& c : safeName)
	{
		if (!(isalnum((unsigned char)c) || c == '_' || c == '-'))
		{
			c = '_';
		}
	}

	const std::string path = dir + "\\" + safeName + ".lights.xml";

	HANDLE h = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE)
	{
		outResult = "Failed to create file: " + path;
		return false;
	}

	DWORD written = 0;
	BOOL ok = WriteFile(h, xml.data(), static_cast<DWORD>(xml.size()), &written, nullptr);
	CloseHandle(h);

	if (!ok || written != xml.size())
	{
		outResult = "Write failed: " + path;
		return false;
	}

	outResult = path;
	return true;
}

char g_searchBuffer[256] = "";
void* g_selectedDrawable = nullptr;
std::string g_lastSaveMessage;

bool g_gizmoEnabled = false;
bool g_showFalloffGizmo = true;
bool g_showExponentShell = true;
gizmo::Operation g_gizmoOp = gizmo::TRANSLATE;
bool g_snap = false;
float g_snapValue = 0.1f;
int g_gizmoTarget = 0;
float g_gizmoRotation[3] = { 0, 0, 0 };
int g_prevGizmoTarget = -1;
gizmo::Operation g_prevGizmoOp = gizmo::TRANSLATE;
} // namespace

static HookFunction hookFunction([]()
{
	g_gameViewport = hook::get_address<CViewportGame**>(hook::get_pattern("33 C0 48 39 05 ? ? ? ? 74 2E 48 8B 0D ? ? ? ? 48 85 C9 74 22", 5));

	const uintptr_t moduleBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));

	{
		auto p = hook::pattern("48 8B C4 55 53 56 57 41 54 41 55 41 56 41 57 48 8D 68 ? 48 81 EC ? ? ? ? 45 33 E4");
		if (p.size() == 1)
		{
			void* addr = p.get(0).get<void>(0);
			g_addAttachedLights = reinterpret_cast<AddAttachedLightsFn>(addr);
			trace("[LightEditor] AddAttachedLights @ +%llX\n", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(addr) - moduleBase));
		}
		else
		{
			trace("[LightEditor] AddAttachedLights pattern matched %zu times (need 1) - add/delete disabled\n", p.size());
		}
	}
	{
		auto p = hook::pattern("48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 8B 81 C0 00 00 00 A8 20 0F 84 ? ? ? ? 48 8B 69 20 83 E0 DF 89 81 C0 00 00 00");
		if (p.size() == 1)
		{
			void* addr = p.get(0).get<void>(0);
			g_removeAttachedLights = reinterpret_cast<RemoveAttachedLightsFn>(addr);
			trace("[LightEditor] RemoveAttachedLights @ +%llX\n", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(addr) - moduleBase));
		}
		else
		{
			trace("[LightEditor] RemoveAttachedLights pattern matched %zu times (need 1) - add/delete disabled\n", p.size());
		}
	}
});

static InitFunction initFunction([]()
{
	static bool lightEditorEnabled;
	static ConVar<bool> lightEditorVar("lightEditor", ConVar_Archive | ConVar_UserPref, false, &lightEditorEnabled);

	// Apply queued add/remove mutations on the game thread.
	OnMainGameFrame.Connect([]()
	{
		ProcessPendingOps();
	});

	ConHost::OnShouldDrawGui.Connect([](bool* should)
	{
		*should = *should || lightEditorEnabled;
	});

	ConHost::OnDrawGui.Connect([]()
	{
		if (!lightEditorEnabled)
		{
			return;
		}

		ImGui::SetNextWindowSize(ImVec2(420.0f, 720.0f), ImGuiCond_FirstUseEver);

		if (ImGui::Begin("Model Lights", &lightEditorEnabled))
		{
			ImGui::TextDisabled("Enter a loaded model name (e.g., 'prop_streetlight_01')");
			if (ImGui::InputText("##LightModelSearch", g_searchBuffer, IM_ARRAYSIZE(g_searchBuffer)))
			{
				g_selectedDrawable = FindDrawable(g_searchBuffer);
			}
			ImGui::SameLine();
			if (ImGui::Button("Clear"))
			{
				g_searchBuffer[0] = '\0';
				g_selectedDrawable = nullptr;
			}

			ImGui::Separator();

			if (!g_selectedDrawable)
			{
				ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "No drawable found or loaded.");
				ImGui::TextDisabled("The model must already be streamed in (.ydr drawable).");
				ImGui::End();
				return;
			}

			ImGui::Text("Model: %s", g_searchBuffer);

			int lightCount = GetLightCount(g_selectedDrawable);
			ImGui::TextDisabled("%d embedded light(s)", lightCount);

			if (lightCount <= 0)
			{
				ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "This model has no embedded lights.");
				ImGui::End();
				return;
			}

			const bool canEdit = LightRebuildAvailable();
			if (!canEdit)
			{
				ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "Add/Delete unavailable (light-entity functions not found).");
			}

			const int addSrcIndex = (g_gizmoTarget >= 0 && g_gizmoTarget < lightCount) ? g_gizmoTarget : 0;

			ImGui::BeginDisabled(!canEdit);
			if (ImGui::Button("Add Light"))
			{
				if (CLightAttr* src = GetLight(g_selectedDrawable, addSrcIndex))
				{
					PendingOp op{};
					op.kind = PendingKind::Add;
					op.drawable = g_selectedDrawable;
					op.newLight = *src;
					snprintf(op.modelName, sizeof(op.modelName), "%s", g_searchBuffer);
					QueueOp(op);
				}
			}
			ImGui::EndDisabled();
			if (ImGui::IsItemHovered())
			{
				ImGui::SetTooltip("Appends a copy of Light %d (rebuilds the model's lights)", addSrcIndex);
			}
			ImGui::SameLine();
			if (ImGui::Button("Save Lights XML"))
			{
				std::string xml = BuildLightsXml(g_selectedDrawable, lightCount);
				std::string result;
				g_lastSaveMessage = SaveLightsXml(xml, g_searchBuffer, result)
					? ("Saved: " + result)
					: ("Error: " + result);
			}
			if (!g_lastSaveMessage.empty())
			{
				ImGui::TextDisabled("%s", g_lastSaveMessage.c_str());
			}

			ImGui::Separator();
			ImGui::Checkbox("3D Gizmo (move/rotate)", &g_gizmoEnabled);
			ImGui::SameLine();
			ImGui::Checkbox("Falloff / cone gizmo", &g_showFalloffGizmo);

			const bool anyGizmo = g_gizmoEnabled || g_showFalloffGizmo;

			// Anchor to a placed instance of the model so we edit in world space.
			ModelAnchor anchor;
			if (anyGizmo)
			{
				anchor = FindModelAnchor(g_searchBuffer);

				if (!GetViewport())
				{
					ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Camera viewport not acquired - gizmo cannot draw.");
				}

				if (anchor.valid)
				{
					ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f),
						"Anchored to instance @ (%.1f, %.1f, %.1f)",
						anchor.position[0], anchor.position[1], anchor.position[2]);
				}
				else
				{
					ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "No placed instance found - editing at local origin (0,0,0).");
				}

				if (g_gizmoTarget < 0 || g_gizmoTarget >= lightCount) g_gizmoTarget = 0;
				char preview[32];
				snprintf(preview, sizeof(preview), "Light %d", g_gizmoTarget);
				ImGui::SetNextItemWidth(160.0f);
				if (ImGui::BeginCombo("Target##gizmo", preview))
				{
					for (int i = 0; i < lightCount; ++i)
					{
						char item[32];
						snprintf(item, sizeof(item), "Light %d", i);
						if (ImGui::Selectable(item, g_gizmoTarget == i)) g_gizmoTarget = i;
					}
					ImGui::EndCombo();
				}
			}

			if (g_gizmoEnabled)
			{
				if (ImGui::RadioButton("Move", g_gizmoOp == gizmo::TRANSLATE)) g_gizmoOp = gizmo::TRANSLATE;
				ImGui::SameLine();
				if (ImGui::RadioButton("Rotate (direction)", g_gizmoOp == gizmo::ROTATE)) g_gizmoOp = gizmo::ROTATE;
				ImGui::SameLine();
				ImGui::Checkbox("Snap", &g_snap);
				if (g_snap)
				{
					ImGui::SameLine();
					ImGui::SetNextItemWidth(80.0f);
					ImGui::DragFloat("##snap", &g_snapValue, 0.05f, 0.01f, 100.0f, "%.2f");
				}
			}

			if (g_showFalloffGizmo)
			{
				ImGui::Checkbox("falloff exponent", &g_showExponentShell);
			}

			ImGui::Separator();

			for (int i = 0; i < lightCount; ++i)
			{
				CLightAttr* light = GetLight(g_selectedDrawable, i);
				if (!light) continue;

				ImGui::PushID(i);

				char header[64];
				snprintf(header, sizeof(header), "Light %d  (%s)###light%d", i, LightTypeName(light->lightType), i);

				if (ImGui::CollapsingHeader(header, ImGuiTreeNodeFlags_DefaultOpen))
				{
					ImGui::Indent();

					{
						ImGui::BeginDisabled(!canEdit);
						ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.15f, 0.15f, 1.0f));
						ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.70f, 0.20f, 0.20f, 1.0f));
						if (ImGui::Button("Delete this light"))
						{
							PendingOp op{};
							op.kind = PendingKind::Remove;
							op.drawable = g_selectedDrawable;
							op.index = i;
							snprintf(op.modelName, sizeof(op.modelName), "%s", g_searchBuffer);
							QueueOp(op);
						}
						ImGui::PopStyleColor(2);
						ImGui::EndDisabled();
						if (ImGui::IsItemHovered())
						{
							ImGui::SetTooltip("Removes Light %d from the drawable (rebuilds the model's lights)", i);
						}
						ImGui::Spacing();
					}

					{
						BeginProp("Type");
						const char* preview = LightTypeName(light->lightType);
						if (ImGui::BeginCombo("##v", preview))
						{
							static const eLightType kTypes[] = { eLightType::Point, eLightType::Spot, eLightType::Capsule };
							for (eLightType t : kTypes)
							{
								bool selected = (light->lightType == (uint8_t)t);
								if (ImGui::Selectable(LightTypeName((uint8_t)t), selected)) light->lightType = (uint8_t)t;
								if (selected) ImGui::SetItemDefaultFocus();
							}
							ImGui::EndCombo();
						}
						EndProp();
					}

					ColorProperty("Color", light->color);
					FloatProperty("Intensity", &light->intensity, 0.05f, 0.0f, 1000.0f);
					FloatProperty("Range / Falloff", &light->falloff, 0.05f, 0.0f, 10000.0f);
					FloatProperty("Falloff Exponent", &light->falloffExponent, 0.05f, 0.0f, 1000.0f);
					ByteProperty("Flashiness", &light->flashiness, 20);

					ImGui::Spacing();
					ImGui::TextDisabled("Transform");
					Float3Property("Position (local)", light->position);
					Float3Property("Direction", light->direction);

					if (light->lightType == (uint8_t)eLightType::Spot)
					{
						FloatProperty("Cone Inner Angle", &light->coneInnerAngle, 0.25f, 0.0f, 90.0f);
						FloatProperty("Cone Outer Angle", &light->coneOuterAngle, 0.25f, 0.0f, 90.0f);
					}
					if (light->lightType == (uint8_t)eLightType::Capsule)
					{
						Float3Property("Capsule Extents", light->extents);
					}

					ImGui::Spacing();
					ImGui::TextDisabled("Corona");
					FloatProperty("Corona Size", &light->coronaSize, 0.01f, 0.0f, 100.0f);
					FloatProperty("Corona Intensity", &light->coronaIntensity, 0.05f, 0.0f, 1000.0f);
					FloatProperty("Corona Z-Bias", &light->coronaZBias, 0.001f);

					ImGui::Spacing();
					ImGui::TextDisabled("Volume");
					FloatProperty("Volume Intensity", &light->volumeIntensity, 0.01f, 0.0f, 100.0f);
					FloatProperty("Volume Size Scale", &light->volumeSizeScale, 0.01f, 0.0f, 100.0f);
					ColorProperty("Volume Outer Color", light->volumeOuterColor);
					FloatProperty("Volume Outer Intensity", &light->volumeOuterIntensity, 0.01f, 0.0f, 100.0f);

					ImGui::Spacing();
					ImGui::TextDisabled("Shadows & Fade");
					FloatProperty("Shadow Near Clip", &light->shadowNearClip, 0.01f, 0.0f, 1000.0f);
					ByteProperty("Shadow Blur", &light->shadowBlur);
					ByteProperty("Light Fade Dist", &light->lightFadeDistance);
					ByteProperty("Shadow Fade Dist", &light->shadowFadeDistance);
					ByteProperty("Specular Fade Dist", &light->specularFadeDistance);
					ByteProperty("Volumetric Fade Dist", &light->volumetricFadeDistance);

					ImGui::Spacing();
					FlagsPanel(&light->flags);
					TimeFlagsPanel(&light->timeFlags);
					ImGui::TextDisabled("boneId: %u   groupId: %u", light->boneId, light->groupId);

					ImGui::Unindent();
					ImGui::Spacing();
				}

				ImGui::PopID();
			}

			const grcViewport* vp = (g_gizmoEnabled || g_showFalloffGizmo) ? GetViewport() : nullptr;
			float viewProj[16];
			if (vp)
			{
				MakeViewProj(vp, viewProj);
			}

			if (vp && g_showFalloffGizmo && g_gizmoTarget >= 0 && g_gizmoTarget < lightCount)
			{
				CLightAttr* target = GetLight(g_selectedDrawable, g_gizmoTarget);
				if (target)
				{
					float worldPos[3], wdir[3], wtan[3];
					if (anchor.valid)
					{
						for (int k = 0; k < 3; ++k)
						{
							worldPos[k] = anchor.position[k]
								+ anchor.right[k] * target->position[0]
								+ anchor.forward[k] * target->position[1]
								+ anchor.up[k] * target->position[2];
							wdir[k] = anchor.right[k] * target->direction[0]
								+ anchor.forward[k] * target->direction[1]
								+ anchor.up[k] * target->direction[2];
							wtan[k] = anchor.right[k] * target->tangent[0]
								+ anchor.forward[k] * target->tangent[1]
								+ anchor.up[k] * target->tangent[2];
						}
					}
					else
					{
						for (int k = 0; k < 3; ++k)
						{
							worldPos[k] = target->position[k];
							wdir[k] = target->direction[k];
							wtan[k] = target->tangent[k];
						}
					}

					const float dl_ = sqrtf(wdir[0] * wdir[0] + wdir[1] * wdir[1] + wdir[2] * wdir[2]);
					if (dl_ > 1e-5f) { wdir[0] /= dl_; wdir[1] /= dl_; wdir[2] /= dl_; }
					const float tl_ = sqrtf(wtan[0] * wtan[0] + wtan[1] * wtan[1] + wtan[2] * wtan[2]);
					if (tl_ > 1e-5f) { wtan[0] /= tl_; wtan[1] /= tl_; wtan[2] /= tl_; }

					DrawLightFalloffGizmo(viewProj, target, worldPos, wdir, wtan, g_showExponentShell);
				}
			}

			if (vp && g_gizmoEnabled && g_gizmoTarget >= 0 && g_gizmoTarget < lightCount)
			{
				CLightAttr* target = GetLight(g_selectedDrawable, g_gizmoTarget);
				if (target)
				{
					if (g_gizmoTarget != g_prevGizmoTarget || g_gizmoOp != g_prevGizmoOp)
					{
						if (g_gizmoOp == gizmo::ROTATE)
						{
							DirectionToEuler(target->direction, g_gizmoRotation);
						}
						g_prevGizmoTarget = g_gizmoTarget;
						g_prevGizmoOp = g_gizmoOp;
					}

					float gizmoPos[3];
					if (anchor.valid)
					{
						for (int k = 0; k < 3; ++k)
						{
							gizmoPos[k] = anchor.position[k]
								+ anchor.right[k] * target->position[0]
								+ anchor.forward[k] * target->position[1]
								+ anchor.up[k] * target->position[2];
						}
					}
					else
					{
						gizmoPos[0] = target->position[0];
						gizmoPos[1] = target->position[1];
						gizmoPos[2] = target->position[2];
					}

					ImGuiIO& io = ImGui::GetIO();
					gizmo::BeginFrame();
					gizmo::SetRect(io.DisplaySize.x, io.DisplaySize.y);
					gizmo::SetMouseOverGui(ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows));

					// camera world position lives in the inverse-view translation row
					const float camPos[3] = { vp->m_inverseView[12], vp->m_inverseView[13], vp->m_inverseView[14] };

					float snapVal = g_snapValue;
					float* snapPtr = g_snap ? &snapVal : nullptr;

					bool changed = gizmo::Manipulate(viewProj, camPos, g_gizmoOp, gizmoPos, g_gizmoRotation, snapPtr);

					if (changed && g_gizmoOp == gizmo::TRANSLATE)
					{
						if (anchor.valid)
						{
							const float d[3] = {
								gizmoPos[0] - anchor.position[0],
								gizmoPos[1] - anchor.position[1],
								gizmoPos[2] - anchor.position[2],
							};
							target->position[0] = d[0] * anchor.right[0] + d[1] * anchor.right[1] + d[2] * anchor.right[2];
							target->position[1] = d[0] * anchor.forward[0] + d[1] * anchor.forward[1] + d[2] * anchor.forward[2];
							target->position[2] = d[0] * anchor.up[0] + d[1] * anchor.up[1] + d[2] * anchor.up[2];
						}
						else
						{
							target->position[0] = gizmoPos[0];
							target->position[1] = gizmoPos[1];
							target->position[2] = gizmoPos[2];
						}
					}
					else if (changed && g_gizmoOp == gizmo::ROTATE)
					{
						EulerToDirection(g_gizmoRotation, target->direction, target->tangent);
					}
				}
			}
		}

		ImGui::End();
	});
});
