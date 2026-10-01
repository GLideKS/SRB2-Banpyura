// SRB2 PS2 port: networking without sockets.
//
// Replaces i_tcp.c, mserv.c, http-mserv.c and the transfer/curl/addon half of d_netfil.c.
// The engine still runs as "server + local client": d_net.c's loopback ("rebound",
// node 0) is untouched, only the socket driver, the master server and file
// transfers are gone. Every "send to node != 0 without netgame" stays fatal in HSendPacket.
//
// Contract with the core:
//  * I_InitNetwork()/I_InitTcpNetwork() return false and leave I_NetOpenSocket (and the
//    ban/address hooks) NULL, so "connect"/"host" print "There is no network driver".
//  * No file is ever sent, requested or downloaded; no master server is ever contacted.
//  * cvars keep their original names/defaults/flags so config.cfg and the menus work.

#include "../doomdef.h"
#include "../doomstat.h"
#include "../command.h"
#include "../d_main.h"
#include "../m_argv.h"
#include "../md5.h"
#include "../filesrch.h"
#include "../z_zone.h"
#include "../netcode/i_net.h"
#include "../netcode/i_tcp.h"
#include "../netcode/d_net.h"
#include "../netcode/d_clisrv.h"
#include "../netcode/d_netfil.h"
#include "../netcode/mserv.h"
#include "../netcode/protocol.h"

// ===== socket driver (i_tcp.c) =====

UINT16 current_port = 0;

boolean I_InitNetwork(void)
{
	return false; // no external driver
}

boolean I_InitTcpDriver(void)
{
	return false; // no TCP/IP stack
}

void I_ShutdownTcpDriver(void)
{
}

boolean I_InitTcpNetwork(void)
{
	// The original returns false before touching any hook when the stack is missing.
	if (M_CheckParm("-server") || M_CheckParm("-connect") || dedicated)
		CONS_Alert(CONS_WARNING, "Networking is not available on this platform\n");
	return false;
}

boolean Net_IsNodeIPv6(INT32 node)
{
	(void)node;
	return false; // same as the NO_IPV6 build
}

// ===== master server (mserv.c, http-mserv.c) =====
// Built without MASTERSERVER: callbacks do nothing, nothing is ever registered.

static boolean ServerName_CanChange(const char *newvalue)
{
	if (strlen(newvalue) < MAXSERVERNAME)
		return true;

	CONS_Alert(CONS_NOTICE, "The server name must be shorter than %d characters\n", MAXSERVERNAME);
	return false;
}

static void MasterServer_OnChange(void) {}
static void Update_parameters(void) {}
static void RoomId_OnChange(void) {}
static void MasterServer_Debug_OnChange(void) {}

static CV_PossibleValue_t masterserver_update_rate_cons_t[] = {
	{2,  "MIN"},
	{60, "MAX"},
	{0,NULL}
};
static CV_PossibleValue_t cv_masterserver_room_values[] = {{-1, "MIN"}, {999999999, "MAX"}, {0, NULL}};

consvar_t cv_masterserver = CVAR_INIT ("masterserver", "https://ds.ms.srb2.org/MS/0", CV_SAVE|CV_CALL, NULL, MasterServer_OnChange);
consvar_t cv_servername = CVAR_INIT_WITH_CALLBACKS ("servername", "SRB2 server", CV_SAVE|CV_NETVAR|CV_CALL|CV_NOINIT|CV_ALLOWLUA, NULL, Update_parameters, ServerName_CanChange);
consvar_t cv_masterserver_update_rate = CVAR_INIT ("masterserver_update_rate", "15", CV_SAVE|CV_CALL|CV_NOINIT, masterserver_update_rate_cons_t, Update_parameters);
consvar_t cv_masterserver_room_id = CVAR_INIT ("masterserver_room_id", "-1", CV_CALL, cv_masterserver_room_values, RoomId_OnChange);
consvar_t cv_masterserver_timeout = CVAR_INIT ("masterserver_timeout", "5", CV_SAVE, CV_Unsigned, NULL);
consvar_t cv_masterserver_debug = CVAR_INIT ("masterserver_debug", "Off", CV_SAVE|CV_CALL, CV_OnOff, MasterServer_Debug_OnChange);
consvar_t cv_masterserver_token = CVAR_INIT ("masterserver_token", "", CV_SAVE, NULL, NULL);

// Read by the (compiled-in) multiplayer menu; always empty, zero-filled.
msg_rooms_t room_list[NUM_LIST_ROOMS+1];

void AddMServCommands(void)
{
	CV_RegisterVar(&cv_masterserver);
	CV_RegisterVar(&cv_masterserver_update_rate);
	CV_RegisterVar(&cv_masterserver_room_id);
	CV_RegisterVar(&cv_masterserver_timeout);
	CV_RegisterVar(&cv_masterserver_debug);
	CV_RegisterVar(&cv_masterserver_token);
	CV_RegisterVar(&cv_servername);
	// "listserv"/"masterserver_update" exist only with MASTERSERVER
}

