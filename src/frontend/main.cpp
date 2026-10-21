#include <string>

#include <SDL3/SDL_main.h>

#include "frontend/app.h"

int main(int argc, char** argv) {
  std::string romPath;
  if (argc > 1) romPath = argv[1];

  mynext::frontend::App app;
  if (!app.init(romPath)) return 1;

  app.run();
  return 0;
}
