#include "stdafx.h"
#include "context.h"

#include <libcommon/guid.h>
#include <libcommon/string.h>
#include <libcommon/error.h>
#include <libcommon/memory.h>
#include <libcommon/network/nci.h>
#include <log/log.h>

#include <windows.h>

#include <vector>
#include <list>
#include <sstream>
#include <algorithm>

#include <setupapi.h>
#include <devguid.h>
#include <combaseapi.h>
#include <initguid.h>
#include <devpkey.h>

namespace
{

const wchar_t TAP_HARDWARE_ID[] = L"tapmullvad0901";
const wchar_t DEPRECATED_TAP_HARDWARE_ID[] = L"tap0901";

template<typename T>
void LogAdapters(const std::wstring &description, const T &adapters)
{
	//
	// Flatten the information so we can log it more easily.
	//

	std::vector<std::wstring> details;

	for (const auto &adapter : adapters)
	{
		details.emplace_back(L"Adapter");

		details.emplace_back(std::wstring(L"    Guid: ").append(adapter.guid));
		details.emplace_back(std::wstring(L"    Name: ").append(adapter.name));
		details.emplace_back(std::wstring(L"    Alias: ").append(adapter.alias));
	}

	PluginLogWithDetails(description, details);
}

std::wstring GetNetCfgInstanceId(HDEVINFO devInfo, const SP_DEVINFO_DATA &devInfoData)
{
	HKEY hNet = SetupDiOpenDevRegKey(
		devInfo,
		const_cast<SP_DEVINFO_DATA *>(&devInfoData),
		DICS_FLAG_GLOBAL,
		0,
		DIREG_DRV,
		KEY_READ
	);

	if (hNet == INVALID_HANDLE_VALUE)
	{
		THROW_WINDOWS_ERROR(GetLastError(), "SetupDiOpenDevRegKey");
	}

	std::vector<wchar_t> instanceId(MAX_PATH + sizeof(L'\0'));
	DWORD strSize = instanceId.size() * sizeof(wchar_t);

	const auto status = RegGetValueW(
		hNet,
		nullptr,
		L"NetCfgInstanceId",
		RRF_RT_REG_SZ,
		nullptr,
		instanceId.data(),
		&strSize
	);

	RegCloseKey(hNet);

	if (ERROR_SUCCESS != status)
	{
		THROW_WINDOWS_ERROR(status, "RegGetValueW");
	}

	instanceId[strSize / sizeof(wchar_t)] = L'\0';

	return instanceId.data();
}

std::wstring GetDeviceInstanceId(
	HDEVINFO devInfo,
	SP_DEVINFO_DATA* devInfoData
)
{
	DWORD requiredSize = 0;

	SetupDiGetDeviceInstanceIdW(
		devInfo,
		devInfoData,
		nullptr,
		0,
		&requiredSize
	);

	std::vector<wchar_t> deviceInstanceId;
	deviceInstanceId.resize(1 + requiredSize * sizeof(wchar_t));

	const auto status = SetupDiGetDeviceInstanceIdW(
		devInfo,
		devInfoData,
		&deviceInstanceId[0],
		deviceInstanceId.size(),
		nullptr
	);

	if (FALSE == status)
	{
		THROW_WINDOWS_ERROR(GetLastError(), "SetupDiGetDeviceInstanceIdW() failed");
	}

	return deviceInstanceId.data();
}

std::wstring GetDeviceStringProperty(
	HDEVINFO devInfo,
	SP_DEVINFO_DATA *devInfoData,
	const DEVPROPKEY *property
)
{
	//
	// Obtain required buffer size
	//

	DWORD requiredSize = 0;
	DEVPROPTYPE type;

	const auto sizeStatus = SetupDiGetDevicePropertyW(
		devInfo,
		devInfoData,
		property,
		&type,
		nullptr,
		0,
		&requiredSize,
		0
	);

	if (FALSE == sizeStatus)
	{
		const auto lastError = GetLastError();

		if (ERROR_INSUFFICIENT_BUFFER != lastError)
		{
			THROW_WINDOWS_ERROR(lastError, "SetupDiGetDevicePropertyW");
		}
	}

	std::vector<wchar_t> buffer;
	buffer.resize(1 + requiredSize / sizeof(wchar_t));

	//
	// Read property
	//

	const auto status = SetupDiGetDevicePropertyW(
		devInfo,
		devInfoData,
		property,
		&type,
		reinterpret_cast<PBYTE>(&buffer[0]),
		buffer.size() * sizeof(wchar_t),
		nullptr,
		0
	);

	if (FALSE == status)
	{
		THROW_WINDOWS_ERROR(GetLastError(), "Failed to read device property");
	}

	return buffer.data();
}

std::optional<std::wstring> GetDeviceRegistryStringProperty(
	HDEVINFO devInfo,
	SP_DEVINFO_DATA *devInfoData,
	DWORD property
)
{
	//
	// Obtain required buffer size
	//

	DWORD requiredSize = 0;

	const auto sizeStatus = SetupDiGetDeviceRegistryPropertyW(
		devInfo,
		devInfoData,
		property,
		nullptr,
		nullptr,
		0,
		&requiredSize
	);

	const DWORD lastError = GetLastError();
	if (FALSE == sizeStatus && ERROR_INSUFFICIENT_BUFFER != lastError)
	{
		// ERROR_INVALID_DATA may mean that the property does not exist
		// TODO: Check if there may be other causes.
		if (ERROR_INVALID_DATA != lastError)
		{
			THROW_WINDOWS_ERROR(lastError, "SetupDiGetDeviceRegistryPropertyW");
		}

		return std::nullopt;
	}

	//
	// Read property
	//

	std::vector<wchar_t> buffer;
	buffer.resize(1 + requiredSize / sizeof(wchar_t));

	const auto status = SetupDiGetDeviceRegistryPropertyW(
		devInfo,
		devInfoData,
		property,
		nullptr,
		reinterpret_cast<PBYTE>(&buffer[0]),
		buffer.size() * sizeof(wchar_t),
		nullptr
	);

	if (FALSE == status)
	{
		THROW_WINDOWS_ERROR(GetLastError(), "Failed to read device property");
	}

	return { buffer.data() };
}

std::set<Context::NetworkAdapter> GetTapAdapters(const std::wstring &tapHardwareId)
{
	std::set<Context::NetworkAdapter> adapters;

	HDEVINFO devInfo = SetupDiGetClassDevs(
		&GUID_DEVCLASS_NET,
		nullptr,
		nullptr,
		DIGCF_PRESENT
	);

	if (INVALID_HANDLE_VALUE == devInfo)
	{
		THROW_WINDOWS_ERROR(GetLastError(), "SetupDiGetClassDevs() failed");
	}

	common::memory::ScopeDestructor scopeDestructor;
	scopeDestructor += [devInfo]()
	{
		SetupDiDestroyDeviceInfoList(devInfo);
	};

	common::network::Nci nci;

	for (int memberIndex = 0; ; memberIndex++)
	{
		SP_DEVINFO_DATA devInfoData = { 0 };
		devInfoData.cbSize = sizeof(devInfoData);

		if (FALSE == SetupDiEnumDeviceInfo(devInfo, memberIndex, &devInfoData))
		{
			const auto lastError = GetLastError();

			if (ERROR_NO_MORE_ITEMS == lastError)
			{
				// Done
				break;
			}

			THROW_WINDOWS_ERROR(lastError, "SetupDiEnumDeviceInfo() failed while enumerating network adapters");
		}

		//
		// Check whether this is a TAP adapter
		//

		const auto hardwareId = GetDeviceRegistryStringProperty(devInfo, &devInfoData, SPDRP_HARDWAREID);
		if (!hardwareId.has_value()
			|| wcscmp(hardwareId.value().c_str(), tapHardwareId.c_str()) != 0)
		{
			continue;
		}

		//
		// Construct NetworkAdapter
		//

		try
		{
			const std::wstring guid = GetNetCfgInstanceId(devInfo, devInfoData);
			GUID guidObj = common::Guid::FromString(guid);

			adapters.emplace(Context::NetworkAdapter(
				guid,
				GetDeviceStringProperty(devInfo, &devInfoData, &DEVPKEY_Device_DriverDesc),
				nci.getConnectionName(guidObj),
				GetDeviceInstanceId(devInfo, &devInfoData)
			));
		}
		catch (const std::exception &e)
		{
			//
			// Log exception and skip this adapter
			//

			std::string msg = "Skipping TAP adapter due to exception caught while iterating: ";
			msg.append(e.what());
			PluginLog(std::wstring(msg.begin(), msg.end()));
		}
	}

	return adapters;
}

} // anonymous namespace

