#include "frontend/debugger.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>

#include "core/disassembler.h"

namespace rainbow::frontend {
namespace {

const ImVec4 kWarn{1.0f, 0.85f, 0.2f, 1.0f};
const ImVec4 kMuted{0.65f, 0.65f, 0.65f, 1.0f};
const ImVec4 kGood{0.45f, 0.9f, 0.45f, 1.0f};

// One decoded line of the listing.
struct ListingEntry {
  u16 address = 0;
  Disassembled decoded;
};

ByteReader machineReader(Machine& machine) {
  return [&machine](u16 address) { return machine.memFetch(address, 0); };
}

bool parseHex(const char* text, u16& value) {
  if (text == nullptr || *text == '\0') return false;
  char* end = nullptr;
  const unsigned long parsed = std::strtoul(text, &end, 16);
  if (end == text || parsed > 0xFFFF) return false;
  value = u16(parsed);
  return true;
}

// Decodes a window of instructions around `pc`. A debugger cannot walk a
// listing backwards, so it starts a little before the PC and keeps the
// alignment that actually lands on it; if no alignment does, the listing
// simply begins at the PC.
std::vector<ListingEntry> buildListing(Machine& machine, u16 pc, int before, int after) {
  const ByteReader read = machineReader(machine);
  const bool z80n = machine.isNext();

  std::vector<ListingEntry> all;
  const u16 start = u16(pc - 40);
  u16 at = start;
  for (int i = 0; i < 200 && u16(at - start) < 160; ++i) {
    const Disassembled decoded = disassemble(at, read, z80n);
    all.push_back({at, decoded});
    at = u16(at + std::max(1, decoded.length));
  }

  int pivot = -1;
  for (int i = 0; i < int(all.size()); ++i) {
    if (all[i].address == pc) {
      pivot = i;
      break;
    }
  }

  if (pivot < 0) {
    all.clear();
    at = pc;
    for (int i = 0; i <= after; ++i) {
      const Disassembled decoded = disassemble(at, read, z80n);
      all.push_back({at, decoded});
      at = u16(at + std::max(1, decoded.length));
    }
    pivot = 0;
  }

  const int from = std::max(0, pivot - before);
  const int to = std::min(int(all.size()), pivot + after + 1);
  return std::vector<ListingEntry>(all.begin() + from, all.begin() + to);
}

std::string hexBytes(Machine& machine, const ListingEntry& entry) {
  std::string text;
  for (int i = 0; i < entry.decoded.length && i < 4; ++i) {
    char byte[4];
    std::snprintf(byte, sizeof byte, "%02X ", machine.memFetch(u16(entry.address + i), 0));
    text += byte;
  }
  return text;
}

void drawRegisters(Machine& machine) {
  const Z80::Registers& r = machine.cpu().regs();

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

  const u8 f = r.f;
  ImGui::Text("F %02X  S%d Z%d Y%d H%d X%d P%d N%d C%d%s", f, (f & 0x80) != 0,
              (f & 0x40) != 0, (f & 0x20) != 0, (f & 0x10) != 0, (f & 0x08) != 0,
              (f & 0x04) != 0, (f & 0x02) != 0, (f & 0x01) != 0,
              r.halted ? "   HALTED" : "");

  ImGui::Text("RAM %u  screen %u  ROM %u   border %u   IRQ %s", machine.ramBank(),
              machine.screenBank(), machine.romBank(), machine.border(),
              machine.irqLine() ? "low" : "high");
}

void drawBreakpoints(Machine& machine, DebuggerUi& ui) {
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5.0f);
  const bool submitted =
      ImGui::InputText("##breakpoint", ui.breakpointInput, sizeof ui.breakpointInput,
                       ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue);
  ImGui::SameLine();
  u16 address = 0;
  const bool valid = parseHex(ui.breakpointInput, address);
  ImGui::BeginDisabled(!valid);
  const bool added = ImGui::Button("Add breakpoint");
  ImGui::EndDisabled();

