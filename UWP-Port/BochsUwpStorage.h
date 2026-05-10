#pragma once

namespace UWP_Port
{
	class BochsUwpStorage
	{
	public:
		static concurrency::task<Platform::String^> EnsureDefaultConfigAsync();
		static concurrency::task<Platform::String^> CreateConfigForDiskAsync(Platform::String^ diskPath);
		static concurrency::task<Platform::String^> CreateConfigAsync(
			Platform::String^ diskPath,
			Platform::String^ biosPath,
			Platform::String^ vgaBiosPath);
		static concurrency::task<Platform::String^> CreateConfigAsync(
			Platform::String^ diskPath,
			Platform::String^ floppyPath,
			Platform::String^ cdromPath,
			Platform::String^ bootTarget,
			int memoryMb,
			Platform::String^ cpuModel,
			Platform::String^ biosPath,
			Platform::String^ vgaBiosPath);
		static concurrency::task<Platform::String^> CreateConfigAsync(
			Platform::String^ diskPath,
			Platform::String^ floppyPath,
			Platform::String^ cdromPath,
			Platform::String^ bootTarget,
			int memoryMb,
			Platform::String^ cpuModel,
			Platform::String^ biosPath,
			Platform::String^ vgaBiosPath,
			bool soundEnabled,
			bool networkEnabled);
		static concurrency::task<Platform::String^> CreateConfigAsync(
			Platform::String^ diskPath,
			Platform::String^ floppyPath,
			Platform::String^ cdromPath,
			Platform::String^ bootTarget,
			int memoryMb,
			Platform::String^ cpuModel,
			Platform::String^ biosPath,
			Platform::String^ vgaBiosPath,
			bool soundEnabled,
			bool networkEnabled,
			Platform::String^ diskImageMode);
		static concurrency::task<Platform::String^> CreateConfigAsync(
			Platform::String^ diskPath,
			Platform::String^ floppyPath,
			Platform::String^ cdromPath,
			Platform::String^ sharedFolderPath,
			Platform::String^ bootTarget,
			int memoryMb,
			Platform::String^ cpuModel,
			Platform::String^ biosPath,
			Platform::String^ vgaBiosPath,
			bool soundEnabled,
			bool networkEnabled,
			Platform::String^ diskImageMode);
		static Platform::String^ CreateConfigText(
			Platform::String^ diskPath,
			Platform::String^ floppyPath,
			Platform::String^ cdromPath,
			Platform::String^ bootTarget,
			int memoryMb,
			Platform::String^ cpuModel,
			Platform::String^ biosPath,
			Platform::String^ vgaBiosPath,
			bool soundEnabled,
			bool networkEnabled);
		static Platform::String^ CreateConfigText(
			Platform::String^ diskPath,
			Platform::String^ floppyPath,
			Platform::String^ cdromPath,
			Platform::String^ bootTarget,
			int memoryMb,
			Platform::String^ cpuModel,
			Platform::String^ biosPath,
			Platform::String^ vgaBiosPath,
			bool soundEnabled,
			bool networkEnabled,
			Platform::String^ diskImageMode);
		static Platform::String^ CreateConfigText(
			Platform::String^ diskPath,
			Platform::String^ floppyPath,
			Platform::String^ cdromPath,
			Platform::String^ sharedFolderPath,
			Platform::String^ bootTarget,
			int memoryMb,
			Platform::String^ cpuModel,
			Platform::String^ biosPath,
			Platform::String^ vgaBiosPath,
			bool soundEnabled,
			bool networkEnabled,
			Platform::String^ diskImageMode);
		static concurrency::task<Platform::String^> PickDiskImageToLocalFolderAsync();
		static concurrency::task<Platform::String^> PickFloppyImageToLocalFolderAsync();
		static concurrency::task<Platform::String^> PickCdromImageToLocalFolderAsync();
		static concurrency::task<Platform::String^> PickBiosImageToLocalFolderAsync();
		static concurrency::task<Platform::String^> PickSharedFolderAsync();
		static Platform::String^ DetectDiskImageMode(Platform::String^ diskPath);
		static int NormalizeGuestMemoryMb(int memoryMb);
		static int EffectiveHostMemoryMb(int memoryMb);
		static int MemoryBlockSizeKb();
		static Platform::String^ GetSaveStateFolderPath();
		static Platform::String^ GetSaveStateFolderPath(Platform::String^ slotId);
		static concurrency::task<bool> HasSaveStateAsync();
		static concurrency::task<bool> HasSaveStateAsync(Platform::String^ slotId);
	};
}
