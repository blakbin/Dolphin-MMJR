// Copyright 2008 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "Core/PowerPC/PPCSymbolDB.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "Common/CommonTypes.h"
#include "Common/File.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PPCAnalyst.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/PowerPC/SignatureDB/SignatureDB.h"

PPCSymbolDB g_symbolDB;

PPCSymbolDB::PPCSymbolDB() : debugger{&PowerPC::debug_interface}
{
}

PPCSymbolDB::~PPCSymbolDB() = default;

// Adds the function to the list, unless it's already there
Common::Symbol* PPCSymbolDB::AddFunction(u32 start_addr)
{
  // It's already in the list
  if (m_functions.find(start_addr) != m_functions.end())
    return nullptr;

  Common::Symbol symbol;
  if (!PPCAnalyst::AnalyzeFunction(start_addr, symbol))
    return nullptr;

  m_functions[start_addr] = std::move(symbol);
  Common::Symbol* ptr = &m_functions[start_addr];
  ptr->type = Common::Symbol::Type::Function;
  m_checksum_to_function[ptr->hash].insert(ptr);
  return ptr;
}

void PPCSymbolDB::AddKnownSymbol(u32 startAddr, u32 size, const std::string& name,
                                 Common::Symbol::Type type)
{
  auto iter = m_functions.find(startAddr);
  if (iter != m_functions.end())
  {
    // already got it, let's just update name, checksum & size to be sure.
    Common::Symbol* tempfunc = &iter->second;
    tempfunc->Rename(name);
    tempfunc->hash = HashSignatureDB::ComputeCodeChecksum(startAddr, startAddr + size - 4);
    tempfunc->type = type;
    tempfunc->size = size;
  }
  else
  {
    // new symbol. run analyze.
    Common::Symbol tf;
    tf.Rename(name);
    tf.type = type;
    tf.address = startAddr;
    if (tf.type == Common::Symbol::Type::Function)
    {
      PPCAnalyst::AnalyzeFunction(startAddr, tf, size);
      // Do not truncate symbol when a size is expected
      if (size != 0 && tf.size != size)
      {
        WARN_LOG_FMT(SYMBOLS, "Analysed symbol ({}) size mismatch, {} expected but {} computed",
                     name, size, tf.size);
        tf.size = size;
      }
      m_checksum_to_function[tf.hash].insert(&m_functions[startAddr]);
    }
    else
    {
      tf.size = size;
    }
    m_functions[startAddr] = tf;
  }
}

Common::Symbol* PPCSymbolDB::GetSymbolFromAddr(u32 addr)
{
  auto it = m_functions.lower_bound(addr);
  if (it == m_functions.end())
    return nullptr;

  // If the address is exactly the start address of a symbol, we're done.
  if (it->second.address == addr)
    return &it->second;

  // Otherwise, check whether the address is within the bounds of a symbol.
  if (it != m_functions.begin())
    --it;
  if (addr >= it->second.address && addr < it->second.address + it->second.size)
    return &it->second;

  return nullptr;
}

std::string PPCSymbolDB::GetDescription(u32 addr)
{
  Common::Symbol* symbol = GetSymbolFromAddr(addr);
  if (symbol)
    return symbol->name;
  else
    return " --- ";
}

void PPCSymbolDB::FillInCallers()
{
  for (auto& p : m_functions)
  {
    p.second.callers.clear();
  }

  for (auto& entry : m_functions)
  {
    Common::Symbol& f = entry.second;
    for (const Common::SCall& call : f.calls)
    {
      const Common::SCall new_call(entry.first, call.call_address);
      const u32 function_address = call.function;

      auto func_iter = m_functions.find(function_address);
      if (func_iter != m_functions.end())
      {
        Common::Symbol& called_function = func_iter->second;
        called_function.callers.push_back(new_call);
      }
      else
      {
        // LOG(SYMBOLS, "FillInCallers tries to fill data in an unknown function 0x%08x.",
        // FunctionAddress);
        // TODO - analyze the function instead.
      }
    }
  }
}

void PPCSymbolDB::PrintCalls(u32 funcAddr) const
{
  const auto iter = m_functions.find(funcAddr);
  if (iter == m_functions.end())
  {
    WARN_LOG_FMT(SYMBOLS, "Symbol does not exist");
    return;
  }

  const Common::Symbol& f = iter->second;
  DEBUG_LOG_FMT(SYMBOLS, "The function {} at {:08x} calls:", f.name, f.address);
  for (const Common::SCall& call : f.calls)
  {
    const auto n = m_functions.find(call.function);
    if (n != m_functions.end())
    {
      DEBUG_LOG_FMT(SYMBOLS, "* {:08x} : {}", call.call_address, n->second.name);
    }
  }
}

void PPCSymbolDB::PrintCallers(u32 funcAddr) const
{
  const auto iter = m_functions.find(funcAddr);
  if (iter == m_functions.end())
    return;

  const Common::Symbol& f = iter->second;
  DEBUG_LOG_FMT(SYMBOLS, "The function {} at {:08x} is called by:", f.name, f.address);
  for (const Common::SCall& caller : f.callers)
  {
    const auto n = m_functions.find(caller.function);
    if (n != m_functions.end())
    {
      DEBUG_LOG_FMT(SYMBOLS, "* {:08x} : {}", caller.call_address, n->second.name);
    }
  }
}

