#include "pch.h"
#include "BochsUwpStorage.h"

#include <algorithm>
#include <cwctype>
#include <fileapi.h>
#include <vector>

using namespace concurrency;
using namespace Platform;
using namespace Windows::ApplicationModel;
using namespace Windows::Storage;
using namespace Windows::Storage::AccessCache;
using namespace Windows::Storage::Pickers;

namespace
{
	static bool StartsWithIgnoreCase(const std::wstring& value, const wchar_t *prefix)
	{
		size_t prefixLength = wcslen(prefix);
		if (value.length() < prefixLength)
		{
			return false;
		}

		return _wcsnicmp(value.c_str(), prefix, prefixLength) == 0;
	}

	static bool EndsWithIgnoreCase(const std::wstring& value, const wchar_t *suffix)
	{
		size_t suffixLength = wcslen(suffix);
		if (value.length() < suffixLength)
		{
			return false;
		}

		return _wcsicmp(value.c_str() + value.length() - suffixLength, suffix) == 0;
	}

	static std::wstring ToLower(std::wstring value)
	{
		std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch)
		{
			return static_cast<wchar_t>(std::towlower(ch));
		});
		return value;
	}

	static std::wstring ExtensionOf(const std::wstring& value)
	{
		size_t query = value.find_first_of(L"?#");
		std::wstring path = query == std::wstring::npos ? value : value.substr(0, query);
		size_t slash = path.find_last_of(L"\\/");
		size_t dot = path.find_last_of(L'.');
		if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash))
		{
			return std::wstring();
		}
		return ToLower(path.substr(dot));
	}

	static bool IsKnownDiskImageMode(const std::wstring& mode)
	{
		return mode == L"flat" ||
			mode == L"concat" ||
			mode == L"sparse" ||
			mode == L"growing" ||
			mode == L"undoable" ||
			mode == L"volatile" ||
			mode == L"vpc" ||
			mode == L"vbox" ||
			mode == L"vmware3" ||
			mode == L"vmware4" ||
			mode == L"vvfat" ||
			mode == L"uwp";
	}

	static std::wstring DetectDiskImageModeFromPath(const std::wstring& path)
	{
		if (StartsWithIgnoreCase(path, L"uwp://"))
		{
			return L"uwp";
		}

		std::wstring ext = ExtensionOf(path);
		if (ext == L".vmdk")
		{
			return L"vmware4";
		}
		if (ext == L".vdi")
		{
			return L"vbox";
		}
		if (ext == L".vhd" || ext == L".vpc")
		{
			return L"vpc";
		}
		return L"flat";
	}

	static std::wstring NormalizeDiskImageMode(String^ diskPath, String^ requestedMode)
	{
		std::wstring path = diskPath != nullptr ? std::wstring(diskPath->Data()) : std::wstring();
		if (StartsWithIgnoreCase(path, L"uwp://"))
		{
			return L"uwp";
		}

		if (requestedMode != nullptr && requestedMode->Length() > 0)
		{
			std::wstring mode = ToLower(std::wstring(requestedMode->Data()));
			if (mode == L"auto")
			{
				return DetectDiskImageModeFromPath(path);
			}
			if (mode == L"raw")
			{
				return L"flat";
			}
			if (mode == L"vhd")
			{
				return L"vpc";
			}
			if (mode == L"vdi")
			{
				return L"vbox";
			}
			if (mode == L"vmdk")
			{
				return L"vmware4";
			}
			if (IsKnownDiskImageMode(mode))
			{
				return mode;
			}
		}

		return DetectDiskImageModeFromPath(path);
	}

	static bool IsBrokeredRawDisk(const std::wstring& value)
	{
		return EndsWithIgnoreCase(value, L".img") ||
			EndsWithIgnoreCase(value, L".ima") ||
			EndsWithIgnoreCase(value, L".flp") ||
			EndsWithIgnoreCase(value, L".dsk") ||
			EndsWithIgnoreCase(value, L".fd") ||
			EndsWithIgnoreCase(value, L".vfd") ||
			EndsWithIgnoreCase(value, L".bin") ||
			EndsWithIgnoreCase(value, L".raw");
	}

	static String^ BuildUwpDiskUri(String^ token, String^ name)
	{
		std::wstring uri = L"uwp://";
		uri += token->Data();
		if (name != nullptr && name->Length() > 0)
		{
			uri += L"/";
			uri += name->Data();
		}
		return ref new String(uri.c_str());
	}

	static void AppendBootDevice(std::wstring& bootOrder, bool& first, const wchar_t *device)
	{
		if (!first)
		{
			bootOrder += L", ";
		}
		bootOrder += device;
		first = false;
	}

	static std::wstring TrimBootToken(const std::wstring& value)
	{
		size_t start = value.find_first_not_of(L" \t\r\n");
		if (start == std::wstring::npos)
		{
			return std::wstring();
		}
		size_t end = value.find_last_not_of(L" \t\r\n");
		return value.substr(start, end - start + 1);
	}

	static std::wstring NormalizeBootDevice(const std::wstring& value)
	{
		std::wstring token = TrimBootToken(value);
		if (token == L"a" || token == L"floppy")
		{
			return L"floppy";
		}
		if (token == L"c" || token == L"disk" || token == L"hd" || token == L"hdd")
		{
			return L"disk";
		}
		if (token == L"cd" || token == L"cdrom" || token == L"iso")
		{
			return L"cdrom";
		}
		if (token == L"none")
		{
			return L"none";
		}
		return std::wstring();
	}

	static bool BootDeviceAvailable(const std::wstring& device, bool hasDisk, bool hasFloppy, bool hasCdrom)
	{
		return (device == L"disk" && hasDisk) ||
			(device == L"floppy" && hasFloppy) ||
			(device == L"cdrom" && hasCdrom);
	}

	static bool BootDeviceAlreadyAdded(const std::vector<std::wstring>& devices, const std::wstring& device)
	{
		for (const auto& existing : devices)
		{
			if (existing == device)
			{
				return true;
			}
		}
		return false;
	}

	static void AddBootDeviceIfValid(
		std::vector<std::wstring>& devices,
		const std::wstring& device,
		bool hasDisk,
		bool hasFloppy,
		bool hasCdrom)
	{
		if (devices.size() >= 3 || device.empty() || device == L"none" ||
			!BootDeviceAvailable(device, hasDisk, hasFloppy, hasCdrom) ||
			BootDeviceAlreadyAdded(devices, device))
		{
			return;
		}
		devices.push_back(device);
	}

	static std::wstring BuildBootOrder(
		const std::wstring& requestedBoot,
		bool hasDisk,
		bool hasFloppy,
		bool hasCdrom)
	{
		std::vector<std::wstring> devices;
		size_t start = 0;
		while (start <= requestedBoot.length())
		{
			size_t comma = requestedBoot.find(L',', start);
			std::wstring token = requestedBoot.substr(
				start,
				comma == std::wstring::npos ? std::wstring::npos : comma - start);
			AddBootDeviceIfValid(devices, NormalizeBootDevice(token), hasDisk, hasFloppy, hasCdrom);
			if (comma == std::wstring::npos)
			{
				break;
			}
			start = comma + 1;
		}

		AddBootDeviceIfValid(devices, L"disk", hasDisk, hasFloppy, hasCdrom);
		AddBootDeviceIfValid(devices, L"cdrom", hasDisk, hasFloppy, hasCdrom);
		AddBootDeviceIfValid(devices, L"floppy", hasDisk, hasFloppy, hasCdrom);

		std::wstring bootOrder;
		bool first = true;
		for (const auto& device : devices)
		{
			AppendBootDevice(bootOrder, first, device.c_str());
		}
		if (bootOrder.empty())
		{
			bootOrder = L"disk";
		}

		return bootOrder;
	}

	static void AppendPciSlot(std::wstring& config, int& slot, const wchar_t *device)
	{
		if (slot > 5)
		{
			return;
		}

		config += L", slot";
		config += std::to_wstring(slot++);
		config += L"=";
		config += device;
	}

	static task<String^> CopyDiskImageToLocalFolderAsync(StorageFile^ pickedFile)
	{
		StorageFolder^ localFolder = ApplicationData::Current->LocalFolder;
		return create_task(pickedFile->CopyAsync(
			localFolder,
			pickedFile->Name,
			NameCollisionOption::ReplaceExisting)).then([](StorageFile^ localCopy)
		{
			return localCopy->Path;
		});
	}

	static task<String^> CopyPickedFileToLocalFolderAsync(StorageFile^ pickedFile)
	{
		StorageFolder^ localFolder = ApplicationData::Current->LocalFolder;
		return create_task(pickedFile->CopyAsync(
			localFolder,
			pickedFile->Name,
			NameCollisionOption::ReplaceExisting)).then([](StorageFile^ localCopy)
		{
			return localCopy->Path;
		});
	}

	static String^ DefaultBochsrcText(
		String^ diskPath,
		String^ floppyPath,
		String^ cdromPath,
		String^ bootTarget,
		int memoryMb,
		String^ cpuModel,
		String^ biosPath,
		String^ vgaBiosPath,
		bool soundEnabled,
		bool networkEnabled,
		String^ diskImageMode)
	{
		std::wstring assetsPath(Package::Current->InstalledLocation->Path->Data());
		assetsPath += L"\\Assets\\";
		std::wstring bios = (biosPath != nullptr && biosPath->Length() > 0)
			? std::wstring(biosPath->Data())
			: assetsPath + L"BIOS-bochs-latest";
		std::wstring vgaBios = (vgaBiosPath != nullptr && vgaBiosPath->Length() > 0)
			? std::wstring(vgaBiosPath->Data())
			: assetsPath + L"VGABIOS-lgpl-latest.bin";
		std::wstring boot = (bootTarget != nullptr && bootTarget->Length() > 0)
			? std::wstring(bootTarget->Data())
			: L"disk";
		int memory = memoryMb >= 16 ? memoryMb : 512;
		if (memory > 2048)
		{
			memory = 2048;
		}
		std::wstring cpu = (cpuModel != nullptr && cpuModel->Length() > 0)
			? std::wstring(cpuModel->Data())
			: L"corei7_haswell_4770";
		bool effectiveSoundEnabled = soundEnabled;

		std::wstring config;
		config += L"config_interface: textconfig\n";
		config += L"display_library: uwp_dx\n";
		config += L"memory: guest=";
		config += std::to_wstring(memory);
		config += L", host=";
		config += std::to_wstring(memory);
		config += L"\n";
		config += L"cpu: model=";
		config += cpu;
		config += L", count=1, ips=50000000, reset_on_triple_fault=1, ignore_bad_msrs=1, cpuid_limit_winnt=0\n";
		config += L"romimage: file=\"";
		config += bios;
		config += L"\"\n";
		config += L"vgaromimage: file=\"";
		config += vgaBios;
		config += L"\"\n";

		int pciSlot = 1;
		config += L"pci: enabled=1, chipset=i440fx";
		if (networkEnabled)
		{
			AppendPciSlot(config, pciSlot, L"ne2k");
		}
		if (effectiveSoundEnabled)
		{
			AppendPciSlot(config, pciSlot, L"es1370");
		}
		config += L"\n";

		std::wstring plugins;
		if (effectiveSoundEnabled)
		{
			plugins += L"speaker=1, sb16=1, es1370=1";
		}
		if (networkEnabled)
		{
			if (!plugins.empty())
			{
				plugins += L", ";
			}
			plugins += L"ne2k=1";
		}
		if (!plugins.empty())
		{
			config += L"plugin_ctrl: ";
			config += plugins;
			config += L"\n";
		}
		if (effectiveSoundEnabled)
		{
			config += L"sound: waveoutdrv=uwp, waveindrv=uwp, midioutdrv=uwp\n";
			config += L"speaker: enabled=1, mode=sound, volume=15\n";
			config += L"sb16: enabled=1, wavemode=1, midimode=1\n";
			config += L"es1370: enabled=1, wavemode=1, midimode=1\n";
		}
		if (networkEnabled)
		{
			config += L"ne2k: ioaddr=0x300, irq=10, mac=52:54:00:12:34:56, ethmod=slirp, script=\"\"\n";
		}

		bool hasDisk = diskPath != nullptr && diskPath->Length() > 0;
		bool hasFloppy = floppyPath != nullptr && floppyPath->Length() > 0;
		bool hasCdrom = cdromPath != nullptr && cdromPath->Length() > 0;
		std::wstring bootOrder = BuildBootOrder(boot, hasDisk, hasFloppy, hasCdrom);

		if (hasFloppy)
		{
			config += L"floppya: image=\"";
			config += floppyPath->Data();
			config += L"\", status=inserted\n";
		}
		else
		{
			config += L"floppya: type=1_44\n";
		}

		if (hasDisk || hasCdrom)
		{
			config += L"ata0: enabled=1, ioaddr1=0x1f0, ioaddr2=0x3f0, irq=14\n";
		}

		if (hasDisk)
		{
			std::wstring disk(diskPath->Data());
			std::wstring mode = NormalizeDiskImageMode(diskPath, diskImageMode);
			config += L"ata0-master: type=disk, path=\"";
			config += disk;
			config += L"\", mode=";
			config += mode;
			config += L"\n";
		}

		if (hasCdrom)
		{
			config += hasDisk
				? L"ata0-slave: type=cdrom, path=\""
				: L"ata0-master: type=cdrom, path=\"";
			config += cdromPath->Data();
			config += L"\", status=inserted\n";
		}

		config += L"boot: ";
		config += bootOrder;
		config += L"\n";
		config += L"log: -\n";
		return ref new String(config.c_str());
	}
}

