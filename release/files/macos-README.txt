Cosmo2ex for macOS
==================

Runs on macOS 11 or newer, on Apple Silicon and on Intel Macs (universal programs).

First start
-----------
The programs are not signed with an Apple Developer ID, so macOS blocks them when they come from the internet
("cannot be opened because the developer cannot be verified"). Allow them once - open Terminal and run this
(drag this folder into the Terminal window instead of typing its path):

    xattr -dr com.apple.quarantine /path/to/this/folder

Running
-------
Double-click CosmosEx.command. It opens a Terminal window, starts ce_cores and webserver in it, and shows the output
of both. Open the web interface on the address it prints (https://localhost:8080 on this Mac).

Press Ctrl+C or close the Terminal window to stop Cosmo2ex. If ce_cores crashes, it's restarted after 3 seconds
(but not after 5 crashes within a minute - then Cosmo2ex stops); if the webserver stops, ce_cores is stopped too.
macOS may ask to allow incoming network connections - the Cosmo2ex device connects to ce_cores over the network,
and the web interface can be opened from other computers.

Start at login: System Settings > General > Login Items > + > select CosmosEx.command.

Files
-----
This folder can be anywhere, e.g. in Applications. Settings, logs and runtime data are in your own folder,
~/Library/Application Support/cosmo2ex - set in .env, which is next to ce_run (Finder hides files starting with
a dot - press Cmd+Shift+. to show them).

Upgrading: stop Cosmo2ex, replace this folder with the new version (copy your .env over, if you changed it) and
run the xattr command again. Settings in ~/Library/Application Support/cosmo2ex are kept.

Licenses
--------
Cosmo2ex is open design, non-commercial license (PolyForm Noncommercial 1.0.0): free to use, change and share for
non-commercial use, not to sell. See the licenses folder.