  if (valid && (submitted || added)) {
    machine.setBreakpoint(address);
    ui.breakpointInput[0] = '\0';
  }

  if (machine.breakpoints().empty()) {
    ImGui::TextColored(kMuted, "No breakpoints. Click a listing line to toggle one.");
    return;
  }

  ImGui::SameLine();
  if (ImGui::Button("Clear all")) machine.clearBreakpoints();

  // A copy, because removing an entry would invalidate the live list.
  const std::vector<u16> points = machine.breakpoints();
  for (u16 bp : points) {
    ImGui::PushID(bp);
    ImGui::TextColored(kWarn, "%04X", bp);
    ImGui::SameLine();
    if (ImGui::SmallButton("remove")) machine.clearBreakpoint(bp);
    ImGui::PopID();
  }
}

void drawListing(Machine& machine) {
  const u16 pc = machine.cpu().regs().pc;
  ImGui::TextColored(kMuted, "The listing follows PC. Click a line to toggle a breakpoint.");

  const std::vector<ListingEntry> lines = buildListing(machine, pc, 6, 14);
  for (const ListingEntry& entry : lines) {
    const bool armed = machine.hasBreakpoint(entry.address);
    const bool current = entry.address == pc;
    char label[96];
    std::snprintf(label, sizeof label, "%c %04X  %-12s %s", armed ? '*' : ' ',
                  entry.address, hexBytes(machine, entry).c_str(), entry.decoded.text.c_str());

    if (current) ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
    if (ImGui::Selectable(label, current, ImGuiSelectableFlags_None, ImVec2(0, 0))) {
      if (armed) {
        machine.clearBreakpoint(entry.address);
      } else {
        machine.setBreakpoint(entry.address);
      }
    }
    if (current) ImGui::PopStyleColor();
  }
}

void drawCpuTab(Machine& machine, DebuggerUi& ui) {
  drawRegisters(machine);
  ImGui::Separator();
  drawBreakpoints(machine, ui);
  ImGui::Separator();
  drawListing(machine);
}

void drawMemoryTab(Machine& machine, DebuggerUi& ui) {
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5.0f);
  ImGui::InputText("##address", ui.memoryAddress, sizeof ui.memoryAddress,
                   ImGuiInputTextFlags_CharsHexadecimal);

  u16 base = 0;
  if (!parseHex(ui.memoryAddress, base)) base = 0;

  ImGui::SameLine();
  if (ImGui::Button("-64")) std::snprintf(ui.memoryAddress, sizeof ui.memoryAddress, "%04X", u16(base - 64));
  ImGui::SameLine();
  if (ImGui::Button("+64")) std::snprintf(ui.memoryAddress, sizeof ui.memoryAddress, "%04X", u16(base + 64));
  ImGui::SameLine();
  if (ImGui::Button("At PC")) {
    std::snprintf(ui.memoryAddress, sizeof ui.memoryAddress, "%04X", machine.cpu().regs().pc);
  }

  ImGui::TextColored(kMuted, "Edit a byte and press Enter to poke it.");
  const float byteWidth = ImGui::GetFontSize() * 1.9f;
  for (int row = 0; row < 8; ++row) {
    const u16 line = u16(base + row * 8);
    ImGui::Text("%04X", line);
    for (int col = 0; col < 8; ++col) {
      const u16 address = u16(line + col);
      char text[4];
      std::snprintf(text, sizeof text, "%02X", machine.memRead(address, 0));
      ImGui::SameLine();
      ImGui::PushID(address);
      ImGui::SetNextItemWidth(byteWidth);
      if (address == machine.cpu().regs().pc) ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
      if (ImGui::InputText("##byte", text, sizeof text,
                           ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue)) {
        u16 value = 0;
        if (parseHex(text, value) && value <= 0xFF) machine.memWrite(address, u8(value), 0);
      }
      if (address == machine.cpu().regs().pc) ImGui::PopStyleColor();
      ImGui::PopID();
    }
  }
}