task<String^> UWP_Port::BochsUwpStorage::EnsureDefaultConfigAsync()
{
	return CreateConfigForDiskAsync(nullptr);
}

task<String^> UWP_Port::BochsUwpStorage::CreateConfigForDiskAsync(String^ diskPath)
{
	return CreateConfigAsync(diskPath, nullptr, nullptr);
}

task<String^> UWP_Port::BochsUwpStorage::CreateConfigAsync(String^ diskPath, String^ biosPath, String^ vgaBiosPath)
{
	return CreateConfigAsync(diskPath, nullptr, nullptr, nullptr, 512, nullptr, biosPath, vgaBiosPath, true, true);
}

task<String^> UWP_Port::BochsUwpStorage::CreateConfigAsync(
	String^ diskPath,
	String^ floppyPath,
	String^ cdromPath,
	String^ bootTarget,
	int memoryMb,
	String^ cpuModel,
	String^ biosPath,
	String^ vgaBiosPath)
{
	return CreateConfigAsync(
		diskPath,
		floppyPath,
		cdromPath,
		bootTarget,
		memoryMb,
		cpuModel,
		biosPath,
		vgaBiosPath,
		true,
		true,
		nullptr);
}

task<String^> UWP_Port::BochsUwpStorage::CreateConfigAsync(
	String^ diskPath,
	String^ floppyPath,
	String^ cdromPath,
	String^ bootTarget,
	int memoryMb,
	String^ cpuModel,
	String^ biosPath,
	String^ vgaBiosPath,
	bool soundEnabled,
	bool networkEnabled)
{
	return CreateConfigAsync(
		diskPath,
		floppyPath,
		cdromPath,
		bootTarget,
		memoryMb,
		cpuModel,
		biosPath,
		vgaBiosPath,
		soundEnabled,
		networkEnabled,
		nullptr);
}

