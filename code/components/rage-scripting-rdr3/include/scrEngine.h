/*
 * This file is part of the CitizenFX project - http://citizen.re/
 *
 * See LICENSE and MENTIONS in the root of the source tree for information
 * regarding licensing.
 */

// This file contains code adapted from the original GTA IV script hook, the 
// copyright notice for which follows below.

/*****************************************************************************\

Copyright (C) 2009, Aru <oneforaru at gmail dot com>

This software is provided 'as-is', without any express or implied
warranty.  In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
claim that you wrote the original software. If you use this software
in a product, an acknowledgment in the product documentation would be
appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be
misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.

\*****************************************************************************/

#pragma once
#include "pgCollection.h"
#include "scrThread.h"

#include <vector>

namespace rage
{
class 
#ifdef COMPILING_RAGE_SCRIPTING_RDR3
	__declspec(dllexport)
#else
	__declspec(dllimport)
#endif
	scrEngine
{
public:
	static pgPtrCollection<GtaThread>* GetThreadCollection();

	static uint32_t IncrementThreadId();

	// gets the active thread
	static scrThread* GetActiveThread();

	// sets the currently running thread
	static void SetActiveThread(scrThread* thread);

	// adds a precreated custom thread to the runtime and starts it
	static void CreateThread(GtaThread* thread);

	// native function handler type
	typedef void(__cdecl * NativeHandler)(scrNativeCallContext* context);

	// gets a native function handler
	static NativeHandler GetNativeHandler(uint64_t hash);

	// queues the registration of a custom native function handler
	static void RegisterNativeHandler(const char* nativeName, NativeHandler handler);

	// queues the registration of a custom native function handler with an identifier
	static void RegisterNativeHandler(uint64_t nativeIdentifier, NativeHandler handler);
	
	// returns true if the native should be blocked
	// some natives may be blocked only on certain builds
	static bool ShouldBlockNative(uint64_t hash);

	// returns a list of native hashes that should be blocked
	static std::vector<uint64_t> GetBlockedNatives();

public:
	static fwEvent<> OnScriptInit;

	static fwEvent<bool&> CheckNativeScriptAllowed;
};

class scriptHandlerMgr
{
public:
	virtual inline ~scriptHandlerMgr() {}

	virtual void m1() = 0;

	virtual void m2() = 0;

	virtual void m3() = 0;

	virtual void m4() = 0;

	virtual void m5() = 0;

	virtual void m6() = 0;

	virtual void m7() = 0;

	virtual void m8() = 0;

	virtual void m9() = 0;

	virtual void AttachScript(scrThread* thread) = 0;

	virtual void DetachScript(scrThread* thread) = 0;
};
}

class NativeContextRaw :
	public rage::scrNativeCallContext
{
public:
	inline NativeContextRaw(void* args, void* rets, int nargs)
	{
		m_pArgs = args;
		m_pReturn = rets;
		m_nArgCount = nargs;
		m_nDataCount = 0;
	}

	inline void SetArgumentCount(int argumentCount)
	{
		m_nArgCount = argumentCount;
	}
};

class NativeContext :
	public rage::scrNativeCallContext
{
private:
	// Configuration
	enum
	{
		MaxNativeParams = 32,
		ArgSize = 8,
	};

	// Anything temporary that we need
	uint8_t m_TempStack[MaxNativeParams * ArgSize];

public:
	inline NativeContext()
	{
		m_pArgs = &m_TempStack;
		m_pReturn = &m_TempStack;		// It's okay to point both args and return at 
										// the same pointer. The game should handle this.
		m_nArgCount = 0;
		m_nDataCount = 0;

		memset(m_TempStack, 0, sizeof(m_TempStack));
	}

	template <typename T>
	inline void Push(T value)
	{
		if (sizeof(T) > ArgSize)
		{
			throw "Argument has an invalid size";
		}
		else if (sizeof(T) < ArgSize)
		{
			// Ensure we don't have any stray data
			*reinterpret_cast<uintptr_t*>(m_TempStack + ArgSize * m_nArgCount) = 0;
		}

		*reinterpret_cast<T*>(m_TempStack + ArgSize * m_nArgCount) = value;
		m_nArgCount++;
	}

	inline void Reverse()
	{
		uintptr_t tempValues[MaxNativeParams];
		uintptr_t* args = (uintptr_t*)m_pArgs;

		for (int i = 0; i < m_nArgCount; i++)
		{
			int target = m_nArgCount - i - 1;

			tempValues[target] = args[i];
		}

		memcpy(m_TempStack, tempValues, sizeof(m_TempStack));
	}

	template <typename T>
	inline T GetResult()
	{
		return *reinterpret_cast<T*>(m_TempStack);
	}
};

class NativeInvoke
{
public:
	template<uint64_t Hash, typename R, typename... Args>
	static inline R Invoke(Args... args)
	{
		NativeContext cxt;
		(cxt.Push(args), ...);

		static auto fn = rage::scrEngine::GetNativeHandler(Hash);
		if (fn != 0)
		{
			fn(&cxt);
		}

		cxt.SetVectorResults();

		if constexpr (!std::is_void_v<R>)
		{
			return cxt.GetResult<R>();
		}
	}
};

struct scrVector
{
	float x;

private:
	uint32_t pad;

public:
	float y;

private:
	uint32_t pad2;

public:
	float z;

private:
	uint32_t pad3;
};
