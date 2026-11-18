# Music Player

Plays MP3 files from the SD card using Tactility's audio decoder and audio-stream driver.

## Features

- Lists MP3 files in a configurable SD-card folder (defaults to `/sdcard/Music`)
- Reads ID3 title, artist, album, and duration tags, with the filename as a title fallback
- Play, pause, stop, previous, and next controls; playback advances through the list
- Playback progress
- A top-right Settings button provides folder, audio output, and volume controls

## Use

Copy MP3 files into `/sdcard/Music` or another folder on the SD card. Open the top-right Settings button, enter the folder, and tap **Scan** to refresh the list. Select a track to begin playback.

MP3 decoding is provided by Tactility's ESP audio-decoder module, so this app is available on ESP32-S3, ESP32-P4, and ESP32-S3.1 targets with an audio output device.
