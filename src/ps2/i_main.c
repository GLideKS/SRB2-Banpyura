// SONIC ROBO BLAST 2 - PS2 entry point: bring the console up, then the usual D_SRB2Main / D_SRB2Loop.
/// \file
/// \brief Main program, simply calls D_SRB2Main and D_SRB2Loop, the high level loop.

#include "../ps2ref.h"

#include "../doomdef.h"
#include "../m_argv.h"
#include "../d_main.h"
#include "../m_misc.h"
#include "../i_system.h"

#include "ps2_boot.h"

// -logfile NAME: also write the console log to NAME in the data directory (the EE stdout goes to the
// emulator log anyway).
static void InitLogging(void)
{
	if (M_CheckParm("-logfile") && M_IsNextParm())
	{
		snprintf(logfilename, sizeof logfilename, "%s/%s", ps2boot.datadir, M_GetNextParm());
		logstream = fopen(logfilename, "w");
		if (!logstream)
			printf("Couldn't open log file %s\n", logfilename);
	}
}

/**	\brief	The main function

	\param	argc	number of arg
	\param	*argv	string table

	\return	int
*/
int main(int argc, char **argv)
{
	PS2Boot_Init(&argc, &argv); // IOP modules, argv from the loader plus <data>/ps2args

	myargc = argc;
	myargv = argv;
	PS2Ref_Init();

	// disable text input right off the bat, since we don't need it at the start.
	I_SetTextInputMode(false);

	InitLogging();
	I_StartupSystem();

	// startup SRB2
	CONS_Printf("Setting up SRB2...\n");
	D_SRB2Main();
	CONS_Printf("Entering main game loop...\n");
	// never return
	D_SRB2Loop();

	// return to the loader
	return 0;
}