task<String^> UWP_Port::BochsUwpStorage::CreateConfigAsync(
	String^ diskPath,
	String^ floppyPath,
	String^ cdromPath,
	String^ bootTarget,
	int memoryMb,
	String^ cpuModel,
	String^ biosPath,
	String^ vgaBiosPath,
	bool soundEnabled,
	bool networkEnabled,
	String^ diskImageMode)
{
	StorageFolder^ localFolder = ApplicationData::Current->LocalFolder;
	String^ configText = CreateConfigText(
		diskPath,
		floppyPath,
		cdromPath,
		bootTarget,
		memoryMb,
		cpuModel,
		biosPath,
		vgaBiosPath,
		soundEnabled,
		networkEnabled,
		diskImageMode);
	return create_task(localFolder->CreateFileAsync(
		ref new String(L"bochsrc.generated.txt"),
		CreationCollisionOption::OpenIfExists)).then([](StorageFile^ file)
	{
		return file;
	}).then([configText](StorageFile^ file)
	{
		return create_task(FileIO::WriteTextAsync(file, configText)).then([file]()
		{
			return file->Path;
		});
	});
}

String^ UWP_Port::BochsUwpStorage::CreateConfigText(
	String^ diskPath,
	String^ floppyPath,
	String^ cdromPath,
	String^ bootTarget,
	int memoryMb,
	String^ cpuModel,
	String^ biosPath,
	String^ vgaBiosPath,
	bool soundEnabled,
	bool networkEnabled)
{
	return CreateConfigText(
		diskPath,
		floppyPath,
		cdromPath,
		bootTarget,
		memoryMb,
		cpuModel,
		biosPath,
		vgaBiosPath,
		soundEnabled,
		networkEnabled,
		nullptr);
}

