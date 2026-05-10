# UWP DirectX Bochs Port

This document describes the current state of the Bochs UWP/XAML/DirectX port.
The goal of the project is to keep the native Bochs core intact and concentrate
UWP-specific code in the `UWP-Port` project.

## Projects

- `UWP-Port/UWP-Port.vcxproj`: C++/CX UWP application, XAML screen, DirectX
  render loop, input, XAudio2/AudioGraph/MIDI audio, image selection and
  application lifecycle.
- `bochs_core_uwp/bochs_core_uwp.vcxproj`: static library that packages the
  executable Bochs core without exposing the desktop `main()`.
- `vs2019/*`: Bochs static libraries used by the UWP core. The projects are
  referenced with `UseUwpCoreRuntime=true`, which defines
  `BX_UWP_CORE_LIBRARY=1` and `BX_WITH_UWP_DX=1`.

Validated build:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' UWP-Port.vcxproj /p:Configuration=Debug /p:Platform=x64 /m /v:minimal
```

Release build from `cmd.exe`:

```cmd
cd /d C:\bochs\UWP-Port
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" UWP-Port.vcxproj /p:Configuration=Release /p:Platform=x64 /m /v:minimal
```

The Debug x64 package is generated in `UWP-Port\AppPackages` or in
`UWP-Port\x64\Debug\UWP-Port`, depending on the packaging mode produced by
MSBuild.

## Boundaries

- `gui/uwp_dx.cc` implements the Bochs `bx_gui_c` backend.
- `gui/uwp_dx_bridge.h` defines the C ABI between the Bochs core and the UWP
  host.
- `UWP-Port/BochsUwpBridge.*` stores the framebuffer, keyboard/mouse events,
  focus state, mouse capture state and shutdown requests.
- `UWP-Port/BochsRuntime.*` creates the emulation thread, calls
  `bochs_core_uwp_run()`, pauses, resumes, saves state and requests shutdown.
- `UWP-Port/UWP_PortMain.*` connects the XAML page to the runtime, renderer,
  input and audio layers.
- `UWP-Port/Content/BochsFrameRenderer.*` sends the Bochs BGRA framebuffer to a
  dynamic `ID3D11Texture2D` and draws it on the `SwapChainPanel`.
- `UWP-Port/BochsUwpStorage.*` selects images, generates
  `bochsrc.generated.txt` and manages the save-state directory.
- `UWP-Port/BochsUwpFileBridge.*` exposes brokered `uwp://...` files to the core
  through `IRandomAccessStream`, with open, read, write, seek, flush, resize and
  size-query operations.
- `UWP-Port/BochsUwpAudio.*` implements PCM output through XAudio2, PCM input
  through AudioGraph and MIDI output through `MidiSynthesizer`.

## Rendering

The UWP backend keeps an intermediate BGRA8 framebuffer on the CPU side. The UWP
renderer presents this framebuffer as `DXGI_FORMAT_B8G8R8A8_UNORM`.

- SVGA modes that write 32-bit pixels copy tiles directly into BGRA8.
- Indexed graphics modes keep an 8-bit shadow buffer and apply the Bochs palette
  before publishing.
- Text mode uses the common Bochs text path; glyphs are rasterized on the CPU
  and sent to the same BGRA8 framebuffer.
- `graphics_tile_update()` updates framebuffer regions.
- `graphics_tile_update_in_place()` marks the framebuffer as dirty.
- `flush()` publishes the frame to `BochsUwpBridge`.
- `BochsFrameRenderer` maps a dynamic D3D texture, copies the frame and draws a
  full-screen quad.

## Keyboard and Mouse

UWP input is received on the UI thread and consumed by the emulation thread in
`gui/uwp_dx.cc`.

Keyboard:

- `CoreWindow::KeyDown` and `KeyUp` are converted to normalized scan codes or
  virtual keys.
- The backend maps common keys, numeric keypad, left/right modifiers,
  Windows/Menu, Print Screen, Pause/Break, F1-F12 and several extended power,
  media and navigation keys.
- `bx_gui_c::key_event()` forwards directly to `DEV_kbd_gen_scancode()`.
- On focus loss, `release_keyboard_state()` clears modifiers and toggles.

Mouse:

- The port supports absolute and relative coordinates.
- When the guest requests absolute mode, the `SwapChainPanel` position is scaled
  into the Bochs coordinate range.
- When the guest uses relative/captured mode, `MouseDevice::MouseMoved` provides
  physical deltas and the system cursor is hidden.
- `Ctrl+Alt+M` explicitly toggles relative mouse capture: it releases the mouse
  when it is captured and requests capture again when the user wants to continue.
