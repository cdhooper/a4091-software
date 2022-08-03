#include "port.h"
#include "printf.h"
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <libraries/expansionbase.h>
#include <devices/trackdisk.h>
#include <clib/expansion_protos.h>
#include <clib/exec_protos.h>
#include <clib/alib_protos.h>
#include <inline/expansion.h>
#include <exec/io.h>
#include <proto/dos.h>
#include <exec/memory.h>
#include <exec/interrupts.h>
#include <exec/execbase.h>
#include <exec/errors.h>
#include <exec/lists.h>
#include <dos/dostags.h>
#include <devices/scsidisk.h>
// #include <inline/exec.h>
// #include <inline/dos.h>

#include "device.h"

#include "scsi_all.h"
#include "scsipiconf.h"
#include "sd.h"
#include "sys_queue.h"
#include "siopreg.h"
#include "siopvar.h"
#include "attach.h"
#include "cmdhandler.h"
#include "device.h"
#include "nsd.h"

#ifdef DEBUG_CMD
#define PRINTF_CMD(args...) printf(args)
#else
#define PRINTF_CMD(args...)
#endif

#define BIT(x)        (1 << (x))

extern struct ExecBase *SysBase;

a4091_save_t *asave = NULL;


/* Structure of the startup message closely follows IOStdReq */
typedef struct {
    struct Message  msg;        // io_Message
    struct MsgPort *msg_port;   // Handler's message port (io_Unit)
    UWORD           cmd;        // CMD_STARTUP            (io_Command)
    UBYTE           boardnum;   // Desired board number   (io_Flags)
    BYTE            io_Error;   // Success=0 or failure code
} start_msg_t;


void
irq_poll(uint got_int, struct siop_softc *sc)
{
    if (sc->sc_flags & SIOP_INTSOFF) {
        siop_regmap_p rp    = sc->sc_siopp;
        uint8_t       istat = rp->siop_istat;

        if (istat & (SIOP_ISTAT_SIP | SIOP_ISTAT_DIP)) {
            sc->sc_istat = istat;
            sc->sc_sstat0 = rp->siop_sstat0;
            sc->sc_dstat  = rp->siop_dstat;
            siopintr(sc);
        }
    } else if (got_int) {
        siopintr(sc);
    }
}

static void
restart_timer(void)
{
    if (asave->as_timerio != NULL) {
        asave->as_timerio->tr_time.tv_secs  = 1;
        asave->as_timerio->tr_time.tv_micro = 0;
        SendIO(&asave->as_timerio->tr_node);
        asave->as_timer_running = 1;
    }
}

static void
close_timer(void)
{
    if (asave->as_timerio != NULL) {
        if (asave->as_timer_running)
            WaitIO(&asave->as_timerio->tr_node);
        CloseDevice(&asave->as_timerio->tr_node);
        asave->as_timer_running = 0;
    }

    if (asave->as_timerport != NULL) {
        DeletePort(asave->as_timerport);
        asave->as_timerport = NULL;
    }

    if (asave->as_timerio != NULL) {
        DeleteExtIO(&asave->as_timerio->tr_node);
        asave->as_timerio = NULL;
    }
}

static int
open_timer(void)
{
    int rc;
    asave->as_timerport = CreatePort(NULL, 0);
    if (asave->as_timerport == NULL) {
        return (ERROR_NO_MEMORY);
    }

    if (!(asave->as_timerio = (struct timerequest *)
            CreateExtIO(asave->as_timerport, sizeof (struct timerequest)))) {
        printf("Fail: CreateExtIO timer\n");
        close_timer();
        return (ERROR_NO_MEMORY);
    }

    rc = OpenDevice(TIMERNAME, UNIT_VBLANK, &asave->as_timerio->tr_node, 0);
    if (rc != 0) {
        printf("Fail: open "TIMERNAME"\n");
        close_timer();
        return (rc);
    }

    asave->as_timerio->tr_node.io_Command = TR_ADDREQUEST;

    return (0);
}