String^ UWP_Port::BochsUwpStorage::CreateConfigText(
	String^ diskPath,
	String^ floppyPath,
	String^ cdromPath,
	String^ bootTarget,
	int memoryMb,
	String^ cpuModel,
	String^ biosPath,
	String^ vgaBiosPath,
	bool soundEnabled,
	bool networkEnabled,
	String^ diskImageMode)
{
	return DefaultBochsrcText(
		diskPath,
		floppyPath,
		cdromPath,
		bootTarget,
		memoryMb,
		cpuModel,
		biosPath,
		vgaBiosPath,
		soundEnabled,
		networkEnabled,
		diskImageMode);
}

task<String^> UWP_Port::BochsUwpStorage::PickDiskImageToLocalFolderAsync()
{
	FileOpenPicker^ picker = ref new FileOpenPicker();
	picker->ViewMode = PickerViewMode::List;
	picker->SuggestedStartLocation = PickerLocationId::ComputerFolder;
	picker->FileTypeFilter->Append(ref new String(L".img"));
	picker->FileTypeFilter->Append(ref new String(L".raw"));
	picker->FileTypeFilter->Append(ref new String(L".bin"));
	picker->FileTypeFilter->Append(ref new String(L".dsk"));
	picker->FileTypeFilter->Append(ref new String(L".hdd"));
	picker->FileTypeFilter->Append(ref new String(L".vhd"));
	picker->FileTypeFilter->Append(ref new String(L".vpc"));
	picker->FileTypeFilter->Append(ref new String(L".vmdk"));
	picker->FileTypeFilter->Append(ref new String(L".vdi"));

	return create_task(picker->PickSingleFileAsync()).then([](StorageFile^ pickedFile)
	{
		if (pickedFile == nullptr)
		{
			return task_from_result<String^>(nullptr);
		}

		if (IsBrokeredRawDisk(std::wstring(pickedFile->Name->Data())))
		{
			try
			{
				String^ token = StorageApplicationPermissions::FutureAccessList->Add(pickedFile);
				return task_from_result(BuildUwpDiskUri(token, pickedFile->Name));
			}
			catch (...)
			{
			}
		}

		return CopyDiskImageToLocalFolderAsync(pickedFile);
	});
}

