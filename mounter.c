
// Generic autoboot/automount RDB parser and mounter.
// - KS 1.3 support, including autoboot mode.
// - 68000 compatible.
// - Boot ROM and executable modes.
// - Autoboot capable (Boot ROM mode only).
// - Full automount support
// - Full RDB filesystem support.
//
// Copyright 2021-2022 Toni Wilen
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this
//    list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
#ifdef DEBUG_MOUNTER
#define USE_SERIAL_OUTPUT
#endif

#include "port.h"

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/alerts.h>
#include <exec/ports.h>
#include <exec/execbase.h>
#include <exec/io.h>
#include <devices/trackdisk.h>
#include <devices/hardblocks.h>
#include <devices/scsidisk.h>
#include <resources/filesysres.h>
#include <libraries/expansion.h>
#include <libraries/expansionbase.h>
#include <libraries/configvars.h>
#include <clib/alib_protos.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/doshunks.h>

#include <string.h>
#include <stdio.h>

#include <proto/exec.h>
#include <proto/expansion.h>
#include <proto/dos.h>

#include "scsimsg.h"
#include "ndkcompat.h"
#include "mounter.h"
#include "device.h"
#include "a4091.h"
#include "attach.h"

#define TRACE 1
#undef TRACE_LSEG
#define Trace printf

#ifdef TRACE_LSEG
#define dbg_lseg printf
#else
#define dbg_lseg(x...) do { } while (0)
#endif

#if TRACE
#define dbg Trace
#else
#define dbg
#endif

#define MAX_BLOCKSIZE 2048
#define LSEG_DATASIZE (512 / 4 - 5)

#if NO_CONFIGDEV
extern UBYTE entrypoint, entrypoint_end;
extern UBYTE bootblock, bootblock_end;
#endif

struct FileSysResource *FileSysResBase = NULL;

struct MountData
{
	struct ExecBase *SysBase;
	struct ExpansionBase *ExpansionBase;
	struct DosLibrary *DOSBase;
	struct IOExtTD *request;
	struct ConfigDev *configDev;
	const UBYTE *creator;
	const UBYTE *devicename;

	ULONG lsegblock;
	ULONG lseglongs;
	ULONG lsegoffset;
	struct LoadSegBlock *lsegbuf;
	UWORD lsegwordbuf;
	UWORD lseghasword;

	ULONG unitnum;
	LONG ret;
	UBYTE buf[MAX_BLOCKSIZE * 3];
	UBYTE zero[2];
	BOOL wasLastDev;
	BOOL wasLastLun;
	BOOL slowSpinup;
	int blocksize;
};

// KS 1.3 compatibility functions
APTR W_CreateIORequest(struct MsgPort *ioReplyPort, ULONG size, struct ExecBase *SysBase)
{
	struct IORequest *ret = NULL;
	if(ioReplyPort == NULL)
		return NULL;
	ret = (struct IORequest*)AllocMem(size, MEMF_PUBLIC | MEMF_CLEAR);
	if(ret != NULL)
	{
		ret->io_Message.mn_ReplyPort = ioReplyPort;
		ret->io_Message.mn_Length = size;
	}
	return ret;
}
void W_DeleteIORequest(APTR iorequest, struct ExecBase *SysBase)
{
	if(iorequest != NULL) {
		FreeMem(iorequest, ((struct Message*)iorequest)->mn_Length);
	}
}
struct MsgPort *W_CreateMsgPort(struct ExecBase *SysBase)
{
	struct MsgPort *ret;
	ret = (struct MsgPort*)AllocMem(sizeof(struct MsgPort), MEMF_PUBLIC | MEMF_CLEAR);
	if(ret != NULL)
	{
		BYTE sb = AllocSignal(-1);
		if (sb != -1)
		{
			ret->mp_Flags = PA_SIGNAL;
			ret->mp_Node.ln_Type = NT_MSGPORT;
  			NewList(&ret->mp_MsgList);
			ret->mp_SigBit = sb;
			ret->mp_SigTask = FindTask(NULL);
			return ret;
		}
		FreeMem(ret, sizeof(struct MsgPort));
	}
	return NULL;
}
void W_DeleteMsgPort(struct MsgPort *port, struct ExecBase *SysBase)
{
	if(port != NULL)
	{
		FreeSignal(port->mp_SigBit);
		FreeMem(port, sizeof(struct MsgPort));
	}
}

// Flush cache (Filesystem relocation)
static void cacheclear(struct MountData *md)
{
	struct ExecBase *SysBase = md->SysBase;
	if (SysBase->LibNode.lib_Version >= 37) {
		CacheClearU();
	}
}

// Simply memory copy.
// Only used for few short copies, it does not need to be optimal.
// Required because compiler built-in memcpy() can have
// extra dependencies which will make boot rom build
// impossible.
static void copymem(void *dstp, void *srcp, UWORD size)
{
	UBYTE *dst = (UBYTE*)dstp;
	UBYTE *src = (UBYTE*)srcp;
	while (size != 0) {
		*dst++ = *src++;
		size--;
	}
}