static const UWORD nsd_supported_cmds[] = {
    CMD_READ, CMD_WRITE, TD_SEEK, TD_FORMAT,
    CMD_STOP, CMD_START,
    TD_GETGEOMETRY,
    TD_READ64, TD_WRITE64, TD_SEEK64, TD_FORMAT64,
    HD_SCSICMD,
    TD_PROTSTATUS, TD_CHANGENUM, TD_CHANGESTATE,
    NSCMD_DEVICEQUERY,
    NSCMD_TD_READ64, NSCMD_TD_WRITE64, NSCMD_TD_SEEK64, NSCMD_TD_FORMAT64,
    TAG_END
};

struct DosLibrary *DOSBase;
static int
cmd_do_iorequest(struct IORequest * ior)
{
    int             rc;
    uint64_t        blkno;
    uint            blkshift;
    struct IOExtTD *iotd = (struct IOExtTD *) ior;

    ior->io_Error = 0;
    switch (ior->io_Command) {
        case ETD_READ:
        case CMD_READ:
            PRINTF_CMD("CMD_READ %lx %lx\n",
                       iotd->iotd_Req.io_Offset, iotd->iotd_Req.io_Length);
            if (iotd->iotd_Req.io_Length == 0)
                goto io_done;
            blkshift = ((struct scsipi_periph *) ior->io_Unit)->periph_blkshift;
            blkno = iotd->iotd_Req.io_Offset >> blkshift;
CMD_READ_continue:
            rc = sd_readwrite(iotd->iotd_Req.io_Unit, blkno, B_READ,
                              iotd->iotd_Req.io_Data,
                              iotd->iotd_Req.io_Length, ior);
            if (rc == 0) {
                iotd->iotd_Req.io_Actual = iotd->iotd_Req.io_Length;
                /* cmd_complete() does ReplyMsg() */
            } else {
                iotd->iotd_Req.io_Error = rc;
io_done:
                iotd->iotd_Req.io_Actual = 0;
                ReplyMsg(&ior->io_Message);
            }
            break;

        case ETD_WRITE:
        case CMD_WRITE:
        case ETD_FORMAT:
        case TD_FORMAT:
            PRINTF_CMD("CMD_WRITE %lx %lx\n",
                       iotd->iotd_Req.io_Offset, iotd->iotd_Req.io_Length);
            if (iotd->iotd_Req.io_Length == 0)
                goto io_done;
            blkshift = ((struct scsipi_periph *) ior->io_Unit)->periph_blkshift;
            blkno = iotd->iotd_Req.io_Offset >> blkshift;
CMD_WRITE_continue:
            rc = sd_readwrite(iotd->iotd_Req.io_Unit, blkno, B_WRITE,
                              iotd->iotd_Req.io_Data,
                              iotd->iotd_Req.io_Length, ior);
            if (rc == 0) {
                iotd->iotd_Req.io_Actual = iotd->iotd_Req.io_Length;
                /* cmd_complete() does ReplyMsg() */
            } else {
                iotd->iotd_Req.io_Error = rc;
                iotd->iotd_Req.io_Actual = 0;
                ReplyMsg(&ior->io_Message);
            }
            break;

        case HD_SCSICMD:      // Send any SCSI command to drive (SCSI Direct)
            rc = sd_scsidirect(iotd->iotd_Req.io_Unit,
                               iotd->iotd_Req.io_Data, ior);
            if (rc != 0) {
                iotd->iotd_Req.io_Error = rc;
                ReplyMsg(&ior->io_Message);
            }
            break;

        case NSCMD_TD_READ64:
            printf("NSCMD");
        case TD_READ64:
            printf("TD64_READ %lx:%lx %lx\n", iotd->iotd_Req.io_Actual,
                   iotd->iotd_Req.io_Offset, iotd->iotd_Req.io_Length);
            if (iotd->iotd_Req.io_Length == 0)
                goto io_done;
            blkshift = ((struct scsipi_periph *) ior->io_Unit)->periph_blkshift;
            blkno = ((uint64_t) iotd->iotd_Req.io_Actual << (32 - blkshift)) |
                    (iotd->iotd_Req.io_Offset >> blkshift);
            goto CMD_READ_continue;

        case NSCMD_TD_FORMAT64:
        case NSCMD_TD_WRITE64:
            printf("NSCMD");
        case TD_FORMAT64:
        case TD_WRITE64:
            printf("TD64_WRITE %lx:%lx %lx\n", iotd->iotd_Req.io_Actual,
                   iotd->iotd_Req.io_Offset, iotd->iotd_Req.io_Length);
            if (iotd->iotd_Req.io_Length == 0)
                goto io_done;
            blkshift = ((struct scsipi_periph *) ior->io_Unit)->periph_blkshift;
            blkno = ((uint64_t) iotd->iotd_Req.io_Actual << (32 - blkshift)) |
                    (iotd->iotd_Req.io_Offset >> blkshift);
            goto CMD_WRITE_continue;

#ifdef ENABLE_SEEK
        case NSCMD_TD_SEEK64:
        case TD_SEEK64:
            blkshift = ((struct scsipi_periph *) ior->io_Unit)->periph_blkshift;
            blkno = ((uint64_t) iotd->iotd_Req.io_Actual << (32 - blkshift)) |
                    (iotd->iotd_Req.io_Offset >> blkshift);
            goto CMD_SEEK_continue;
        case ETD_SEEK:
        case TD_SEEK:
            blkshift = ((struct scsipi_periph *) ior->io_Unit)->periph_blkshift;
            blkno = iotd->iotd_Req.io_Offset >> blkshift;
CMD_SEEK_continue:
            rc = sd_seek(iotd->iotd_Req.io_Unit, blkno, ior);
            if (rc != 0) {
                iotd->iotd_Req.io_Error = rc;
                ReplyMsg(&ior->io_Message);
            }
            break;
#endif
        case TD_GETGEOMETRY:  // Get drive capacity, blocksize, etc
            rc = sd_getgeometry(iotd->iotd_Req.io_Unit,
                                iotd->iotd_Req.io_Data, ior);
            if (rc != 0) {
                iotd->iotd_Req.io_Error = rc;
                ReplyMsg(&ior->io_Message);
            }
            // TD_GETGEOMETRY without media should return TDERR_DiskChanged 29
            break;

        case NSCMD_DEVICEQUERY: {
            struct NSDeviceQueryResult *nsd =
                (struct NSDeviceQueryResult *) iotd->iotd_Req.io_Data;
            if (iotd->iotd_Req.io_Length < 16) {
                ior->io_Error = ERROR_BAD_LENGTH;
            } else {
                nsd->DevQueryFormat      = 0;
                nsd->SizeAvailable       = sizeof (*nsd);
                nsd->DeviceType          = NSDEVTYPE_TRACKDISK;
                nsd->DeviceSubType       = 0;
                nsd->SupportedCommands   = (UWORD *) nsd_supported_cmds;
                iotd->iotd_Req.io_Actual = sizeof (*nsd);
            }
            ReplyMsg(&ior->io_Message);
            break;
        }

        case TD_PROTSTATUS:   // Is the disk write protected?
            ior->io_Error = sd_get_protstatus(iotd->iotd_Req.io_Unit,
                                              &iotd->iotd_Req.io_Actual);
            ReplyMsg(&ior->io_Message);
            break;

        case TD_CHANGENUM:     // Number of disk changes
            // XXX: Need to implement this for removable disks
            iotd->iotd_Req.io_Actual = 1;
            ReplyMsg(&ior->io_Message);
            break;

        case TD_CHANGESTATE:   // Is there a disk in the drive?
            // XXX: Need to implement this for removable disks
            iotd->iotd_Req.io_Actual = 0;
            ReplyMsg(&ior->io_Message);
            break;

        case CMD_STOP:         // Send SCSI STOP
            rc = sd_startstop(iotd->iotd_Req.io_Unit, ior, 0, 0);
            if (rc != 0) {
                iotd->iotd_Req.io_Error = rc;
                ReplyMsg(&ior->io_Message);
            }
            break;

        case CMD_START:        // Send SCSI START
            rc = sd_startstop(iotd->iotd_Req.io_Unit, ior, 1, 0);
            if (rc != 0) {
                iotd->iotd_Req.io_Error = rc;
                ReplyMsg(&ior->io_Message);
            }
            break;

        case CMD_ATTACH:  // Attach (open) a new SCSI device
            PRINTF_CMD("CMD_ATTACH\n");
            rc = attach(&asave->as_device_self, iotd->iotd_Req.io_Offset,
                        (struct scsipi_periph **) &ior->io_Unit);
            if (rc != 0) {
                ior->io_Error = rc;
            } else {
                (void) sd_blocksize((struct scsipi_periph *) ior->io_Unit);
            }

            ReplyMsg(&ior->io_Message);
            break;

        case CMD_DETACH:  // Detach (close) a SCSI device
            PRINTF_CMD("CMD_DETACH\n");
            detach((struct scsipi_periph *) ior->io_Unit);
            ReplyMsg(&ior->io_Message);
            break;

        case CMD_TERM:
            PRINTF_CMD("CMD_TERM\n");
            deinit_chan(&asave->as_device_self);
            close_timer();
            CloseLibrary((struct Library *) DOSBase);
            asave->as_isr = NULL;
            FreeMem(asave, sizeof (*asave));
            asave = NULL;
            Forbid();
            DeletePort(myPort);
            myPort = NULL;
            ReplyMsg(&ior->io_Message);
            return (1);

        case CMD_INVALID:      // Invalid command (0)
        case CMD_RESET:        // Not supported by SCSI
        case CMD_UPDATE:       // Not supported by SCSI
        case CMD_CLEAR:        // Not supported by SCSI
        case CMD_FLUSH:        // Not supported by SCSI
        case TD_RAWREAD:       // Not supported by SCSI (raw bits from disk)
        case TD_RAWWRITE:      // Not supported by SCSI (raw bits to disk)
        case TD_GETDRIVETYPE:  // Not supported by SCSI (floppy-only DRIVExxxx)
        case TD_GETNUMTRACKS:  // Not supported by SCSI (floppy-only)
        case TD_REMOVE:        // Notify when disk changes
        case TD_ADDCHANGEINT:  // TD_REMOVE done right
        case TD_REMCHANGEINT:  // Remove softint set by ADDCHANGEINT
        case TD_EJECT:         // For those drives that support it
        default:
            /* Unknown command */
            printf("Unknown cmd %x\n", ior->io_Command);
            /* FALLTHROUGH */
        case TD_MOTOR:         // Not supported by SCSI (floppy-only)
            ior->io_Error = ERROR_UNKNOWN_COMMAND;
            ReplyMsg(&ior->io_Message);
            break;
    }
    return (0);
}


