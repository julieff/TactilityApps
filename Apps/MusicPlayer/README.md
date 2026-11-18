# Music Player

Plays MP3 files from the SD card using Tactility's audio decoder and audio-stream driver.

## Features

- Browses folders and MP3 files on the SD card, starting at `/sdcard`
- Reads ID3 title, artist, album, and duration tags, with the filename as a title fallback
- Play, pause, stop, previous, and next controls; playback advances through the list
- Playback progress
- A persistent mini-player bar opens a full-screen player when tapped
- A top-right Settings button provides folder, audio output, and volume controls

## Use

Browse into a folder and tap an MP3 file to play it. Use the mini-player bar at the bottom to pause/resume or skip forward; tap elsewhere on that bar to open the full-screen player. Open Settings from the top-right button to scan another folder or change the audio output.

MP3 decoding is provided by Tactility's ESP audio-decoder module, so this app is available on ESP32-S3, ESP32-P4, and ESP32-S3.1 targets with an audio output device.
