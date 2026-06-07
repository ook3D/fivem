#include <StdInc.h>
#include "YdrExport.h"

#include <Streaming.h>
#include <fiCollectionWrapper.h>

#include <vector>
#include <cstring>
#include <Windows.h>

namespace
{
constexpr int kMaxBits = 15;
constexpr int kMaxLCodes = 286;
constexpr int kMaxDCodes = 30;
constexpr int kMaxCodes = kMaxLCodes + kMaxDCodes;
constexpr int kFixLCodes = 288;

struct InflState
{
	uint8_t* out; size_t outLen; size_t outCnt;
	const uint8_t* in; size_t inLen; size_t inCnt;
	int bitBuf; int bitCnt;
};

struct Huffman { short* count; short* symbol; };

int inf_bits(InflState* s, int need)
{
	long val = s->bitBuf;
	while (s->bitCnt < need)
	{
		if (s->inCnt == s->inLen) return -1;
		val |= (long)(s->in[s->inCnt++]) << s->bitCnt;
		s->bitCnt += 8;
	}
	s->bitBuf = (int)(val >> need);
	s->bitCnt -= need;
	return (int)(val & ((1L << need) - 1));
}

int inf_stored(InflState* s)
{
	s->bitBuf = 0; s->bitCnt = 0;
	if (s->inCnt + 4 > s->inLen) return -1;
	unsigned len = s->in[s->inCnt] | (s->in[s->inCnt + 1] << 8);
	unsigned nlen = s->in[s->inCnt + 2] | (s->in[s->inCnt + 3] << 8);
	if (len != (~nlen & 0xffff)) return -1;
	s->inCnt += 4;
	if (s->inCnt + len > s->inLen) return -1;
	if (s->outCnt + len > s->outLen) return -1;
	memcpy(s->out + s->outCnt, s->in + s->inCnt, len);
	s->inCnt += len; s->outCnt += len;
	return 0;
}

int inf_decode(InflState* s, const Huffman* h)
{
	int code = 0, first = 0, index = 0;
	for (int len = 1; len <= kMaxBits; ++len)
	{
		int b = inf_bits(s, 1);
		if (b < 0) return -10;
		code |= b;
		int count = h->count[len];
		if (code - count < first) return h->symbol[index + (code - first)];
		index += count; first += count; first <<= 1; code <<= 1;
	}
	return -9;
}

void inf_construct(Huffman* h, const short* length, int n)
{
	for (int len = 0; len <= kMaxBits; ++len) h->count[len] = 0;
	for (int symbol = 0; symbol < n; ++symbol) h->count[length[symbol]]++;
	short offs[kMaxBits + 1];
	offs[1] = 0;
	for (int len = 1; len < kMaxBits; ++len) offs[len + 1] = offs[len] + h->count[len];
	for (int symbol = 0; symbol < n; ++symbol)
		if (length[symbol] != 0) h->symbol[offs[length[symbol]]++] = (short)symbol;
}

int inf_codes(InflState* s, const Huffman* lencode, const Huffman* distcode)
{
	static const short lens[29] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
	static const short lext[29] = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
	static const short dists[30] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
	static const short dext[30] = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

	for (;;)
	{
		int symbol = inf_decode(s, lencode);
		if (symbol < 0) return symbol;
		if (symbol == 256) return 0;
		if (symbol < 256)
		{
			if (s->outCnt == s->outLen) return -200;
			s->out[s->outCnt++] = (uint8_t)symbol;
			continue;
		}
		symbol -= 257;
		if (symbol >= 29) return -10;
		int len = lens[symbol] + inf_bits(s, lext[symbol]);
		symbol = inf_decode(s, distcode);
		if (symbol < 0) return symbol;
		int dist = dists[symbol] + inf_bits(s, dext[symbol]);
		if ((size_t)dist > s->outCnt) return -11;
		if (s->outCnt + len > s->outLen) return -200;
		for (int i = 0; i < len; ++i) { s->out[s->outCnt] = s->out[s->outCnt - dist]; s->outCnt++; }
	}
}

int inf_fixed(InflState* s)
{
	static short lencnt[kMaxBits + 1], lensym[kFixLCodes];
	static short distcnt[kMaxBits + 1], distsym[kMaxDCodes];
	static Huffman lencode = { lencnt, lensym };
	static Huffman distcode = { distcnt, distsym };
	static bool built = false;
	if (!built)
	{
		short lengths[kFixLCodes];
		int symbol = 0;
		for (; symbol < 144; ++symbol) lengths[symbol] = 8;
		for (; symbol < 256; ++symbol) lengths[symbol] = 9;
		for (; symbol < 280; ++symbol) lengths[symbol] = 7;
		for (; symbol < kFixLCodes; ++symbol) lengths[symbol] = 8;
		inf_construct(&lencode, lengths, kFixLCodes);
		for (symbol = 0; symbol < kMaxDCodes; ++symbol) lengths[symbol] = 5;
		inf_construct(&distcode, lengths, kMaxDCodes);
		built = true;
	}
	return inf_codes(s, &lencode, &distcode);
}

int inf_dynamic(InflState* s)
{
	static const short order[19] = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
	int nlen = inf_bits(s, 5) + 257;
	int ndist = inf_bits(s, 5) + 1;
	int ncode = inf_bits(s, 4) + 4;
	if (nlen > kMaxLCodes || ndist > kMaxDCodes) return -3;
	short lengths[kMaxCodes];
	int index = 0;
	for (; index < ncode; ++index) lengths[order[index]] = (short)inf_bits(s, 3);
	for (; index < 19; ++index) lengths[order[index]] = 0;
	short lencnt[kMaxBits + 1], lensym[kMaxLCodes];
	short distcnt[kMaxBits + 1], distsym[kMaxDCodes];
	Huffman lencode = { lencnt, lensym };
	Huffman distcode = { distcnt, distsym };
	inf_construct(&lencode, lengths, 19);
	index = 0;
	while (index < nlen + ndist)
	{
		int symbol = inf_decode(s, &lencode);
		if (symbol < 0) return symbol;
		if (symbol < 16) { lengths[index++] = (short)symbol; }
		else
		{
			int len = 0;
			if (symbol == 16) { if (index == 0) return -5; len = lengths[index - 1]; symbol = 3 + inf_bits(s, 2); }
			else if (symbol == 17) { symbol = 3 + inf_bits(s, 3); }
			else { symbol = 11 + inf_bits(s, 7); }
			if (index + symbol > nlen + ndist) return -6;
			while (symbol--) lengths[index++] = (short)len;
		}
	}
	if (lengths[256] == 0) return -9;
	inf_construct(&lencode, lengths, nlen);
	inf_construct(&distcode, lengths + nlen, ndist);
	return inf_codes(s, &lencode, &distcode);
}

bool InflateRaw(const uint8_t* src, size_t srcLen, uint8_t* dst, size_t dstLen)
{
	InflState s{};
	s.out = dst; s.outLen = dstLen; s.outCnt = 0;
	s.in = src; s.inLen = srcLen; s.inCnt = 0;
	s.bitBuf = 0; s.bitCnt = 0;
	int last, type, err = 0;
	do
	{
		last = inf_bits(&s, 1);
		type = inf_bits(&s, 2);
		if (type == 0) err = inf_stored(&s);
		else if (type == 1) err = inf_fixed(&s);
		else if (type == 2) err = inf_dynamic(&s);
		else return false;
		if (err != 0) return false;
	} while (!last);
	return s.outCnt == dstLen;
}

const int kLenBase[29] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
const int kLenExtra[29] = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
const int kDistBase[30] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
const int kDistExtra[30] = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

struct BitWriter
{
	std::vector<uint8_t>& out;
	uint32_t acc = 0; int nbits = 0;
	explicit BitWriter(std::vector<uint8_t>& o) : out(o) {}
	void put(uint32_t v, int n)
	{
		acc |= (v & ((n >= 32) ? 0xFFFFFFFFu : ((1u << n) - 1))) << nbits;
		nbits += n;
		while (nbits >= 8) { out.push_back((uint8_t)(acc & 0xFF)); acc >>= 8; nbits -= 8; }
	}
	void putCode(uint32_t code, int n)
	{
		uint32_t r = 0;
		for (int i = 0; i < n; ++i) r = (r << 1) | ((code >> i) & 1);
		put(r, n);
	}
	void flush() { if (nbits > 0) { out.push_back((uint8_t)(acc & 0xFF)); acc = 0; nbits = 0; } }
};

void emitLiteral(BitWriter& bw, uint8_t c)
{
	if (c < 144) bw.putCode(0x30u + c, 8);
	else bw.putCode(0x190u + (c - 144), 9);
}

void emitMatch(BitWriter& bw, int len, int dist)
{
	int li = 28; while (li > 0 && kLenBase[li] > len) --li;
	const int lcode = 257 + li;
	if (lcode <= 279) bw.putCode((uint32_t)(lcode - 256), 7);
	else bw.putCode(0xC0u + (lcode - 280), 8);
	if (kLenExtra[li]) bw.put((uint32_t)(len - kLenBase[li]), kLenExtra[li]);
	int di = 29; while (di > 0 && kDistBase[di] > dist) --di;
	bw.putCode((uint32_t)di, 5);
	if (kDistExtra[di]) bw.put((uint32_t)(dist - kDistBase[di]), kDistExtra[di]);
}

void Deflate(const uint8_t* src, size_t srcLen, std::vector<uint8_t>& out)
{
	constexpr int kWindow = 32768, kMinMatch = 3, kMaxMatch = 258;
	constexpr int kHashBits = 15, kHashSize = 1 << kHashBits, kMaxChain = 256;
	BitWriter bw(out);
	bw.put(1, 1); // BFINAL
	bw.put(1, 2); // BTYPE = 01 fixed
	const size_t n = srcLen;
	std::vector<int> head(kHashSize, -1);
	std::vector<int> prev(n, -1);
	auto hashAt = [&](size_t p) -> uint32_t
	{
		uint32_t h = (uint32_t)src[p] | ((uint32_t)src[p + 1] << 8) | ((uint32_t)src[p + 2] << 16);
		return (h * 2654435761u) >> (32 - kHashBits);
	};
	auto insert = [&](size_t p)
	{
		if (p + kMinMatch > n) return;
		uint32_t h = hashAt(p);
		prev[p] = head[h]; head[h] = (int)p;
	};
	size_t i = 0;
	while (i < n)
	{
		int bestLen = 0, bestDist = 0;
		if (i + kMinMatch <= n)
		{
			const int maxLen = (int)((n - i < (size_t)kMaxMatch) ? (n - i) : (size_t)kMaxMatch);
			int j = head[hashAt(i)];
			int chain = kMaxChain;
			while (j >= 0 && chain-- > 0)
			{
				if ((int)i - j > kWindow) break;
				if (src[j + bestLen] == src[i + bestLen])
				{
					int ml = 0;
					while (ml < maxLen && src[j + ml] == src[i + ml]) ++ml;
					if (ml > bestLen) { bestLen = ml; bestDist = (int)i - j; if (ml >= maxLen) break; }
				}
				j = prev[j];
			}
		}
		if (bestLen >= kMinMatch)
		{
			emitMatch(bw, bestLen, bestDist);
			const size_t end = i + bestLen;
			while (i < end) { insert(i); ++i; }
		}
		else { emitLiteral(bw, src[i]); insert(i); ++i; }
	}
	bw.putCode(0, 7); // end of block
	bw.flush();
}

uint32_t FlagSize(uint32_t v)
{
	uint32_t ls = v & 0xF, h16 = (v >> 4) & 1, h8 = (v >> 5) & 3, h4 = (v >> 7) & 0xF,
		h2 = (v >> 11) & 0x3F, base = (v >> 17) & 0x7F, t2 = (v >> 24) & 1, t4 = (v >> 25) & 1, t8 = (v >> 26) & 1, t16 = (v >> 27) & 1;
	uint32_t leaf = 8192u << ls;
	return ((h16 << 4) + (h8 << 3) + (h4 << 2) + (h2 << 1) + base) * leaf
		+ (t2 ? leaf / 2 : 0) + (t4 ? leaf / 4 : 0) + (t8 ? leaf / 8 : 0) + (t16 ? leaf / 16 : 0);
}

uint32_t SinglePageSysFlag(size_t target, uint32_t version, uint32_t& outSize)
{
	if (target < 1) target = 1;
	uint32_t k = 0;
	while (((size_t)8192 << k) < target && k < 19) ++k;
	uint32_t leafShift, bits;
	if (k <= 4)
	{
		leafShift = 0;
		static const uint32_t kBit[5] = { 1u << 17, 1u << 11, 1u << 7, 1u << 5, 1u << 4 };
		bits = kBit[k];
	}
	else { leafShift = k - 4; bits = (1u << 4); }
	outSize = (uint32_t)((size_t)8192 << k);
	return (leafShift & 0xF) | bits | ((version & 0xF) << 28);
}

constexpr uint64_t kVirtualBase = 0x50000000ull;
constexpr uint64_t kPhysicalBase = 0x60000000ull;

size_t DerefToOffset(uint64_t ptr, size_t virtualSize, size_t physicalSize)
{
	if (ptr == 0) return SIZE_MAX;
	if (ptr >= kVirtualBase && ptr < kPhysicalBase)
	{
		size_t off = (size_t)(ptr - kVirtualBase);
		return off < virtualSize ? off : SIZE_MAX;
	}
	if (ptr >= kPhysicalBase)
	{
		size_t off = (size_t)(ptr - kPhysicalBase);
		return off < physicalSize ? (virtualSize + off) : SIZE_MAX;
	}
	return SIZE_MAX;
}

struct ExtractResult
{
	std::vector<uint8_t> image; // [virtual pages][physical pages]
	size_t virtualSize = 0;
	size_t physicalSize = 0;
	uint32_t virtFlags = 0;
	uint32_t physFlags = 0;
	int version = 0;
	bool ok = false;
	std::string error;
};

ExtractResult ExtractRaw(const char* modelName)
{
	ExtractResult r;

	auto mgr = streaming::Manager::GetInstance();
	if (!mgr) { r.error = "streaming manager unavailable"; return r; }

	auto mod = mgr->moduleMgr.GetStreamingModule("ydr");
	if (!mod) { r.error = "ydr streaming module not found"; return r; }

	uint32_t slot = 0xFFFFFFFF;
	mod->FindSlot(&slot, modelName);
	if (slot == 0xFFFFFFFF) { r.error = "model not found / not streamed in"; return r; }

	uint32_t globalIdx = mod->baseIdx + slot;
	if ((int)globalIdx < 0 || globalIdx >= (uint32_t)mgr->numEntries) { r.error = "streaming index out of range"; return r; }

	StreamingDataEntry& e = mgr->Entries[globalIdx];
	uint32_t handle = e.handle;
	if (handle == 0 || handle == 0xFFFFFFFF) { r.error = "no streaming handle (asset not backed by a file?)"; return r; }

	rage::fiCollection* col = streaming::GetRawStreamerByIndex(streaming::GetCollectionIndex(handle));
	if (!col) { r.error = "no collection for handle"; return r; }

	uint16_t entIdx = streaming::GetEntryIndex(handle);
	rage::fiCollection::RawEntry* re = col->GetEntry(entIdx);
	if (!re) { r.error = "no entry for handle"; return r; }

	r.virtFlags = re->fe.virtFlags;
	r.physFlags = re->fe.physFlags;
	r.virtualSize = FlagSize(r.virtFlags);
	r.physicalSize = FlagSize(r.physFlags);
	r.version = (((r.virtFlags >> 28) & 0xF) << 4) | ((r.physFlags >> 28) & 0xF);

	const size_t total = r.virtualSize + r.physicalSize;
	if (total == 0 || total > 64u * 1024u * 1024u) { r.error = "implausible resource size"; return r; }

	uint32_t consumed = (uint32_t)re->fe.size;
	if (consumed <= 0x10) { r.error = "unsupported / empty resource entry"; return r; }

	// RPF resources have a 16-byte on-disk header before the deflate payload (skip it).
	const uint32_t payloadLen = consumed - 0x10;
	uint64_t ptr = 0;
	int64_t bulk = col->OpenCollectionEntry(entIdx, &ptr);
	std::vector<uint8_t> blob(payloadLen);
	uint32_t read = col->ReadBulk((uint64_t)bulk, ptr + 0x10, blob.data(), payloadLen);
	col->CloseBulk((uint64_t)bulk);
	if (read < payloadLen) { r.error = "ReadBulk returned a short read"; return r; }

	r.image.resize(total);
	if (!InflateRaw(blob.data(), payloadLen, r.image.data(), total))
	{
		if (payloadLen == total) { memcpy(r.image.data(), blob.data(), total); } // stored
		else { r.error = "could not inflate the resource (encrypted .ysc, or unexpected format)"; return r; }
	}

	r.ok = true;
	return r;
}

void AppendHeader(std::vector<uint8_t>& file, int version, uint32_t sysFlag, uint32_t gfxFlag)
{
	auto a32 = [&file](uint32_t v)
	{
		file.push_back((uint8_t)(v & 0xff)); file.push_back((uint8_t)((v >> 8) & 0xff));
		file.push_back((uint8_t)((v >> 16) & 0xff)); file.push_back((uint8_t)((v >> 24) & 0xff));
	};
	a32(0x37435352u); // 'RSC7'
	a32((uint32_t)version);
	a32(sysFlag);
	a32(gfxFlag);
}

bool WriteFileBytes(const char* outPath, const std::vector<uint8_t>& file, std::string& outError)
{
	HANDLE h = CreateFileA(outPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) { outError = "could not create the output file"; return false; }
	DWORD written = 0;
	BOOL ok = WriteFile(h, file.data(), (DWORD)file.size(), &written, nullptr);
	CloseHandle(h);
	if (!ok || written != file.size()) { outError = "write failed"; return false; }
	return true;
}
} // namespace