void scsipi_completion_poll(struct scsipi_channel *chan);

__asm("_geta4: lea ___a4_init,a4 \n"
      "        rts");

static void
cmd_handler(void)
{
    struct MsgPort        *msgport;
    struct IORequest      *ior;
    struct Process        *proc;
    struct siop_softc     *sc;
    struct scsipi_channel *chan;
    int                   *active;
    start_msg_t           *msg;
    ULONG                  int_mask;
    ULONG                  cmd_mask;
    ULONG                  wait_mask;
    ULONG                  timer_mask;
    uint32_t               mask;
#if 0
    register long devbase asm("a6");

    /* Builtin compiler function to set A4 to the global data area */
    geta4();

    devbase = msg->devbase;
    (void) devbase;
#endif

    proc = (struct Process *) FindTask((char *)NULL);

    /* get the startup message */
    while ((msg = (start_msg_t *) GetMsg(&proc->pr_MsgPort)) == NULL)
        WaitPort(&proc->pr_MsgPort);

    SysBase = *(struct ExecBase **) 4UL;
    DOSBase = (struct DosLibrary *) OpenLibrary("dos.library", 37L);

    msgport = CreatePort(NULL, 0);
    msg->msg_port = msgport;
    if (msgport == NULL) {
        msg->io_Error = ERROR_NO_MEMORY;
        goto fail_msgport;
    }

    asave = AllocMem(sizeof (*asave), MEMF_CLEAR | MEMF_PUBLIC);
    if (asave == NULL) {
        msg->io_Error = ERROR_NO_MEMORY;
        goto fail_allocmem;
    }

    msg->io_Error = open_timer();
    if (msg->io_Error != 0)
        goto fail_timer;

    msg->io_Error = init_chan(&asave->as_device_self, &msg->boardnum);
    if (msg->io_Error != 0) {
        close_timer();
fail_timer:
        FreeMem(asave, sizeof (*asave));
fail_allocmem:
        /* Terminate handler and give up */
        DeletePort(msgport);
fail_msgport:
        ReplyMsg((struct Message *)msg);
        Forbid();
        return;
    }

    ReplyMsg((struct Message *)msg);
    restart_timer();

    sc         = &asave->as_device_private;
    active     = &sc->sc_channel.chan_active;
    cmd_mask   = BIT(msgport->mp_SigBit);
    int_mask   = BIT(asave->as_irq_signal);
    timer_mask = BIT(asave->as_timerport->mp_SigBit);
    wait_mask  = int_mask | timer_mask | cmd_mask;
    chan       = &sc->sc_channel;

    while (1) {
        mask = Wait(wait_mask);

        if (asave->as_exiting)
            break;

        do {
            irq_poll(mask & int_mask, sc);
        } while ((SetSignal(0, 0) & int_mask) && ((mask |= Wait(wait_mask))));

        if (mask & timer_mask) {
            WaitIO(&asave->as_timerio->tr_node);
//          printf("timer\n");
            scsipi_completion_timeout_check(chan);
            restart_timer();
        }

        if (*active > 20) {
            wait_mask = int_mask | timer_mask;
            continue;
        } else {
            wait_mask = int_mask | timer_mask | cmd_mask;
        }

        while ((ior = (struct IORequest *)GetMsg(msgport)) != NULL) {
            if (cmd_do_iorequest(ior))
                return;  // Exit handler
            if (*active > 20) {
                wait_mask = int_mask;
                break;
            }
        }

        /* Run the retry completion queue, if anything is present */
        scsipi_completion_poll(chan);
    }
}

