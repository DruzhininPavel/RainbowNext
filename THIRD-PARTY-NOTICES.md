# Third-party notices

RainbowNext is released under the [BSD Zero Clause License](LICENSE). The
following components are not written by the RainbowNext authors and keep their
own licenses; their notices are reproduced here, and they are the only parts of
a release archive that carry someone else's copyright.

## SDL3

<https://github.com/libsdl-org/SDL> — zlib license. Linked into the release
binaries (statically), and built from source when the build machine has no
system copy.

```
Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

This software is provided 'as-is', without any express or implied
warranty.  In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.
```

## Dear ImGui

<https://github.com/ocornut/imgui> — MIT license. Compiled into the frontend.

```
The MIT License (MIT)

Copyright (c) 2014-2026 Omar Cornut

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## What is not covered

**No Spectrum ROM image, NextZXOS image, game, tape, disk or snapshot is
distributed with RainbowNext**, and none is covered by the license above. The
`roms/` folder ships empty on purpose; see [`roms/README.md`](roms/README.md).
Anything you put there stays under its own copyright and is your
responsibility.

Other emulators (Fuse, ZEsarUX, JNEXT, MAME, …) were read as **behavioural
references only**; no code was copied from them. [`docs/REFERENCES.md`](docs/REFERENCES.md)
lists what each one was consulted for. This is why RainbowNext can be released
under a permissive license at all — a GPL emulator's source cannot be lifted
into a 0BSD project, so if a future change wants to reuse code from one of
them, that file's license has to be revisited first.