- Left, right, middle, XButton1 and XButton2 are included in the button mask.
- The wheel is sent through the event `z` axis.
- Capture toggles follow the Bochs code: keyboard and button combinations go
  through `mouse_toggle_check()` / `toggle_mouse_enable()`.

## Storage and Bochsrc

The configuration file is generated at
`ApplicationData::Current->LocalFolder\bochsrc.generated.txt`.

The current generator includes:

- `display_library: uwp_dx`
- memory, CPU, BIOS and VGABIOS
- `clock: sync=both, time0=local, rtc_sync=1` so Bochs' native realtime and
  slowdown timers keep guest seconds aligned with host time
- `pci: enabled=1, chipset=i440fx`
- `plugin_ctrl` for audio, network and optional devices
- `sound`, `speaker`, `sb16`, `es1370`
- `ne2k` through slirp when networking is enabled
- `floppya` when a floppy image is selected
- `ata0` and `ata0-master` for HDD, with `mode=` inferred or selected in the UI
- `ata0-slave` for ISO/CD-ROM
- `boot` with up to three ordered devices
- `log: -`

UWP file layer:

- Files picked by the user can be registered in
  `StorageApplicationPermissions::FutureAccessList` and represented in
  `bochsrc` as `uwp://token/name`.
- `BochsUwpFileBridge` resolves `uwp://...`, opens the `StorageFile` as an
  `IRandomAccessStream` and exposes synchronous open, close, seek, read, write,
  flush, set-size and get-size operations to the core.
- The bridge honors read-only and read-write opens. If a floppy cannot be opened
  read-write, the controller retries read-only and marks the media write
  protected.
- The `uwp_image_t` backend uses this layer for raw HDD images in `uwp` mode.
- The floppy controller also uses this layer for brokered raw floppy images.
- The parser for `floppya: image="uwp://..."` also uses the UWP layer to query
  the real image size and detect the correct geometry before boot.
- The Win32 CD-ROM backend used by the UWP build uses the same layer for
  brokered ISO/CDR files.

Files and image modes:

- Default BIOS and VGABIOS files are packaged in `Assets`.
- Raw HDD files (`.img`, `.ima`, `.flp`, `.dsk`, `.fd`, `.vfd`, `.bin`, `.raw`)
  use `FutureAccessList` and the `uwp://...` URI when possible, opening through
  `IRandomAccessStream` in the core with `uwp` mode.
- The HDD picker accepts `.img`, `.raw`, `.bin`, `.dsk`, `.hdd`, `.vhd`, `.vpc`,
  `.vmdk` and `.vdi`.
- The image mode can stay on `auto` or be chosen manually in the UI: `flat`,
  `vpc`, `vbox`, `vmware4`, `vmware3`, `sparse`, `growing`, `undoable` or
  `volatile`.
- Automatic inference uses `vmware4` for `.vmdk`, `vbox` for `.vdi`, `vpc` for
  `.vhd`/`.vpc`, `uwp` for `uwp://...` and `flat` for common raw images.
- ISO/CD-ROM accepts `.iso`, `.cdr` and `.toast`, and now uses `uwp://...`
  directly when the picked file can be registered in the `FutureAccessList`.
- Floppy accepts `.img`, `.ima`, `.flp`, `.dsk`, `.fd`, `.vfd`, `.bin` and
  `.raw`, also with a direct `uwp://...` path for brokered raw images.
- For brokered floppy images, the core recognizes standard sizes such as 160K,
  180K, 320K, 360K, 720K, 1.2M, 1.44M, 1.68/1.72/1.8M and 2.88M. Non-standard
  sizes are still rejected as unknown floppy images.
- If brokered registration fails, the picker still falls back to copying into
  `LocalFolder` for compatibility.

## Boot

Bochs accepts a `boot:` sequence with up to three entries and no duplicates. The
first device must be BIOS-bootable (`floppy`, `disk` or `cdrom`). UWP now exposes
three selectors:

- `HDD` -> `disk`
- `ISO` -> `cdrom`
- `Floppy` -> `floppy`
- `None` for the second/third position

The generator also normalizes internal aliases such as `hd`, `hdd`, `iso`, `cd`,
`a` and `c`, removes duplicates and ignores devices without selected media. If
nothing is selected, the fallback is `boot: disk`.

When ISO/CD-ROM media exists but no HDD exists, the generator mounts the optical
drive as `ata0-master`. When HDD and ISO are both present, the HDD stays on
`ata0-master` and the ISO is mounted as `ata0-slave`.

## PCI and i440FX

The UWP core builds and uses the native Bochs PCI stack:

