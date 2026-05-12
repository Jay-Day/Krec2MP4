# Krec2MP4

## What's it do?
Takes .krec files you've saved, loads them up in a headless instance of Mupen64+, replays the commands recorded by the .krec file, records the frames with FFMPEG (faster than 1:1 speeds) and outputs them in a folder of your choice.

<img width="617" height="698" alt="image" src="https://github.com/user-attachments/assets/2c78846f-8f29-4a2e-a13f-569686dfe623" />


## Setup
1. Download the release
2. Extract the zip
3. Run Krec2MP4_GUI
4. Select the ROM Path for the game that you want to record
5. Select the .krec file for the game you want to record. (These are in the `records/` folder of RMG-K)
6. Select an output path and filename (If you do Batch mode, it will name them the krec filename.mp4)
7. Select your video resolution up to 1440
8. Available encoders are setup at runtime, anything in that dropdown should be available to use
9. Adjust your quality (Recommended Medium)
10. Select your FPS, A-A and Anistropic sliders and click convert.
11. To see finished files, click Open Folder
