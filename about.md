# ILL Replay Bot

A replay bot for **showcases**, built for the Impossible Levels List.

- Inputs are stored per **physics tick**, not per rendered frame, so an FPS drop while recording video does not desync the replay.
- Record at reduced speed, play back at full speed.
- Practice mode works: after respawning at a checkpoint, the replay is cut back to that point.
- An on-screen label shows the mode and the measured TPS.

While the bot is on (recording or playback), the level runs in test mode: **no progress, stars, or completions are saved**. Botted runs are for showcases only.
