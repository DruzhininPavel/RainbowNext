#include <cctype>
#include <cstdio>
#include <string>

#include <SDL3/SDL_main.h>

#include "frontend/app.h"

namespace {

std::string lowerExtension(const std::string& path) {
  const size_t dot = path.find_last_of('.');
  if (dot == std::string::npos) return {};
  std::string ext = path.substr(dot);
  for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
  return ext;
}

void printUsage() {
  std::printf(
      "usage: rainbow [options] [file]\n"
      "\n"
      "  file              .rom, .tap, .tzx, .sna, .z80, .nex, .dsk, .edsk, .img\n"
      "                    .rbn or .rzx, picked by extension\n"
      "  --rom FILE        Spectrum ROM image\n"
      "  --machine NAME    machine to start: 48, 128, plus2, plus3, pentagon, next\n"
      "  --tape FILE       TAP or TZX image to insert into the tape deck\n"
      "  --snapshot FILE   SNA, Z80 snapshot or NEX program to load at startup\n"
      "  --disk FILE       DSK or EDSK image to insert into the +3 disk drive\n"
      "  --sd FILE         SD card image for the DivMMC; NextZXOS boots from it\n"
      "  --esxdos DIR      host directory that answers the program's esxDOS calls\n"
      "  --state FILE      save state to restore at startup\n"
      "  --rzx FILE        RZX replay to play back at startup\n"
      "  --debugger        start paused with the debugger window open\n"
      "  -h, --help        this message\n");
}

} // namespace

int main(int argc, char** argv) {
  rainbow::frontend::App::Options options;

  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    const bool hasValue = i + 1 < argc;

    if (argument == "--debugger") {
      options.debugger = true;
      continue;
    }
    if (argument == "-h" || argument == "--help") {
      printUsage();
      return 0;
    }
    if ((argument == "--rom" || argument == "--machine" || argument == "--tape" ||
         argument == "--snapshot" || argument == "--disk" || argument == "--sd" ||
         argument == "--esxdos" || argument == "--state" || argument == "--rzx") &&
        hasValue) {
      const std::string value = argv[++i];
      if (argument == "--rom") options.rom = value;
      if (argument == "--machine") options.machine = value;
      if (argument == "--tape") options.tape = value;
      if (argument == "--snapshot") options.snapshot = value;
      if (argument == "--disk") options.disk = value;
      if (argument == "--sd") options.sd = value;
      if (argument == "--esxdos") options.esxdos = value;
      if (argument == "--state") options.state = value;
      if (argument == "--rzx") options.rzx = value;
      continue;
    }

    // A bare path is routed by its extension so the desktop can pass a file.
    const std::string extension = lowerExtension(argument);
    if (extension == ".rom") {
      options.rom = argument;
    } else if (extension == ".tap" || extension == ".tzx") {
      options.tape = argument;
    } else if (extension == ".sna" || extension == ".z80" || extension == ".nex") {
      options.snapshot = argument;
    } else if (extension == ".dsk" || extension == ".edsk") {
      options.disk = argument;
    } else if (extension == ".img") {
      options.sd = argument;
    } else if (extension == ".rbn") {
      options.state = argument;
    } else if (extension == ".rzx") {
      options.rzx = argument;
    } else {
      std::fprintf(stderr, "rainbow: unsupported argument '%s'\n", argument.c_str());
      printUsage();
      return 2;
    }
  }

  rainbow::frontend::App app;
  if (!app.init(options)) return 1;

  app.run();
  return 0;
}
