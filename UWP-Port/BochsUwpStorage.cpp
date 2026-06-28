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
	const int DefaultGuestMemoryMb = 512;
	const int MinGuestMemoryMb = 16;
	const int MaxGuestMemoryMb = 8192;
	const int MaxResidentHostMemoryMb = 512;
	const int UwpMemoryBlockSizeKb = 1024;

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

	static bool CanEnumerateFolderWithWin32Apis(String^ folderPath)
	{
		if (folderPath == nullptr || folderPath->Length() == 0)
		{
			return false;
		}

		std::wstring filter(folderPath->Data());
		if (!filter.empty() && filter.back() != L'\\' && filter.back() != L'/')
		{
			filter += L"\\";
		}
		filter += L"*";

		WIN32_FIND_DATAW findData;
		HANDLE findHandle = FindFirstFileExW(
			filter.c_str(),
			FindExInfoBasic,
			&findData,
			FindExSearchNameMatch,
			nullptr,
			0);
		if (findHandle == INVALID_HANDLE_VALUE)
		{
			return false;
		}

		FindClose(findHandle);
		return true;
	}

	static bool CanOpenFileWithWin32Apis(String^ filePath)
	{
		if (filePath == nullptr || filePath->Length() == 0)
		{
			return false;
		}

		CREATEFILE2_EXTENDED_PARAMETERS params = {};
		params.dwSize = sizeof(params);
		params.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
		HANDLE handle = CreateFile2(
			filePath->Data(),
			GENERIC_READ,
			FILE_SHARE_READ | FILE_SHARE_WRITE,
			OPEN_EXISTING,
			&params);
		if (handle == INVALID_HANDLE_VALUE)
		{
			return false;
		}

		CloseHandle(handle);
		return true;
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

	static bool HasValue(String^ value)
	{
		return value != nullptr && value->Length() > 0;
	}

	static std::wstring StringValue(String^ value)
	{
		return HasValue(value) ? std::wstring(value->Data()) : std::wstring();
	}

	static bool TagEquals(String^ value, const wchar_t *tag)
	{
		return HasValue(value) && tag != nullptr && _wcsicmp(value->Data(), tag) == 0;
	}

	static int NormalizeNetworkSocketPort(int value)
	{
		if (value < 1024)
		{
			return 40000;
		}
		if (value > 65534)
		{
			return 65534;
		}
		return value;
	}

	static std::wstring NormalizeNetworkModel(String^ networkAdapter)
	{
		std::wstring adapter = HasValue(networkAdapter) ? ToLower(StringValue(networkAdapter)) : L"none";
		if (adapter.find(L"e1000") != std::wstring::npos)
		{
			return L"e1000";
		}
		if (adapter.find(L"ne2k") != std::wstring::npos)
		{
			return L"ne2k";
		}
		return L"none";
	}

	static std::wstring NormalizeNetworkBackend(String^ networkAdapter)
	{
		std::wstring adapter = HasValue(networkAdapter) ? ToLower(StringValue(networkAdapter)) : L"none";
		if (adapter.find(L"socket") != std::wstring::npos)
		{
			return L"socket";
		}
		if (adapter.find(L"vnet") != std::wstring::npos)
		{
			return L"vnet";
		}
		if (NormalizeNetworkModel(networkAdapter) != L"none")
		{
			return L"slirp";
		}
		return L"none";
	}

	static std::wstring NormalizePciChipset(String^ pciChipset)
	{
		std::wstring chipset = HasValue(pciChipset) ? ToLower(StringValue(pciChipset)) : L"i440fx";
		if (chipset == L"i430fx" || chipset == L"i440fx" || chipset == L"i440bx")
		{
			return chipset;
		}
		return L"i440fx";
	}

	static void AppendPlugin(std::wstring& plugins, const wchar_t *plugin)
	{
		if (plugin == nullptr || plugin[0] == 0)
		{
			return;
		}
		if (!plugins.empty())
		{
			plugins += L", ";
		}
		plugins += plugin;
		plugins += L"=1";
	}

	static void AppendAtaController(std::wstring& config, int controller)
	{
		static const wchar_t *resources[] =
		{
			L"ata0: enabled=1, ioaddr1=0x1f0, ioaddr2=0x3f0, irq=14\n",
			L"ata1: enabled=1, ioaddr1=0x170, ioaddr2=0x370, irq=15\n",
			L"ata2: enabled=1, ioaddr1=0x1e8, ioaddr2=0x3e0, irq=11\n",
			L"ata3: enabled=1, ioaddr1=0x168, ioaddr2=0x360, irq=9\n"
		};
		if (controller >= 0 && controller < 4)
		{
			config += resources[controller];
		}
	}

	static void AppendPciAdvancedOptions(
		std::wstring& config,
		const std::wstring& chipset,
		bool acpiEnabled,
		bool hpetEnabled,
		bool ioApicEnabled)
	{
		std::vector<std::wstring> options;
		if (!acpiEnabled && chipset == L"i440fx")
		{
			options.push_back(L"noacpi");
		}
		if (!hpetEnabled)
		{
			options.push_back(L"nohpet");
		}
		if (!ioApicEnabled)
		{
			options.push_back(L"noioapic");
		}
		if (options.empty())
		{
			return;
		}

		config += L", advopts=\"";
		for (size_t i = 0; i < options.size(); ++i)
		{
			if (i > 0)
			{
				config += L",";
			}
			config += options[i];
		}
		config += L"\"";
	}

	static void AppendNetworkConfig(std::wstring& config, String^ networkAdapter, int networkSocketPort)
	{
		std::wstring model = NormalizeNetworkModel(networkAdapter);
		if (model == L"none")
		{
			return;
		}

		std::wstring backend = NormalizeNetworkBackend(networkAdapter);
		std::wstring ethdev;
		if (backend == L"socket")
		{
			ethdev = std::to_wstring(NormalizeNetworkSocketPort(networkSocketPort));
		}
		else if (backend == L"vnet")
		{
			ethdev = ApplicationData::Current->LocalFolder->Path->Data();
		}

		if (model == L"ne2k")
		{
			config += L"ne2k: ioaddr=0x300, irq=10, mac=52:54:00:12:34:56, ethmod=";
			config += backend;
			if (!ethdev.empty())
			{
				config += L", ethdev=\"";
				config += ethdev;
				config += L"\"";
			}
			config += L", script=\"\"\n";
			return;
		}

		config += L"e1000: enabled=1, mac=52:54:00:12:34:56, ethmod=";
		config += backend;
		if (!ethdev.empty())
		{
			config += L", ethdev=\"";
			config += ethdev;
			config += L"\"";
		}
		config += L", script=\"\"\n";
	}

	static std::wstring AtaDeviceName(int slot)
	{
		std::wstring name(L"ata");
		name += std::to_wstring(slot / 2);
		name += (slot % 2) == 0 ? L"-master" : L"-slave";
		return name;
	}

	static void AppendAtaDisk(
		std::wstring& config,
		int slot,
		String^ path,
		String^ diskImageMode)
	{
		if (!HasValue(path) || slot < 0)
		{
			return;
		}

		config += AtaDeviceName(slot);
		config += L": type=disk, path=\"";
		config += path->Data();
		config += L"\", mode=";
		config += NormalizeDiskImageMode(path, diskImageMode);
		config += L", translation=lba\n";
	}

	static void AppendAtaCdrom(std::wstring& config, int slot, String^ path)
	{
		if (!HasValue(path) || slot < 0)
		{
			return;
		}

		config += AtaDeviceName(slot);
		config += L": type=cdrom, path=\"";
		config += path->Data();
		config += L"\", status=inserted\n";
	}

	static void AppendAtaVvfat(std::wstring& config, int slot, String^ sharedFolderPath)
	{
		if (!HasValue(sharedFolderPath) || slot < 0)
		{
			return;
		}

		std::wstring vvfatJournal(ApplicationData::Current->LocalFolder->Path->Data());
		vvfatJournal += L"\\vvfat-readonly-redolog";
		config += AtaDeviceName(slot);
		config += L": type=disk, path=\"readonly:";
		config += sharedFolderPath->Data();
		config += L"\", mode=vvfat, journal=\"";
		config += vvfatJournal;
		config += L"\", translation=lba\n";
	}

	static int NextAtaSlot(int& nextSlot)
	{
		if (nextSlot >= 8)
		{
			return -1;
		}
		return nextSlot++;
	}

	static void AppendUsbConfig(
		std::wstring& config,
		String^ usbController,
		String^ usbDevice,
		String^ usbImagePath)
	{
		if (TagEquals(usbController, L"none"))
		{
			return;
		}

		std::wstring controller = HasValue(usbController) ? StringValue(usbController) : L"none";
		std::wstring device = HasValue(usbDevice) ? StringValue(usbDevice) : L"none";
		if (controller == L"none")
		{
			return;
		}

		config += L"usb_";
		config += controller;
		config += L": enabled=1";
		if (controller == L"ehci")
		{
			config += L", companion=uhci";
		}
		if (controller == L"xhci")
		{
			config += L", n_ports=4";
		}

		if (device != L"none")
		{
			config += L", port1=";
			config += device;
			if (device == L"disk" || device == L"cdrom" || device == L"floppy")
			{
				if (HasValue(usbImagePath))
				{
					config += L", options1=\"path:";
					config += usbImagePath->Data();
					if (controller == L"xhci" && device == L"disk")
					{
						config += L", speed:super, proto:bbb";
					}
					else if (controller == L"ehci")
					{
						config += L", speed:high";
					}
					config += L"\"";
				}
			}
			else if (device == L"tablet")
			{
				config += L", options1=\"speed:low\"";
			}
		}
		config += L"\n";
	}

	static void AppendSerialConfig(std::wstring& config, String^ serialMode)
	{
		if (TagEquals(serialMode, L"null"))
		{
			config += L"com1: enabled=1, mode=null\n";
			return;
		}
		if (TagEquals(serialMode, L"file"))
		{
			std::wstring serialPath(ApplicationData::Current->LocalFolder->Path->Data());
			serialPath += L"\\com1.txt";
			config += L"com1: enabled=1, mode=file, dev=\"";
			config += serialPath;
			config += L"\"\n";
		}
	}

	static void AppendParallelConfig(std::wstring& config, bool parallelEnabled)
	{
		if (!parallelEnabled)
		{
			return;
		}

		std::wstring parallelPath(ApplicationData::Current->LocalFolder->Path->Data());
		parallelPath += L"\\parport1.out";
		config += L"parport1: enabled=1, file=\"";
		config += parallelPath;
		config += L"\"\n";
	}

	static int NormalizeVideoUpdateFreq(int value)
	{
		if (value < 0)
		{
			return 10;
		}
		if (value > 75)
		{
			return 75;
		}
		return value;
	}

	static int NormalizeVbeMemoryMb(int value)
	{
		switch (value)
		{
		case 4:
		case 8:
		case 16:
		case 32:
			return value;
		default:
			return 16;
		}
	}

	static int NormalizeRfbTimeout(int value)
	{
		if (value < 0)
		{
			return 0;
		}
		if (value > 300)
		{
			return 300;
		}
		return value;
	}

	static bool UseRfbDisplay(String^ displayLibrary)
	{
		return TagEquals(displayLibrary, L"rfb");
	}

	static void AppendDisplayConfig(std::wstring& config, String^ displayLibrary, int rfbTimeout)
	{
		if (UseRfbDisplay(displayLibrary))
		{
			config += L"display_library: rfb, options=\"timeout=";
			config += std::to_wstring(NormalizeRfbTimeout(rfbTimeout));
			config += L",no_gui_console\"\n";
			return;
		}

		config += L"display_library: uwp_dx\n";
	}

	static void AppendVideoConfig(
		std::wstring& config,
		String^ videoExtension,
		int videoUpdateFreq,
		bool videoRealtime,
		int vbeMemoryMb)
	{
		std::wstring extension = HasValue(videoExtension) ? StringValue(videoExtension) : L"vbe";
		if (extension != L"none" && extension != L"vbe" && extension != L"cirrus")
		{
			extension = L"vbe";
		}

		config += L"vga: extension=";
		config += extension;
		config += L", update_freq=";
		config += std::to_wstring(NormalizeVideoUpdateFreq(videoUpdateFreq));
		config += L", realtime=";
		config += videoRealtime ? L"1" : L"0";
		config += L", ddc=builtin";
		if (extension == L"vbe")
		{
			config += L", vbe_memsize=";
			config += std::to_wstring(NormalizeVbeMemoryMb(vbeMemoryMb));
		}
		config += L"\n";
	}

	static String^ DefaultBochsrcText(
		String^ diskPath,
		String^ disk2Path,
		String^ floppyPath,
		String^ floppyBPath,
		String^ cdromPath,
		String^ cdrom2Path,
		String^ sharedFolderPath,
		String^ bootTarget,
		int memoryMb,
		String^ cpuModel,
		String^ biosPath,
		String^ vgaBiosPath,
		bool soundEnabled,
		String^ networkAdapter,
		String^ diskImageMode,
		String^ disk2ImageMode,
		String^ usbController,
		String^ usbDevice,
		String^ usbImagePath,
		String^ serialMode,
		bool parallelEnabled,
		String^ videoExtension,
		int videoUpdateFreq,
		bool videoRealtime,
		int vbeMemoryMb,
		String^ displayLibrary,
		int rfbTimeout,
		int networkSocketPort,
		String^ pciChipset,
		bool acpiEnabled,
		bool hpetEnabled,
		bool ioApicEnabled)
	{
		std::wstring assetsPath(Package::Current->InstalledLocation->Path->Data());
		assetsPath += L"\\Assets\\";
		std::wstring bios = (biosPath != nullptr && biosPath->Length() > 0)
			? std::wstring(biosPath->Data())
			: assetsPath + L"BIOS-bochs-latest";
		String^ effectiveVideoExtension = UseRfbDisplay(displayLibrary)
			? ref new String(L"none")
			: videoExtension;
		bool useCirrus = TagEquals(effectiveVideoExtension, L"cirrus");
		std::wstring vgaBios = (vgaBiosPath != nullptr && vgaBiosPath->Length() > 0)
			? std::wstring(vgaBiosPath->Data())
			: assetsPath + (useCirrus ? L"VGABIOS-lgpl-latest-cirrus.bin" : L"VGABIOS-lgpl-latest.bin");
		std::wstring boot = (bootTarget != nullptr && bootTarget->Length() > 0)
			? std::wstring(bootTarget->Data())
			: L"disk";
		int memory = UWP_Port::BochsUwpStorage::NormalizeGuestMemoryMb(memoryMb);
		int hostMemory = UWP_Port::BochsUwpStorage::EffectiveHostMemoryMb(memory);
		std::wstring cpu = (cpuModel != nullptr && cpuModel->Length() > 0)
			? std::wstring(cpuModel->Data())
			: L"corei7_haswell_4770";
		bool effectiveSoundEnabled = soundEnabled;
		std::wstring networkModel = NormalizeNetworkModel(networkAdapter);
		std::wstring chipset = NormalizePciChipset(pciChipset);
		if (chipset == L"i430fx")
		{
			acpiEnabled = false;
			hpetEnabled = false;
		}
		else if (chipset == L"i440bx")
		{
			acpiEnabled = true;
		}

		std::wstring config;
		config += L"config_interface: textconfig\n";
		AppendDisplayConfig(config, displayLibrary, rfbTimeout);
		config += L"memory: guest=";
		config += std::to_wstring(memory);
		config += L", host=";
		config += std::to_wstring(hostMemory);
		config += L", block_size=";
		config += std::to_wstring(UWP_Port::BochsUwpStorage::MemoryBlockSizeKb());
		config += L"\n";
		config += L"cpu: model=";
		config += cpu;
		config += L", count=1, ips=50000000, reset_on_triple_fault=1, ignore_bad_msrs=1, cpuid_limit_winnt=0\n";
		config += L"clock: sync=both, time0=local, rtc_sync=1\n";
		config += L"romimage: file=\"";
		config += bios;
		config += L"\"\n";
		config += L"vgaromimage: file=\"";
		config += vgaBios;
		config += L"\"\n";
		AppendVideoConfig(config, effectiveVideoExtension, videoUpdateFreq, videoRealtime, vbeMemoryMb);

		int pciSlot = 1;
		config += L"pci: enabled=1, chipset=";
		config += chipset;
		if (useCirrus)
		{
			AppendPciSlot(config, pciSlot, L"cirrus");
		}
		if (networkModel == L"ne2k")
		{
			AppendPciSlot(config, pciSlot, L"ne2k");
		}
		else if (networkModel == L"e1000")
		{
			AppendPciSlot(config, pciSlot, L"e1000");
		}
		if (effectiveSoundEnabled)
		{
			AppendPciSlot(config, pciSlot, L"es1370");
		}
		if (HasValue(usbController) && !TagEquals(usbController, L"none"))
		{
			std::wstring usbSlot(L"usb_");
			usbSlot += usbController->Data();
			AppendPciSlot(config, pciSlot, usbSlot.c_str());
		}
		AppendPciAdvancedOptions(config, chipset, acpiEnabled, hpetEnabled, ioApicEnabled);
		config += L"\n";

		std::wstring plugins;
		if (effectiveSoundEnabled)
		{
			AppendPlugin(plugins, L"speaker");
			AppendPlugin(plugins, L"sb16");
			AppendPlugin(plugins, L"es1370");
		}
		if (useCirrus)
		{
			AppendPlugin(plugins, L"svga_cirrus");
		}
		if (networkModel == L"ne2k")
		{
			AppendPlugin(plugins, L"ne2k");
		}
		else if (networkModel == L"e1000")
		{
			AppendPlugin(plugins, L"e1000");
		}
		if (HasValue(usbController) && !TagEquals(usbController, L"none"))
		{
			std::wstring usbPlugin(L"usb_");
			usbPlugin += usbController->Data();
			AppendPlugin(plugins, usbPlugin.c_str());
			if (TagEquals(usbController, L"ehci"))
			{
				AppendPlugin(plugins, L"usb_uhci");
			}
		}
		if (HasValue(serialMode) && !TagEquals(serialMode, L"disabled"))
		{
			AppendPlugin(plugins, L"serial");
		}
		if (parallelEnabled)
		{
			AppendPlugin(plugins, L"parallel");
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
		AppendNetworkConfig(config, networkAdapter, networkSocketPort);
		AppendUsbConfig(config, usbController, usbDevice, usbImagePath);
		AppendSerialConfig(config, serialMode);
		AppendParallelConfig(config, parallelEnabled);

		bool hasDisk = diskPath != nullptr && diskPath->Length() > 0;
		bool hasDisk2 = disk2Path != nullptr && disk2Path->Length() > 0;
		bool hasFloppy = floppyPath != nullptr && floppyPath->Length() > 0;
		bool hasFloppyB = floppyBPath != nullptr && floppyBPath->Length() > 0;
		bool hasCdrom = cdromPath != nullptr && cdromPath->Length() > 0;
		bool hasCdrom2 = cdrom2Path != nullptr && cdrom2Path->Length() > 0;
		bool hasSharedFolder = sharedFolderPath != nullptr && sharedFolderPath->Length() > 0;
		std::wstring bootOrder = BuildBootOrder(
			boot,
			hasDisk || hasDisk2,
			hasFloppy || hasFloppyB,
			hasCdrom || hasCdrom2);

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
		if (hasFloppyB)
		{
			config += L"floppyb: image=\"";
			config += floppyBPath->Data();
			config += L"\", status=inserted\n";
		}
		else
		{
			config += L"floppyb: type=1_44\n";
		}

		int nextAtaSlot = 0;
		int maxAtaSlot = -1;
		if (hasDisk)
		{
			maxAtaSlot = (std::max)(maxAtaSlot, NextAtaSlot(nextAtaSlot));
		}
		if (hasDisk2)
		{
			maxAtaSlot = (std::max)(maxAtaSlot, NextAtaSlot(nextAtaSlot));
		}
		if (hasCdrom)
		{
			maxAtaSlot = (std::max)(maxAtaSlot, NextAtaSlot(nextAtaSlot));
		}
		if (hasCdrom2)
		{
			maxAtaSlot = (std::max)(maxAtaSlot, NextAtaSlot(nextAtaSlot));
		}
		if (hasSharedFolder)
		{
			maxAtaSlot = (std::max)(maxAtaSlot, NextAtaSlot(nextAtaSlot));
		}

		for (int controller = 0; controller <= maxAtaSlot / 2; ++controller)
		{
			AppendAtaController(config, controller);
		}

		nextAtaSlot = 0;
		AppendAtaDisk(config, hasDisk ? NextAtaSlot(nextAtaSlot) : -1, diskPath, diskImageMode);
		AppendAtaDisk(config, hasDisk2 ? NextAtaSlot(nextAtaSlot) : -1, disk2Path, disk2ImageMode);
		AppendAtaCdrom(config, hasCdrom ? NextAtaSlot(nextAtaSlot) : -1, cdromPath);
		AppendAtaCdrom(config, hasCdrom2 ? NextAtaSlot(nextAtaSlot) : -1, cdrom2Path);
		AppendAtaVvfat(config, hasSharedFolder ? NextAtaSlot(nextAtaSlot) : -1, sharedFolderPath);

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
		nullptr,
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
		nullptr,
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
	return CreateConfigAsync(
		diskPath,
		floppyPath,
		cdromPath,
		nullptr,
		bootTarget,
		memoryMb,
		cpuModel,
		biosPath,
		vgaBiosPath,
		soundEnabled,
		networkEnabled,
		diskImageMode);
}

task<String^> UWP_Port::BochsUwpStorage::CreateConfigAsync(
	String^ diskPath,
	String^ floppyPath,
	String^ cdromPath,
	String^ sharedFolderPath,
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
		sharedFolderPath,
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

task<String^> UWP_Port::BochsUwpStorage::CreateConfigAsync(
	String^ diskPath,
	String^ disk2Path,
	String^ floppyPath,
	String^ floppyBPath,
	String^ cdromPath,
	String^ cdrom2Path,
	String^ sharedFolderPath,
	String^ bootTarget,
	int memoryMb,
	String^ cpuModel,
	String^ biosPath,
	String^ vgaBiosPath,
	bool soundEnabled,
	String^ networkAdapter,
	String^ diskImageMode,
	String^ disk2ImageMode,
	String^ usbController,
	String^ usbDevice,
	String^ usbImagePath,
	String^ serialMode,
	bool parallelEnabled,
	String^ videoExtension,
	int videoUpdateFreq,
	bool videoRealtime,
	int vbeMemoryMb,
	String^ displayLibrary,
	int rfbTimeout,
	int networkSocketPort,
	String^ pciChipset,
	bool acpiEnabled,
	bool hpetEnabled,
	bool ioApicEnabled)
{
	StorageFolder^ localFolder = ApplicationData::Current->LocalFolder;
	String^ configText = CreateConfigText(
		diskPath,
		disk2Path,
		floppyPath,
		floppyBPath,
		cdromPath,
		cdrom2Path,
		sharedFolderPath,
		bootTarget,
		memoryMb,
		cpuModel,
		biosPath,
		vgaBiosPath,
		soundEnabled,
		networkAdapter,
		diskImageMode,
		disk2ImageMode,
		usbController,
		usbDevice,
		usbImagePath,
		serialMode,
		parallelEnabled,
		videoExtension,
		videoUpdateFreq,
		videoRealtime,
		vbeMemoryMb,
		displayLibrary,
		rfbTimeout,
		networkSocketPort,
		pciChipset,
		acpiEnabled,
		hpetEnabled,
		ioApicEnabled);
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
		nullptr,
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
	return CreateConfigText(
		diskPath,
		floppyPath,
		cdromPath,
		nullptr,
		bootTarget,
		memoryMb,
		cpuModel,
		biosPath,
		vgaBiosPath,
		soundEnabled,
		networkEnabled,
		diskImageMode);
}

String^ UWP_Port::BochsUwpStorage::CreateConfigText(
	String^ diskPath,
	String^ floppyPath,
	String^ cdromPath,
	String^ sharedFolderPath,
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
		nullptr,
		floppyPath,
		nullptr,
		cdromPath,
		nullptr,
		sharedFolderPath,
		bootTarget,
		memoryMb,
		cpuModel,
		biosPath,
		vgaBiosPath,
		soundEnabled,
		networkEnabled ? ref new String(L"ne2k") : ref new String(L"none"),
		diskImageMode,
		nullptr,
		ref new String(L"none"),
		ref new String(L"none"),
		nullptr,
		ref new String(L"disabled"),
		false,
		ref new String(L"vbe"),
		10,
		true,
		16,
		ref new String(L"uwp_dx"),
		0,
		40000,
		ref new String(L"i440fx"),
		true,
		true,
		true);
}

String^ UWP_Port::BochsUwpStorage::CreateConfigText(
	String^ diskPath,
	String^ disk2Path,
	String^ floppyPath,
	String^ floppyBPath,
	String^ cdromPath,
	String^ cdrom2Path,
	String^ sharedFolderPath,
	String^ bootTarget,
	int memoryMb,
	String^ cpuModel,
	String^ biosPath,
	String^ vgaBiosPath,
	bool soundEnabled,
	String^ networkAdapter,
	String^ diskImageMode,
	String^ disk2ImageMode,
	String^ usbController,
	String^ usbDevice,
	String^ usbImagePath,
	String^ serialMode,
	bool parallelEnabled,
	String^ videoExtension,
	int videoUpdateFreq,
	bool videoRealtime,
	int vbeMemoryMb,
	String^ displayLibrary,
	int rfbTimeout,
	int networkSocketPort,
	String^ pciChipset,
	bool acpiEnabled,
	bool hpetEnabled,
	bool ioApicEnabled)
{
	return DefaultBochsrcText(
		diskPath,
		disk2Path,
		floppyPath,
		floppyBPath,
		cdromPath,
		cdrom2Path,
		sharedFolderPath,
		bootTarget,
		memoryMb,
		cpuModel,
		biosPath,
		vgaBiosPath,
		soundEnabled,
		networkAdapter,
		diskImageMode,
		disk2ImageMode,
		usbController,
		usbDevice,
		usbImagePath,
		serialMode,
		parallelEnabled,
		videoExtension,
		videoUpdateFreq,
		videoRealtime,
		vbeMemoryMb,
		displayLibrary,
		rfbTimeout,
		networkSocketPort,
		pciChipset,
		acpiEnabled,
		hpetEnabled,
		ioApicEnabled);
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

		if (CanOpenFileWithWin32Apis(pickedFile->Path))
		{
			return task_from_result(pickedFile->Path);
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

task<String^> UWP_Port::BochsUwpStorage::PickSharedFolderAsync()
{
	FolderPicker^ picker = ref new FolderPicker();
	picker->ViewMode = PickerViewMode::List;
	picker->SuggestedStartLocation = PickerLocationId::ComputerFolder;
	picker->FileTypeFilter->Append(ref new String(L"*"));

	return create_task(picker->PickSingleFolderAsync()).then([](StorageFolder^ pickedFolder)
	{
		if (pickedFolder == nullptr)
		{
			return task_from_result<String^>(nullptr);
		}

		if (pickedFolder->Path == nullptr || pickedFolder->Path->Length() == 0)
		{
			return task_from_result<String^>(nullptr);
		}

		if (!CanEnumerateFolderWithWin32Apis(pickedFolder->Path))
		{
			throw ref new FailureException(
				ref new String(L"A pasta foi selected, mas o core do Bochs nao conseguiu acessa-la pelo caminho real. Habilite o acesso a arquivos para o app nas configuracoes de privacidade do Windows e tente novamente."));
		}

		try
		{
			StorageApplicationPermissions::FutureAccessList->AddOrReplace(
				ref new String(L"BochsSharedFolder"),
				pickedFolder);
		}
		catch (...)
		{
		}

		return task_from_result(pickedFolder->Path);
	});
}

String^ UWP_Port::BochsUwpStorage::DetectDiskImageMode(String^ diskPath)
{
	std::wstring path = diskPath != nullptr ? std::wstring(diskPath->Data()) : std::wstring();
	std::wstring mode = DetectDiskImageModeFromPath(path);
	return ref new String(mode.c_str());
}

int UWP_Port::BochsUwpStorage::NormalizeGuestMemoryMb(int memoryMb)
{
	if (memoryMb < MinGuestMemoryMb)
	{
		return DefaultGuestMemoryMb;
	}
	if (memoryMb > MaxGuestMemoryMb)
	{
		return MaxGuestMemoryMb;
	}
	return memoryMb;
}

int UWP_Port::BochsUwpStorage::EffectiveHostMemoryMb(int memoryMb)
{
	int guestMemory = NormalizeGuestMemoryMb(memoryMb);
	return (std::min)(guestMemory, MaxResidentHostMemoryMb);
}

int UWP_Port::BochsUwpStorage::MemoryBlockSizeKb()
{
	return UwpMemoryBlockSizeKb;
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
