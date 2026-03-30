# RetroAndroidShell

Remote shell and temporary root support for Retroid and Ayaneo handhelds

## Background

Both Retroid and Ayaneo support running a script as root. This is very useful but not as convienant as having apps be able to use root, a rooted shell, rooted adb and FTP server with root access without the need to flash new bootimages, etc with Magisk

## Installation

Download the latest version from [Releases](https://github.com/nckstwrt/RetroAndroidShell/releases)

These files need to be placed on your device at /sdcard

This folder will be the root of your accessible internal storage

With ADB it would be `adb push enable_root.sh /sdcard`(for all 3 files)

With the device plugged in on Windows it would be copying to the root of "internal shared storage"

## Running

On **Retroid** Devices you need to run the script via: **Settings -> Handheld Settings -> Advanced -> Run script as root**

On **Ayaneo** Devices you need to run the script via: **AyaSettings -> Device -> Root Script**

Then run the script `enable_root.sh` it will unpack `root_install.tar.gz` to /data/local/tmp and then remove `root_install.tar.gz`. The rooted socat shell (port 4444) and FTP (port 21) will then be running. Any apps required root should now be able to use root.

Restarting the device will stop socat and bftpd running and disable root. As will running `disable_root.sh` as a root script.

## Usage

### On Windows

* Use [Putty](https://www.chiark.greenend.org.uk/~sgtatham/putty/latest.html) to connect

* Add your devices IP Address (use an app like What is my IP address on your device if you don't know)

* Set the port to 4444

* Set Connection type: Other -> Raw

* Select Terminal settings on the left and Force off Local echo and Local line editing

### On Termux/Linux

* Install netcat and rlwrap (in termux: `pkg i nmap rlwrap`)

* rlwrap nc localhost 4444
  
  or

* install socat (in termux: `pkg i socat`)

* socat -,raw,echo=0 TCP:localhost:4444

(replace localhost with the IP address of your device)

### FTP

For FTP you can use [FileZilla](https://filezilla-project.org/download.php) or any FTP client and connect FTP to the IP address as user `anonymous` (password can be blank) for full access.

### ADB

It now also restarts the ADB daemon to be in root mode and available over wireless. So you can:

* adb connect <ip address>

* adb shell

And you will have root via ADB
