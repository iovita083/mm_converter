# mm_converter

An automated tool to convert mod song packs for *Hatsune Miku: Project DIVA Mega Mix+* to data folders for MicroMix+. Currently not finished although it is working properly.


## Installation & Use

1. Ensure [FFmpeg](https://ffmpeg.org/) is installed and added to your system `PATH`.
2. Download [UsmToolkit](https://github.com/Rikux3/UsmToolkit). Place the `mm_converter.exe` in the same directory as `UsmToolkit` (or ensure `UsmToolkit` is in your system `PATH`).
3. Run `mm_converter.exe`.
4.
   - Select your "Mods" source folder and "Output" folder (the tool will attempt to auto-detect these).
   - Select the songs you wish to convert from the list.
   - Click **"Convert selected"** to start the process. 
   - *Note*: If the "Skip video" option is unchecked, the process will take significantly longer. This is because the videos need to be re-encoded from VP9 to h.264