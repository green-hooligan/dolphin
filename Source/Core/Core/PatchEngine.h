// Copyright 2013 Dolphin Emulator Project
// Licensed under GPLv2
// Refer to the license.txt file included.

#pragma once

#include "IniFile.h"

namespace PatchEngine
{

enum PatchType
{
	PATCH_8BIT,
	PATCH_16BIT,
	PATCH_32BIT,
};

extern const char *PatchTypeStrings[];

struct PatchEntry
{
	PatchEntry() {}
	PatchEntry(PatchType _t, u32 _addr, u32 _value) : type(_t), address(_addr), value(_value) {}
	PatchType type;
	u32 address;
	u32 value;
};

struct Patch
{
	std::string name;
	std::vector<PatchEntry> entries;
	bool active;
	bool user_defined; // False if this code is shipped with Dolphin.
};

int GetSpeedhackCycles(const u32 addr);
void LoadPatchSection(const char *section, std::vector<Patch> &patches,
                      IniFile &globalIni, IniFile &localIni);
void LoadPatches();
void ApplyFramePatches();
void ApplyARPatches();
void Shutdown();

inline int GetPatchTypeCharLength(PatchType type)
{
	int size = 8;
	switch (type)
	{
	case PatchEngine::PATCH_8BIT:
		size = 2;
		break;

	case PatchEngine::PATCH_16BIT:
		size = 4;
		break;

	case PatchEngine::PATCH_32BIT:
		size = 8;
		break;
	}
	return size;
}

}  // namespace
