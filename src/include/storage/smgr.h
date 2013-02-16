/*-------------------------------------------------------------------------
 *
 * smgr.h
 *	  storage manager switch public interface declarations.
 *
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/smgr.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SMGR_H
#define SMGR_H

#include "utils/hsearch.h"
#include "utils/pg_crc.h"
#include "fmgr.h"
#include "storage/block.h"
#include "storage/relfilenode.h"
#include "storage/s_lock.h"
#include "access/xlogdefs.h"

/*
 * smgr.c maintains a table of SMgrRelation objects, which are essentially
 * cached file handles.  An SMgrRelation is created (if not already present)
 * by smgropen(), and destroyed by smgrclose().  Note that neither of these
 * operations imply I/O, they just create or destroy a hashtable entry.
 * (But smgrclose() may release associated resources, such as OS-level file
 * descriptors.)
 *
 * An SMgrRelation may have an "owner", which is just a pointer to it from
 * somewhere else; smgr.c will clear this pointer if the SMgrRelation is
 * closed.	We use this to avoid dangling pointers from relcache to smgr
 * without having to make the smgr explicitly aware of relcache.  There
 * can't be more than one "owner" pointer per SMgrRelation, but that's
 * all we need.
 *
 * SMgrRelations that do not have an "owner" are considered to be transient,
 * and are deleted at end of transaction.
 */
typedef struct SMgrRelationData
{
	/* rnode is the hashtable lookup key, so it must be first! */
	RelFileNodeBackend smgr_rnode;		/* relation physical identifier */

	/* pointer to owning pointer, or NULL if none */
	struct SMgrRelationData **smgr_owner;

	/*
	 * These next three fields are not actually used or manipulated by smgr,
	 * except that they are reset to InvalidBlockNumber upon a cache flush
	 * event (in particular, upon truncation of the relation).	Higher levels
	 * store cached state here so that it will be reset when truncation
	 * happens.  In all three cases, InvalidBlockNumber means "unknown".
	 */
	BlockNumber smgr_targblock; /* current insertion target block */
	BlockNumber smgr_fsm_nblocks;		/* last known size of fsm fork */
	BlockNumber smgr_vm_nblocks;	/* last known size of vm fork */

	/* additional public fields may someday exist here */

	/*
	 * Fields below here are intended to be private to smgr.c and its
	 * submodules.	Do not touch them from elsewhere.
	 */
	int			smgr_which;		/* storage manager selector */

	/* for md.c; NULL for forks that are not open */
	struct _MdfdVec *md_fd[MAX_FORKNUM + 1];

	/* if unowned, list link in list of all unowned SMgrRelations */
	struct SMgrRelationData *next_unowned_reln;
} SMgrRelationData;

typedef SMgrRelationData *SMgrRelation;

#define SmgrIsTemp(smgr) \
	RelFileNodeBackendIsTemp((smgr)->smgr_rnode)




typedef struct RelName
{
	char filename[32];
}RelName;

typedef struct RelLastBlock
{
	RelName rel_name;
	BlockNumber last_block_num;
	struct timeval tv;
}RelLastBlockData;
typedef RelLastBlockData *RelLastBlock;

#define LASTBLOCKHASHSIZE 100


typedef struct BlockTag
{
	RelFileNode rnode;
	ForkNumber forkno;
	BlockNumber blkno;
}BlockTag;

typedef struct BlockLSNData
{
	BlockTag blk_tag;
	XLogRecPtr lsn;
}BlockLSNData;
typedef BlockLSNData *BlockLSN;

#define BLOCKLSNHASHSIZE (1 << 20)
#define BlockInfo "global/blockinfo"


typedef struct XLogApplyData
{
	XLogRecPtr apply;
}XLogApplyData;
typedef XLogApplyData *XLogApply;


extern HTAB *LastBlockHash;
extern HTAB *BlockLSNHash;
extern FILE *BlockInfoFile;
extern	XLogApply xlog_apply;

extern void smgrinit(void);
extern SMgrRelation smgropen(RelFileNode rnode, BackendId backend);
extern bool smgrexists(SMgrRelation reln, ForkNumber forknum);
extern void smgrsetowner(SMgrRelation *owner, SMgrRelation reln);
extern void smgrclose(SMgrRelation reln);
extern void smgrcloseall(void);
extern void smgrclosenode(RelFileNodeBackend rnode);
extern void smgrcreate(SMgrRelation reln, ForkNumber forknum, bool isRedo);
extern void smgrdounlink(SMgrRelation reln, bool isRedo);
extern void smgrdounlinkfork(SMgrRelation reln, ForkNumber forknum, bool isRedo);
extern void smgrextend(SMgrRelation reln, ForkNumber forknum,
		   BlockNumber blocknum, char *buffer, bool skipFsync);
