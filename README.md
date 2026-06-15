# mm_converter

An automated tool to convert mod song packs for *Hatsune Miku: Project DIVA Mega Mix+* to data folders for MicroMix+.


## Installation & Use

1. Ensure [FFmpeg](https://ffmpeg.org/) is installed and added to your system `PATH`.
2. Run `mm_converter.exe`.
3.
   - Select your "Mods" source folder and "Output" folder (the tool will attempt to auto-detect these).
   - Select the songs you wish to convert from the list.
   - Click **"Convert selected"** to start the process. 
   - *Note*: If the "Skip video" option is unchecked, the process will take significantly longer. This is because the videos need to be re-encoded from VP9 to h.264 to work in MicroMix+.