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
tranc/VoodooHDA_BS.xcodeproj
and compile

download VoodooHDA.prefPane
1. go to the Release302 of my Release
2. download VoodooHDA.prefPane.zip
3. after downloading, extract it

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
sudo cp -R /path_to/VoodooHDA.prefPane /Library/PreferencePanes/
~~~~

5. Wait while the system saids that the kext must be approved
6. Go to System Settings and approve the kext.
7. Reboot.
8. Enjoy your favorite music.

### Fix for Mic is showing but not getting any input (AMD/Intel)

1. Go to the System Settings>VoodooHDA.
2. Then select your microphone.
3. Set the **IMix** & **Speaker** slider to maximum.
Mic should work.
