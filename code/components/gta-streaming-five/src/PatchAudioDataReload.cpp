#include <StdInc.h>

#ifdef GTA_FIVE
#include <Hooking.h>
#include "Hooking.Patterns.h"
#include "Hooking.Stubs.h"
#include <CrossBuildRuntime.h>

#include <ICoreGameInit.h>
#include <Streaming.h>
#include <fiCollectionWrapper.h>

struct audMetadataDataFileMounter
{
	void** vtable;
	void* metadataManager;
	char unloadingChunkNames[48][32];
	uint32_t framesToUnload[48];
	uint32_t unloadingChunksCount;
};

static bool (*g_origLoadMetadataDataFile)(audMetadataDataFileMounter* mounter, const char* file);
static void (*g_updateMetadataMounter)(audMetadataDataFileMounter* mounter);

static void (*g_origUnloadWavePackDataFile)(void* mounter, const char* file);
static const char* (*g_getBankName)(uint32_t bankId);
static uint32_t (*g_getBankIndexFromName)(void* soundFactory, const char* bankName, bool applyRemapping);
static void* g_soundFactory;
static char** g_waveSlots;
static uint32_t* g_numWaveSlots;
static bool (*g_forceUnloadBank)(void* waveSlot);
static bool (*g_loadBank)(void* waveSlot, uint32_t bankId, uint8_t priority);
static void (*g_loadWave)(void* waveSlot, uint32_t bankId, uint32_t waveNameHash, uint8_t priority);

// audWaveSlot layout
static constexpr size_t kWaveSlotSize = 0x88;
static constexpr size_t kLoadedWaveNameHashOffset = 0x14;
static constexpr size_t kBankIdToLoadOffset = 0x28;
static constexpr size_t kLoadedBankIdOffset = 0x2A;
static constexpr size_t kLoadTypeOffset = 0x2F; // 0 = single wave, 1 = whole bank
static constexpr size_t kSlotTypeOffset = 0x30; // 1 = streaming slot
static constexpr size_t kLoadedPriorityOffset = 0x32;
static constexpr size_t kStateFlagsOffset = 0x33; // 1 = load requested, 2 = loading

struct PendingBankReload
{
	uint32_t slotIndex;
	std::string bankName;
	bool isWave;
	uint32_t waveNameHash;
	uint8_t priority;
};

// entities keep using the wave slot they requested, so a force-unloaded slot has to be refilled or its sounds stay silent
static std::vector<PendingBankReload> g_pendingBankReloads;

static char* GetWaveSlot(uint32_t index)
{
	return *g_waveSlots + index * kWaveSlotSize;
}

static void ReloadPendingBanks()
{
	for (auto it = g_pendingBankReloads.begin(); it != g_pendingBankReloads.end();)
	{
		uint32_t bankId = g_getBankIndexFromName(g_soundFactory, it->bankName.c_str(), true);

		if (bankId >= 0xFFFF)
		{
			++it;
			continue;
		}

		char* waveSlot = GetWaveSlot(it->slotIndex);

		// something else claimed the slot in the meantime, dont evict it
		bool isFree = *(uint16_t*)(waveSlot + kLoadedBankIdOffset) == 0xFFFF && *(uint16_t*)(waveSlot + kBankIdToLoadOffset) == 0xFFFF && (waveSlot[kStateFlagsOffset] & 3) == 0;

		if (isFree)
		{
			if (it->isWave)
			{
				g_loadWave(waveSlot, bankId, it->waveNameHash, it->priority);
			}
			else
			{
				g_loadBank(waveSlot, bankId, it->priority);
			}

			trace("Reloading audio bank %s into wave slot %d.\n", it->bankName, it->slotIndex);
		}

		it = g_pendingBankReloads.erase(it);
	}
}

static bool LoadMetadataDataFile(audMetadataDataFileMounter* mounter, const char* file)
{
	if (mounter->unloadingChunksCount != 0 && Instance<ICoreGameInit>::Get()->GetGameLoaded())
	{
		std::fill(std::begin(mounter->framesToUnload), std::end(mounter->framesToUnload), 0);
		g_updateMetadataMounter(mounter);
	}

	bool result = g_origLoadMetadataDataFile(mounter, file);

	// bank ids come from the sound data string table, only resolve once the new sound data is in so we dont pick up the old chunks ids
	if (!g_pendingBankReloads.empty() && mounter->metadataManager == (char*)g_soundFactory + 8)
	{
		ReloadPendingBanks();
	}

	return result;
}

// bank names from .rel files use backslashes (e.g. "PRP_GLOCK17_WEAPONS\bank"), resource paths use forward slashes
static bool IsInFolder(const char* path, const std::string& folder)
{
	return _strnicmp(path, folder.c_str(), folder.size()) == 0 && (path[folder.size()] == '/' || path[folder.size()] == '\\');
}

