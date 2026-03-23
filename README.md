# RetroAndroidShell

Remote shell support for Retroid and Ayaneo handhelds

## Background

Both Retroid and Ayaneo support running a script as root. This is very useful but not as convienant as having a shell and FTP server with root access without the need to flash new bootimages, etc with Magisk

## Installation

Download the latest version from []()

These 2 files need to be placed on your device at /sdcard

This folder will be the root of your accessible internal storage

## Running

On Retroid Devices you need to run the script via: Settings -> Handheld Settings -> Advanced -> Run script as root

On Ayaneo Device you need to run the script via: AyaSettings -> Device -> Root Script

## Usage

##### On Windows

* Use [Putty](https://www.chiark.greenend.org.uk/~sgtatham/putty/latest.html) to connect

* Add your devices IP Address (use an app like What is my IP address on your device if you don't know)

* Set the port to 4444

* Set Connection type: Other -> Raw

* Select Terminal settings on the left and Force off Local echo and Local line editing

##### On Termux/Linux

* Install netcat and rlwrap (in termux: `pkg i nmap rlwrap`)

* rlwrap nc localhost 4444
  
  or

* install socat (in termux: pkg i socat)

* socat -,raw,echo=0 TCP:localhost:4444

##### FTP

For FTP you can use [FileZilla](https://filezilla-project.org/download.php) or any FTP client and connect FTP to the IP address as user `anonymous` (password can be blank) for full access.