- `iodev/pci.cc`: i430FX/i440FX/i440BX host bridge.
- `iodev/pci2isa.cc`: PIIX/PIIX3/PIIX4 PCI-to-ISA bridge.
- `iodev/pci_ide.cc`: PIIX PCI IDE controller.

The generated `bochsrc` explicitly selects:

```text
pci: enabled=1, chipset=i440fx
```

When network or PCI audio is active, the generator also reserves slots:

- `slotN=ne2k` for the NE2K PCI/slirp adapter.
- `slotN=es1370` for ES1370 PCI audio.

PIIX3/PCI IDE is loaded by Bochs' own initialization flow when PCI and ATA are
enabled.

## Audio, Input and MIDI

Audio has been reintegrated using the Bochs sound stack:

- `BX_SUPPORT_SOUNDLOW`, `BX_SUPPORT_SB16` and `BX_SUPPORT_ES1370` are active in
  the UWP core.
- `iodev/sound/sounduwp.cc` registers the `uwp` low-level driver.
- `sounduwp` implements real `waveout`, `wavein` and `midiout` objects.
- `UWP-Port/BochsUwpAudio.cpp` opens XAudio2, creates mastering/source voices
  and submits output PCM buffers.
- PCM input uses `AudioGraph`, `AudioDeviceInputNode` and
  `AudioFrameOutputNode` to capture from the microphone and feed SB16/ES1370.
- MIDI output uses `Windows::Devices::Midi::MidiSynthesizer` and translates
  Bochs MIDI commands to UWP messages, including channel events, SysEx and
  common system messages.
- The manifest declares `DeviceCapability` for `microphone` and `midi`.
- The generated bochsrc uses:

```text
sound: waveoutdrv=uwp, waveindrv=uwp, midioutdrv=uwp
speaker: enabled=1, mode=sound, volume=15
sb16: enabled=1, wavemode=1, midimode=1
es1370: enabled=1, wavemode=1, midimode=1
```

Wave output plays through XAudio2. Wave input and MIDI use the native UWP
low-level driver implementations. If the device or microphone/MIDI permission is
not available, the backend fails safely: capture returns silence and emulation
continues.

## Networking

The network toggle generates `plugin_ctrl: ne2k=1` and:

```text
ne2k: ioaddr=0x300, irq=10, mac=52:54:00:12:34:56, ethmod=slirp, script=""
```

The app links `ws2_32.lib`, and the UWP core uses the existing network projects
with slirp mode already integrated into the build.

## Lifecycle, Save State and Diagnostics

The UWP lifecycle goes through `BochsRuntime`:

- Start creates the core thread and starts the render loop.
- Pause calls `bochs_core_uwp_pause()` and waits for the core to enter pause.
- Resume calls `bochs_core_uwp_resume()`.
- Save state pauses emulation and saves into the selected slot.
- The app exposes three save-state slots. Slot 1 preserves the historical path
  `ApplicationData::Current->LocalFolder\BochsSaveState`; slots 2 and 3 use
  `BochsSaveState2` and `BochsSaveState3`.
- Saving into an occupied slot asks for overwrite confirmation.
- Restoring while emulation is active asks for confirmation before shutting down
  the current session.
- Restore starts the core with the restore path (`-r` at the native boundary).
- Shutdown requests shutdown from the core, bridge and audio layer.
- Suspend/resize/DPI/orientation handle D3D resources and render-loop state.

The screen checks for `config` inside the selected slot and enables restore when
a saved state exists. The problems panel also shows event count, session log,
diagnostic summary, selected media, effective HDD mode, boot order, current
save-state, sound and network state.

## Current Limitations

- The `uwp://...` path covers raw HDD, raw floppy and ISO/CDR/TOAST access
  through `IRandomAccessStream`. Formats that depend on multiple auxiliary
  files, such as some VMDK layouts, may still require copying to `LocalFolder`
  or keeping related files accessible together through the original format
  backend.
- Floppy through `uwp://...` depends on the image size to detect the type. An
  image with a non-standard size will not be mounted as a bootable floppy.
- VHD/VDI/VMDK images still use the corresponding Bochs image backends when
  copied or located through a traditional file path; brokered `uwp` mode is
  intended for raw images.
- The `LocalFolder` fallback is still used when `FutureAccessList` does not
  accept the file or when the format backend requires normal path-based access.
- Audio input depends on Windows microphone permission and the default capture
  device. If Windows denies permission, the guest receives silence.
- MIDI depends on the UWP MIDI synthesizer available on the system.
- The UI is still C++/CX/XAML in the DirectX template style. The native
  boundaries (`BochsUwpBridge`, renderer and runtime) allow a future migration
  to C++/WinRT without changing the `uwp_dx` backend.
