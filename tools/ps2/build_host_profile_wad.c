// Native PS2_PROFILE platform glue: locate existing packs, not the stock pk3.
// The SDL system backend otherwise remains intact (clock, video, sound, home).
#include "doomdef.h"
#include "i_system.h"

const char *I_LocateWad(void)
{
	const char *directory = I_GetEnv("SRB2WADDIR");
	char path[1024];
	FILE *pack;
	int length = snprintf(path, sizeof(path), "%s/SRB2.PAK", directory ? directory : ".");
	if (length < 0 || (size_t)length >= sizeof(path))
		I_Error("Host PS2_PROFILE pack directory path is too long");
	pack = fopen(path, "rb");
	if (!pack)
		I_Error("Host PS2_PROFILE: %s not found; set SRB2WADDIR to the pack directory", path);
	fclose(pack);
	I_OutputMsg("Host PS2_PROFILE packs: %s\n", directory ? directory : ".");
	return directory;
}
