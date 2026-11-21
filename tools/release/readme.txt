LEGO Junkbot for Switch 2 - @VERSION@

This zip has the game code and a generator that makes the game's asset package
from your own legally owned Junkbot files.

WHAT YOU NEED
- This zip.
- Your own junkbot.dir file or junkbot-code folder. If you do not have one, the
  generator can download the public junkbot-code tree for you.
- Internet access the first time you run the generator.
- A Windows, macOS, or Linux computer.

WINDOWS - EASY WAY
1. Unzip this folder somewhere, such as your Desktop.
2. Double-click make-assets.bat.
3. Wait until it prints "done". The first run may take several minutes while it
   installs small free tools and builds the intro renderer.
4. Copy junkbot.nro and the junkbot.otr it creates into:

     SD:/switch/junkbot/

   Create that folder if it does not exist, then open junkbot.nro.

You can also drag your junkbot.dir or junkbot-code folder onto make-assets.bat
to use your own files instead of the public code tree.

MAC / LINUX
1. Unzip this folder.
2. Open a Terminal in this folder and run:
     ./make-assets.sh
3. Or drag in your own files:
     ./make-assets.sh --dir /path/to/junkbot.dir
     ./make-assets.sh --code /path/to/junkbot-code
4. Copy junkbot.nro and junkbot.otr into SD:/switch/junkbot/ on your SD card,
   then open junkbot.nro.

If you already have junkbot.dir in SD:/switch/junkbot/, you can skip the
generator. The game will build the package on the console once, but the intro
movie will be skipped.

Please do not share junkbot.otr, extracted game files, or this game's original
art and audio. Build them from a copy you own.
