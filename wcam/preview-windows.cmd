@echo off
setlocal
where ffmpeg.exe >nul 2>&1
if errorlevel 1 (
  echo FFmpeg was not found. Reopen Windows Terminal after installation.
  pause
  exit /b 1
)
where ffplay.exe >nul 2>&1
if errorlevel 1 (
  echo FFplay was not found. Install FFmpeg Essentials and reopen Windows Terminal.
  pause
  exit /b 1
)
echo Close Windows Camera and other camera applications before continuing.
echo A preview window will open. Close it to stop preview.
echo Startup PPS warnings may occur until the next SPS/PPS and IDR arrive.
ffmpeg.exe -hide_banner -f dshow -video_size 1280x720 -framerate 30 -vcodec h264 -i video="wcam H264 color bar" -c:v copy -copyinkf -f h264 pipe:1 | ffplay.exe -hide_banner -f h264 -framerate 30 -i pipe:0
echo Preview ended. If no window appeared, copy the error messages above.
pause
