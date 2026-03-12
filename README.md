# Time Stretch for Move Everything

Real-time audio time stretching on your Ableton Move using the Bungee library. Change the tempo of any WAV file without altering its pitch.

## Features

- Real-time preview — hear the stretched audio before saving
- High quality time stretching via Bungee library
- Set source bar count and target BPM
- Overwrite original or save as new file
- Browse destination folders for organized saving

## Prerequisites

- [Move Everything](https://github.com/charlesvestal/move-everything) installed on your Ableton Move
- SSH access enabled: http://move.local/development/ssh

## Install

### Via Module Store (Recommended)

1. Launch Move Everything on your Move
2. Select **Module Store** from the main menu
3. Navigate to **Tools** > **Time Stretch**
4. Select **Install**

### Build from Source

```bash
git clone https://github.com/charlesvestal/move-everything-stretch
cd move-anything-stretch
./scripts/build.sh
./scripts/install.sh
```

## Usage

1. Open the Tools menu (Shift+Vol+Step13)
2. Select **Time Stretch**
3. Browse and select a WAV file
4. Set **Bars** to match the song length (1/4 to 16)
5. Set **Target BPM** (40-300)
6. Press any pad to preview the stretched audio
7. Select **Save** to overwrite or create a new file

## Controls

| Control | Function |
|---------|----------|
| Jog wheel | Navigate menu / adjust values |
| Jog click | Enter edit mode / confirm |
| Any pad | Toggle playback |
| Back | Exit edit mode / exit tool |

## Credits

- **Bungee library**: High quality time stretching
- **Move Everything framework**: [Charles Vestal](https://github.com/charlesvestal/move-everything)

## License

MIT License — See [LICENSE](LICENSE)

## AI Assistance Disclaimer

This module is part of Move Everything and was developed with AI assistance, including Claude, Codex, and other AI assistants.

All architecture, implementation, and release decisions are reviewed by human maintainers.
AI-assisted content may still contain errors, so please validate functionality, security, and license compatibility before production use.