// ===== file transfers (d_netfil.c) =====

INT32 fileneedednum = 0;
fileneeded_t *fileneeded = NULL;
char downloaddir[512] = "DOWNLOAD"; // d_main.c rewrites it from srb2home
INT32 addontypes[NUMADDONTYPES] = {0};
file_download_t filedownload; // current = 0; CL_ConnectToServer sets -1
HTTP_login *curl_logins = NULL;

luafiletransfer_t *luafiletransfers = NULL; // Lua is stubbed: never non-NULL
boolean waitingforluafiletransfer = false;
boolean waitingforluafilecommand = false;
char luafiledir[256 + 16] = "luafiles";

static CV_PossibleValue_t maxsend_cons_t[] = {{0, "MIN"}, {204800, "MAX"}, {0, NULL}};
consvar_t cv_maxsend = CVAR_INIT ("maxsend", "4096", CV_SAVE|CV_NETVAR, maxsend_cons_t, NULL);

consvar_t cv_noticedownload = CVAR_INIT ("noticedownload", "Off", CV_SAVE|CV_NETVAR, CV_OnOff, NULL);

static CV_PossibleValue_t downloadspeed_cons_t[] = {{1, "MIN"}, {300, "MAX"}, {0, NULL}};
consvar_t cv_downloadspeed = CVAR_INIT ("downloadspeed", "16", CV_SAVE|CV_NETVAR, downloadspeed_cons_t, NULL);

// A server told us it needs files; we cannot fetch them.
static boolean filesrequired = false;

// Server side: advertise no needed files (a PS2 host can never serve any).
UINT8 *PutFileNeeded(UINT16 firstfile)
{
	(void)firstfile;

	if (netbuffer->packettype == PT_MOREFILESNEEDED)
	{
		netbuffer->u.filesneededcfg.num = 0;
		return netbuffer->u.filesneededcfg.files;
	}

	netbuffer->u.serverinfo.fileneedednum = 0;
	return netbuffer->u.serverinfo.fileneeded;
}

void AllocFileNeeded(INT32 size)
{
	if (fileneeded == NULL)
		fileneeded = Z_Calloc(sizeof(fileneeded_t) * size, PU_STATIC, NULL);
	else
		fileneeded = Z_Realloc(fileneeded, sizeof(fileneeded_t) * size, PU_STATIC, NULL);
}

void FreeFileNeeded(void)
{
	Z_Free(fileneeded);
	fileneeded = NULL;
	filesrequired = false;
}

// Remote server file list: keep no table, only remember that something is required.
void D_ParseFileneeded(INT32 fileneedednum_parm, UINT8 *fileneededstr, UINT16 firstfile)
{
	(void)fileneededstr;
	fileneedednum = 0;
	if (firstfile + fileneedednum_parm > 0)
		filesrequired = true;
}

// Bookkeeping only (as the original); the data never arrives, so the join times out normally.
void CL_PrepareDownloadSaveGame(const char *tmpsave)
{
	filedownload.current = -1;

	FreeFileNeeded();
	AllocFileNeeded(1);

	fileneedednum = 1;
	fileneeded[0].type = FILENEEDED_SAVEGAME;
	fileneeded[0].status = FS_REQUESTED;
	fileneeded[0].justdownloaded = false;
	fileneeded[0].totalsize = UINT32_MAX;
	fileneeded[0].file = NULL;
	memset(fileneeded[0].md5sum, 0, 16);
	strcpy(fileneeded[0].filename, tmpsave);
}

// Nothing can be downloaded.
UINT8 CL_CheckDownloadable(boolean direct)
{
	(void)direct;
	return DLSTATUS_NODOWNLOAD;
}

void CL_AbortDownloadResume(void)
{
	// no paused downloads exist
}

boolean CL_SendFileRequest(void)
{
	return false; // could not send the request
}

// 1 = all files present (nothing required); 0 = download required (CL_CheckDownloadable then refuses)
INT32 CL_CheckFiles(void)
{
	return filesrequired ? 0 : 1;
}

void CL_CheckAddonTypes(void)
{
	memset(&addontypes, 0, sizeof(addontypes));
}

boolean CL_LoadServerFiles(void)
{
	return true; // nothing left to load
}

void CL_PrepareDownloadLuaFile(void)
{
	// Lua file transfers cannot exist
}

void AddRamToSendQueue(INT32 node, void *data, size_t size, freemethod_t freemethod, UINT8 fileid)
{
	(void)fileid;

	CONS_Debug(DBG_NETPLAY, "File transfers unavailable, dropping %u-byte block for node %d\n", (UINT32)size, node);

	// Release the block as the sender would have after transmission.
	switch (freemethod)
	{
		case SF_Z_RAM:
			Z_Free(data);
			break;
		case SF_RAM:
			free(data);
			break;
		default: // SF_FILE (not a RAM block), SF_NOFREERAM
			break;
	}
}

boolean AddLuaFileToSendQueue(INT32 node, const char *filename)
{
	(void)node;
	(void)filename;
	return false;
}

