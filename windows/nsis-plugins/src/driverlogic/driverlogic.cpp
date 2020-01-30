#include <stdafx.h>
#include "../error.h"
#include "context.h"
#include <libcommon/string.h>
#include <libcommon/error.h>
#include <libcommon/valuemapper.h>
#include <windows.h>

// Suppress warnings caused by broken legacy code
#pragma warning (push)
#pragma warning (disable: 4005)
#include <nsis/pluginapi.h>
#pragma warning (pop)

Context *g_context = nullptr;

namespace
{

EXTERN_C IMAGE_DOS_HEADER __ImageBase;

void PinDll()
{
	//
	// Apparently NSIS loads and unloads the plugin module for EVERY call it makes to the plugin.
	// This makes it kind of difficult to maintain state.
	//
	// We can work around this by incrementing the module reference count.
	// When NSIS calls FreeLibrary() the reference count decrements and becomes one.
	//

	wchar_t self[MAX_PATH];

	if (0 == GetModuleFileNameW((HINSTANCE)&__ImageBase, self, _countof(self)))
	{
		THROW_ERROR("Failed to pin plugin module");
	}

	//
	// NSIS sometimes frees a plugin module more times than it loads it.
	// This hasn't been observed for this particular plugin but let's up the
	// reference count a bit extra anyway.
	//
	for (int i = 0; i < 100; ++i)
	{
		LoadLibraryW(self);
	}
}

} // anonymous namespace

//
// Initialize
//
// Call this function once during startup.
//

void __declspec(dllexport) NSISCALL Initialize
(
	HWND hwndParent,
	int string_size,
	LPTSTR variables,
	stack_t **stacktop,
	extra_parameters *extra,
	...
)
{
	EXDLL_INIT();

	try
	{
		if (nullptr == g_context)
		{
			g_context = new Context;

			PinDll();
		}

		pushstring(L"");
		pushint(NsisStatus::SUCCESS);
	}
	catch (std::exception &err)
	{
		pushstring(common::string::ToWide(err.what()).c_str());
		pushint(NsisStatus::GENERAL_ERROR);
	}
	catch (...)
	{
		pushstring(L"Unspecified error");
		pushint(NsisStatus::GENERAL_ERROR);
	}
}

//
// RemoveOldMullvadTap
//
// Deletes the old Mullvad TAP adapter with ID tap0901.
//
//
enum class RemoveOldMullvadTapStatus
{
	GENERAL_ERROR = 0,
	SUCCESS_NO_REMAINING_TAP_ADAPTERS,
	SUCCESS_SOME_REMAINING_TAP_ADAPTERS
};

void __declspec(dllexport) NSISCALL RemoveOldMullvadTap
(
	HWND hwndParent,
	int string_size,
	LPTSTR variables,
	stack_t **stacktop,
	extra_parameters *extra,
	...
)
{
	EXDLL_INIT();

	try
	{
		pushstring(L"");
		
		switch (Context::DeleteOldMullvadAdapter())
		{
			case Context::DeletionResult::NO_REMAINING_TAP_ADAPTERS:
			{
				pushint(RemoveOldMullvadTapStatus::SUCCESS_NO_REMAINING_TAP_ADAPTERS);
				break;
			}

			case Context::DeletionResult::SOME_REMAINING_TAP_ADAPTERS:
			{
				pushint(RemoveOldMullvadTapStatus::SUCCESS_SOME_REMAINING_TAP_ADAPTERS);
				break;
			}

			default:
			{
				THROW_ERROR("Unexpected case");
			}
		}
	}
	catch (std::exception &err)
	{
		pushstring(common::string::ToWide(err.what()).c_str());
		pushint(RemoveOldMullvadTapStatus::GENERAL_ERROR);
	}
	catch (...)
	{
		pushstring(L"Unspecified error");
		pushint(RemoveOldMullvadTapStatus::GENERAL_ERROR);
	}
}


//
// IdentifyNewAdapter
//
// Call this function after installing a TAP adapter.
//
// By comparing with the previously captured baseline we're able to
// identify the new adapter.
//

void __declspec(dllexport) NSISCALL IdentifyNewAdapter
(
	HWND hwndParent,
	int string_size,
	LPTSTR variables,
	stack_t **stacktop,
	extra_parameters *extra,
	...
)
{
	EXDLL_INIT();

	if (nullptr == g_context)
	{
		pushstring(L"Initialize() function was not called or was not successful");
		pushint(NsisStatus::GENERAL_ERROR);
		return;
	}

	try
	{
		auto adapter = g_context->getAdapter();

		pushstring(adapter.alias.c_str());
		pushint(NsisStatus::SUCCESS);
	}
	catch (std::exception &err)
	{
		pushstring(common::string::ToWide(err.what()).c_str());
		pushint(NsisStatus::GENERAL_ERROR);
	}
	catch (...)
	{
		pushstring(L"Unspecified error");
		pushint(NsisStatus::GENERAL_ERROR);
	}
}

//
// Deinitialize
//
// Call this function once during shutdown.
//

void __declspec(dllexport) NSISCALL Deinitialize
(
	HWND hwndParent,
	int string_size,
	LPTSTR variables,
	stack_t **stacktop,
	extra_parameters *extra,
	...
)
{
	EXDLL_INIT();

	try
	{
		delete g_context;

		pushstring(L"");
		pushint(NsisStatus::SUCCESS);
	}
	catch (std::exception &err)
	{
		pushstring(common::string::ToWide(err.what()).c_str());
		pushint(NsisStatus::GENERAL_ERROR);
	}
	catch (...)
	{
		pushstring(L"Unspecified error");
		pushint(NsisStatus::GENERAL_ERROR);
	}

	g_context = nullptr;
}
