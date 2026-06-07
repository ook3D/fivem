#pragma once
#include <cstdint>
#include <cstddef>
#include <string>

namespace ydrexport
{
	struct Patch
	{
		size_t virtualOffset;
		const void* data;
		size_t size;
	};

	bool ExportPatched(const char* modelName, const char* outPath,
		const Patch* patches, int patchCount, int* outApplied, std::string& outError);

	bool ExportLights(const char* modelName, const char* outPath,
		size_t lightFieldOffset, const void* liveLights, int liveCount, size_t lightStride,
		std::string& outError);
}
