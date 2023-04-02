VoodooHDA
========

### Compilation.
If you are using systems up to Catalina then open project 
tranc/VoodooHDA.xcodeproj

For systems BigSur and up you have to download the project VoodooHDA
also download MacKernelSDK
~~~~
git clone https://github.com/CloverHackyColor/VoodooHDA.git
cd $HOME/VoodooHDA/tranc
git clone https://github.com/joevt/MacKernelSDK.git
xcodebuild -project $HOME/VoodooHDA/tranc/VoodooHDA_BS.xcodeproj -alltargets -configuration Release

~~~~

#### View Compile Pics: ⬇︎
<details> 
  <summary>View Compile Pics</summary>

![Screenshot 1](https://user-images.githubusercontent.com/6248794/229353522-34a9abfd-ec8e-49ff-93c5-9d12746777b6.png)

![Screenshot 2](https://user-images.githubusercontent.com/6248794/229353531-10a54ffb-9d5c-487e-8122-8a598ff78f52.png)



</details>

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