static void UnloadWavePackDataFile(void* mounter, const char* file)
{
	g_origUnloadWavePackDataFile(mounter, file);

	const char* packName = strrchr(file, '/');
	std::string prefix = packName ? packName + 1 : file;

	// walk every slot rather than the loaded bank table, as wave-mode slots only register in the loaded wave table
	for (uint32_t i = 0; i < *g_numWaveSlots; i++)
	{
		char* waveSlot = GetWaveSlot(i);
		uint16_t bankId = *(uint16_t*)(waveSlot + kLoadedBankIdOffset);

		// streaming slots override ForceUnloadBank with extra cleanup, skip them rather than call the base version
		if (bankId == 0xFFFF || waveSlot[kSlotTypeOffset] == 1)
		{
			continue;
		}

		const char* bankName = g_getBankName(bankId);

		if (!bankName || !IsInFolder(bankName, prefix))
		{
			continue;
		}

		// ForceUnloadBank clears these, grab them first
		PendingBankReload reload{ i, bankName, waveSlot[kLoadTypeOffset] == 0, *(uint32_t*)(waveSlot + kLoadedWaveNameHashOffset), (uint8_t)waveSlot[kLoadedPriorityOffset] };

		if (g_forceUnloadBank(waveSlot))
		{
			trace("Unloaded audio bank %s from wave slot %d.\n", bankName, i);

			g_pendingBankReloads.push_back(std::move(reload));
		}
		else
		{
			trace("Could not unload audio bank %s as it is still referenced, it will not be reloaded.\n", bankName);
		}
	}

	std::string packPath = file;
	auto& rawEntries = streaming::GetRawStreamerByIndex(0)->m_entries;

	for (uint32_t i = 0; i < rawEntries.GetCount(); i++)
	{
		auto& rawEntry = rawEntries[i];

		if (rawEntry.fileName && IsInFolder(rawEntry.fileName, packPath))
		{
			rawEntry.timestamp = 0;
		}
	}
}

static HookFunction hookFunction([]()
{
	g_updateMetadataMounter = hook::get_pattern<void(audMetadataDataFileMounter*)>("48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 20 83 B9 D0 06 00 00 00");
	g_getBankName = hook::get_call(hook::get_pattern<const char*(uint32_t)>("E8 ? ? ? ? 33 D2 48 8B C8 E8 ? ? ? ? 89 84 37"));
	g_soundFactory = hook::get_address<void*>(reinterpret_cast<char*>(g_getBankName) + 5);

	g_getBankIndexFromName = hook::get_pattern<uint32_t(void*, const char*, bool)>("48 89 5C 24 08 57 48 83 EC 20 48 8B C2 48 8B F9 33 D2 48 8B C8 41 8A D8 E8 ? ? ? ? 48 8D 4F 08");

	auto waveSlotsLocation = hook::get_pattern<char>("39 1D ? ? ? ? 8D 4B 01 44 8A EB 44 8A FB 89 5D ? 44 8B E3 48 89 5C 24 ? 48 89 5C 24 ? 8B F3 76 ? 44 8B F3 49 63 FE 48 03 3D ? ? ? ? 74 ? 8A 47 33");
	g_numWaveSlots = hook::get_address<uint32_t*>(waveSlotsLocation + 2);
	g_waveSlots = hook::get_address<char**>(waveSlotsLocation + 0x2C);

	g_forceUnloadBank = hook::get_pattern<bool(void*)>("48 89 5C 24 08 57 48 83 EC 20 48 8B D9 48 8D 0D ? ? ? ? E8 ? ? ? ? 0F B6 53 2E");
	g_loadBank = hook::get_pattern<bool(void*, uint32_t, uint8_t)>("48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 41 56 48 83 EC 20 41 BE FF FF 00 00");
	g_loadWave = hook::get_pattern<void(void*, uint32_t, uint32_t, uint8_t)>("81 FA FF FF 00 00 73 ? 48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B D9 48 8D 0D ? ? ? ? 41 8B F1 41 8B E8 8B FA E8");

	if (xbr::IsGameBuildOrGreater<3258>())
	{
		g_origLoadMetadataDataFile = hook::trampoline(hook::get_pattern("48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 41 54 41 56 41 57 48 83 EC 60 48 8B EA 48 8B F9 48 8D 50 C8 48 8B CD 41 B8 20 00 00 00 E8"), LoadMetadataDataFile);
	}
	else
	{
		g_origLoadMetadataDataFile = hook::trampoline(hook::get_pattern("48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 57 41 54 41 55 41 56 41 57 48 83 EC 60 48 8B EA 48 8B F1"), LoadMetadataDataFile);
	}

	g_origUnloadWavePackDataFile = hook::trampoline(hook::get_pattern("41 B8 20 00 00 00 E8 ? ? ? ? 33 C0 48 8D 54 24 ? 48 8D 4C 24 ? 44 8D 40 08", -0xC), UnloadWavePackDataFile);
});
#endif
