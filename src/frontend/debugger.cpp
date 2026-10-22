#include "frontend/debugger.h"

#include <cstdio>

#include <imgui.h>

namespace rainbow::frontend {
namespace {

void flagRow(const char* name, u8 value) {
  ImGui::Text("%-4s %02X  S%d Z%d Y%d H%d X%d P%d N%d C%d", name, value,
              (value & 0x80) != 0, (value & 0x40) != 0, (value & 0x20) != 0,
              (value & 0x10) != 0, (value & 0x08) != 0, (value & 0x04) != 0,
              (value & 0x02) != 0, (value & 0x01) != 0);
}

} // namespace

bool drawDebugger(Machine& machine, bool* open) {
  if (!ImGui::Begin("Debugger", open)) {
    ImGui::End();
    return open == nullptr || *open;
  }

  const Z80::Registers& r = machine.cpu().regs();

  ImGui::Text("%s", machineName(machine.type()));
  ImGui::Text("Frame %llu   T %u / %u", static_cast<unsigned long long>(machine.frames()),
              machine.now(), machine.tstatesPerFrameValue());
  ImGui::Separator();

  if (ImGui::BeginTable("regs", 4, ImGuiTableFlags_SizingFixedFit)) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn(); ImGui::Text("PC  %04X", r.pc);
    ImGui::TableNextColumn(); ImGui::Text("SP  %04X", r.sp);
    ImGui::TableNextColumn(); ImGui::Text("IX  %04X", r.ix);
    ImGui::TableNextColumn(); ImGui::Text("IY  %04X", r.iy);

    ImGui::TableNextRow();
    ImGui::TableNextColumn(); ImGui::Text("AF  %04X", machine.cpu().af());
    ImGui::TableNextColumn(); ImGui::Text("BC  %04X", machine.cpu().bc());
    ImGui::TableNextColumn(); ImGui::Text("DE  %04X", machine.cpu().de());
    ImGui::TableNextColumn(); ImGui::Text("HL  %04X", machine.cpu().hl());

    ImGui::TableNextRow();
    ImGui::TableNextColumn(); ImGui::Text("AF' %02X%02X", r.a2, r.f2);
    ImGui::TableNextColumn(); ImGui::Text("BC' %02X%02X", r.b2, r.c2);
    ImGui::TableNextColumn(); ImGui::Text("DE' %02X%02X", r.d2, r.e2);
    ImGui::TableNextColumn(); ImGui::Text("HL' %02X%02X", r.h2, r.l2);

    ImGui::TableNextRow();
    ImGui::TableNextColumn(); ImGui::Text("I   %02X", r.i);
    ImGui::TableNextColumn(); ImGui::Text("R   %02X", r.r);
    ImGui::TableNextColumn(); ImGui::Text("IM  %u", r.im);
    ImGui::TableNextColumn(); ImGui::Text("IFF %d/%d", r.iff1, r.iff2);
    ImGui::EndTable();
  }

  ImGui::Separator();
  flagRow("F", r.f);
  ImGui::Separator();

  ImGui::Text("RAM bank %u   screen bank %u   ROM bank %u", machine.ramBank(),
              machine.screenBank(), machine.romBank());
  ImGui::Text("Port 7FFD %02X   border %u   IRQ %s", machine.port7ffd(), machine.border(),
              machine.irqLine() ? "low" : "high");
  ImGui::Text("ROM %s", machine.romLoaded() ? "loaded" : "MISSING");

  ImGui::Separator();
  ImGui::TextUnformatted("Memory at PC");

  if (ImGui::BeginTable("mem", 8, ImGuiTableFlags_SizingFixedFit)) {
    for (int row = 0; row < 8; ++row) {
      ImGui::TableNextRow();
      const u16 base = u16(r.pc + row * 8);
      ImGui::TableNextColumn();
      ImGui::Text("%04X", base);
      for (int col = 0; col < 8; ++col) {
        ImGui::TableNextColumn();
        const u8 byte = machine.memRead(u16(base + col), 0);
        if (base + col == r.pc) {
          ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "%02X", byte);
        } else {
          ImGui::Text("%02X", byte);
        }
      }
    }
    ImGui::EndTable();
  }

  ImGui::End();
  return open == nullptr || *open;
}

} // namespace rainbow::frontend
