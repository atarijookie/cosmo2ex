Cosmo2ex for Windows
====================

First start
-----------
The programs are not signed with a code signing certificate, so on the first start of a downloaded copy Windows may
show "Windows protected your PC" (SmartScreen). Click "More info", then "Run anyway" - it's asked only once.
Before unzipping you can also right-click the zip > Properties > tick "Unblock" > OK, then Windows doesn't ask at all.

Running
-------
Double-click ce_run.exe. It starts ce_cores.exe and webserver.exe, and shows the output of both in its window.
Open the web interface on the address it prints (https://localhost:8080 on this computer).

Close the window or press Ctrl+C in it to stop Cosmo2ex. If ce_cores crashes, it's restarted after 3 seconds (but
not after 5 crashes within a minute - then Cosmo2ex stops); if the webserver stops, ce_cores is stopped too.
Windows Firewall may ask to allow ce_cores.exe and webserver.exe on the network - the Cosmo2ex device connects to
ce_cores over the network, and the web interface can be opened from other computers.

Start with Windows: press Win+R, type  shell:startup  and put a shortcut to ce_run.exe into the folder that opens.

Files
-----
This folder can be anywhere, e.g. C:\Cosmo2ex. Settings, logs and runtime data are in your own folder,
%LOCALAPPDATA%\cosmo2ex - set in .env, which is next to ce_run.exe.

Upgrading: stop Cosmo2ex, replace this folder with the new version (copy your .env over, if you changed it).
Settings in %LOCALAPPDATA%\cosmo2ex are kept.

Licenses
--------
Cosmo2ex is open design, non-commercial license (PolyForm Noncommercial 1.0.0): free to use, change and share for
non-commercial use, not to sell. See the licenses folder - Cosmo2ex license, and the Cygwin library (cygwin1.dll,
LGPLv3) with its source code.
