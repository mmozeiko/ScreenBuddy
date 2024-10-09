Screen Buddy
============

Simple application for remote desktop control over internet for Windows 10 and 11.

Get latest binary here: [ScreenBuddy.exe][]

**WARNING**: Windows Defender or other AV software might report false positive detection

Features:
 * privacy friendly - no accounts, no registration, no telemetry
 * simple to use - no network configuration needed, works across NAT's
 * secure - all data in transit is end-to-end encrypted
 * efficient - uses GPU accelerated video encoder & decoder for minimal CPU usage
 * lightweight - native code application, uses very small amount of memory
 * small - zero external dependencies, only default Windows libraries are used
 * integrated file transfer - upload file to remote computer who shares the screen

![image](https://github.com/user-attachments/assets/1cd6ee61-b202-4d4e-9b54-4225ed025bd7)

Building
========

To build the binary from source code, have [Visual Studio][VS] installed, and simply run `build.cmd`.

Technical Details
=================

If you want to read source code, you can find there & learn about following things:

 * Doing simple win32 UI with dialog API and basic interactions with window controls
 * Using [Media Foundation][] for hardware accelarated H264 video encoding and decoding
 * Using [Video Processor MFT] to convert RGB texture to NV12 for encoding, and back from decoding
 * Using asynchronous Media Foundation transform events for video encoding to keep code simple & running on main thread
 * Capturing screen to D3D11 texture, using code from [wcap][]
 * Simple D3D11 shader to render texture, optionally scaling it down by preserving aspect ratio
 * Using [DerpNet][] library for network communication via Tailscale relays
 * Using [WinHTTP][] for https requests to gather inital info about Tailscale relay regions
 * Parsing JSON with [Windows.Data.Json][] API, using code from [TwitchNotify][]
 * Copying & pasting text from/to clipboard
 * Simple progress dialog using Windows [TaskDialog][] common control
 * Basic drag & drop to handle files dropped on window, using [DragAcceptFiles][] function
 * Retrieving Windows registered file icon using [SHGetFileInfo][] function

TODOs
=====

Missing features and/or future improvements:

 - [ ] Sending keyboard input, currently only mouse input is supported
 - [ ] Optionally hide local mouse cursor, will require receiving how cursor changes from remote computer
 - [ ] Better network code, use non-blocking DNS resolving, connection & send calls
 - [ ] Improved encoding, adjust bitrate based on how fast network sends are going through
 - [ ] Better error handling, currently many situations may show very simple disconnection message without any details
 - [ ] Integrate wcap improved color conversion code for better image quality 
 - [ ] More polished UI, allow choosing options - bitrate, framerate, which monitor to share, or share only single window
 - [ ] Clean up the code and comment how things work, currently it is a very rushed and hacky 1-day job
 - [ ] Performance & memory optimizations, in many places can preallocate API resources and skip various API calls
 - [ ] File transfer to both directions
 - [ ] Caputure, encode and send to remote view also the audio output

License
=======

This is free and unencumbered software released into the public domain.

Anyone is free to copy, modify, publish, use, compile, sell, or distribute this software, either in source code form or as
a compiled binary, for any purpose, commercial or non-commercial, and by any means.

[ScreenBuddy.exe]: https://raw.githubusercontent.com/wiki/mmozeiko/ScreenBuddy/ScreenBuddy.exe
[Media Foundation]: https://learn.microsoft.com/en-us/windows/win32/medfound/microsoft-media-foundation-sdk
[Video Processor MFT]: https://learn.microsoft.com/en-us/windows/win32/medfound/video-processor-mft
[WinHTTP]: https://learn.microsoft.com/en-us/windows/win32/winhttp/winhttp-start-page
[Windows.Data.Json]: https://learn.microsoft.com/en-us/uwp/api/windows.data.json
[wcap]: https://github.com/mmozeiko/wcap/
[DerpNet]: https://github.com/mmozeiko/derpnet/
[TwitchNotify]: https://github.com/mmozeiko/TwitchNotify/
[VS]: https://visualstudio.microsoft.com/vs/
[TaskDialog]: https://learn.microsoft.com/en-us/windows/win32/controls/task-dialogs-overview
[DragAcceptFiles]: https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-dragacceptfiles
[SHGetFileInfo]: https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-shgetfileinfow
