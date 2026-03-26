This directory holds the built virtual mouse driver files for packaging.

Required files (from the WDK build output):
- ZakoVirtualMouse.dll  - The UMDF 2.x HID minidriver
- ZakoVirtualMouse.inf  - Driver installation information
- ZakoVirtualMouse.cer  - Test signing certificate
- ZakoVirtualMouse.cat  - Driver catalog (optional, for production signing)

Build the driver:
  Open drivers/virtual_mouse/ZakoVirtualMouse.sln in Visual Studio
  Build Release|x64
  Copy output from drivers/virtual_mouse/x64/Release/ to this directory
