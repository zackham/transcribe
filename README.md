# Voice Transcribe

A lightweight, non-intrusive voice transcription tool for Linux that uses OpenAI's Whisper API. Records audio from your microphone, transcribes it using AI, and copies the result to your clipboard - all with a single keyboard shortcut.

## Features

- **Instant Recording**: Press your hotkey to start recording immediately
- **Visual Feedback**: Non-intrusive overlay shows recording status, elapsed time, and audio levels
- **Smart Toggle**: Press the hotkey again to stop recording and transcribe
- **Clipboard Integration**: Transcribed text is automatically copied to your clipboard
- **No Focus Stealing**: The overlay window never takes focus from your current application
- **Wayland Native**: Built for modern Linux desktops (tested on Hyprland)

## Demo

The recording overlay appears in the center of your screen:
- Shows connection status ("Connecting to microphone..." → "Microphone ready" → "Recording...")
- Displays elapsed recording time
- Shows real-time audio level visualization
- Indicates processing status after recording stops
- Confirms when text is copied to clipboard

## Prerequisites

- Linux with Wayland compositor (tested on Hyprland)
- ALSA audio system
- Python 3 with GTK3 bindings
- OpenAI API key

## Dependencies

Install the required system packages:

```bash
# Arch Linux
sudo pacman -S alsa-lib curl gtk3 python-gobject cairo

# Ubuntu/Debian
sudo apt install libasound2-dev libcurl4-openssl-dev libgtk-3-0 python3-gi python3-gi-cairo gir1.2-gtk-3.0

# Fedora
sudo dnf install alsa-lib-devel libcurl-devel gtk3 python3-gobject cairo-devel
```

## Installation

1. Clone this repository:
```bash
git clone https://github.com/yourusername/voice-transcribe.git
cd voice-transcribe
```

2. Create a `.env` file with your OpenAI API key:
```bash
echo "OPENAI_API_KEY=sk-your-api-key-here" > .env
```

3. Compile the program:
```bash
gcc -o voice-transcribe voice-transcribe.c -lasound -lcurl -lm -pthread
```

4. (Optional) Install system-wide:
```bash
sudo cp voice-transcribe /usr/local/bin/
```

## Configuration

### Hyprland

Add this to your `~/.config/hypr/hyprland.conf`:

```conf
# Voice Transcription
bind = SUPER, I, exec, /path/to/voice-transcribe

# Window rules for the overlay
windowrulev2 = float, title:^(Recording)$
windowrulev2 = size 400 100, title:^(Recording)$
windowrulev2 = center, title:^(Recording)$
windowrulev2 = pin, title:^(Recording)$
windowrulev2 = noborder, title:^(Recording)$
windowrulev2 = noshadow, title:^(Recording)$
windowrulev2 = nofocus, title:^(Recording)$
windowrulev2 = noinitialfocus, title:^(Recording)$
windowrulev2 = stayfocused, title:^(Recording)$
windowrulev2 = noblur, title:^(Recording)$
windowrulev2 = forcergbx, title:^(Recording)$
windowrulev2 = animation none, title:^(Recording)$
windowrulev2 = rounding 0, title:^(Recording)$
windowrulev2 = opacity 1.0 override 1.0 override, title:^(Recording)$
```

### Other Wayland Compositors

The program should work with any Wayland compositor that supports layer-shell protocol. You may need to adjust the window rules according to your compositor's syntax.

## Usage

1. Press your configured hotkey (default: SUPER+I) to start recording
2. Speak into your microphone
3. Press the hotkey again to stop recording and transcribe
4. The transcribed text is automatically copied to your clipboard
5. Paste the text wherever you need it (CTRL+V or SUPER+V)

### Recording States

- **Connecting to microphone...** - Initializing audio device
- **Microphone ready** - Audio device connected successfully
- **Recording...** - Actively recording audio (shows elapsed time)
- **Processing...** - Recording stopped, preparing audio
- **Uploading to OpenAI...** - Sending audio for transcription
- **Copied to clipboard!** - Success! Text is in your clipboard
- **Transcription failed** - Error occurred (check your API key)
- **No audio recorded** - No audio data was captured

### Tips

- Maximum recording time is 5 minutes
- Press ESC during recording to cancel (feature depends on compositor)
- The program runs in the background and won't block your work
- Only one recording session can be active at a time

## Troubleshooting

### No audio is being recorded
- Check that your microphone is properly connected
- Verify ALSA can see your microphone: `arecord -l`
- Try recording with ALSA directly: `arecord -d 5 test.wav`

### API errors
- Verify your OpenAI API key is valid
- Check that you have API credits available
- Ensure your `.env` file is in the same directory as the executable

### Window doesn't appear or appears incorrectly
- Ensure Python GTK bindings are installed
- Check that your compositor supports the required window hints
- Try running the program from terminal to see error messages

### Program won't compile
- Install all development headers for dependencies
- On some systems you may need pkg-config: `pkg-config --cflags --libs gtk+-3.0`

## How It Works

1. **Toggle Mechanism**: Uses PID file tracking to enable start/stop with the same hotkey
2. **Audio Capture**: Records 16kHz mono audio using ALSA
3. **Background Processing**: Forks to background immediately to avoid blocking
4. **Visualization**: Spawns a Python GTK overlay for visual feedback
5. **Transcription**: Sends WAV audio to OpenAI's Whisper API
6. **Clipboard**: Uses `wl-copy` to put text in Wayland clipboard

## Privacy & Security

- Audio is only recorded when you explicitly start recording
- Audio files are temporary and deleted after transcription
- Your OpenAI API key is never logged or displayed
- No telemetry or usage tracking
- All processing happens locally except for the API call to OpenAI

## License

MIT License - See LICENSE file for details

## Contributing

Contributions are welcome! Please feel free to submit pull requests or open issues for bugs and feature requests.

## Acknowledgments

- OpenAI for the Whisper transcription API
- The Hyprland community for window management guidance
- ALSA project for reliable audio capture on Linux