void SV_HandleLuaFileSent(UINT8 node)
{
	(void)node;
}

void RemoveAllLuaFileTransfers(void)
{
	// the list is always empty
}

void SV_AbortLuaFileTransfer(INT32 node)
{
	(void)node;
}

void SV_AbortSendFiles(INT32 node)
{
	(void)node; // nothing is queued
}

void CloseNetFile(void)
{
	FreeFileNeeded();
}

void FileSendTicker(void)
{
}

void FileReceiveTicker(void)
{
}

void PT_FileAck(SINT8 node)
{
	(void)node; // we never send files
}

void PT_FileReceived(SINT8 node)
{
	(void)node;
}

void PT_FileFragment(SINT8 node, INT32 netconsole)
{
	(void)netconsole;

	// Unsolicited fragment: nothing is being downloaded, so ignore it like a late packet.
	// A peer that is not even in the game gets dropped, as in the original.
	if (!netnodes[node].ingame)
		Net_CloseConnection(node);
}

void PT_RequestFile(SINT8 node)
{
	Net_CloseConnection(node); // we do not serve files
}

void Command_Downloads_f(void)
{
	// no transfers in progress
}

// ===== HTTP (curl) =====

boolean CURLPrepareFile(const char *url, int dfilenum)
{
	(void)url;
	(void)dfilenum;
	return false; // HTTP download could not start
}

void CURLAbortFile(void)
{
}

void CURLGetFile(void)
{
}

HTTP_login *CURLGetLogin(const char *url, HTTP_login ***return_prev_next)
{
	HTTP_login  *login;
	HTTP_login **prev_next;

	for (prev_next = &curl_logins; (login = *prev_next); prev_next = &login->next)
	{
		if (strcmp(login->url, url) == 0)
		{
			if (return_prev_next)
				*return_prev_next = prev_next;
			return login;
		}
	}

	return NULL;
}

// ===== file lookup helpers (verbatim from d_netfil.c; used by w_wad.c, g_demo.c, command.c) =====

void nameonly(char *s)
{
	for (size_t j = strlen(s); j != (size_t)-1; j--)
		if ((s[j] == '\\') || (s[j] == ':') || (s[j] == '/'))
		{
			void *ns = &(s[j+1]);
			size_t len = strlen(ns);
			memmove(s, ns, len+1);
			return;
		}
}

// Returns the length in characters of the last element of a path.
size_t nameonlylength(const char *s)
{
	size_t len = strlen(s);

	for (size_t j = len; j != (size_t)-1; j--)
		if ((s[j] == '\\') || (s[j] == ':') || (s[j] == '/'))
			return len - j - 1;

	return len;
}

filestatus_t checkfilemd5(char *filename, const UINT8 *wantedmd5sum)
{
#if defined (NOMD5)
	(void)wantedmd5sum;
	(void)filename;
#else
	FILE *fhandle;
	UINT8 md5sum[16];

	if (!wantedmd5sum)
		return FS_FOUND;

	fhandle = fopen(filename, "rb");
	if (fhandle)
	{
		md5_stream(fhandle,md5sum);
		fclose(fhandle);
		if (!memcmp(wantedmd5sum, md5sum, 16))
			return FS_FOUND;
		return FS_MD5SUMBAD;
	}

	I_Error("Couldn't open %s for md5 check", filename);
#endif
	return FS_FOUND;
}

// Note: if completepath is true, "filename" is modified, but only if FS_FOUND is going to be returned
filestatus_t findfile(char *filename, const UINT8 *wantedmd5sum, boolean completepath)
{
	filestatus_t homecheck; // store result of last file search
	boolean badmd5 = false; // store whether md5 was bad from either of the first two searches (if nothing was found in the third)

	// first, check SRB2's "home" directory
	homecheck = filesearch(filename, srb2home, wantedmd5sum, completepath, 10);

	if (homecheck == FS_FOUND)
		return FS_FOUND;
	else if (homecheck == FS_MD5SUMBAD)
		badmd5 = true;

	// next, check SRB2's "path" directory
	homecheck = filesearch(filename, srb2path, wantedmd5sum, completepath, 10);

	if (homecheck == FS_FOUND)
		return FS_FOUND;
	else if (homecheck == FS_MD5SUMBAD)
		badmd5 = true;

	// finally check "." directory
	homecheck = filesearch(filename, ".", wantedmd5sum, completepath, 10);

	if (homecheck != FS_NOTFOUND)
		return homecheck;

	return (badmd5 ? FS_MD5SUMBAD : FS_NOTFOUND);
}

// Searches for a folder (full path, or relative to srb2home, srb2path, ".").
filestatus_t findfolder(const char *path)
{
	// Check the path by itself first.
	if (concatpaths(path, NULL) == 1)
		return FS_FOUND;

#define checkpath(startpath) \
	if (concatpaths(path, startpath) == 1) \
		return FS_FOUND

	checkpath(srb2home);
	checkpath(srb2path);
	checkpath(".");

#undef checkpath

	return FS_NOTFOUND;
}