void drawProfilerTab(Machine& machine) {
  bool profiling = machine.profiling();
  if (ImGui::Checkbox("Sample instruction addresses", &profiling)) machine.setProfiling(profiling);
  ImGui::SameLine();
  if (ImGui::Button("Clear")) machine.resetProfile();

  const u64 total = machine.sampleTotal();
  ImGui::Text("%llu instructions sampled", static_cast<unsigned long long>(total));
  if (total == 0) {
    ImGui::TextColored(kMuted, "Nothing sampled yet. Enable sampling and let the machine run.");
    return;
  }

  std::vector<std::pair<u32, u16>> rows;
  for (u32 address = 0; address < 65536; ++address) {
    const u32 count = machine.sampleCount(u16(address));
    if (count != 0) rows.push_back({count, u16(address)});
  }
  std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
    if (a.first != b.first) return a.first > b.first;
    return a.second < b.second;
  });

  const ByteReader read = machineReader(machine);
  const int shown = std::min<int>(20, int(rows.size()));
  ImGui::TextColored(kMuted, "%zu addresses seen, hottest %d", rows.size(), shown);

  if (ImGui::BeginTable("profile", 3, ImGuiTableFlags_SizingFixedFit)) {
    ImGui::TableSetupColumn("Address");
    ImGui::TableSetupColumn("Share");
    ImGui::TableSetupColumn("Instruction");
    ImGui::TableHeadersRow();
    for (int i = 0; i < shown; ++i) {
      const u32 count = rows[i].first;
      const u16 address = rows[i].second;
      const double share = 100.0 * double(count) / double(total);

      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::Text("%04X", address);
      ImGui::TableNextColumn();
      if (share >= 10.0) ImGui::TextColored(kGood, "%5.1f%%", share);
      else ImGui::Text("%5.1f%%", share);
      ImGui::TableNextColumn();
      ImGui::Text("%s", disassemble(address, read, machine.isNext()).text.c_str());
    }
    ImGui::EndTable();
  }
}

} // namespace

void stepOverInstruction(Machine& machine) {
  const u16 pc = machine.cpu().regs().pc;
  const ByteReader read = machineReader(machine);
  const Disassembled decoded = disassemble(pc, read, machine.isNext());
  if (decoded.flow != Flow::Call) {
    machine.stepInstruction();
    return;
  }

  // Walk out of the routine, giving up after a few million instructions so a
  // runaway program cannot freeze the window.
  const u16 after = u16(pc + decoded.length);
  machine.stepInstruction();
  for (int i = 0; i < 4'000'000; ++i) {
    if (machine.cpu().regs().pc == after) break;
    machine.stepInstruction();
  }
}

DebuggerRequest drawDebugger(Machine& machine, DebuggerUi& ui, bool* open) {
  DebuggerRequest request;
  request.visible = open == nullptr || *open;

  ImGui::SetNextWindowSize(ImVec2(620, 640), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Debugger", open)) {
    ImGui::End();
    return request;
  }

  ImGui::Text("%s   frame %llu   T %u / %u", machineName(machine.type()),
              static_cast<unsigned long long>(machine.frames()), machine.now(),
              machine.tstatesPerFrameValue());

  if (ImGui::Button("Run")) {
    // A breakpoint on the current PC would stop the next frame instantly, so
    // step past it before handing control back to the frame loop.
    if (machine.hasBreakpoint(machine.cpu().regs().pc)) machine.stepInstruction();
    request.resume = true;
  }
  ImGui::SameLine();
  if (ImGui::Button("Step")) machine.stepInstruction();
  ImGui::SameLine();
  if (ImGui::Button("Step over")) stepOverInstruction(machine);
  ImGui::SameLine();
  if (ImGui::Button("Reset")) {
    machine.reset();
  }

  ImGui::Separator();
  if (ImGui::BeginTabBar("debugger")) {
    if (ImGui::BeginTabItem("CPU")) {
      drawCpuTab(machine, ui);
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Memory")) {
      drawMemoryTab(machine, ui);
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Profiler")) {
      drawProfilerTab(machine);
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }

  ImGui::End();
  return request;
}

} // namespace rainbow::frontend
