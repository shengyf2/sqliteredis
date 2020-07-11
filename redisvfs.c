#ifdef STATIC_REDISVFS
#include "sqlite3.h"
#else
#include <sqlite3ext.h>
SQLITE_EXTENSION_INIT1
#endif // STATIC_REDISVFS

#include <stdio.h>
#include <string.h>
#include <hiredis.h>
#include "redisvfs.h"

// Debugging
#define DLOG(fmt,...) fprintf(stderr, "%s[%d]: %s: " fmt "\n", __FILE__, __LINE__, __func__, ##__VA_ARGS__),fflush(stderr)
//
// Reference the parent VFS that we reference in pAppData
#define PARENT_VFS(vfs) ((sqlite3_vfs *)(vfs->pAppData))

/* keyspace helpers */

/*
 * File API implementation
 *
 * These all work on a specific open file. pointers to these
 * are rolled up in an sqlite3_io_methods struct, which is referrenced
 * by each sqlite3_file * generated by redisvfs_open
 */

int redisvfs_close(sqlite3_file *fp) {
	DLOG("stub");
	RedisFile *rf = (RedisFile *)fp;
	if (rf->redisctx) {
		redisFree(rf->redisctx);
		rf->redisctx = 0;
	}
	return SQLITE_OK;
}

int redisvfs_read(sqlite3_file *fp, void *buf, int iAmt, sqlite3_int64 iOfst) {
	RedisFile *rf = (RedisFile *)fp;
	DLOG("(fp=%p prefix='%s' offset=%lld len=%d)", rf, rf->keyprefix, iOfst, iAmt);
	return !SQLITE_OK; // FIXME: Implement
}
int redisvfs_write(sqlite3_file *fp, const void *buf, int iAmt, sqlite3_int64 iOfst) {
	DLOG("stub");
	return !SQLITE_OK; // FIXME: Implement
}
int redisvfs_truncate(sqlite3_file *fp, sqlite3_int64 size) {
	DLOG("stub");
	return !SQLITE_OK; // FIXME: Implement
}
int redisvfs_sync(sqlite3_file *fp, int flags) {
	DLOG("stub");
	// Noop. All our writes are synchronous.
	// TODO: We can put a hard barrier in here to redis and block if we really want
	return SQLITE_OK;
}
int redisvfs_fileSize(sqlite3_file *fp, sqlite3_int64 *pSize) {
	DLOG("stub");
	return !SQLITE_OK; // FIXME: Implement
}
int redisvfs_lock(sqlite3_file *fp, int eLock) {
	DLOG("stub");
	return !SQLITE_OK; // FIXME: Implement
}
int redisvfs_unlock(sqlite3_file *fp, int eLock) {
	DLOG("stub");
	return !SQLITE_OK; // FIXME: Implement
}
int redisvfs_checkReservedLock(sqlite3_file *fp, int *pResOut) {
	DLOG("stub");
	return !SQLITE_OK; // FIXME: Implement
}
int redisvfs_fileControl(sqlite3_file *fp, int op, void *pArg) {
	DLOG("stub");
	return !SQLITE_OK; // FIXME: Implement
}
int redisvfs_sectorSize(sqlite3_file *fp) {
	DLOG("stub");
	return REDISVFS_BLOCKSIZE;
}
int redisvfs_deviceCharacteristics(sqlite3_file *fp) {
	DLOG("entry");
	// Describe ordering and consistency guarantees that we
	// can provide.  See sqlite3.h
	// TODO implement SQLITE_IOCAP_BATCH_ATOMIC
	// TODO: If we remove SQLITE_IOCAP_ATOMIC and replace with caveated
	// atomic op flags, we can remove transactions with redis entirely
	return ( SQLITE_IOCAP_ATOMIC | SQLITE_IOCAP_SAFE_APPEND |
		SQLITE_IOCAP_SEQUENTIAL | SQLITE_IOCAP_POWERSAFE_OVERWRITE |
		SQLITE_IOCAP_UNDELETABLE_WHEN_OPEN );
}

