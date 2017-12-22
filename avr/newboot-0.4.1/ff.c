/*----------------------------------------------------------------------------/
/  FatFs - FAT file system module  R0.06                     (C)ChaN, 2008
/ Shrunk-via-hacksaw version by Ingo Korb with contributions by Jim Brain
/-----------------------------------------------------------------------------/
/ The FatFs module is an experimenal project to implement FAT file system to
/ cheap microcontrollers. This is a free software and is opened for education,
/ research and development under license policy of following trems.
/
/  Copyright (C) 2008, ChaN, all right reserved.
/
/ * The FatFs module is a free software and there is no warranty.
/ * You can use, modify and/or redistribute it for personal, non-profit or
/ * commercial use without restriction under your responsibility.
/ * Redistributions of source code must retain the above copyright notice.
/
/---------------------------------------------------------------------------/
/  Feb 26, 2006  R0.00  Prototype.
/
/  Apr 29, 2006  R0.01  First stable version.
/
/  Jun 01, 2006  R0.02  Added FAT12 support.
/                       Removed unbuffered mode.
/                       Fixed a problem on small (<32M) patition.
/  Jun 10, 2006  R0.02a Added a configuration option (_FS_MINIMUM).
/
/  Sep 22, 2006  R0.03  Added f_rename().
/                       Changed option _FS_MINIMUM to _FS_MINIMIZE.
/  Dec 11, 2006  R0.03a Improved cluster scan algolithm to write files fast.
/                       Fixed f_mkdir() creates incorrect directory on FAT32.
/
/  Feb 04, 2007  R0.04  Supported multiple drive system.
/                       Changed some interfaces for multiple drive system.
/                       Changed f_mountdrv() to f_mount().
/                       Added f_mkfs().
/  Apr 01, 2007  R0.04a Supported multiple partitions on a plysical drive.
/                       Added a capability of extending file size to f_lseek().
/                       Added minimization level 3.
/                       Fixed an endian sensitive code in f_mkfs().
/  May 05, 2007  R0.04b Added a configuration option _USE_NTFLAG.
/                       Added FSInfo support.
/                       Fixed DBCS name can result FR_INVALID_NAME.
/                       Fixed short seek (<= csize) collapses the file object.
/
/  Aug 25, 2007  R0.05  Changed arguments of f_read(), f_write() and f_mkfs().
/                       Fixed f_mkfs() on FAT32 creates incorrect FSInfo.
/                       Fixed f_mkdir() on FAT32 creates incorrect directory.
/  Feb 03, 2008  R0.05a Added f_truncate() and f_utime().
/                       Fixed off by one error at FAT sub-type determination.
/                       Fixed btr in f_read() can be mistruncated.
/                       Fixed cached sector is not flushed when create and close
/                       without write.
/
/  Apr 01, 2008 R0.06   Added fputc(), fputs(), fprintf() and fgets().
/                       Improved performance of f_lseek() on moving to the same
/                       or following cluster.
/
/---------------------------------------------------------------------------*/

#include <avr/pgmspace.h>
#include <string.h>
#include "config.h"
#include "ff.h"         /* FatFs declarations */
#include "diskio.h"     /* Include file for user provided disk functions */


/*--------------------------------------------------------------------------

   Module Private Functions

---------------------------------------------------------------------------*/
#if _USE_DRIVE_PREFIX != 0
static
FATFS *FatFs[_LOGICAL_DRIVES];  /* Pointer to the file system objects (logical drives) */
#endif
//static
//WORD fsid;              /* File system mount ID */

#if _USE_1_BUF != 0
# define FSBUF static_buf
static
BUF static_buf;
#else
# define FSBUF (fs->buf)
#endif

#if _USE_FS_BUF != 0
# define FPBUF FSBUF
#else
# define FPBUF (fp->buf)
#endif

/*-----------------------------------------------------------------------*/
/* Change window offset                                                  */
/*-----------------------------------------------------------------------*/

