# Time Stretch for Move Everything

Pitch-preserving time stretch tool for WAV files on Ableton Move, powered by the [Bungee](https://github.com/bungee-audio-stretch/bungee) audio stretcher library.

Load a sample, set the bar length and target BPM, preview in real time, and save the result.

## Features

- Real-time Bungee-based time stretching with looped preview
- Auto-guesses bar length from file duration and project BPM
- Adjustable target BPM (40-300) with live audition
- Save as overwrite or new file with directory browser
- Standard Move Everything menu UI

## Prerequisites

- [Move Everything](https://github.com/charlesvestal/move-anything) installed on your Ableton Move
- SSH access enabled: http://move.local/development/ssh

## Install

### Via Module Store (Recommended)

1. Launch Move Everything on your Move
2. Select **Module Store** from the main menu
3. Navigate to **Tools** > **Time Stretch**
4. Select **Install**

### Build from Source

Requires Docker (recommended) or ARM64 cross-compiler.

```bash
git clone https://github.com/charlesvestal/move-anything-stretch
cd move-anything-stretch
./scripts/build.sh
./scripts/install.sh
```

The build clones and statically links the Bungee library automatically.

## Usage

1. Open the **Tools** menu (Shift+Vol+Step13)
2. Select **Time Stretch**
3. Browse and select a WAV file
4. Adjust **Bars** and **Target BPM** with the jog wheel
5. Press any pad to play/stop the stretched preview
6. Select **Save...** to overwrite or save a new file

## Controls

| Control | Function |
|---------|----------|
| Jog wheel | Scroll menu / adjust values |
| Jog click | Enter edit / confirm |
| Back | Cancel edit / exit tool |
| Any pad | Play / stop preview |
| Knobs | Adjust selected parameter |

## Credits

- **Bungee audio stretcher**: [Parabola Research Limited](https://github.com/bungee-audio-stretch/bungee) (MPL-2.0)
- **PFFFT**: [Julien Pommier](https://github.com/hahnloser/pffft) (BSD-like)
- **Eigen**: [eigen.tuxfamily.org](https://eigen.tuxfamily.org) (MPL-2.0)
- **Move Everything port**: Charles Vestal

## License

MIT License - See [LICENSE](LICENSE)

The Bungee library and its dependencies are licensed under MPL-2.0 and BSD-like terms respectively. See LICENSE for details.

## AI Assistance Disclaimer

This module is part of Move Everything and was developed with AI assistance, including Claude, Codex, and other AI assistants.

All architecture, implementation, and release decisions are reviewed by human maintainers.
AI-assisted content may still contain errors, so please validate functionality, security, and license compatibility before production use.
