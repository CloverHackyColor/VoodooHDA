

VoodooHDA
========

VoodooHDA is an open source audio driver for macOS based on the FreeBSD snd_hda driver.
A replacement for AppleHDA.kext on Hackintosh machines — analog and HDMI audio.

### Compilation.
If you are using systems up to Catalina then open project 
tranc/VoodooHDA.xcodeproj

For systems BigSur and up you have to download the project VoodooHDA
also download MacKernelSDK

    git clone https://github.com/joevt/MacKernelSDK.git

(this is tested version)
and make a symlink into project

    cd /path_to/VoodooHDA/
    ln -s /path_to/MacKernelSDK MacKernelSDK

open project
tranc/VoodooHDA_BS.xcodeproj
and compile

download VoodooHDA.prefPane
1. go to the Release302 of my Release
2. download VoodooHDA.prefPane.zip
3. after downloading, extract it

---

## NEW in VoodooHDA  — Bootloader injection without any workaround

Apple's complete IOAudioFamily source code (v740.1) is now compiled directly INTO
VoodooHDA.kext (see the IOAudioFamily/ folder — APSL 2.0 license notice included).

The original VoodooHDA depended on the external IOAudioFamily.kext — which is NOT in
the boot kernel collection since macOS Big Sur (11). That is why bootloader injection
failed since 2020 with:

    OC, Clover: Prelinked injection VoodooHDA.kext () - Invalid Parameter

With the family embedded, VoodooHDA's only dependencies are kernel KPIs + IOPCIFamily
(which always exist at boot) — exactly like AppleALC.

Result: ONE kext, injected directly from the bootloader, on macOS 11 to 26.

### What changed vs the original

- Added: IOAudioFamily/ — Apple's IOAudioFamily 740.1 sources compiled into the target
- Changed: OSBundleLibraries reduced to IOPCIFamily + kernel KPIs
  (IOAudioFamily and IOGraphicsFamily dependencies removed)
- Changed: VoodooHDA personality IOProbeScore = 99000 (deterministic HDEF claim)
- Fixed: 10+ family source files — compat macros restored
  (IOMallocType / IOFreeType / require — removed from modern SDK headers)
- Removed: IOGraphicsFamily dependency (framebuffer notifier call neutralized —
  HDMI pipe auto-activation feature is NOT in 4.0)

### V-3.6.9x — Installation (OpenCore / Clover)

1. Exclude other Audio kexts (AppleALC, etc.)
2. Copy VoodooHDA.kext to EFI/OC/Kexts/ (or EFI/CLOVER/kexts/Other/)
3. OpenCore: config.plist -> Kernel -> Add -> one entry:

       BundlePath:     VoodooHDA.kext
       PlistPath:      Contents/Info.plist
       ExecutablePath: Contents/MacOS/VoodooHDA
       Enabled:        YES

4. No other steps. No SIP changes — works with SIP fully enabled
   (verified: macOS Tahoe 26.7.1, OpenCore 1.0.7, csr-active-config = 00000000).
5. Reboot.


### V-3.6.9x — Known limits

- Audio output still depends on codec support — the same per-machine lottery
  as every VoodooHDA version (some codecs work, some don't)
- HDMI pipe auto-activation (IOGraphicsFamily feature) is not present in 4.0
- The prefPane settings apply as usual (e.g. VoodooHDAEnableVolumeChangeFix)

---

## Installation (classic — /Library/Extensions, all macOS versions)

1. Exclude other Audio kexts
2. Set SIP disable kext or just 

    sudo nvram csr-active-config=0xA85

3. Reboot
4.

    sudo cp -R /path_to/VoodooHDA.kext /Library/Extensions/
    sudo cp -R /path_to/VoodooHDA.prefPane /Library/PreferencePanes/

5. Wait while the system saids that the kext must be approved
6. Go to System Settings and approve the kext.
7. Reboot.
8. Enjoy your favorite music.

---

## Which method to choose?

| Method | macOS | Steps | SIP |
|---|---|---|---|
| V-3.6.9x — Bootloader injection | 11 - 26 | Copy kext + one OC entry | Full SIP works |
| Classic — /Library/Extensions | all | Copy + approve | Lowered (0xA85) |

---

### Fix for Mic is showing but not getting any input (AMD/Intel)

1. Go to the System Settings>VoodooHDA.
2. Then select your microphone.
3. Set the IMix & Speaker slider to maximum.
Mic should work.

## Credits

- Slice, Zenith432, AutumnRain — developer
- chris1111 — Bootloader injection