void
cmd_complete(void *ior, int8_t rc)
{
    struct IOStdReq *ioreq = ior;

    if (ior == NULL) {
        printf("NULL ior in cmd_complete\n");
        return;
    }

    ioreq->io_Error = rc;
    ReplyMsg(&ioreq->io_Message);
}

int
start_cmd_handler(uint *boardnum)
{
    struct Process *proc;
    struct DosLibrary *DOSBase;
    start_msg_t msg;
//    register long devbase asm("a6");

    DOSBase = (struct DosLibrary *) OpenLibrary("dos.library", 37L);
    if (DOSBase == NULL)
        return (1);

    proc = CreateNewProcTags(NP_Entry, (ULONG) cmd_handler,
                             NP_StackSize, 8192,
                             NP_Priority, 0,
                             NP_Name, (ULONG) "a4091.device",
//                           NP_CloseOutput, FALSE,
                             TAG_DONE);
    CloseLibrary((struct Library *) DOSBase);
    if (proc == NULL)
        return (1);

    /* Send the startup message with the board to initialize */
    memset(&msg, 0, sizeof (msg));
    msg.msg.mn_Length = sizeof (start_msg_t) - sizeof (struct Message);
    msg.msg.mn_ReplyPort = CreatePort(NULL, 0);
    msg.msg.mn_Node.ln_Type = NT_MESSAGE;
    msg.msg_port  = NULL;
    msg.cmd       = CMD_STARTUP;
    msg.boardnum  = *boardnum;
    msg.io_Error  = ERROR_OPEN_FAIL;  // Default, which should be overwritten
    PutMsg(&proc->pr_MsgPort, (struct Message *)&msg);
    WaitPort(msg.msg.mn_ReplyPort);
    DeletePort(msg.msg.mn_ReplyPort);
    myPort = msg.msg_port;
    *boardnum = msg.boardnum;

    return (msg.io_Error);
}

