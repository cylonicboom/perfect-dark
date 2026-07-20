# Chaos external sounds

Drop WAV or MP3 files here for effects that play external audio
(pd.play_file — format is detected by content, not extension).

- `ring.wav` or `ring.mp3` — used by the "Ring ring!" effect (the Discord
  call ringtone). The effect errors harmlessly if neither file exists.
  Not shipped with the repo (copyrighted audio).
- `Silo.mp3` — used by the "Silo Countdown" effect (the 8-minute self-destruct
  track, played looped with the mission music killed underneath). Best-effort:
  the countdown and detonation still run if the file is missing (just silent).
  Not shipped with the repo (copyrighted audio).