// Check block checksum
static UWORD checksum(UBYTE *buf, struct MountData *md)
{
	ULONG chk = 0;
	for (UWORD i = 0; i < md->blocksize; i += 4) {
		ULONG v = (buf[i + 0] << 24) | (buf[i + 1] << 16) | (buf[i + 2] << 8) | (buf[i + 3 ] << 0);
		chk += v;
	}
	if (chk) {
		dbg("Checksum error %08"PRIx32"\n", chk);
		return FALSE;
	}
	return TRUE;
}


#define MAX_RETRIES 3

// Read single block with retries
static BOOL readblock(UBYTE *buf, ULONG block, ULONG id, struct MountData *md)
{
	struct ExecBase *SysBase = md->SysBase;
	struct IOExtTD *request = md->request;
	UWORD i, max_retries = MAX_RETRIES;
	if (md->slowSpinup)
		max_retries = 15;

	request->iotd_Req.io_Command = CMD_READ;
	request->iotd_Req.io_Offset = block << 9;
	request->iotd_Req.io_Data = buf;
	request->iotd_Req.io_Length = md->blocksize;
	for (i = 0; i < max_retries; i++) {
		LONG err = DoIO((struct IORequest*)request);
		if (!err) {
			break;
		}
		if (err != ERROR_NOT_READY) {
			dbg("Read block %"PRIu32" error %"PRId32"\n", block, err);
			/* Error retry handled in a4091.device, fail quickly here. */
			i = max_retries;
			break;
		}
		/* Give the drive more time to spin up */
		dbg("Drive not ready.\n");
		delay(1000000);
	}
	if (i == max_retries) {
		return FALSE;
	}
	ULONG v = (buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | (buf[3] << 0);
	dbg_lseg("Read block %"PRIu32" %08"PRIx32"\n", block, v);
	if (id != 0xffffffff) {
		if (v != id) {
			return FALSE;
		}
	}
	if (!checksum(buf, md)) {
		return FALSE;
	}
	return TRUE;
}

// Read multiple longs from LSEG blocks
static BOOL lseg_read_longs(struct MountData *md, ULONG longs, ULONG *data)
{
	dbg_lseg("lseg_read_longs, longs %"PRId32"  ptr %p, remaining %"PRId32"\n", longs, data, md->lseglongs);
	ULONG cnt = 0;
	md->lseghasword = FALSE;
	while (longs > cnt) {
		if (md->lseglongs > 0) {
			data[cnt] = md->lsegbuf->lsb_LoadData[md->lsegoffset];
			md->lsegoffset++;
			md->lseglongs--;
			cnt++;
			if (longs == cnt) {
				return TRUE;
			}
		}
		if (!md->lseglongs) {
			if (md->lsegblock == 0xffffffff) {
				dbg("lseg_read_long premature end!\n");
				return FALSE;
			}
			if (!readblock((UBYTE*)md->lsegbuf, md->lsegblock, IDNAME_LOADSEG, md)) {
				return FALSE;
			}
			md->lseglongs = LSEG_DATASIZE;
			md->lsegoffset = 0;
			dbg_lseg("lseg_read_long lseg block %"PRId32" loaded, next %"PRId32"\n", md->lsegblock, md->lsegbuf->lsb_Next);
			md->lsegblock = md->lsegbuf->lsb_Next;
		}
	}
	return TRUE;
}
// Read single long from LSEG blocks
static BOOL lseg_read_long(struct MountData *md, ULONG *data)
{
	BOOL v;
	if (md->lseghasword) {
		ULONG temp;
		v = lseg_read_longs(md, 1, &temp);
		*data = (md->lsegwordbuf << 16) | (temp >> 16);
		md->lsegwordbuf = (UWORD)temp;
		md->lseghasword = TRUE;
	} else {
		v = lseg_read_longs(md, 1, data);
	}
	dbg_lseg("lseg_read_long %08"PRIx32"\n", *data);
	return v;
}
// Read single word from LSEG blocks
// Internally reads long and buffers second word.
static BOOL lseg_read_word(struct MountData *md, ULONG *data)
{
	if (md->lseghasword) {
		*data = md->lsegwordbuf;
		md->lseghasword = FALSE;
		dbg("lseg_read_word 2/2 %08"PRIx32"\n", *data);
		return TRUE;
	}
	ULONG temp;
	BOOL v = lseg_read_longs(md, 1, &temp);
	if (v) {
		md->lseghasword = TRUE;
		md->lsegwordbuf = (UWORD)temp;
		*data = temp >> 16;
	}
	dbg("lseg_read_word 1/2 %08"PRIx32"\n", *data);
	return v;
}

struct RelocHunk
{
	ULONG hunkSize;
	ULONG *hunkData;
};

// Filesystem relocator
static APTR fsrelocate(struct MountData *md)
{
	struct ExecBase *SysBase = md->SysBase;
	ULONG data;
	struct RelocHunk *relocHunks;
	LONG firstHunk, lastHunk;
	LONG totalHunks;
	WORD hunkCnt;
	WORD ret = 0;
	APTR firstProcessedHunk = NULL;

	if (!lseg_read_long(md, &data)) {
		return NULL;
	}
	if (data != HUNK_HEADER) {
		return NULL;
	}
	// skip first two longs
	lseg_read_long(md, &firstHunk);
	lseg_read_long(md, &firstHunk);
	firstHunk = lastHunk = -1;
	// first hunk
	lseg_read_long(md, &firstHunk);
	// last hunk
	lseg_read_long(md, &lastHunk);
	if (firstHunk < 0 || lastHunk < 0 || firstHunk > lastHunk) {
		return NULL;
	}
	totalHunks = lastHunk - firstHunk + 1;
	dbg("first hunk %"PRId32", last hunk %"PRId32"\n", firstHunk, lastHunk);
	relocHunks = AllocMem(totalHunks * sizeof(struct RelocHunk), MEMF_CLEAR);
	if (!relocHunks) {
		return NULL;
	}

	// Pre-allocate hunks
	ULONG *prevChunk = NULL;
	hunkCnt = 0;
	while (hunkCnt < totalHunks) {
		struct RelocHunk *rh = &relocHunks[hunkCnt];
		ULONG hunkHeadSize;
		ULONG memoryFlags = MEMF_PUBLIC;
		if (!lseg_read_long(md, &hunkHeadSize)) {
			goto end;
		}
		if ((hunkHeadSize & (HUNKF_CHIP | HUNKF_FAST)) == (HUNKF_CHIP | HUNKF_FAST)) {
			if (!lseg_read_long(md, &memoryFlags)) {
				goto end;
			}
		} else if (hunkHeadSize & HUNKF_CHIP) {
			memoryFlags |= MEMF_CHIP;
		}
		hunkHeadSize &= ~(HUNKF_CHIP | HUNKF_FAST);
		rh->hunkSize = hunkHeadSize;
		rh->hunkData = AllocMem((hunkHeadSize + 2) * sizeof(ULONG), memoryFlags | MEMF_CLEAR);
		if (!rh->hunkData) {
			goto end;
		}
		dbg("hunk %"PRId32": ptr %p, size %"PRId32", memory flags %08"PRIx32"\n", hunkCnt + firstHunk, rh->hunkData, hunkHeadSize, memoryFlags);
		rh->hunkData[0] = rh->hunkSize + 2;
		rh->hunkData[1] = MKBADDR(prevChunk);
		prevChunk = &rh->hunkData[1];
		rh->hunkData += 2;

		if (!firstProcessedHunk) {
			firstProcessedHunk = (APTR)(rh->hunkData - 1);
		}
		hunkCnt++;
	}
	dbg("hunks allocated\n");

	// Load hunks/relocate
	hunkCnt = 0;
	struct RelocHunk *rh = NULL;
	while (hunkCnt <= totalHunks) {
		ULONG hunkType;
		if (!lseg_read_long(md, &hunkType)) {
			if (hunkCnt >= totalHunks) {
				break;  // normal end
			}
			goto end;
		}
		dbg("HUNK %08"PRIx32"\n", hunkType);
		switch(hunkType)
		{
			case HUNK_CODE:
			case HUNK_DATA:
			case HUNK_BSS:
			{
				ULONG hunkSize;
				if (hunkCnt >= totalHunks) {
					goto end;  // overflow
				}
				rh = &relocHunks[hunkCnt++];
				if (!lseg_read_long(md, &hunkSize)) {
					goto end;
				}
				if (hunkSize > rh->hunkSize) {
					goto end;
				}
				if (hunkType != HUNK_BSS) {
					if (!lseg_read_longs(md, hunkSize, rh->hunkData)) {
						goto end;
					}
				}
			}
			break;
			case HUNK_RELOC32:
			case HUNK_RELOC32SHORT:
			{
				ULONG relocCnt, relocHunk;
				if (rh == NULL) {
					goto end;
				}
				for (;;) {
					if (!lseg_read_long(md, &relocCnt)) {
						goto end;
					}
					if (!relocCnt) {
						break;
					}
					if (!lseg_read_long(md, &relocHunk)) {
						goto end;
					}
					relocHunk -= firstHunk;
					if (relocHunk >= totalHunks) {
						goto end;
					}
					dbg("HUNK_RELOC32: relocs %"PRId32" hunk %"PRId32"\n", relocCnt, relocHunk + firstHunk);
					struct RelocHunk *rhr = &relocHunks[relocHunk];
					while (relocCnt != 0) {
						ULONG relocOffset;
						if (hunkType == HUNK_RELOC32SHORT) {
							if (!lseg_read_word(md, &relocOffset)) {
								goto end;
							}
						} else {
							if (!lseg_read_long(md, &relocOffset)) {
								goto end;
							}
						}
						if (relocOffset > (rh->hunkSize - 1) * sizeof(ULONG)) {
							goto end;
						}
						UBYTE *hData = (UBYTE*)rh->hunkData + relocOffset;
						if (relocOffset & 1) {
							// Odd address, 68000/010 support.
							ULONG v = (hData[0] << 24) | (hData[1] << 16) | (hData[2] << 8) | (hData[3] << 0);
							v += (ULONG)rhr->hunkData;
							hData[0] = v >> 24;
							hData[1] = v >> 16;
							hData[2] = v >>  8;
							hData[3] = v >>  0;
						} else {
							*((ULONG*)hData) += (ULONG)rhr->hunkData;
						}
						relocCnt--;
					}
				}
			}
			break;
			case HUNK_END:
			// do nothing
			if (hunkCnt >= totalHunks) {
				ret = 1;  // normal end
				goto end;
			}
			break;
			default:
			dbg("Unexpected HUNK!\n");
			goto end;
		}
	}
        ret = 1;

end:
	if (!ret) {
		dbg("reloc failed\n");
		hunkCnt = 0;
		while (hunkCnt < totalHunks) {
			struct RelocHunk *rh = &relocHunks[hunkCnt];
			if (rh->hunkData) {
				FreeMem(rh->hunkData - 2, (rh->hunkSize + 2) * sizeof(ULONG));
			}
			hunkCnt++;
		}
		firstProcessedHunk = NULL;
	} else {
		cacheclear(md);
		dbg("reloc ok, first hunk %p\n", firstProcessedHunk);
	}

	FreeMem(relocHunks, totalHunks * sizeof(struct RelocHunk));

	return firstProcessedHunk;
}

// Scan FileSystem.resource, create new if it is not found or existing entry has older version number.
static struct FileSysEntry *FSHDProcess(struct FileSysHeaderBlock *fshb, ULONG dostype, ULONG version, BOOL newOnly, struct MountData *md)
{
	struct ExecBase *SysBase = md->SysBase;
	struct FileSysEntry *fse = NULL;
	const UBYTE *creator = md->creator ? md->creator : md->zero;
	const char resourceName[] = "FileSystem.resource";

	Forbid();
	struct FileSysResource *fsr = OpenResource(FSRNAME);
	if (!fsr) {
		// FileSystem.resource didn't exist (KS 1.3), create it.
		fsr = AllocMem(sizeof(struct FileSysResource) + strlen(resourceName) + 1 + strlen(creator) + 1, MEMF_PUBLIC | MEMF_CLEAR);
		if (fsr) {
			char *FsResName  = (UBYTE *)(fsr + 1);
			char *CreatorStr = (UBYTE *)FsResName + (strlen(resourceName) + 1);
			NewList(&fsr->fsr_FileSysEntries);
			fsr->fsr_Node.ln_Type = NT_RESOURCE;
			strcpy(FsResName, resourceName);
			fsr->fsr_Node.ln_Name = FsResName;
			strcpy(CreatorStr, creator);
			fsr->fsr_Creator = CreatorStr;
			AddTail(&SysBase->ResourceList, &fsr->fsr_Node);
		}
		dbg("FileSystem.resource created %p\n", fsr);
	}
	if (fsr) {
		fse = (struct FileSysEntry*)fsr->fsr_FileSysEntries.lh_Head;
		while (fse->fse_Node.ln_Succ)  {
			if (fse->fse_DosType == dostype) {
				if (fse->fse_Version >= version) {
					// FileSystem.resource filesystem is same or newer, don't update
					if (newOnly) {
						dbg("FileSystem.resource scan: %p dostype %08"PRIx32" found, FSRES version %08"PRIx32" >= FSHD version %08"PRIx32"\n", fse, dostype, fse->fse_Version, version);
						fse = NULL;
					}
					goto end;
				}
			}
			fse = (struct FileSysEntry*)fse->fse_Node.ln_Succ;
		}
		if (fshb && newOnly) {
			fse = AllocMem(sizeof(struct FileSysEntry) + strlen(creator) + 1, MEMF_PUBLIC | MEMF_CLEAR);
			if (fse) {
				// Process patchflags
				ULONG *dstPatch = &fse->fse_Type;
				ULONG *srcPatch = &fshb->fhb_Type;
				ULONG patchFlags = fshb->fhb_PatchFlags;
				while (patchFlags) {
					*dstPatch++ = *srcPatch++;
					patchFlags >>= 1;
				}
				fse->fse_DosType = fshb->fhb_DosType;
				fse->fse_Version = fshb->fhb_Version;
				fse->fse_PatchFlags = fshb->fhb_PatchFlags;
				strcpy((UBYTE*)(fse + 1), creator);
				fse->fse_Node.ln_Name = (UBYTE*)(fse + 1);
			}
			dbg("FileSystem.resource scan: dostype %08"PRIx32" not found or old version: created new\n", dostype);
		}
	}
end:
	Permit();
	return fse;
}
// Add new FileSysEntry to FileSystem.resource or free it if filesystem load failed.
static void FSHDAdd(struct FileSysEntry *fse, struct MountData *md)
{
	struct ExecBase *SysBase = md->SysBase;
	if (fse->fse_SegList) {
		Forbid();
		struct FileSysResource *fsr = OpenResource(FSRNAME);
		if (fsr) {
			AddHead(&fsr->fsr_FileSysEntries, &fse->fse_Node);
			dbg("FileSysEntry %p added to FileSystem.resource, dostype %08"PRIx32"\n", fse, fse->fse_DosType);
			fse = NULL;
		}
		Permit();
	}
	if (fse) {
		dbg("FileSysEntry %p freed, dostype %08"PRIx32"\n", fse, fse->fse_DosType);
		FreeMem(fse, sizeof(struct FileSysEntry));
	}
}

// Parse FileSystem Header Blocks, load and relocate filesystem if needed.
static struct FileSysEntry *ParseFSHD(UBYTE *buf, ULONG block, ULONG dostype, struct MountData *md)
{
	struct FileSysHeaderBlock *fshb = (struct FileSysHeaderBlock*)buf;
	struct FileSysEntry *fse = NULL;

	for (;;) {
		if (block == 0xffffffff) {
			break;
		}
		if (!readblock(buf, block, IDNAME_FILESYSHEADER, md)) {
			break;
		}
		dbg("FSHD found, block %"PRIu32", dostype %08"PRIx32", looking for dostype %08"PRIx32"\n", block, fshb->fhb_DosType, dostype);
		if (fshb->fhb_DosType == dostype) {
			dbg("FSHD dostype match found\n");
			fse = FSHDProcess(fshb, dostype, fshb->fhb_Version, TRUE, md);
			if (fse) {
				md->lsegblock = fshb->fhb_SegListBlocks;
				md->lsegbuf = (struct LoadSegBlock*)(buf + md->blocksize);
				md->lseglongs = 0;
				APTR seg = fsrelocate(md);
				fse->fse_SegList = MKBADDR(seg);
				// Add to FileSystem.resource if succeeded, delete entry if failure.
				FSHDAdd(fse, md);
			}
			break;
		}
		block = fshb->fhb_Next;
	}
	if (!fse) {
		fse = FSHDProcess(NULL, dostype, 0, FALSE, md);
	}
	return fse;
}

#if NO_CONFIGDEV
// Create fake ConfigDev and DiagArea to support autoboot without requiring real autoconfig device.
static void CreateFakeConfigDev(struct MountData *md)
{
	struct ExecBase *SysBase = md->SysBase;
	struct ExpansionBase *ExpansionBase = md->ExpansionBase;
	struct ConfigDev *configDev;

	configDev = AllocConfigDev();
	if (configDev) {
		configDev->cd_BoardAddr = (void*)&entrypoint;
		configDev->cd_BoardSize = (UBYTE*)&entrypoint_end - (UBYTE*)&entrypoint;
		configDev->cd_Rom.er_Type = ERTF_DIAGVALID;
		ULONG bbSize = &bootblock_end - &bootblock;
		ULONG daSize = sizeof(struct DiagArea) + bbSize;
		struct DiagArea *diagArea = AllocMem(daSize, MEMF_CLEAR | MEMF_PUBLIC);
		if (diagArea) {
			diagArea->da_Config = DAC_CONFIGTIME;
			diagArea->da_BootPoint = sizeof(struct DiagArea);
			diagArea->da_Size = (UWORD)daSize;
			copymem(diagArea + 1, &bootblock, bbSize);
			*((ULONG*)&configDev->cd_Rom.er_Reserved0c) = (ULONG)diagArea;
			cacheclear(md);
		}
		md->configDev = configDev;
	}
}
#endif

struct ParameterPacket
{
	const UBYTE *dosname;
	const UBYTE *execname;
	ULONG unitnum;
	ULONG flags;
	struct DosEnvec de;
};

static UBYTE ToUpper(UBYTE c)
{
	if (c >= 'a' || c <= 'z') {
		c |= 0x20;
	}
	return c;
}

// Case-insensitive BSTR string comparison
static BOOL CompareBSTRNoCase(const UBYTE *src1, const UBYTE *src2)
{
	UBYTE len1 = *src1++;
	UBYTE len2 = *src2++;
	if (len1 != len2) {
		return FALSE;
	}
	for (UWORD i = 0; i < len1; i++) {
		UBYTE c1 = *src1++;
		UBYTE c2 = *src2++;
		c1 = ToUpper(c1);
		c2 = ToUpper(c2);
		if (c1 != c2) {
			return FALSE;
		}
	}
	return TRUE;
}

// Check for duplicate device names
static void CheckAndFixDevName(struct MountData *md, UBYTE *bname)
{
	struct ExecBase *SysBase = md->SysBase;

	Forbid();
	struct BootNode *bn = (struct BootNode*)md->ExpansionBase->MountList.lh_Head;
	while (bn->bn_Node.ln_Succ) {
		struct DeviceNode *dn = bn->bn_DeviceNode;
		const UBYTE *bname2 = BADDR(dn->dn_Name);
		if (CompareBSTRNoCase(bname, bname2)) {
			UBYTE len = bname[0];
			UBYTE *name = bname + 1;
			dbg("Duplicate device name '%s'\n", name);
			if (len > 2 && name[len - 2] == '.' && name[len - 1] >= '0' && name[len - 1] < '9') {
				// if already ends to .<digit>: increase digit by one
				name[len - 1]++;
			} else {
				// else append .1
				name[len++] = '.';
				name[len++] = '1';
				name[len] = 0;
				bname[0] += 2;
			}
			dbg("-> new device name '%s'\n", name);
			// retry
			bn = (struct BootNode*)md->ExpansionBase->MountList.lh_Head;
			continue;
		}
		bn = (struct BootNode*)bn->bn_Node.ln_Succ;
	}
	Permit();
}

// Add DeviceNode to Expansion MountList.
static void AddNode(struct PartitionBlock *part, struct ParameterPacket *pp, struct DeviceNode *dn, UBYTE *name, struct MountData *md)
{
	struct ExecBase *SysBase = md->SysBase;
	struct ExpansionBase *ExpansionBase = md->ExpansionBase;
	struct DosLibrary *DOSBase = md->DOSBase;

	LONG bootPri = (part->pb_Flags & PBFF_BOOTABLE) ? pp->de.de_BootPri : -128;
	if (ExpansionBase->LibNode.lib_Version >= 37) {
		// KS 2.0+
		if (!md->DOSBase && bootPri > -128) {
			dbg("KS20+ Mounting as bootable: pri %08"PRIx32"\n", bootPri);
			AddBootNode(bootPri, ADNF_STARTPROC, dn, md->configDev);
		} else {
			dbg("KS20+: Mounting as non-bootable\n");
			AddDosNode(bootPri, ADNF_STARTPROC, dn);
		}
	} else {
		// KS 1.3
		if (!md->DOSBase && bootPri > -128) {
			dbg("KS13 Mounting as bootable: pri %08"PRIx32"\n", bootPri);
			// Create and insert bootnode manually.
			struct BootNode *bn = AllocMem(sizeof(struct BootNode), MEMF_CLEAR | MEMF_PUBLIC);
			if (bn) {
				bn->bn_Node.ln_Type = NT_BOOTNODE;
				bn->bn_Node.ln_Pri = (BYTE)bootPri;
				bn->bn_Node.ln_Name = (UBYTE*)md->configDev;
				bn->bn_DeviceNode = dn;
				Forbid();
				Enqueue(&md->ExpansionBase->MountList, &bn->bn_Node);
				Permit();
			}
		} else {
			dbg("KS13: Mounting as non-bootable\n");
			AddDosNode(bootPri, 0, dn);
			if (md->DOSBase) {
				// KS 1.3 ADNF_STARTPROC is not supported
				// need to use DeviceProc() to start the filesystem process.
				UWORD len = strlen(name);
				name[len++] = ':';
				name[len] = 0;
				void * __attribute__((unused)) mp = DeviceProc(name);
				dbg("DeviceProc() returned %p\n", mp);
			}
		}
	}
}

// Parse PART block, mount drive.
static ULONG ParsePART(UBYTE *buf, ULONG block, ULONG filesysblock, struct MountData *md)
{
	struct ExecBase *SysBase = md->SysBase;
	struct ExpansionBase *ExpansionBase = md->ExpansionBase;
	struct PartitionBlock *part = (struct PartitionBlock*)buf;
	ULONG nextpartblock = 0xffffffff;

	if (!readblock(buf, block, IDNAME_PARTITION, md)) {
		return nextpartblock;
	}
	dbg("PART found, block %"PRIu32"\n", block);
	nextpartblock = part->pb_Next;
	if (!(part->pb_Flags & PBFF_NOMOUNT)) {
		struct ParameterPacket *pp = AllocMem(sizeof(struct ParameterPacket), MEMF_PUBLIC | MEMF_CLEAR);
		if (pp) {
			copymem(&pp->de, &part->pb_Environment, 17 * sizeof(ULONG));
			struct FileSysEntry *fse = ParseFSHD(buf + md->blocksize, filesysblock, pp->de.de_DosType, md);
			pp->execname = md->devicename;
			pp->unitnum = md->unitnum;
			pp->dosname = part->pb_DriveName + 1;
			part->pb_DriveName[(*part->pb_DriveName) + 1] = 0;
			dbg("PART '%s'\n", pp->dosname);
			CheckAndFixDevName(md, part->pb_DriveName);
			struct DeviceNode *dn = MakeDosNode(pp);
			if (dn) {
				if (fse) {
					// Process PatchFlags
					ULONG *dstPatch = &dn->dn_Type;
					ULONG *srcPatch = &fse->fse_Type;
					ULONG patchFlags = fse->fse_PatchFlags;
					while (patchFlags) {
						if (patchFlags & 1) {
							*dstPatch = *srcPatch;
						}
						patchFlags >>= 1;
						srcPatch++;
						dstPatch++;
					}
				}
				dbg("Mounting partition\n");
#if NO_CONFIGDEV
				if (!md->configDev && !md->DOSBase) {
					CreateFakeConfigDev(md);
				}
#endif
				AddNode(part, pp, dn, part->pb_DriveName + 1, md);
				md->ret++;
			} else {
				dbg("Device node creation failed\n");
			}
			FreeMem(pp, sizeof(struct ParameterPacket));
		}
	}
	return nextpartblock;
}

// Scan PART blocks
static LONG ParseRDSK(UBYTE *buf, struct MountData *md)
{
	struct RigidDiskBlock *rdb = (struct RigidDiskBlock*)buf;
	ULONG partblock = rdb->rdb_PartitionList;
	ULONG filesysblock = rdb->rdb_FileSysHeaderList;
	ULONG flags = rdb->rdb_Flags;
	for (;;) {
		if (partblock == 0xffffffff) {
			break;
		}
		partblock = ParsePART(buf, partblock, filesysblock, md);
	}
	md->wasLastDev = !asave->ignore_last && (flags & RDBFF_LAST) != 0;
	md->wasLastLun = (flags & RDBFF_LASTLUN) != 0;
	return md->ret;
}

// Search for RDB
static LONG ScanRDSK(struct MountData *md)
{
	struct ExecBase *SysBase = md->SysBase;
	LONG ret = -1;
	for (UWORD i = 0; i < RDB_LOCATION_LIMIT; i++) {
		if (readblock(md->buf, i, 0xffffffff, md)) {
			struct RigidDiskBlock *rdb = (struct RigidDiskBlock*)md->buf;
			if (rdb->rdb_ID == IDNAME_RIGIDDISK) {
				dbg("RDB found, block %"PRIu32"\n", i);
				ret = ParseRDSK(md->buf, md);
				break;
			}
		}
	}
	md->request->iotd_Req.io_Command = TD_MOTOR;
	md->request->iotd_Req.io_Length  = 0;
	DoIO((struct IORequest*)md->request);
	return ret;
}

static struct FileSysEntry *scan_filesystems(void)
{
	struct FileSysEntry *fse, *cdfs=NULL;

	/* NOTE - you should actually be in a Forbid while accessing any
	 * system list for which no other method of arbitration is available.
	 * However, for this example we will be printing the information
	 * (which would break a Forbid anyway) so we won't Forbid.
	 * In real life, you should Forbid, copy the information you need,
	 * Permit, then print the info.
	 */
	if (!(FileSysResBase = (struct FileSysResource *)OpenResource(FSRNAME))) {
		printf("Cannot open %s\n",FSRNAME);
	} else {
		printf("DosType   Version   Creator\n");
		printf("------------------------------------------------\n");
		for ( fse = (struct FileSysEntry *)FileSysResBase->fsr_FileSysEntries.lh_Head;
			  fse->fse_Node.ln_Succ;
			  fse = (struct FileSysEntry *)fse->fse_Node.ln_Succ) {
#ifdef DEBUG_MOUNTER
			int x;
			for (x=24; x>=8; x-=8)
				putchar((fse->fse_DosType >> x) & 0xFF);

			putchar((fse->fse_DosType & 0xFF) < 0x30
							? (fse->fse_DosType & 0xFF) + 0x30
							: (fse->fse_DosType & 0xFF));
#endif
			printf("	  %s%d",(fse->fse_Version >> 16)<10 ? " " : "", (fse->fse_Version >> 16));
			printf(".%d%s",(fse->fse_Version & 0xFFFF), (fse->fse_Version & 0xFFFF)<10 ? " " : "");
			printf("	 %s",fse->fse_Node.ln_Name);

			if (fse->fse_DosType==0x43443031) {
				cdfs=fse;
#ifndef ALL_FILESYSTEMS
				break;
#endif
			}
		}

	}
	return cdfs;
}

// Search for Bootable CDROM
static LONG ScanCDROM(struct MountData *md)
{
	struct FileSysEntry *fse=NULL;
	char dosName[] = "CD0";
	static unsigned int cnt = 0;

	ULONG parmPkt[] = {
		(ULONG) dosName,
		(ULONG) md->devicename,
		md->unitnum,	  /* unit number */
		0,			 /* OpenDevice flags */
		17,			// de_TableSize
		2048>>2,	   // de_SizeBlock
		0,			 // de_SecOrg
		1,			 // de_Surfaces
		1,			 // de_SectorPerBlock
		1,			 // de_BlocksPerTrack
		0,			 // de_Reserved
		0,			 // de_PreAlloc
		0,			 // de_Interleave
		0,			 // de_LowCyl
		0,			 // de_HighCyl
		5,			 // de_NumBuffers
		1,			 // de_BufMemType
		0x100000,	  // de_MaxTransfer
		0x7FFFFFFE,	// de_Mask
		2,			 // de_BootPri
		0x43443031,	// de_DosType = "CD01"
	};

	fse=scan_filesystems();
	if (!fse) {
		printf("Could not load filesystem\n");
		return -1;
	}

	dosName[2]='0' + cnt;
	struct DeviceNode *node = MakeDosNode(parmPkt);
	if (!node) {
		printf("Could not create DosNode\n");
		return -1;
	}

	// TODO some consistency check that this is actually
	// a bootable Amiga CDROM
	// - iso toc
	// - CDTV or CD32 disk

	// Process PatchFlags.
	ULONG *dstPatch = &node->dn_Type;
	ULONG *srcPatch = &fse->fse_Type;
	ULONG patchFlags = fse->fse_PatchFlags;
	while (patchFlags) {
		if (patchFlags & 1) {
			*dstPatch = *srcPatch;
		}
		patchFlags >>= 1;
		srcPatch++;
		dstPatch++;
	}

	AddBootNode(2, ADNF_STARTPROC, node, md->configDev);
	cnt++;

	return 1;
}

struct MountStruct
{
	// Device name. ("myhddriver.device")
	// Offset 0.
	const UBYTE *deviceName;
	// Unit number pointer or single integer value.
	// if >= 0x100 (256), pointer to array of ULONGs, first ULONG is number of unit numbers followed (for example { 2, 0, 1 }. 2 units, unit numbers 0 and 1).
	// if < 0x100 (256): used as a single unit number value.
	// Offset 4.
	ULONG *unitNum;
	// Name string used to set Creator field in FileSystem.resource (if KS 1.3) and in FileSystem.resource entries.
	// If NULL: use device name.
	// Offset 8.
	const UBYTE *creatorName;
	// ConfigDev: set if autoconfig board autoboot support is wanted.
	// If NULL and bootable partition found: fake ConfigDev is automatically created.
	// Offset 12.
	struct ConfigDev *configDev;
	// SysBase.
	// Offset 16.
	struct ExecBase *SysBase;
	// LUNs
	// Offset 20.
	BOOL luns;
	// Short/Long Spinup
	// Offset 22.
	BOOL slowSpinup;
};

// Return values:
// If single unit number:
// -1 = No RDB found, device failed to open, disk error or RDB block checksum error.
// 0 = RDB found but no partitions found, disk error or mount failure.
// >0: Number of partitions mounted.
// If unit number array:
// Unit number is replaced with error code:
// Error codes are same as above except:
// -2 = Skipped, previous unit had RDBFF_LAST set.
LONG MountDrive(struct MountStruct *ms)
{
	LONG ret = -1;
	struct MsgPort *port = NULL;
	struct IOExtTD *request = NULL;
	struct ExpansionBase *ExpansionBase;
	struct ExecBase *SysBase = ms->SysBase;
	scsi_inquiry_data_t inq_res;

	dbg("Starting..\n");
	ExpansionBase = (struct ExpansionBase*)OpenLibrary("expansion.library", 34);
	if (ExpansionBase) {
		struct MountData *md = AllocMem(sizeof(struct MountData), MEMF_CLEAR | MEMF_PUBLIC);
		if (md) {
			md->DOSBase = (struct DosLibrary*)OpenLibrary("dos.library", 34);
			md->SysBase = SysBase;
			md->ExpansionBase = ExpansionBase;
			dbg("SysBase=%p ExpansionBase=%p DosBase=%p\n", md->SysBase, md->ExpansionBase, md->DOSBase);
			md->configDev = ms->configDev;
			md->creator = ms->creatorName;
			md->slowSpinup = ms->slowSpinup;
			port = W_CreateMsgPort(SysBase);
			if(port) {
				request = (struct IOExtTD*)W_CreateIORequest(port, sizeof(struct IOExtTD), SysBase);
				if(request) {
					ULONG target;
					ULONG lun = 0;
					for (target = 0; target < 8; target++, lun = 0) {
						ULONG unitNum;
next_lun:
						unitNum = target + lun * 10;
						dbg("OpenDevice('%s', %"PRId32", %p, 0)\n", ms->deviceName, unitNum, request);
						UBYTE err = OpenDevice(ms->deviceName, unitNum, (struct IORequest*)request, 0);
						if (err == 0) {
							md->request = request;
							md->devicename = ms->deviceName;
							md->unitnum = unitNum;
							ret = -1;

							err = dev_scsi_inquiry(request, unitNum, &inq_res);
							if (err == 0) {
								switch (inq_res.device & SID_TYPE) {
								case 5: // CDROM
									if (!asave->cdrom_boot) {
										printf("CDROM boot disabled.\n");
										break;
									}
									md->blocksize=2048;
									ret = ScanRDSK(md);
									if (ret==-1)
										ret = ScanCDROM(md);
									break;
								case 0: // DISK
									md->blocksize=512;
									ret = ScanRDSK(md);
									break;
								default:
									printf("Don't know how to boot from device type %d.\n",
										inq_res.device & 0x1f);
									break;
								}
							}

							CloseDevice((struct IORequest*)request);
							if (ms->luns && (lun++ < 8) &&
							    (!md->wasLastLun)) {
								goto next_lun;
							}

							if (md->wasLastDev) {
								dbg("RDBFF_LAST exit\n");
								break;
							}
						} else {
							dbg("OpenDevice(%s,%"PRId32") failed: %"PRId32"\n", ms->deviceName, unitNum, (BYTE)err);
						}
					}
					W_DeleteIORequest(request, SysBase);
				}
				W_DeleteMsgPort(port, SysBase);
			}
			FreeMem(md, sizeof(struct MountData));
			if (md->DOSBase) {
				CloseLibrary(&md->DOSBase->dl_lib);
			}
		}
		CloseLibrary(&ExpansionBase->LibNode);
	}
	dbg("Exit code %"PRId32"\n", ret);
	return ret;
}

int mount_drives(struct ConfigDev *cd, struct Library *dev)
{
	extern char real_device_name[];
	struct MountStruct ms;
	ULONG unitNum[8];
	int i, j = 1, ret = 0;
	UBYTE dip_switches = *(UBYTE *)(cd->cd_BoardAddr + A4091_OFFSET_SWITCHES);
	UBYTE hostid = dip_switches & 7;

	/* Produce unitNum at runtime */
	unitNum[0] = 7;
	for (i=0; i<8; i++)
		if (hostid != i)
			unitNum[j++] = i;

	printf("Mounter:\n");
	ms.deviceName = real_device_name;
	ms.unitNum = unitNum;
	ms.creatorName = NULL;
	ms.configDev = cd;
	ms.SysBase =  SysBase;
	ms.luns = !(dip_switches & BIT(7));  // 1: LUNs enabled 0: LUNs disabled
	ms.slowSpinup = !(dip_switches & BIT(4));  // 0: Short Spinup 1: Long Spinup

	ret = MountDrive(&ms);

	printf("ret = %x\nunitNum = { ", ret);
	for (i=0; i<8; i++)
		printf("%x%s", unitNum[i], i<7?", ":" }\n");

	return ret;
}