task<String^> UWP_Port::BochsUwpStorage::PickFloppyImageToLocalFolderAsync()
{
	FileOpenPicker^ picker = ref new FileOpenPicker();
	picker->ViewMode = PickerViewMode::List;
	picker->SuggestedStartLocation = PickerLocationId::ComputerFolder;
	picker->FileTypeFilter->Append(ref new String(L".img"));
	picker->FileTypeFilter->Append(ref new String(L".ima"));
	picker->FileTypeFilter->Append(ref new String(L".flp"));
	picker->FileTypeFilter->Append(ref new String(L".dsk"));
	picker->FileTypeFilter->Append(ref new String(L".fd"));
	picker->FileTypeFilter->Append(ref new String(L".vfd"));
	picker->FileTypeFilter->Append(ref new String(L".bin"));
	picker->FileTypeFilter->Append(ref new String(L".raw"));

	return create_task(picker->PickSingleFileAsync()).then([](StorageFile^ pickedFile)
	{
		if (pickedFile == nullptr)
		{
			return task_from_result<String^>(nullptr);
		}

		if (IsBrokeredRawDisk(std::wstring(pickedFile->Name->Data())))
		{
			try
			{
				String^ token = StorageApplicationPermissions::FutureAccessList->Add(pickedFile);
				return task_from_result(BuildUwpDiskUri(token, pickedFile->Name));
			}
			catch (...)
			{
			}
		}

		return CopyPickedFileToLocalFolderAsync(pickedFile);
	});
}