extern void smgrprefetch(SMgrRelation reln, ForkNumber forknum,
			 BlockNumber blocknum);
extern void smgrread(SMgrRelation reln, ForkNumber forknum,
		 BlockNumber blocknum, char *buffer);
extern void smgrwrite(SMgrRelation reln, ForkNumber forknum,
		  BlockNumber blocknum, char *buffer, bool skipFsync);
extern BlockNumber smgrnblocks(SMgrRelation reln, ForkNumber forknum);
extern void smgrtruncate(SMgrRelation reln, ForkNumber forknum,
			 BlockNumber nblocks);
extern void smgrimmedsync(SMgrRelation reln, ForkNumber forknum);
extern void smgrpreckpt(void);
extern void smgrsync(void);
extern void smgrpostckpt(void);
extern void AtEOXact_SMgr(void);

extern bool is_primary_mode();
extern bool is_standby_mode();
extern bool is_tracked(char *filename);

extern HTAB* init_last_block_hash();
extern void modify_last_block_hash(char *filename, BlockNumber blocknum, HASHACTION action);
extern BlockNumber get_last_block_hash(char *filename, HASHACTION action);

extern HTAB* init_block_lsn_hash();
extern void update_block_lsn(RelFileNode rnode, ForkNumber forknum, BlockNumber blocknum,
								XLogRecPtr lsn, HASHACTION action);
extern XLogRecPtr get_block_lsn(RelFileNode rnode, ForkNumber forknum, BlockNumber blocknum);
extern XLogRecPtr get_standby_block_lsn(RelFileNode rnode, ForkNumber forknum, BlockNumber blocknum);
extern void network_sync(char* buffer, RelFileNode rnode, ForkNumber forknum, BlockNumber blocknum, XLogRecPtr lsn, bool flush);
extern void get_block_info();
extern char* get_block(RelFileNode rnode, ForkNumber forknum, BlockNumber blocknum, XLogRecPtr lsn);
extern void clean_standby_resources();
extern Size BlockLSNSize();
/* internals: move me elsewhere -- ay 7/94 */

/* in md.c */
extern void mdinit(void);
extern void mdclose(SMgrRelation reln, ForkNumber forknum);
extern void mdcreate(SMgrRelation reln, ForkNumber forknum, bool isRedo);
extern bool mdexists(SMgrRelation reln, ForkNumber forknum);
extern void mdunlink(RelFileNodeBackend rnode, ForkNumber forknum, bool isRedo);
extern void mdextend(SMgrRelation reln, ForkNumber forknum,
		 BlockNumber blocknum, char *buffer, bool skipFsync);
extern void mdprefetch(SMgrRelation reln, ForkNumber forknum,
		   BlockNumber blocknum);
extern void mdread(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
	   char *buffer);
extern void mdwrite(SMgrRelation reln, ForkNumber forknum,
		BlockNumber blocknum, char *buffer, bool skipFsync);
extern BlockNumber mdnblocks(SMgrRelation reln, ForkNumber forknum);
extern void mdtruncate(SMgrRelation reln, ForkNumber forknum,
		   BlockNumber nblocks);
extern void mdimmedsync(SMgrRelation reln, ForkNumber forknum);
extern void mdpreckpt(void);
extern void mdsync(void);
extern void mdpostckpt(void);

extern void SetForwardFsyncRequests(void);
extern void RememberFsyncRequest(RelFileNode rnode, ForkNumber forknum,
					 BlockNumber segno);
extern void ForgetRelationFsyncRequests(RelFileNode rnode, ForkNumber forknum);
extern void ForgetDatabaseFsyncRequests(Oid dbid);

/* smgrtype.c */
extern Datum smgrout(PG_FUNCTION_ARGS);
extern Datum smgrin(PG_FUNCTION_ARGS);
extern Datum smgreq(PG_FUNCTION_ARGS);
extern Datum smgrne(PG_FUNCTION_ARGS);

#endif   /* SMGR_H */
