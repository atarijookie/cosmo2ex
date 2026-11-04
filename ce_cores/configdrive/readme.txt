Welcome to the Cosmo2ex config drive!

Here you'll find all the tools you need 
to use the advanced features of your 
Cosmo2ex (meaning: features besides 
basic HDD functionality).

Please make sure you have read the 
manual (http://joo.kie.sk/?page_id=415). 

For a Quickstart guide, see 
http://joo.kie.sk/?page_id=704. 

Find more in-depth info in DrCoolZics 
docs at 
http://joo.kie.sk/cosmosex/download/
CosmosEx_Users_Guide.pdf

Also please visit your Cosmo2ex Web-
interface on it's configured network 
address (see your DHCP server for that 
or configure a static IP) for more 
confortable configuration and more 
infos.

This drive contains the following tools:


CE_BROWS.PRG (in the root of this drive)
------------
Web browser. The pages are drawn by the
host (ce_browser, set BROWSER_BIN in
.env) and sent as pictures, so any site
works, even on a 1 MB ST. Runs in ST
low, mid and high, and TT low, mid and
high - F2 switches between the modes
your monitor can show. F1 lists the
keys.


CE_VIDEO.TTP, CE_AUDIO.TTP (in the root)
--------------------------
Video and audio players. Give them a
file on a translated drive (drag it on
the program, or type its path) - the
host decodes it with ffmpeg (ce_media,
see MEDIA_* in .env), the ST only shows
and plays. Full screen in ST low (or ST
high on a mono monitor), on the TT too
(a TT high monitor: half size in the
middle). Sound: DMA sound on STE / TT /
Falcon, YM chip on ST.
Options: -l<ms> buffer (default 1000),
-f<fps> max frame rate, -s<sec> start,
-n no sound, -h sound at half rate.
Keys: Esc quit, Space
pause, Left / Right 10 s, Up / Down
1 min.

CE_DOOM.PRG (in the root)
-----------
Doom - it runs on the host, the ST
shows it full screen and sends the
keys. Arrows move, Control fires,
Space opens, Shift runs, Alternate +
arrows strafe, 1..7 weapons, Tab map,
Esc menu (quit Doom there), Undo quits
at once. Freedoom is the free game it
plays - or your own DOOM.WAD.
Sound effects (no music): DMA sound,
on a plain ST the YM. -h halves the
sound rate (less CPU on a ST), -n no
sound.


These programs need programs on the
host: ffmpeg (players), Chrome
(browser), Freedoom (Doom) - run
ce_setup_tools in the Cosmo2ex folder
on the host. Until then this drive
doesn't show them.


Folder DRIVERS\
---------------
(only needed for advanced Cosmo2ex 
 functionality, NOT for basic HDD usage)

ce_dd.prg - You only need this in 
            your AUTO folder if you 
            boot from another 
            harddrive (e.g. RAW disk 
            image). By default 
            Cosmo2ex boots a builtin 
            version of this - so 
            normally you don't need 
            to touch it. CE_DD 
            (builtin and standalone)
            handles the more advanced
            features of your Cosmo2ex
            like accessing thumb drives,
            screencast / screenshots
            and keeping the current
            date - it's kind of a
            supercharger for TOS.
            Neither version is needed
            if you only want to use
            RAW disk images and
            none of the other features.

            Screencast and screenshots:
            with CE_DD running, open
            the ScreenCast page in the
            web interface and click
            "Start Screencast" to see
            your ST / TT screen live in
            the browser, or "Take
            Screenshot" for a single
            screen, saved as PNG. No
            extra program is needed on
            the ST - CE_DD sends the
            screen only while the page
            asks for it.


ce_sting.prg - Highly experimental STiNG
               replacement for net-
               working. Not needed for
               mounting networked drives
               (see CE_DD.PRG for that).



Folder TESTS\ 
-------------
(Test tools for debugging issues that 
 may arise)

ce_tests.tos - All the test tools in one
               app, pick the test from
               its menu:
               - ACSI/SCSI communication
                 (Stresstests, R/W
                 checksums etc.)
               - Floppy Disk Emulation
               - File System (mainly
                 Translated Drives)
               - IKBD and a few more
                 ACSI/SCSI test tools