#if 0
/* Methods above are valid for version 1 */
int redisvfs_shmMap(sqlite3_file *fp, int iPg, int pgsz, int, void volatile **pp);
int redisvfs_shmLock(sqlite3_file *fp, int offset, int n, int flags);
void redisvfs_shmBarrier(sqlite3_file *fp);
int redisvfs_shmUnmap(sqlite3_file *fp, int deleteFlag);
/* Methods above are valid for version 2 */
int redisvfs_fetch(sqlite3_file *fp, sqlite3_int64 iOfst, int iAmt, void **pp);
int redisvfs_unfetch(sqlite3_file *fp, sqlite3_int64 iOfst, void *p);
/* Methods above are valid for version 3 */
#endif

/* references to file API implementation. Added to each RedisFile *
 */
const sqlite3_io_methods redisvfs_io_methods = {
	1,
	redisvfs_close,
	redisvfs_read,
	redisvfs_write,
	redisvfs_truncate,
	redisvfs_sync,
	redisvfs_fileSize,
	redisvfs_lock,
	redisvfs_unlock,
	redisvfs_checkReservedLock,
	redisvfs_fileControl,
	redisvfs_sectorSize,
	redisvfs_deviceCharacteristics,
};


/*
 * VFS API implementation
 *
 * Stuff that isn't just for a specific already-open file.
 * This all gets referenced by our sqlite3_vfs *
 */

int redisvfs_open(sqlite3_vfs *vfs, const char *zName, sqlite3_file *f, int flags, int *pOutFlags) {
DLOG("(zName='%s',flags=%d)", zName,flags);
	if (!(flags & SQLITE_OPEN_MAIN_DB)) {
		return SQLITE_CANTOPEN;
	}

	//hardcode hostname and port. for now. grab from database URI later
	const char *hostname = REDISVFS_DEFAULT_HOST;
	int port = REDISVFS_DEFAULT_PORT;


	RedisFile *rf = (RedisFile *)f;
	memset(rf, 0, sizeof(RedisFile));
	rf->base.pMethods = &redisvfs_io_methods;

	rf->keyprefixlen = strnlen(zName, REDISVFS_MAX_PREFIXLEN+1);
	if (rf->keyprefixlen > REDISVFS_MAX_PREFIXLEN) {
DLOG("key prefix ('filename') length too long");
		return SQLITE_CANTOPEN;
	}
	rf->keyprefix = zName;  // Guaranteed to be unchanged until after xClose(*rf)
DLOG("key prefix: '%s'", rf->keyprefix);

	rf->redisctx = redisConnect(hostname,port);
	if (!(rf->redisctx) || rf->redisctx->err) {
		if (rf->redisctx)
		 	fprintf(stderr, "%s: Error: %s\n", __func__, rf->redisctx->errstr);
		return SQLITE_CANTOPEN;
	}


	return SQLITE_OK;
}

int redisvfs_delete(sqlite3_vfs *vfs, const char *zName, int syncDir) {
DLOG("(zName='%s',syncDir=%d)",  zName, syncDir);
	// FIXME: Can implement
	return SQLITE_IOERR_DELETE;
}
int redisvfs_access(sqlite3_vfs *vfs, const char *zName, int flags, int *pResOut) {
DLOG("(zName='%s', flags=%d)", zName, flags);
	if (pResOut != 0)
		*pResOut = 0;
	return SQLITE_OK;
}
int redisvfs_fullPathname(sqlite3_vfs *vfs, const char *zName, int nOut, char *zOut) {
DLOG("(zName='%s',nOut=%d)", zName,nOut);
	sqlite3_snprintf(nOut, zOut, "%s", zName); // effectively strcpy with sqlite3 mm
	return SQLITE_OK;
}