void PPCSymbolDB::LogFunctionCall(u32 addr)
{
  auto iter = m_functions.find(addr);
  if (iter == m_functions.end())
    return;

  Common::Symbol& f = iter->second;
  f.num_calls++;
}

// The use case for handling bad map files is when you have a game with a map file on the disc,
// but you can't tell whether that map file is for the particular release version used in that game,
// or when you know that the map file is not for that build, but perhaps half the functions in the
// map file are still at the correct locations. Which are both common situations. It will load any
// function names and addresses that have a BLR before the start and at the end, but ignore any that
// don't, and then tell you how many were good and how many it ignored. That way you either find out
// it is all good and use it, find out it is partly good and use the good part, or find out that
// only
// a handful of functions lined up by coincidence and then you can clear the symbols. In the future
// I
// want to make it smarter, so it checks that there are no BLRs in the middle of the function
// (by checking the code length), and also make it cope with added functions in the middle or work
// based on the order of the functions and their approximate length. Currently that process has to
// be
// done manually and is very tedious.
// The use case for separate handling of map files that aren't bad is that you usually want to also
// load names that aren't functions(if included in the map file) without them being rejected as
// invalid.
// You can see discussion about these kinds of issues here :
// https://forums.oculus.com/viewtopic.php?f=42&t=11241&start=580
// https://m2k2.taigaforum.com/post/metroid_prime_hacking_help_25.html#metroid_prime_hacking_help_25

// This one can load both leftover map files on game discs (like Zelda), and mapfiles
// produced by SaveSymbolMap below.
// bad=true means carefully load map files that might not be from exactly the right version
bool PPCSymbolDB::LoadMap(const std::string& filename, bool bad)
{
  File::IOFile f(filename, "r");
  if (!f)
    return false;

  // Two columns are used by Super Smash Bros. Brawl Korean map file
  // Three columns are commonly used
  // Four columns are used in American Mensa Academy map files and perhaps other games
  int column_count = 0;
  int good_count = 0;
  int bad_count = 0;

  char line[512];
  std::string section_name;
  while (fgets(line, 512, f.GetHandle()))
  {
    size_t length = strlen(line);
    if (length < 4)
      continue;

    if (length == 34 && strcmp(line, "  address  Size   address  offset\n") == 0)
    {
      column_count = 4;
      continue;
    }

    char temp[256]{};
    sscanf(line, "%255s", temp);

    if (strcmp(temp, "UNUSED") == 0)
      continue;

    // Support CodeWarrior and Dolphin map
    if (StringEndsWith(line, " section layout\n") || strcmp(temp, ".text") == 0 ||
        strcmp(temp, ".init") == 0)
    {
      section_name = temp;
      continue;
    }

    // Skip four columns' header.
    //
    // Four columns example:
    //
    // .text section layout
    //   Starting        Virtual
    //   address  Size   address
    //   -----------------------
    if (strcmp(temp, "Starting") == 0)
      continue;
    if (strcmp(temp, "address") == 0)
      continue;
    if (strcmp(temp, "-----------------------") == 0)
      continue;

    // Skip link map.
    //
    // Link map example:
    //
    // Link map of __start
    //  1] __start(func, weak) found in os.a __start.c
    //   2] __init_registers(func, local) found in os.a __start.c
    //    3] _stack_addr found as linker generated symbol
    // ...
    //           10] EXILock(func, global) found in exi.a EXIBios.c
    if (StringEndsWith(temp, "]"))
      continue;

    // TODO - Handle/Write a parser for:
    //  - Memory map
    //  - Link map
    //  - Linker generated symbols
    if (section_name.empty())
      continue;

    // Detect two columns with three columns fallback
    if (column_count == 0)
    {
      const std::string_view stripped_line = StripSpaces(line);
      if (std::count(stripped_line.begin(), stripped_line.end(), ' ') == 1)
        column_count = 2;
      else
        column_count = 3;
    }

    u32 address, vaddress, size, offset, alignment;
    char name[512], container[512];
    if (column_count == 4)
    {
      // sometimes there is no alignment value, and sometimes it is because it is an entry of
      // something else
      if (length > 37 && line[37] == ' ')
      {
        alignment = 0;
        sscanf(line, "%08x %08x %08x %08x %511s", &address, &size, &vaddress, &offset, name);
        char* s = strstr(line, "(entry of ");
        if (s)
        {
          sscanf(s + 10, "%511s", container);
          char* s2 = (strchr(container, ')'));
          if (s2 && container[0] != '.')
          {
            s2[0] = '\0';
            strcat(container, "::");
            strcat(container, name);
            strcpy(name, container);
          }
        }
      }
      else
      {
        sscanf(line, "%08x %08x %08x %08x %i %511s", &address, &size, &vaddress, &offset,
               &alignment, name);
      }
    }
    else if (column_count == 3)
    {
      // some entries in the table have a function name followed by " (entry of " followed by a
      // container name, followed by ")"
      // instead of a space followed by a number followed by a space followed by a name
      if (length > 27 && line[27] != ' ' && strstr(line, "(entry of "))
      {
        alignment = 0;
        sscanf(line, "%08x %08x %08x %511s", &address, &size, &vaddress, name);
        char* s = strstr(line, "(entry of ");
        if (s)
        {
          sscanf(s + 10, "%511s", container);
          char* s2 = (strchr(container, ')'));
          if (s2 && container[0] != '.')
          {
            s2[0] = '\0';
            strcat(container, "::");
            strcat(container, name);
            strcpy(name, container);
          }
        }
      }
      else
      {
        sscanf(line, "%08x %08x %08x %i %511s", &address, &size, &vaddress, &alignment, name);
      }
    }
    else if (column_count == 2)
    {
      sscanf(line, "%08x %511s", &address, name);
      vaddress = address;
      size = 0;
    }
    else
    {
      break;
    }
    const char* namepos = strstr(line, name);
    if (namepos != nullptr)  // would be odd if not :P
      strcpy(name, namepos);
    name[strlen(name) - 1] = 0;
    if (name[strlen(name) - 1] == '\r')
      name[strlen(name) - 1] = 0;

    // Check if this is a valid entry.
    if (strlen(name) > 0)
    {
      // Can't compute the checksum if not in RAM
      bool good = !bad && PowerPC::HostIsInstructionRAMAddress(vaddress) &&
                  PowerPC::HostIsInstructionRAMAddress(vaddress + size - 4);
      if (!good)
      {
        // check for BLR before function
        PowerPC::TryReadInstResult read_result = PowerPC::TryReadInstruction(vaddress - 4);
        if (read_result.valid && read_result.hex == 0x4e800020)
        {
          // check for BLR at end of function
          read_result = PowerPC::TryReadInstruction(vaddress + size - 4);
          good = read_result.valid && read_result.hex == 0x4e800020;
        }
      }
      if (good)
      {
        ++good_count;
        if (section_name == ".text" || section_name == ".init")
          AddKnownSymbol(vaddress, size, name, Common::Symbol::Type::Function);
        else
          AddKnownSymbol(vaddress, size, name, Common::Symbol::Type::Data);
      }
      else
      {
        ++bad_count;
      }
    }
  }

  Index();
  NOTICE_LOG_FMT(SYMBOLS, "{} symbols loaded, {} symbols ignored.", good_count, bad_count);
  return true;
}

