VoodooHDA
========

### Compilation.
If you are using systems up to Catalina then open project 
tranc/VoodooHDA.xcodeproj

For systems BigSur and up you have to download the project VoodooHDA
also download MacKernelSDK
~~~~
git clone https://github.com/joevt/MacKernelSDK.git
~~~~

(this is tested version)
and make a symlink into project
~~~~
cd /path_to/VoodooHDA/
ln -s /path_to/MacKernelSDK MacKernelSDK
~~~~

open project
tranc/VoodooHDA-BS.xcodeproj
and compile

### Installation

1. Exclude other Audio kexts
2. Set SIP disable kext or just 
~~~~
sudo nvram csr-active-config=0xA85
~~~~

3. Reboot
4.
~~~~
sudo cp -R /path_to/VoodooHDA.kext /Library/Extensions/
~~~~

5. Wait while the system saids that the kext must be approved
6. Go to System Settings and approve the kext.
7. Reboot.
8. Enjoy your favorite music.