namespace ydrexport
{
bool ExportPatched(const char* modelName, const char* outPath,
	const Patch* patches, int patchCount, int* outApplied, std::string& outError)
{
	if (!modelName || !modelName[0] || !outPath) { outError = "invalid arguments"; return false; }

	ExtractResult ex = ExtractRaw(modelName);
	if (!ex.ok) { outError = ex.error; return false; }

	uint8_t* v = ex.image.data();
	int applied = 0;
	for (int i = 0; i < patchCount; ++i)
	{
		const Patch& p = patches[i];
		if (p.size == 0 || !p.data) continue;
		if (p.virtualOffset > ex.virtualSize || p.size > ex.virtualSize - p.virtualOffset) continue;
		memcpy(v + p.virtualOffset, p.data, p.size);
		++applied;
	}
	if (outApplied) *outApplied = applied;

	std::vector<uint8_t> file;
	AppendHeader(file, ex.version ? ex.version : 165, ex.virtFlags, ex.physFlags);
	Deflate(ex.image.data(), ex.image.size(), file);
	return WriteFileBytes(outPath, file, outError);
}

bool ExportLights(const char* modelName, const char* outPath,
	size_t lightFieldOffset, const void* liveLights, int liveCount, size_t lightStride,
	std::string& outError)
{
	if (!modelName || !modelName[0] || !outPath) { outError = "invalid arguments"; return false; }
	if (liveCount < 0 || liveCount > 0xFFFF) { outError = "implausible light count"; return false; }

	ExtractResult ex = ExtractRaw(modelName);
	if (!ex.ok) { outError = ex.error; return false; }

	if (lightFieldOffset + 12 > ex.virtualSize) { outError = "light field offset lies outside the virtual page"; return false; }

	uint8_t* v = ex.image.data();
	uint64_t elemPtr = *reinterpret_cast<uint64_t*>(v + lightFieldOffset);
	uint16_t diskCount = *reinterpret_cast<uint16_t*>(v + lightFieldOffset + 8);

	size_t elemOff = SIZE_MAX;
	if (diskCount > 0)
	{
		elemOff = DerefToOffset(elemPtr, ex.virtualSize, ex.physicalSize);
		if (elemOff == SIZE_MAX) { outError = "could not resolve the on-disk light array pointer"; return false; }
		if (elemOff + (size_t)diskCount * lightStride > ex.virtualSize) { outError = "light array overruns the virtual page"; return false; }
	}

	const uint8_t* live = reinterpret_cast<const uint8_t*>(liveLights);
	std::vector<uint8_t> outImage;
	uint32_t sysFlag = ex.virtFlags;
	uint32_t gfxFlag = ex.physFlags; // physical pages are never touched

	if ((int)diskCount == liveCount && diskCount > 0)
	{
		// Count-preserving: overwrite each light's editable fields, keep the on-disk vtable slot.
		for (int i = 0; i < (int)diskCount; ++i)
			memcpy(v + elemOff + (size_t)i * lightStride + 8, live + (size_t)i * lightStride + 8, lightStride - 8);
		outImage = std::move(ex.image);
	}
	else
	{
		const size_t oldVirtual = ex.virtualSize;
		const size_t arrayOffset = oldVirtual;
		const size_t arrayBytes = (size_t)liveCount * lightStride;
		const size_t newSysEnd = arrayOffset + arrayBytes;
		const uint32_t sysVer = (ex.virtFlags >> 28) & 0xF;

		uint32_t newSysSize = 0;
		sysFlag = SinglePageSysFlag(newSysEnd, sysVer, newSysSize);
		if (newSysSize < newSysEnd) { outError = "system data too large to repage"; return false; }

		std::vector<uint8_t> nv(newSysSize, 0);
		memcpy(nv.data(), v, oldVirtual);

		uint8_t vtbl[8] = {};
		if (diskCount > 0) memcpy(vtbl, v + elemOff, 8); // all CLightAttr share one on-disk vtable

		for (int i = 0; i < liveCount; ++i)
		{
			uint8_t* d = nv.data() + arrayOffset + (size_t)i * lightStride;
			memcpy(d, vtbl, 8);
			memcpy(d + 8, live + (size_t)i * lightStride + 8, lightStride - 8);
		}

		*reinterpret_cast<uint64_t*>(nv.data() + lightFieldOffset) = (liveCount > 0) ? (kVirtualBase + arrayOffset) : 0;
		*reinterpret_cast<uint16_t*>(nv.data() + lightFieldOffset + 8) = (uint16_t)liveCount;
		*reinterpret_cast<uint16_t*>(nv.data() + lightFieldOffset + 10) = (uint16_t)liveCount;

		outImage.resize(newSysSize + ex.physicalSize);
		memcpy(outImage.data(), nv.data(), newSysSize);
		if (ex.physicalSize) memcpy(outImage.data() + newSysSize, v + oldVirtual, ex.physicalSize);
	}

	std::vector<uint8_t> file;
	AppendHeader(file, ex.version ? ex.version : 165, sysFlag, gfxFlag);
	Deflate(outImage.data(), outImage.size(), file);
	return WriteFileBytes(outPath, file, outError);
}
} // namespace ydrexport