static
BOOL move_window (      /* TRUE: successful, FALSE: failed */
  FATFS *fs,            /* File system object */
  BUF *buf,
  DWORD sector          /* Sector number to make apperance in the fs->buf.data[] */
)                       /* Move to zero only writes back dirty window */
{
  DWORD wsect;
#if _USE_1_BUF != 0
  FATFS* ofs = buf->fs;
#else
  FATFS* __attribute__((unused)) ofs = fs;
#endif


  wsect = buf->sect;
#if _USE_1_BUF != 0
  if (wsect != sector || fs != ofs) {   /* Changed current window */
#else
  if (wsect != sector) {                /* Changed current window */
#endif
#if !_FS_READONLY
    BYTE n;
    if (buf->dirty) {                   /* Write back dirty window if needed */
      if (disk_write(ofs->drive, buf->data, wsect, 1) != RES_OK)
        return FALSE;
      buf->dirty = FALSE;
      if (wsect < (ofs->fatbase + ofs->sects_fat)) {  /* In FAT area */
        for (n = ofs->n_fats; n >= 2; n--) {          /* Reflect the change to FAT copy */
          wsect += ofs->sects_fat;
          disk_write(ofs->drive, buf->data, wsect, 1);
        }
      }
    }
#endif
    if (sector) {
      if (disk_read(buf->data, sector) != RES_OK)
        return FALSE;
      buf->sect = sector;
#if _USE_1_BUF != 0
      buf->fs=fs;
#endif
    }
  }
  return TRUE;
}




static
BOOL move_fs_window(
  FATFS* fs,
  DWORD  sector
)
{
  return move_window(fs,&FSBUF,sector);
}




static
BOOL move_fp_window(
  FIL* fp,
  DWORD  sector
)
{
  return move_window(fp->fs,&FPBUF,sector);
}




/*-----------------------------------------------------------------------*/
/* Clean-up cached data                                                  */
/*-----------------------------------------------------------------------*/

#if !_FS_READONLY
static
FRESULT sync (          /* FR_OK: successful, FR_RW_ERROR: failed */
  FATFS *fs             /* File system object */
)
{
  FSBUF.dirty = TRUE;
  if (!move_fs_window(fs, 0)) return FR_RW_ERROR;
#if _USE_FSINFO
  /* Update FSInfo sector if needed */
  if (fs->fs_type == FS_FAT32 && fs->fsi_flag) {
    FSBUF.sect = 0;
    memset(FSBUF.data, 0, 512);
    ST_WORD(&FSBUF.data[BS_55AA], 0xAA55);
    ST_DWORD(&FSBUF.data[FSI_LeadSig], 0x41615252);
    ST_DWORD(&FSBUF.data[FSI_StrucSig], 0x61417272);
    ST_DWORD(&FSBUF.data[FSI_Free_Count], fs->free_clust);
    ST_DWORD(&FSBUF.data[FSI_Nxt_Free], fs->last_clust);
    disk_write(fs->drive, FSBUF.data, fs->fsi_sector, 1);
    fs->fsi_flag = 0;
  }
#endif
  /* Make sure that no pending write process in the physical drive */
  if (disk_ioctl(fs->drive, CTRL_SYNC, NULL) != RES_OK)
    return FR_RW_ERROR;
  return FR_OK;
}
#endif




/*-----------------------------------------------------------------------*/
/* Get a cluster status                                                  */
/*-----------------------------------------------------------------------*/

static
DWORD get_cluster (     /* 0,>=2: successful, 1: failed */
  FATFS *fs,            /* File system object */
  DWORD clust           /* Cluster# to get the link information */
)
{
  WORD __attribute__((unused)) wc, bc;
  DWORD fatsect;


  if (clust >= 2 && clust < fs->max_clust) {        /* Is it a valid cluster#? */
    fatsect = fs->fatbase;
    switch (fs->fs_type) {

#ifndef CONFIG_DISABLE_FAT12
    case FS_FAT12 :
      bc = (WORD)clust * 3 / 2;
      if (!move_fs_window(fs, fatsect + (bc / SS(fs)))) break;
      wc = FSBUF.data[bc & (SS(fs) - 1)]; bc++;
      if (!move_fs_window(fs, fatsect + (bc / SS(fs)))) break;
      wc |= (WORD)FSBUF.data[bc & (SS(fs) - 1)] << 8;
      return (clust & 1) ? (wc >> 4) : (wc & 0xFFF);
#endif

    case FS_FAT16 :
      if (!move_fs_window(fs, fatsect + (clust / (SS(fs) / 2)))) break;
      return LD_WORD(&FSBUF.data[((WORD)clust * 2) & (SS(fs) - 1)]);

    case FS_FAT32 :
      if (!move_fs_window(fs, fatsect + (clust / (SS(fs) / 4)))) break;
      return LD_DWORD(&FSBUF.data[((WORD)clust * 4) & (SS(fs) - 1)]) & 0x0FFFFFFF;
    }
  }

  return 1; /* Out of cluster range, or an error occured */
}




/*-----------------------------------------------------------------------*/
/* Get sector# from cluster#                                             */
/*-----------------------------------------------------------------------*/

static
DWORD clust2sect (      /* !=0: sector number, 0: failed - invalid cluster# */
  FATFS *fs,            /* File system object */
  DWORD clust           /* Cluster# to be converted */
)
{
  clust -= 2;
  if (clust >= (fs->max_clust - 2)) return 0;       /* Invalid cluster# */
  return clust * fs->csize + fs->database;
}




/*-----------------------------------------------------------------------*/
/* Move directory pointer to next                                        */
/*-----------------------------------------------------------------------*/

static
BOOL next_dir_entry (   /* TRUE: successful, FALSE: could not move next */
  DIR *dj               /* Pointer to directory object */
)
{
  DWORD clust;
  WORD idx;


  idx = dj->index + 1;
  if ((idx & ((SS(dj->fs) - 1) / 32)) == 0) {        /* Table sector changed? */
    dj->sect++;                                      /* Next sector */
    if (dj->clust == 0) {                            /* In static table */
      if (idx >= dj->fs->n_rootdir) return FALSE;    /* Reached to end of table */
    } else {                                         /* In dynamic table */
      if (((idx / (SS(dj->fs) / 32)) & (dj->fs->csize - 1)) == 0) {  /* Cluster changed? */
        clust = get_cluster(dj->fs, dj->clust);      /* Get next cluster */
        if (clust < 2 || clust >= dj->fs->max_clust) /* Reached to end of table */
          return FALSE;
        dj->clust = clust;                           /* Initialize for new cluster */
        dj->sect = clust2sect(dj->fs, clust);
      }
    }
  }
  dj->index = idx;  /* Lower several bits of dj->index indicates offset in dj->sect */
  return TRUE;
}




/*-----------------------------------------------------------------------*/
/* Get file status from directory entry                                  */
/*-----------------------------------------------------------------------*/

#if _FS_MINIMIZE <= 1
static
void get_fileinfo (     /* No return code */
  FILINFO *finfo,       /* Ptr to store the file information */
  const BYTE *dir       /* Ptr to the directory entry */
)
{
#if 0
  BYTE n, c, a;
  UCHAR *p;

  p = &finfo->fname[0];
  a = _USE_NTFLAG ? dir[DIR_NTres] : 0;   /* NT flag */
  for (n = 0; n < 8; n++) {   /* Convert file name (body) */
    c = dir[n];
    if (c == ' ') break;
    if (c == 0x05) c = 0xE5;
    if (a & 0x08 && c >= 'A' && c <= 'Z') c += 0x20;
    *p++ = c;
  }
  if (dir[8] != ' ') {        /* Convert file name (extension) */
    *p++ = '.';
    for (n = 8; n < 11; n++) {
      c = dir[n];
      if (c == ' ') break;
      if (a & 0x10 && c >= 'A' && c <= 'Z') c += 0x20;
      *p++ = c;
    }
  }
  *p = '\0';
#endif
  finfo->fname[0] = 1;

  finfo->fsize = LD_DWORD(&dir[DIR_FileSize]);  /* Size */
  finfo->clust = ((DWORD)LD_WORD(&dir[DIR_FstClusHI]) << 16)
    | LD_WORD(&dir[DIR_FstClusLO]);            /* Get cluster# */
}
#endif /* _FS_MINIMIZE <= 1 */




/*-----------------------------------------------------------------------*/
/* Load boot record and check if it is a FAT boot record                 */
/*-----------------------------------------------------------------------*/

static const /*PROGMEM*/ UCHAR fat32string[] = "FAT32";

static
BYTE check_fs (     /* 0:The FAT boot record, 1:Valid boot record but not a FAT, 2:Not a boot record or error */
  FATFS *fs,        /* File system object */
  DWORD sect        /* Sector# (lba) to check if it is a FAT boot record or not */
)
{
  if (!move_fs_window(fs, sect))                    /* Load boot record, save off old data in process */
    return 2;
  if (!sect) {
    if (disk_read(FSBUF.data, sect) != RES_OK)  /* Load boot record, if sector 0 */
      return 2;
    FSBUF.sect = 0;
  }
  if (LD_WORD(&FSBUF.data[BS_55AA]) != 0xAA55)      /* Check record signature (always placed at offset 510 even if the sector size is >512) */
    return 2;

  if (!memcmp(&FSBUF.data[BS_FilSysType], fat32string, 3))        /* Check FAT signature */
    return 0;
  if (!memcmp(&FSBUF.data[BS_FilSysType32], fat32string, 5) && !(FSBUF.data[BPB_ExtFlags] & 0x80))
    return 0;

  return 1;
}




/*-----------------------------------------------------------------------*/
/* Mount a drive                                                         */
/*-----------------------------------------------------------------------*/

FRESULT mount_drv(
  BYTE drv,
  FATFS* fs,
  BYTE chk_wp           /* !=0: Check media write protection for write access */
)
{
  DSTATUS stat;
  BYTE fmt, *tbl;
  DWORD bootsect, fatsize, totalsect, maxclust;

  memset(fs, 0, sizeof(FATFS));       /* Clean-up the file system object */
  //fs->drive = LD2PD(drv);             /* Bind the logical drive and a physical drive */
  stat = disk_initialize();           /* Initialize low level disk I/O layer */
  if (stat & STA_NOINIT)              /* Check if the drive is ready */
    return FR_NOT_READY;
#if S_MAX_SIZ > 512                   /* Get disk sector size if needed */
  if (disk_ioctl(fs->drive, GET_SECTOR_SIZE, &SS(fs)) != RES_OK || SS(fs) > S_MAX_SIZ)
    return FR_NO_FILESYSTEM;
#endif
#if !_FS_READONLY
  if (chk_wp && (stat & STA_PROTECT)) /* Check write protection if needed */
    return FR_WRITE_PROTECTED;
#endif
#if _MULTI_PARTITION == 0
  /* Search FAT partition on the drive */
  fmt = check_fs(fs, bootsect = 0);   /* Check sector 0 as an SFD format */
  if (fmt == 1) {                     /* Not a FAT boot record, it may be patitioned */
    /* Check a partition listed in top of the partition table */
    tbl = &FSBUF.data[MBR_Table + LD2PT(drv) * 16]; /* Partition table */
    if (tbl[4]) {                     /* Is the partition existing? */
      bootsect = LD_DWORD(&tbl[8]);   /* Partition offset in LBA */
      fmt = check_fs(fs, bootsect);   /* Check the partition */
    }
  }
#else
  /* Check only the partition that was requested */
  if (LD2PT(drv) == 0) {
    /* Unpartitioned media */
    fmt = check_fs(fs, bootsect = 0);
  } else {
    /* Read MBR */
    fmt = 1;
    if (disk_read(FSBUF.data, 0) != RES_OK)
      goto failed;

    if (LD2PT(drv) < 5) {
      /* Primary partition */
      tbl = &FSBUF.data[MBR_Table + (LD2PT(drv)-1) * 16];
      fmt = 1;
      if (tbl[4]) {
        bootsect = LD_DWORD(&tbl[8]);
        fmt = check_fs(fs, bootsect);
      }
    } else {
      /* Logical drive */
      BYTE i,curr;
      fmt = 1;
      bootsect = 0;
      fatsize = 0;  // Used to store the offset of the first extended part
      curr = LD2PT(drv)-4;
      /* Walk the chain of extended partitions */
      do {
        /* Check for an extended partition */
        for (i=0;i<4;i++) {
          tbl = &FSBUF.data[MBR_Table + i*16];
          if (tbl[4] == 5 || tbl[4] == 0x0f)
            break;
        }
        if (i == 4) {
          fmt = 255;
          goto failed;
        }
        bootsect = fatsize + LD_DWORD(&tbl[8]);

        if (fatsize == 0)
          fatsize = bootsect;

        /* Read the next sector in the partition chain */
        if (disk_read(FSBUF.data, bootsect) != RES_OK)
          goto failed;
      } while (--curr);
      /* Look for the non-extended, non-empty partition entry */
      for (i=0;i<4;i++) {
        tbl = &FSBUF.data[MBR_Table + i*16];
        if (tbl[4] && tbl[4] != 5 && tbl[4] != 0x0f)
          break;
      }
      if (i == 4) {
        /* End of extended partition chain */
        fmt = 255;
        goto failed;
      }
      bootsect = bootsect + LD_DWORD(&tbl[8]);
      fmt = check_fs(fs, bootsect);
    }
  }
 failed:

#endif

  if (fmt || LD_WORD(&FSBUF.data[BPB_BytsPerSec]) != SS(fs)) { /* No valid FAT patition is found */
    /* No file system found */
    return FR_NO_FILESYSTEM;
  }

  /* Initialize the file system object */
  fatsize = LD_WORD(&FSBUF.data[BPB_FATSz16]);      /* Number of sectors per FAT */
  if (!fatsize) fatsize = LD_DWORD(&FSBUF.data[BPB_FATSz32]);
  fs->sects_fat = fatsize;
  fs->n_fats = FSBUF.data[BPB_NumFATs];             /* Number of FAT copies */
  fatsize *= fs->n_fats;                            /* (Number of sectors in FAT area) */
  fs->fatbase = bootsect + LD_WORD(&FSBUF.data[BPB_RsvdSecCnt]); /* FAT start sector (lba) */
  fs->csize = FSBUF.data[BPB_SecPerClus];           /* Number of sectors per cluster */
  fs->n_rootdir = LD_WORD(&FSBUF.data[BPB_RootEntCnt]); /* Nmuber of root directory entries */
  totalsect = LD_WORD(&FSBUF.data[BPB_TotSec16]);   /* Number of sectors on the file system */
  if (!totalsect) totalsect = LD_DWORD(&FSBUF.data[BPB_TotSec32]);
  fs->max_clust = maxclust = (totalsect             /* max_clust = Last cluster# + 1 */
    - LD_WORD(&FSBUF.data[BPB_RsvdSecCnt]) - fatsize - fs->n_rootdir / (SS(fs)/32)
    ) / fs->csize + 2;

  fmt = FS_FAT12;                     /* Determine the FAT sub type */
  if (maxclust >= 0xFF7) fmt = FS_FAT16;
  if (maxclust >= 0xFFF7) fmt = FS_FAT32;
  fs->fs_type = fmt;

  if (fmt == FS_FAT32)
    fs->dirbase = LD_DWORD(&FSBUF.data[BPB_RootClus]);  /* Root directory start cluster */
  else
    fs->dirbase = fs->fatbase + fatsize;            /* Root directory start sector (lba) */
  fs->database = fs->fatbase + fatsize + fs->n_rootdir / (SS(fs)/32);  /* Data start sector (lba) */

#if !_FS_READONLY
  fs->free_clust = 0xFFFFFFFF;
# if _USE_FSINFO
  /* Get fsinfo if needed */
  if (fmt == FS_FAT32) {
    fs->fsi_sector = bootsect + LD_WORD(&FSBUF.data[BPB_FSInfo]);
    //if (disk_read(fs->drive, FSBUF.data, fs->fsi_sector, 1) == RES_OK &&
    if (move_fs_window(fs,fs->fsi_sector) &&
      LD_WORD(&FSBUF.data[BS_55AA]) == 0xAA55 &&
      LD_DWORD(&FSBUF.data[FSI_LeadSig]) == 0x41615252 &&
      LD_DWORD(&FSBUF.data[FSI_StrucSig]) == 0x61417272) {
      fs->last_clust = LD_DWORD(&FSBUF.data[FSI_Nxt_Free]);
      fs->free_clust = LD_DWORD(&FSBUF.data[FSI_Free_Count]);
    }
  }
# endif
#endif
  fs->fs_type = fmt;      /* FAT syb-type */
  //fs->id = ++fsid;                    /* File system mount ID */
  return FR_OK;
}


/*-----------------------------------------------------------------------*/
/* Check if the file/dir object is valid or not                          */
/*-----------------------------------------------------------------------*/

static
FRESULT validate (    /* FR_OK(0): The object is valid, !=0: Invalid */
  const FATFS *fs     /* Pointer to the file system object */
  //,WORD id             /* Member id of the target object to be checked */
)
{
#if 0
  if (!fs || !fs->fs_type /*|| fs->id != id */)
    return FR_INVALID_OBJECT;
  if (disk_status(fs->drive) & STA_NOINIT)
    return FR_NOT_READY;
#endif

  return FR_OK;
}




/*--------------------------------------------------------------------------

   Public Functions

--------------------------------------------------------------------------*/



/*-----------------------------------------------------------------------*/
/* Mount/Unmount a Locical Drive                                         */
/*-----------------------------------------------------------------------*/

FRESULT f_mount (
  BYTE drv,   /* Logical drive number to be mounted/unmounted */
  FATFS *fs   /* Pointer to new file system object (NULL for unmount)*/
)
{
#if _USE_DRIVE_PREFIX != 0
  if (drv >= _LOGICAL_DRIVES) return FR_INVALID_DRIVE;

  if (FatFs[drv]) FatFs[drv]->fs_type = 0;  /* Clear old object */

  FatFs[drv] = fs;      /* Register and clear new object */
#endif
  if (fs) fs->fs_type = 0;

#if _USE_DEFERRED_MOUNT == 0
  return mount_drv(drv,fs,0);
#else
  return FR_OK;
#endif
}





/*-----------------------------------------------------------------------*/
/* Open or Create a File                                                 */
/*-----------------------------------------------------------------------*/

FRESULT l_openfile (
  FATFS *fs,           /* Pointer to file system object */
  FILINFO *fi,         /* Pointer to file info structure of target */
  FIL *fp              /* Pointer to the blank file object */
)
{
  fp->flag = FA_READ;
  fp->org_clust = fi->clust;
  fp->fsize = fi->fsize;
  fp->fptr = 0;
  fp->csect = 1;
  fp->fs = fs;

  return FR_OK;
}



/*-----------------------------------------------------------------------*/
/* Read File                                                             */
/*-----------------------------------------------------------------------*/

FRESULT f_read (
  FIL *fp,      /* Pointer to the file object */
  void *buff,   /* Pointer to data buffer */
  UINT btr      /* Number of bytes to read */
  //UINT *br      /* Pointer to number of bytes read */
)
{
  FRESULT res;
  DWORD clust, sect, remain;
  UINT rcnt, cc;
  BYTE *rbuff = buff;
  FATFS *fs = fp->fs;


  //*br = 0;
  res = validate(fs /*, fp->id*/);                   /* Check validity of the object */
  if (res != FR_OK) return res;
  if (fp->flag & FA__ERROR) return FR_RW_ERROR; /* Check error flag */
  if (!(fp->flag & FA_READ)) return FR_DENIED;  /* Check access mode */
  remain = fp->fsize - fp->fptr;
  if (btr > remain) btr = (UINT)remain;         /* Truncate read count by number of bytes left */

  for ( ;  btr;                                 /* Repeat until all data transferred */
        rbuff += rcnt, fp->fptr += rcnt, /**br += rcnt,*/ btr -= rcnt) {
    if ((fp->fptr & (SS(fs) - 1)) == 0) {       /* On the sector boundary */
      if (--fp->csect) {                        /* Decrement left sector counter */
        sect = fp->curr_sect + 1;               /* Get current sector */
      } else {                                  /* On the cluster boundary, get next cluster */
        clust = (fp->fptr == 0) ?
          fp->org_clust : get_cluster(fs, fp->curr_clust);
        if (clust < 2 || clust >= fs->max_clust)
          goto fr_error;
        fp->curr_clust = clust;                 /* Current cluster */
        sect = clust2sect(fs, clust);           /* Get current sector */
        fp->csect = fs->csize;                  /* Re-initialize the left sector counter */
      }
#if !_FS_READONLY
      if(!move_fp_window(fp,0)) goto fr_error;
#endif
      fp->curr_sect = sect;           /* Update current sector */
      //cc = btr / SS(fs);              /* When left bytes >= SS(fs), */
      if (btr == SS(fs)) {            /* Read maximum contiguous sectors directly */
        cc = 1;
        if (disk_read(rbuff, sect) != RES_OK)
          goto fr_error;
        fp->csect -= (BYTE)(cc - 1);
        fp->curr_sect += cc - 1;
        rcnt = cc * SS(fs);
        continue;
      }
    }
    if(btr) {  /* if we actually have bytes to read in singles, copy them in */
      rcnt = SS(fs) - ((WORD)fp->fptr & (SS(fs) - 1));       /* Copy fractional bytes from file I/O buffer */
      if (rcnt > btr) rcnt = btr;
      if(!move_fp_window(fp,fp->curr_sect)) goto fr_error;   /* are we there or not? */
      memcpy(rbuff, &FPBUF.data[fp->fptr & (SS(fs) - 1)], rcnt);
    }
  }

  return FR_OK;

fr_error: /* Abort this file due to an unrecoverable error */
  fp->flag |= FA__ERROR;
  return FR_RW_ERROR;
}




#if !_FS_READONLY
/*-----------------------------------------------------------------------*/
/* Write File                                                            */
/*-----------------------------------------------------------------------*/

FRESULT f_write (
  FIL *fp,          /* Pointer to the file object */
  const void *buff, /* Pointer to the data to be written */
  UINT btw,         /* Number of bytes to write */
  UINT *bw          /* Pointer to number of bytes written */
)
{
  FRESULT res;
  DWORD clust, sect;
  UINT wcnt, cc;
  const BYTE *wbuff = buff;
  FATFS *fs = fp->fs;


  *bw = 0;
  res = validate(fs /*, fp->id*/);                     /* Check validity of the object */
  if (res != FR_OK) return res;
  if (fp->flag & FA__ERROR) return FR_RW_ERROR;   /* Check error flag */
  if (!(fp->flag & FA_WRITE)) return FR_DENIED;   /* Check access mode */
  if (fp->fsize + btw < fp->fsize) return FR_OK;  /* File size cannot reach 4GB */

  for ( ;  btw;                                   /* Repeat until all data transferred */
    wbuff += wcnt, fp->fptr += wcnt, *bw += wcnt, btw -= wcnt) {
    if ((fp->fptr & (SS(fs) - 1)) == 0) {         /* On the sector boundary */
      if (--fp->csect) {                          /* Decrement left sector counter */
        sect = fp->curr_sect + 1;                 /* Get current sector */
      } else {                                    /* On the cluster boundary, get next cluster */
        if (fp->fptr == 0) {                      /* Is top of the file */
          clust = fp->org_clust;
          if (clust == 0)                         /* No cluster is created yet */
            fp->org_clust = clust = create_chain(fs, 0);    /* Create a new cluster chain */
        } else {                                  /* Middle or end of file */
          clust = create_chain(fs, fp->curr_clust);         /* Trace or streach cluster chain */
        }
        if (clust == 0) break;                    /* Disk full */
        if (clust == 1 || clust >= fs->max_clust) goto fw_error;
        fp->curr_clust = clust;                   /* Current cluster */
        sect = clust2sect(fs, clust);             /* Get current sector */
        fp->csect = fs->csize;                    /* Re-initialize the left sector counter */
      }
      if(!move_fp_window(fp,0)) goto fw_error;
      fp->curr_sect = sect;                       /* Update current sector */
      cc = btw / SS(fs);                          /* When left bytes >= SS(fs), */
      if (cc) {                                   /* Write maximum contiguous sectors directly */
        if (cc > fp->csect) cc = fp->csect;
        if (disk_write(fs->drive, wbuff, sect, (BYTE)cc) != RES_OK)
          goto fw_error;
        fp->csect -= (BYTE)(cc - 1);
        fp->curr_sect += cc - 1;
        wcnt = cc * SS(fs);
        continue;
      }
    }
    if(btw) {
      wcnt = SS(fs) - ((WORD)fp->fptr & (SS(fs) - 1));  /* Copy fractional bytes to file I/O buffer */
      if (wcnt > btw) wcnt = btw;
      if (
#if _USE_1_BUF == 0
      fp->fptr < fp->fsize &&       /* Fill sector buffer with file data if needed */
#endif
      !move_fp_window(fp,fp->curr_sect))
    goto fw_error;
      memcpy(&FPBUF.data[fp->fptr & (SS(fs) - 1)], wbuff, wcnt);
      FPBUF.dirty=TRUE;
    }
  }

  if (fp->fptr > fp->fsize) fp->fsize = fp->fptr; /* Update file size if needed */
  fp->flag |= FA__WRITTEN;                        /* Set file changed flag */
  return FR_OK;

fw_error: /* Abort this file due to an unrecoverable error */
  fp->flag |= FA__ERROR;
  return FR_RW_ERROR;
}




/*-----------------------------------------------------------------------*/
/* Synchronize the file object                                           */
/*-----------------------------------------------------------------------*/

FRESULT f_sync (
  FIL *fp   /* Pointer to the file object */
)
{
  FRESULT res;
  DWORD tim;
  BYTE *dir;
  FATFS *fs = fp->fs;


  res = validate(fs /*, fp->id*/);       /* Check validity of the object */
  if (res == FR_OK) {
    if (fp->flag & FA__WRITTEN) {   /* Has the file been written? */
      /* Write back data buffer if needed */
      if(!move_fp_window(fp,0)) return FR_RW_ERROR;
      /* Update the directory entry */
      if (!move_fs_window(fs, fp->dir_sect))
        return FR_RW_ERROR;
      dir = fp->dir_ptr;
      dir[DIR_Attr] |= AM_ARC;                        /* Set archive bit */
      ST_DWORD(&dir[DIR_FileSize], fp->fsize);        /* Update file size */
      ST_WORD(&dir[DIR_FstClusLO], fp->org_clust);    /* Update start cluster */
      ST_WORD(&dir[DIR_FstClusHI], fp->org_clust >> 16);
      tim = get_fattime();                            /* Updated time */
      ST_DWORD(&dir[DIR_WrtTime], tim);
      fp->flag &= (BYTE)~FA__WRITTEN;
      res = sync(fs);
    }
  }
  return res;
}

#endif /* !_FS_READONLY */




/*-----------------------------------------------------------------------*/
/* Close File                                                            */
/*-----------------------------------------------------------------------*/

FRESULT f_close (
  FIL *fp   /* Pointer to the file object to be closed */
)
{
  FRESULT res;


#if !_FS_READONLY
  res = f_sync(fp);
#else
  res = validate(fp->fs /*, fp->id*/);
#endif
  if (res == FR_OK) fp->fs = NULL;
  return res;
}




#if _FS_MINIMIZE <= 2
/*-----------------------------------------------------------------------*/
/* Seek File R/W Pointer                                                 */
/*-----------------------------------------------------------------------*/

FRESULT f_lseek (
  FIL *fp,    /* Pointer to the file object */
  DWORD ofs   /* File pointer from top of file */
)
{
  FRESULT res;
  DWORD clust, csize;
  CHAR csect;
  FATFS *fs;
  fs = fp->fs;

  res = validate(fs /*, fp->id*/);          /* Check validity of the object */
  if (res != FR_OK) return res;
  if (fp->flag & FA__ERROR) return FR_RW_ERROR;
  if (!move_fp_window(fp,0)) goto fk_error; /* JLB not sure I need this. */
  if (ofs > fp->fsize)                      /* In read-only mode, clip offset with the file size */
    ofs = fp->fsize;

  /* Move file R/W pointer if needed */
  if (ofs) {
    clust = fp->org_clust;    /* Get start cluster */
    if (clust) {                /* If the file has a cluster chain, it can be followed */
      csize = (DWORD)fs->csize * SS(fs);    /* Cluster size in unit of byte */
      for (;;) {                                  /* Loop to skip leading clusters */
        fp->curr_clust = clust;                   /* Update current cluster */
        if (ofs <= csize) break;
#if !_FS_READONLY
        if (fp->flag & FA_WRITE)                  /* Check if in write mode or not */
          clust = create_chain(fs, clust);        /* Force streached if in write mode */
        else
#endif
          clust = get_cluster(fs, clust);         /* Only follow cluster chain if not in write mode */
        if (clust == 0) {                         /* Stop if could not follow the cluster chain */
          ofs = csize; break;
        }
        if (clust == 1 || clust >= fs->max_clust) goto fk_error;
        fp->fptr += csize;                        /* Update R/W pointer */
        ofs -= csize;
      }
      csect = (CHAR)((ofs - 1) / SS(fs));         /* Sector offset in the cluster */
      fp->curr_sect = clust2sect(fs, fp->curr_clust) + csect;  /* Current sector */
      fp->csect = fs->csize - csect;   /* Left sector counter in the cluster */
      fp->fptr += ofs;                            /* Update file R/W pointer */
    }
  }

  return FR_OK;

fk_error: /* Abort this file due to an unrecoverable error */
  fp->flag |= FA__ERROR;
  return FR_RW_ERROR;
}




#if _FS_MINIMIZE <= 1
/*-----------------------------------------------------------------------*/
/* Create a directroy object                                             */
/*-----------------------------------------------------------------------*/

/**
 * l_openroot - open the root directory
 * @fs     : Pointer to the FATFS structure of the target file system
 * @dj     : Pointer to the DIR structure to be filled
 *
 * This functions works like f_opendir, but instead of a path the directory
 * to be opened the root directory is opened.
 * Always returns FR_OK.
 */
FRESULT l_openroot(FATFS* fs, DIR *dj) {
  dj->fs = fs;
  //dj->id = fs->id;

  /* Open the root directory */
  DWORD cluster = fs->dirbase;
  if (fs->fs_type == FS_FAT32) {
    dj->clust = dj->sclust = cluster;
    dj->sect  = clust2sect(fs, cluster);
  } else {
    dj->clust = dj->sclust = 0;
    dj->sect  = cluster;
  }
  dj->index = 0;
  return FR_OK;
}





/*-----------------------------------------------------------------------*/
/* Read Directory Entry in Sequense                                      */
/*-----------------------------------------------------------------------*/


FRESULT f_readdir (
  DIR *dj,           /* Pointer to the directory object */
  FILINFO *finfo     /* Pointer to file information to return */
)
{
  BYTE *dir, c, res;
  FATFS *fs = dj->fs;

  res = validate(fs /*, dj->id*/);         /* Check validity of the object */
  if (res != FR_OK) return res;

  finfo->fname[0] = 0;
  while (dj->sect) {
    if (!move_fs_window(fs, dj->sect))
      return FR_RW_ERROR;
    dir = &FSBUF.data[(dj->index & ((SS(fs) - 1) >> 5)) * 32]; /* pointer to the directory entry */
    c = dir[DIR_Name];
    if (c == 0) break;                /* Has it reached to end of dir? */
    if (c != 0xE5 && !(dir[DIR_Attr] & AM_VOL))        /* Is it a valid entry? */
      get_fileinfo(finfo, dir);

    if (!next_dir_entry(dj)) dj->sect = 0;                /* Next entry */
    if (finfo->fname[0]) break;       /* Found valid entry */
  }

  return FR_OK;
}

#endif /* _FS_MINIMIZE <= 1 */
#endif /* _FS_MINIMIZE <= 2 */
