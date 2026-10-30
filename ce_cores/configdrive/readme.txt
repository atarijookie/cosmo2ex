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