// Save symbol map similar to CodeWarrior's map file
bool PPCSymbolDB::SaveSymbolMap(const std::string& filename) const
{
  File::IOFile f(filename, "w");
  if (!f)
    return false;

  std::vector<const Common::Symbol*> function_symbols;
  std::vector<const Common::Symbol*> data_symbols;

  for (const auto& function : m_functions)
  {
    const Common::Symbol& symbol = function.second;
    if (symbol.type == Common::Symbol::Type::Function)
      function_symbols.push_back(&symbol);
    else
      data_symbols.push_back(&symbol);
  }

  // Write .text section
  f.WriteString(".text section layout\n");
  for (const auto& symbol : function_symbols)
  {
    // Write symbol address, size, virtual address, alignment, name
    f.WriteString(fmt::format("{0:08x} {1:08x} {2:08x} {3} {4}\n", symbol->address, symbol->size,
                              symbol->address, 0, symbol->name));
  }

  // Write .data section
  f.WriteString("\n.data section layout\n");
  for (const auto& symbol : data_symbols)
  {
    // Write symbol address, size, virtual address, alignment, name
    f.WriteString(fmt::format("{0:08x} {1:08x} {2:08x} {3} {4}\n", symbol->address, symbol->size,
                              symbol->address, 0, symbol->name));
  }

  return true;
}

// Save code map (won't work if Core is running)
//
// Notes:
//  - Dolphin doesn't load back code maps
//  - It's a custom code map format
bool PPCSymbolDB::SaveCodeMap(const std::string& filename) const
{
  constexpr int SYMBOL_NAME_LIMIT = 30;
  File::IOFile f(filename, "w");
  if (!f)
    return false;

  // Write ".text" at the top
  f.WriteString(".text\n");

  u32 next_address = 0;
  for (const auto& function : m_functions)
  {
    const Common::Symbol& symbol = function.second;

    // Skip functions which are inside bigger functions
    if (symbol.address + symbol.size <= next_address)
    {
      // At least write the symbol name and address
      f.WriteString(fmt::format("// {0:08x} beginning of {1}\n", symbol.address, symbol.name));
      continue;
    }

    // Write the symbol full name
    f.WriteString(fmt::format("\n{0}:\n", symbol.name));
    next_address = symbol.address + symbol.size;

    // Write the code
    for (u32 address = symbol.address; address < next_address; address += 4)
    {
      const std::string disasm = debugger->Disassemble(address);
      f.WriteString(fmt::format("{0:08x} {1:<{2}.{3}} {4}\n", address, symbol.name,
                                SYMBOL_NAME_LIMIT, SYMBOL_NAME_LIMIT, disasm));
    }
  }
  return true;
}