//
// These we don't implement but just pass through to the existing default VFS
// As they are more OS abstraction than FS abstraction it doesn't affect us
//
// Note: Turns out that you really need to pass the parent VFS struct referennce into it's own
//       calls not our VFS struct ref. This is obvious in retrospect.
//
#define VFS_SHIM_CALL(_callname,_vfs,...) \
	DLOG("%s->" #_callname,PARENT_VFS(_vfs)->zName), \
	PARENT_VFS(_vfs)->_callname(PARENT_VFS(_vfs), __VA_ARGS__)

void * redisvfs_dlOpen(sqlite3_vfs *vfs, const char *zFilename) {
	return VFS_SHIM_CALL(xDlOpen, vfs, zFilename);
}
void redisvfs_dlError(sqlite3_vfs *vfs, int nByte, char *zErrMsg) {
	VFS_SHIM_CALL(xDlError, vfs, nByte, zErrMsg);
}
void (* redisvfs_dlSym(sqlite3_vfs *vfs, void *pHandle, const char *zSymbol))(void) {
	return VFS_SHIM_CALL(xDlSym, vfs, pHandle, zSymbol);
}
void redisvfs_dlClose(sqlite3_vfs *vfs, void *pHandle) {
	VFS_SHIM_CALL(xDlClose, vfs, pHandle);
}
int redisvfs_randomness(sqlite3_vfs *vfs, int nByte, char *zOut) {
	return VFS_SHIM_CALL(xRandomness, vfs, nByte, zOut);
}
int redisvfs_sleep(sqlite3_vfs *vfs, int microseconds) {
	return VFS_SHIM_CALL(xSleep, vfs, microseconds);
}
int redisvfs_currentTime(sqlite3_vfs *vfs, double *prNow) {
	return VFS_SHIM_CALL(xCurrentTime, vfs, prNow);
}
int redisvfs_getLastError(sqlite3_vfs *vfs, int nBuf, char *zBuf) {
	// FIXME: Implement. Called after  another call fails.
	return VFS_SHIM_CALL(xGetLastError, vfs, nBuf, zBuf);
}
int redisvfs_currentTimeInt64(sqlite3_vfs *vfs, sqlite3_int64 *piNow) {
	return VFS_SHIM_CALL(xCurrentTimeInt64, vfs, piNow);
}

/* VFS object for sqlite3 */
sqlite3_vfs redis_vfs = {
	2, 0, 1024, 0, /* iVersion, szOzFile, mxPathname, pNext */
	"redisvfs", 0,  /* zName, pAppData */
	redisvfs_open,
	redisvfs_delete,
	redisvfs_access,
	redisvfs_fullPathname,
	redisvfs_dlOpen,
	redisvfs_dlError,
	redisvfs_dlSym,
	redisvfs_dlClose,
	redisvfs_randomness,
	redisvfs_sleep,
	redisvfs_currentTime,
	redisvfs_getLastError,
	redisvfs_currentTimeInt64
};


/* Setup VFS structures and initialise */
int redisvfs_register() {
	int ret;

	//FIXME Move out of here. Can be evaluated ompiletime
	redis_vfs.szOsFile = sizeof(RedisFile); 

	// Get the existing default vfs and pilfer it's OS abstrations
	// This will normally work as long as the (previously) default
	// vfs is not unloaded.
	//
	// If the existing default vfs is trying to do the same thing
	// things may get weird, but sqlite3 concrete implementations
	// out of the box will not do that (only vfs shim layers like vfslog)
	//
	//
	sqlite3_vfs *defaultVFS = sqlite3_vfs_find(0);
	if (defaultVFS == 0)
		return SQLITE_NOLFS;

	// Use our pAppData opaque pointer to store a reference to the
	// underlying VFS.
	redis_vfs.pAppData = (void *)defaultVFS;

	// Register outselves as the new default
	ret = sqlite3_vfs_register(&redis_vfs, 1);
	if (ret != SQLITE_OK) {
		fprintf(stderr, "redisvfsinit could not register itself\n");
		return ret;
	}

	return SQLITE_OK;
}


#ifndef STATIC_REDISVFS

#ifdef _WIN32
__declspec(dllexport)
#endif

/* If we are compiling as an sqlite3 extension make a module load entrypoint
 *
 * sqlite3_SONAME_init is a well-known symbol
 */
int sqlite3_redisvfs_init(sqlite3 *db, char **pzErrMsg, const sqlite3_api_routines *pApi) {
	int ret;

	SQLITE_EXTENSION_INIT2(pApi);
	ret = redisvfs_register();
	return (ret == SQLITE_OK) ? SQLITE_OK_LOAD_PERMANENTLY : ret;
}

#endif
