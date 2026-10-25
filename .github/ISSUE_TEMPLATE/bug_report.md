---
name: Problem report
about: Something does not work, looks wrong or runs badly
title: ""
labels: ""
assignees: ""
---

**Device**
- Model:
- SoC / GPU (for example Snapdragon 685 / Adreno 610):
- Android version:
- RAM:

**App**
- APK version (Releases page name):
- Options changed on the home screen (resolution, frame rate, shadows...):

**What happened**
What you did, what you expected, and what happened instead. Where in the game (menu, scene, dungeon...).

**Log**
Connect the phone with USB debugging on, run this before starting the game, reproduce the problem, then stop it
with Ctrl+C and attach `p5r-log.txt`:
```
adb logcat -s p5r:V > p5r-log.txt
```

Please do not attach game files, keys or saves of other people. No game files or keys are provided here.