//static
std::optional<Context::NetworkAdapter> Context::FindMullvadAdapter(const std::set<Context::NetworkAdapter> &tapAdapters)
{
	if (tapAdapters.empty())
	{
		return std::nullopt;
	}
	
	//
	// Look for TAP adapter with alias "Mullvad".
	//

	auto findByAlias = [](const std::set<NetworkAdapter> &adapters, const std::wstring &alias)
	{
		const auto it = std::find_if(adapters.begin(), adapters.end(), [&alias](const NetworkAdapter &candidate)
		{
			return 0 == _wcsicmp(candidate.alias.c_str(), alias.c_str());
		});

		return it;
	};

	static const wchar_t baseAlias[] = L"Mullvad";

	const auto mullvadAdapter = findByAlias(tapAdapters, baseAlias);

	if (tapAdapters.end() != mullvadAdapter)
	{
		return { *mullvadAdapter };
	}

	//
	// Look for TAP adapter with alias "Mullvad-1", "Mullvad-2", etc.
	//

	for (auto i = 0; i < 10; ++i)
	{
		std::wstringstream ss;

		ss << baseAlias << L"-" << i;

		const auto alias = ss.str();

		const auto mullvadAdapter = findByAlias(tapAdapters, alias);

		if (tapAdapters.end() != mullvadAdapter)
		{
			return { *mullvadAdapter };
		}
	}

	return std::nullopt;
}

