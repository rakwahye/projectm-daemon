# projectm-daemon

## Notice
This is an early release WIP. It's not battle-tested. It's part of a larger project and there is a pile of fixes planned already.

## Dependencies

It's recommended to source ProjectM from the latest git.

Get it [here](https://github.com/projectm-visualizer/projectm).

### Packages

- libprojectm (visualizer backend)
- projectm package (for presets)
- parec (pipewire-pulse - for audio stream)
- nc (gnu/openbsd netcat - for audio stream)
- curl (album art)
- playerctl (track listing)

## Usage

Run `make`.

Copy `build/projectm-daemon` and `build/projectm-remote` to your path.

Copy `tools/audio-feed.sh` to your path.

Run projectm-remote launch.

Run `projectm-remote help` to see the available commands.

### Audio input

The audio-feed.sh method is temporary glue.

Start your audio player, find the monitor <N> you want to use with `pactl list short sinks`.

Run with `audio-feed.sh <N> &` and force to BG. 

Enjoy.