void
stop_cmd_handler(void)
{
    struct IORequest ior;

    memset(&ior, 0, sizeof (ior));
    ior.io_Message.mn_ReplyPort = CreateMsgPort();
    ior.io_Command = CMD_TERM;
    ior.io_Unit = NULL;
    PutMsg(myPort, &ior.io_Message);
    WaitPort(ior.io_Message.mn_ReplyPort);
    DeleteMsgPort(ior.io_Message.mn_ReplyPort);
}

typedef struct unit_list unit_list_t;

struct unit_list {
    unit_list_t          *next;
    struct scsipi_periph *periph;
    uint                  scsi_target;
    uint                  count;
};
unit_list_t *unit_list = NULL;

int
open_unit(uint scsi_target, void **io_Unit)
{
    unit_list_t *cur;
    for (cur = unit_list; cur != NULL; cur = cur->next) {
        if (cur->scsi_target == scsi_target) {
            cur->count++;
            *io_Unit = cur->periph;
            return (0);
        }
    }

    struct IOStdReq ior;
    ior.io_Message.mn_ReplyPort = CreateMsgPort();
    ior.io_Command = CMD_ATTACH;
    ior.io_Unit = NULL;
    ior.io_Offset = scsi_target;

    PutMsg(myPort, &ior.io_Message);
    WaitPort(ior.io_Message.mn_ReplyPort);
    DeleteMsgPort(ior.io_Message.mn_ReplyPort);
    if (ior.io_Error != 0)
        return (ior.io_Error);

    *io_Unit = ior.io_Unit;
    if (ior.io_Unit == NULL)
        return (1);  // Attach failed

    /* Add new device to periph list */
    cur = AllocMem(sizeof (*cur), MEMF_PUBLIC);
    if (cur == NULL) {
        FreeMem(cur, sizeof (*cur));
        return (1);
    }

    cur->count = 1;
    cur->periph = (struct scsipi_periph *) ior.io_Unit;
    cur->scsi_target = scsi_target;
    cur->next = unit_list;
    unit_list = cur;
    return (0);
}

void
close_unit(void *io_Unit)
{
    struct scsipi_periph *periph = io_Unit;
    unit_list_t *parent = NULL;
    unit_list_t *cur;
    for (cur = unit_list; cur != NULL; parent = cur, cur = cur->next) {
        if (cur->periph == periph) {
            if (--cur->count > 0)
                return;  // Peripheral is still open

            /* Remove device from list */
            if (parent == NULL)
                unit_list = cur->next;
            else
                parent->next = cur->next;
            FreeMem(cur, sizeof (*cur));

            /* Detach (close) peripheral */
            struct IOStdReq ior;
            ior.io_Message.mn_ReplyPort = CreateMsgPort();
            ior.io_Command = CMD_DETACH;
            ior.io_Unit = (struct Unit *) periph;

            PutMsg(myPort, &ior.io_Message);
            WaitPort(ior.io_Message.mn_ReplyPort);
            DeleteMsgPort(ior.io_Message.mn_ReplyPort);
            return;
        }
    }
    printf("Could not find unit %p to close\n", periph);
}