task<String^> UWP_Port::BochsUwpStorage::PickCdromImageToLocalFolderAsync()
{
	FileOpenPicker^ picker = ref new FileOpenPicker();
	picker->ViewMode = PickerViewMode::List;
	picker->SuggestedStartLocation = PickerLocationId::ComputerFolder;
	picker->FileTypeFilter->Append(ref new String(L".iso"));
	picker->FileTypeFilter->Append(ref new String(L".cdr"));
	picker->FileTypeFilter->Append(ref new String(L".toast"));

	return create_task(picker->PickSingleFileAsync()).then([](StorageFile^ pickedFile)
	{
		if (pickedFile == nullptr)
		{
			return task_from_result<String^>(nullptr);
		}

		try
		{
			String^ token = StorageApplicationPermissions::FutureAccessList->Add(pickedFile);
			return task_from_result(BuildUwpDiskUri(token, pickedFile->Name));
		}
		catch (...)
		{
		}

		return CopyPickedFileToLocalFolderAsync(pickedFile);
	});
}

task<String^> UWP_Port::BochsUwpStorage::PickBiosImageToLocalFolderAsync()
{
	FileOpenPicker^ picker = ref new FileOpenPicker();
	picker->ViewMode = PickerViewMode::List;
	picker->SuggestedStartLocation = PickerLocationId::ComputerFolder;
	picker->FileTypeFilter->Append(ref new String(L"*"));

	return create_task(picker->PickSingleFileAsync()).then([](StorageFile^ pickedFile)
	{
		if (pickedFile == nullptr)
		{
			return task_from_result<String^>(nullptr);
		}

		return CopyPickedFileToLocalFolderAsync(pickedFile);
	});
}

String^ UWP_Port::BochsUwpStorage::DetectDiskImageMode(String^ diskPath)
{
	std::wstring path = diskPath != nullptr ? std::wstring(diskPath->Data()) : std::wstring();
	std::wstring mode = DetectDiskImageModeFromPath(path);
	return ref new String(mode.c_str());
}

String^ UWP_Port::BochsUwpStorage::GetSaveStateFolderPath()
{
	return GetSaveStateFolderPath(ref new String(L"1"));
}

String^ UWP_Port::BochsUwpStorage::GetSaveStateFolderPath(String^ slotId)
{
	std::wstring path(ApplicationData::Current->LocalFolder->Path->Data());
	std::wstring slot = (slotId != nullptr && slotId->Length() > 0)
		? std::wstring(slotId->Data())
		: L"1";
	if (slot == L"1")
	{
		path += L"\\BochsSaveState";
	}
	else
	{
		path += L"\\BochsSaveState";
		path += slot;
	}
	CreateDirectoryW(path.c_str(), nullptr);
	return ref new String(path.c_str());
}

task<bool> UWP_Port::BochsUwpStorage::HasSaveStateAsync()
{
	return HasSaveStateAsync(ref new String(L"1"));
}

task<bool> UWP_Port::BochsUwpStorage::HasSaveStateAsync(String^ slotId)
{
	StorageFolder^ localFolder = ApplicationData::Current->LocalFolder;
	std::wstring folderName = L"BochsSaveState";
	if (slotId != nullptr && slotId->Length() > 0 && wcscmp(slotId->Data(), L"1") != 0)
	{
		folderName += slotId->Data();
	}
	return create_task(localFolder->TryGetItemAsync(ref new String(folderName.c_str()))).then([](IStorageItem^ item)
	{
		StorageFolder^ folder = dynamic_cast<StorageFolder^>(item);
		if (folder == nullptr)
		{
			return task_from_result(false);
		}

		return create_task(folder->TryGetItemAsync(ref new String(L"config"))).then([](IStorageItem^ config)
		{
			return config != nullptr;
		});
	});
}