Context::NetworkAdapter Context::getAdapter()
{
	std::set<NetworkAdapter> added = GetTapAdapters(TAP_HARDWARE_ID);

	if (added.size() > 1)
	{
		LogAdapters(L"Enumerable network TAP adapters", added);

		THROW_ERROR("Identified more TAP adapters than expected");
	}

	return *added.begin();
}

//static
Context::DeletionResult Context::DeleteOldMullvadAdapter()
{
	auto tapAdapters = GetTapAdapters(DEPRECATED_TAP_HARDWARE_ID);
	std::optional<NetworkAdapter> mullvadAdapter = FindMullvadAdapter(tapAdapters);

	if (!mullvadAdapter.has_value())
	{
		THROW_ERROR("Mullvad TAP adapter not found");
	}

	const auto mullvadGuid = mullvadAdapter.value().guid;

	HDEVINFO devInfo = SetupDiGetClassDevsW(
		&GUID_DEVCLASS_NET,
		nullptr,
		nullptr,
		DIGCF_PRESENT
	);

	if (INVALID_HANDLE_VALUE == devInfo)
	{
		THROW_WINDOWS_ERROR(GetLastError(), "SetupDiGetClassDevs() failed");
	}

	common::memory::ScopeDestructor cleanupDevList;
	cleanupDevList += [&devInfo]()
	{
		SetupDiDestroyDeviceInfoList(devInfo);
	};

	int numRemainingAdapters = 0;

	for (int memberIndex = 0; ; memberIndex++)
	{
		SP_DEVINFO_DATA devInfoData = { 0 };
		devInfoData.cbSize = sizeof(devInfoData);
		
		if (FALSE == SetupDiEnumDeviceInfo(devInfo, memberIndex, &devInfoData))
		{
			const auto lastError = GetLastError();

			if (ERROR_NO_MORE_ITEMS == lastError)
			{
				break;
			}

			THROW_WINDOWS_ERROR(lastError, "Error enumerating network adapters");
		}

		const auto hardwareId = GetDeviceRegistryStringProperty(devInfo, &devInfoData, SPDRP_HARDWAREID);

		if (hardwareId.has_value()
			&& wcscmp(DEPRECATED_TAP_HARDWARE_ID, hardwareId.value().data()) == 0)
		{
			if (0 != GetNetCfgInstanceId(devInfo, devInfoData).compare(mullvadGuid))
			{
				numRemainingAdapters++;
				continue;
			}

			if (FALSE == SetupDiRemoveDevice(
				devInfo,
				&devInfoData
			))
			{
				THROW_WINDOWS_ERROR(GetLastError(), "Error removing Mullvad TAP device");
			}
		}
	}

	return (numRemainingAdapters > 0)
		? DeletionResult::SOME_REMAINING_TAP_ADAPTERS
		: DeletionResult::NO_REMAINING_TAP_ADAPTERS;
}
