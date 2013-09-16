/*-------------------------------------------------------------------------
 *
 * smgr.c
 *	  public interface routines to storage manager switch.
 *
 *	  All file system operations in POSTGRES dispatch through these
 *	  routines.
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/smgr/smgr.c
 *
 *-------------------------------------------------------------------------
 */
#include <unistd.h>
#include "postgres.h"

#include "commands/tablespace.h"
#include "storage/bufmgr.h"
#include "storage/ipc.h"
#include "storage/smgr.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "replication/walsender.h"
#include "catalog/pg_class.h"
#include "libpq/libpq.h"
#include "miscadmin.h"
#include "replication/walsender_private.h"
#include "access/xlogutils.h"

#include <stdlib.h> /* for exit() */
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h> /* memset() */
#include <sys/time.h> /* select() */
/*
 * This struct of function pointers defines the API between smgr.c and
 * any individual storage manager module.  Note that smgr subfunctions are
 * generally expected to report problems via elog(ERROR).  An exception is
 * that smgr_unlink should use elog(WARNING), rather than erroring out,
 * because we normally unlink relations during post-commit/abort cleanup,
 * and so it's too late to raise an error.  Also, various conditions that
 * would normally be errors should be allowed during bootstrap and/or WAL
 * recovery --- see comments in md.c for details.
 */
typedef struct f_smgr
{
	void		(*smgr_init) (void);	/* may be NULL */
	void		(*smgr_shutdown) (void);		/* may be NULL */
	void		(*smgr_close) (SMgrRelation reln, ForkNumber forknum);
	void		(*smgr_create) (SMgrRelation reln, ForkNumber forknum,
											bool isRedo);
	bool		(*smgr_exists) (SMgrRelation reln, ForkNumber forknum);
	void		(*smgr_unlink) (RelFileNodeBackend rnode, ForkNumber forknum,
											bool isRedo);
	void		(*smgr_extend) (SMgrRelation reln, ForkNumber forknum,
						 BlockNumber blocknum, char *buffer, bool skipFsync);
	void		(*smgr_prefetch) (SMgrRelation reln, ForkNumber forknum,
											  BlockNumber blocknum);
	void		(*smgr_read) (SMgrRelation reln, ForkNumber forknum,
										  BlockNumber blocknum, char *buffer);
	void		(*smgr_write) (SMgrRelation reln, ForkNumber forknum,
						 BlockNumber blocknum, char *buffer, bool skipFsync);
	BlockNumber (*smgr_nblocks) (SMgrRelation reln, ForkNumber forknum);
	void		(*smgr_truncate) (SMgrRelation reln, ForkNumber forknum,
											  BlockNumber nblocks);
	void		(*smgr_immedsync) (SMgrRelation reln, ForkNumber forknum);
	void		(*smgr_pre_ckpt) (void);		/* may be NULL */
	void		(*smgr_sync) (void);	/* may be NULL */
	void		(*smgr_post_ckpt) (void);		/* may be NULL */
} f_smgr;


static int req_len = sizeof(FlushRequest);

static const f_smgr smgrsw[] = {
	/* magnetic disk */
	{mdinit, NULL, mdclose, mdcreate, mdexists, mdunlink, mdextend,
		mdprefetch, mdread, mdwrite, mdnblocks, mdtruncate, mdimmedsync,
		mdpreckpt, mdsync, mdpostckpt
	}
};

static const int NSmgr = lengthof(smgrsw);


/*
 * Each backend has a hashtable that stores all extant SMgrRelation objects.
 * In addition, "unowned" SMgrRelation objects are chained together in a list.
 */
#define flush_req_port 3552
static HTAB *SMgrRelationHash = NULL;

static SMgrRelation first_unowned_reln = NULL;

static int primary_s = -1;
static int standby_s = -1;
HTAB *LastBlockHash = NULL;
HTAB *BlockLSNHash = NULL;
XLogApply xlog_apply = NULL;

/* local function prototypes */
static void smgrshutdown(int code, Datum arg);
static void remove_from_unowned_list(SMgrRelation reln);


/*
 *	smgrinit(), smgrshutdown() -- Initialize or shut down storage
 *								  managers.
 *
 * Note: smgrinit is called during backend startup (normal or standalone
 * case), *not* during postmaster start.  Therefore, any resources created
 * here or destroyed in smgrshutdown are backend-local.
 */
void
smgrinit(void)
{
	int			i;

	for (i = 0; i < NSmgr; i++)
	{
		if (smgrsw[i].smgr_init)
			(*(smgrsw[i].smgr_init)) ();
	}

	/* register the shutdown proc */
	on_proc_exit(smgrshutdown, 0);
}

/*
 * on_proc_exit hook for smgr cleanup during backend shutdown
 */
static void
smgrshutdown(int code, Datum arg)
{
	int			i;

	for (i = 0; i < NSmgr; i++)
	{
		if (smgrsw[i].smgr_shutdown)
			(*(smgrsw[i].smgr_shutdown)) ();
	}
}

/*
 *	smgropen() -- Return an SMgrRelation object, creating it if need be.
 *
 *		This does not attempt to actually open the underlying file.
 */
SMgrRelation
smgropen(RelFileNode rnode, BackendId backend)
{
	RelFileNodeBackend brnode;
	SMgrRelation reln;
	bool		found;

	if (SMgrRelationHash == NULL)
	{
		/* First time through: initialize the hash table */
		HASHCTL		ctl;

		MemSet(&ctl, 0, sizeof(ctl));
		ctl.keysize = sizeof(RelFileNodeBackend);
		ctl.entrysize = sizeof(SMgrRelationData);
		ctl.hash = tag_hash;
		SMgrRelationHash = hash_create("smgr relation table", 400,
									   &ctl, HASH_ELEM | HASH_FUNCTION);
		first_unowned_reln = NULL;
	}

	/* Look up or create an entry */
	brnode.node = rnode;
	brnode.backend = backend;
	reln = (SMgrRelation) hash_search(SMgrRelationHash,
									  (void *) &brnode,
									  HASH_ENTER, &found);

	/* Initialize it if not present before */
	if (!found)
	{
		int			forknum;

		/* hash_search already filled in the lookup key */
		reln->smgr_owner = NULL;
		reln->smgr_targblock = InvalidBlockNumber;
		reln->smgr_fsm_nblocks = InvalidBlockNumber;
		reln->smgr_vm_nblocks = InvalidBlockNumber;
		reln->smgr_which = 0;	/* we only have md.c at present */

		/* mark it not open */
		for (forknum = 0; forknum <= MAX_FORKNUM; forknum++)
			reln->md_fd[forknum] = NULL;

		/* place it at head of unowned list (to make smgrsetowner cheap) */
		reln->next_unowned_reln = first_unowned_reln;
		first_unowned_reln = reln;
	}

	return reln;
}

/*
 * smgrsetowner() -- Establish a long-lived reference to an SMgrRelation object
 *
 * There can be only one owner at a time; this is sufficient since currently
 * the only such owners exist in the relcache.
 */
void
smgrsetowner(SMgrRelation *owner, SMgrRelation reln)
{
	/* We don't currently support "disowning" an SMgrRelation here */
	Assert(owner != NULL);

	/*
	 * First, unhook any old owner.  (Normally there shouldn't be any, but it
	 * seems possible that this can happen during swap_relation_files()
	 * depending on the order of processing.  It's ok to close the old
	 * relcache entry early in that case.)
	 *
	 * If there isn't an old owner, then the reln should be in the unowned
	 * list, and we need to remove it.
	 */
	if (reln->smgr_owner)
		*(reln->smgr_owner) = NULL;
	else
		remove_from_unowned_list(reln);

	/* Now establish the ownership relationship. */
	reln->smgr_owner = owner;
	*owner = reln;
}

/*
 * remove_from_unowned_list -- unlink an SMgrRelation from the unowned list
 *
 * If the reln is not present in the list, nothing happens.  Typically this
 * would be caller error, but there seems no reason to throw an error.
 *
 * In the worst case this could be rather slow; but in all the cases that seem
 * likely to be performance-critical, the reln being sought will actually be
 * first in the list.  Furthermore, the number of unowned relns touched in any
 * one transaction shouldn't be all that high typically.  So it doesn't seem
 * worth expending the additional space and management logic needed for a
 * doubly-linked list.
 */
static void
remove_from_unowned_list(SMgrRelation reln)
{
	SMgrRelation *link;
	SMgrRelation cur;

	for (link = &first_unowned_reln, cur = *link;
		 cur != NULL;
		 link = &cur->next_unowned_reln, cur = *link)
	{
		if (cur == reln)
		{
			*link = cur->next_unowned_reln;
			cur->next_unowned_reln = NULL;
			break;
		}
	}
}

/*
 *	smgrexists() -- Does the underlying file for a fork exist?
 */
bool
smgrexists(SMgrRelation reln, ForkNumber forknum)
{
	return (*(smgrsw[reln->smgr_which].smgr_exists)) (reln, forknum);
}

/*
 *	smgrclose() -- Close and delete an SMgrRelation object.
 */
void
smgrclose(SMgrRelation reln)
{
	SMgrRelation *owner;
	ForkNumber	forknum;

	for (forknum = 0; forknum <= MAX_FORKNUM; forknum++)
		(*(smgrsw[reln->smgr_which].smgr_close)) (reln, forknum);

	owner = reln->smgr_owner;

	if (!owner)
		remove_from_unowned_list(reln);

	if (hash_search(SMgrRelationHash,
					(void *) &(reln->smgr_rnode),
					HASH_REMOVE, NULL) == NULL)
		elog(ERROR, "SMgrRelation hashtable corrupted");

	/*
	 * Unhook the owner pointer, if any.  We do this last since in the remote
	 * possibility of failure above, the SMgrRelation object will still exist.
	 */
	if (owner)
		*owner = NULL;
}

/*
 *	smgrcloseall() -- Close all existing SMgrRelation objects.
 */
void
smgrcloseall(void)
{
	HASH_SEQ_STATUS status;
	SMgrRelation reln;

	/* Nothing to do if hashtable not set up */
	if (SMgrRelationHash == NULL)
		return;

	hash_seq_init(&status, SMgrRelationHash);

	while ((reln = (SMgrRelation) hash_seq_search(&status)) != NULL)
		smgrclose(reln);
}

/*
 *	smgrclosenode() -- Close SMgrRelation object for given RelFileNode,
 *					   if one exists.
 *
 * This has the same effects as smgrclose(smgropen(rnode)), but it avoids
 * uselessly creating a hashtable entry only to drop it again when no
 * such entry exists already.
 */
void
smgrclosenode(RelFileNodeBackend rnode)
{
	SMgrRelation reln;

	/* Nothing to do if hashtable not set up */
	if (SMgrRelationHash == NULL)
		return;

	reln = (SMgrRelation) hash_search(SMgrRelationHash,
									  (void *) &rnode,
									  HASH_FIND, NULL);
	if (reln != NULL)
		smgrclose(reln);
}

/*
 *	smgrcreate() -- Create a new relation.
 *
 *		Given an already-created (but presumably unused) SMgrRelation,
 *		cause the underlying disk file or other storage for the fork
 *		to be created.
 *
 *		If isRedo is true, it is okay for the underlying file to exist
 *		already because we are in a WAL replay sequence.
 */
void
smgrcreate(SMgrRelation reln, ForkNumber forknum, bool isRedo)
{
	/*
	 * Exit quickly in WAL replay mode if we've already opened the file. If
	 * it's open, it surely must exist.
	 */
	if (isRedo && reln->md_fd[forknum] != NULL)
		return;

	/*
	 * We may be using the target table space for the first time in this
	 * database, so create a per-database subdirectory if needed.
	 *
	 * XXX this is a fairly ugly violation of module layering, but this seems
	 * to be the best place to put the check.  Maybe TablespaceCreateDbspace
	 * should be here and not in commands/tablespace.c?  But that would imply
	 * importing a lot of stuff that smgr.c oughtn't know, either.
	 */
	TablespaceCreateDbspace(reln->smgr_rnode.node.spcNode,
							reln->smgr_rnode.node.dbNode,
							isRedo);

	(*(smgrsw[reln->smgr_which].smgr_create)) (reln, forknum, isRedo);
}

/*
 *	smgrdounlink() -- Immediately unlink all forks of a relation.
 *
 *		All forks of the relation are removed from the store.  This should
 *		not be used during transactional operations, since it can't be undone.
 *
 *		If isRedo is true, it is okay for the underlying file(s) to be gone
 *		already.
 *
 *		This is equivalent to calling smgrdounlinkfork for each fork, but
 *		it's significantly quicker so should be preferred when possible.
 */
void
smgrdounlink(SMgrRelation reln, bool isRedo)
{
	RelFileNodeBackend rnode = reln->smgr_rnode;
	int			which = reln->smgr_which;
	ForkNumber	forknum;

	/* Close the forks at smgr level */
	for (forknum = 0; forknum <= MAX_FORKNUM; forknum++)
		(*(smgrsw[which].smgr_close)) (reln, forknum);

	/*
	 * Get rid of any remaining buffers for the relation.  bufmgr will just
	 * drop them without bothering to write the contents.
	 */
	DropRelFileNodeAllBuffers(rnode);

	/*
	 * It'd be nice to tell the stats collector to forget it immediately, too.
	 * But we can't because we don't know the OID (and in cases involving
	 * relfilenode swaps, it's not always clear which table OID to forget,
	 * anyway).
	 */

	/*
	 * Send a shared-inval message to force other backends to close any
	 * dangling smgr references they may have for this rel.  We should do this
	 * before starting the actual unlinking, in case we fail partway through
	 * that step.  Note that the sinval message will eventually come back to
	 * this backend, too, and thereby provide a backstop that we closed our
	 * own smgr rel.
	 */
	CacheInvalidateSmgr(rnode);

	/*
	 * Delete the physical file(s).
	 *
	 * Note: smgr_unlink must treat deletion failure as a WARNING, not an
	 * ERROR, because we've already decided to commit or abort the current
	 * xact.
	 */
	(*(smgrsw[which].smgr_unlink)) (rnode, InvalidForkNumber, isRedo);
}

/*
 *	smgrdounlinkfork() -- Immediately unlink one fork of a relation.
 *
 *		The specified fork of the relation is removed from the store.  This
 *		should not be used during transactional operations, since it can't be
 *		undone.
 *
 *		If isRedo is true, it is okay for the underlying file to be gone
 *		already.
 */
void
smgrdounlinkfork(SMgrRelation reln, ForkNumber forknum, bool isRedo)
{
	RelFileNodeBackend rnode = reln->smgr_rnode;
	int			which = reln->smgr_which;

	/* Close the fork at smgr level */
	(*(smgrsw[which].smgr_close)) (reln, forknum);

	/*
	 * Get rid of any remaining buffers for the fork.  bufmgr will just drop
	 * them without bothering to write the contents.
	 */
	DropRelFileNodeBuffers(rnode, forknum, 0);

	/*
	 * It'd be nice to tell the stats collector to forget it immediately, too.
	 * But we can't because we don't know the OID (and in cases involving
	 * relfilenode swaps, it's not always clear which table OID to forget,
	 * anyway).
	 */

	/*
	 * Send a shared-inval message to force other backends to close any
	 * dangling smgr references they may have for this rel.  We should do this
	 * before starting the actual unlinking, in case we fail partway through
	 * that step.  Note that the sinval message will eventually come back to
	 * this backend, too, and thereby provide a backstop that we closed our
	 * own smgr rel.
	 */
	CacheInvalidateSmgr(rnode);

	/*
	 * Delete the physical file(s).
	 *
	 * Note: smgr_unlink must treat deletion failure as a WARNING, not an
	 * ERROR, because we've already decided to commit or abort the current
	 * xact.
	 */
	(*(smgrsw[which].smgr_unlink)) (rnode, forknum, isRedo);
}

/*
 *	smgrextend() -- Add a new block to a file.
 *
 *		The semantics are nearly the same as smgrwrite(): write at the
 *		specified position.  However, this is to be used for the case of
 *		extending a relation (i.e., blocknum is at or beyond the current
 *		EOF).  Note that we assume writing a block beyond current EOF
 *		causes intervening file space to become filled with zeroes.
 */
void
smgrextend(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
		   char *buffer, bool skipFsync)
{
	(*(smgrsw[reln->smgr_which].smgr_extend)) (reln, forknum, blocknum,
											   buffer, skipFsync);
}

/*
 *	smgrprefetch() -- Initiate asynchronous read of the specified block of a relation.
 */
void
smgrprefetch(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum)
{

	(*(smgrsw[reln->smgr_which].smgr_prefetch)) (reln, forknum, blocknum);
}

/*
 *	smgrread() -- read a particular block from a relation into the supplied
 *				  buffer.
 *
 *		This routine is called from the buffer manager in order to
 *		instantiate pages in the shared buffer cache.  All storage managers
 *		return pages in the format that POSTGRES expects.
 */
void
smgrread(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
		 char *buffer)
{

	(*(smgrsw[reln->smgr_which].smgr_read)) (reln, forknum, blocknum, buffer);
}

/*
 *	smgrwrite() -- Write the supplied buffer out.
 *
 *		This is to be used only for updating already-existing blocks of a
 *		relation (ie, those before the current EOF).  To extend a relation,
 *		use smgrextend().
 *
 *		This is not a synchronous write -- the block is not necessarily
 *		on disk at return, only dumped out to the kernel.  However,
 *		provisions will be made to fsync the write before the next checkpoint.
 *
 *		skipFsync indicates that the caller will make other provisions to
 *		fsync the relation, so we needn't bother.  Temporary relations also
 *		do not require fsync.
 */
void
smgrwrite(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
		  char *buffer, bool skipFsync)
{

	(*(smgrsw[reln->smgr_which].smgr_write)) (reln, forknum, blocknum,
											  buffer, skipFsync);
}

/*
 *	smgrnblocks() -- Calculate the number of blocks in the
 *					 supplied relation.
 */
BlockNumber
smgrnblocks(SMgrRelation reln, ForkNumber forknum)
{
	return (*(smgrsw[reln->smgr_which].smgr_nblocks)) (reln, forknum);
}

/*
 *	smgrtruncate() -- Truncate supplied relation to the specified number
 *					  of blocks
 *
 * The truncation is done immediately, so this can't be rolled back.
 */
void
smgrtruncate(SMgrRelation reln, ForkNumber forknum, BlockNumber nblocks)
{
	/*
	 * Get rid of any buffers for the about-to-be-deleted blocks. bufmgr will
	 * just drop them without bothering to write the contents.
	 */
	DropRelFileNodeBuffers(reln->smgr_rnode, forknum, nblocks);

	/*
	 * Send a shared-inval message to force other backends to close any smgr
	 * references they may have for this rel.  This is useful because they
	 * might have open file pointers to segments that got removed, and/or
	 * smgr_targblock variables pointing past the new rel end.	(The inval
	 * message will come back to our backend, too, causing a
	 * probably-unnecessary local smgr flush.  But we don't expect that this
	 * is a performance-critical path.)  As in the unlink code, we want to be
	 * sure the message is sent before we start changing things on-disk.
	 */
	CacheInvalidateSmgr(reln->smgr_rnode);

	/*
	 * Do the truncation.
	 */
	(*(smgrsw[reln->smgr_which].smgr_truncate)) (reln, forknum, nblocks);
}

/*
 *	smgrimmedsync() -- Force the specified relation to stable storage.
 *
 *		Synchronously force all previous writes to the specified relation
 *		down to disk.
 *
 *		This is useful for building completely new relations (eg, new
 *		indexes).  Instead of incrementally WAL-logging the index build
 *		steps, we can just write completed index pages to disk with smgrwrite
 *		or smgrextend, and then fsync the completed index file before
 *		committing the transaction.  (This is sufficient for purposes of
 *		crash recovery, since it effectively duplicates forcing a checkpoint
 *		for the completed index.  But it is *not* sufficient if one wishes
 *		to use the WAL log for PITR or replication purposes: in that case
 *		we have to make WAL entries as well.)
 *
 *		The preceding writes should specify skipFsync = true to avoid
 *		duplicative fsyncs.
 *
 *		Note that you need to do FlushRelationBuffers() first if there is
 *		any possibility that there are dirty buffers for the relation;
 *		otherwise the sync is not very meaningful.
 */
void
smgrimmedsync(SMgrRelation reln, ForkNumber forknum)
{
	(*(smgrsw[reln->smgr_which].smgr_immedsync)) (reln, forknum);
}


/*
 *	smgrpreckpt() -- Prepare for checkpoint.
 */
void
smgrpreckpt(void)
{
	int			i;

	for (i = 0; i < NSmgr; i++)
	{
		if (smgrsw[i].smgr_pre_ckpt)
			(*(smgrsw[i].smgr_pre_ckpt)) ();
	}
}

/*
 *	smgrsync() -- Sync files to disk during checkpoint.
 */
void
smgrsync(void)
{
	int			i;


	for (i = 0; i < NSmgr; i++)
	{
		if (smgrsw[i].smgr_sync)
			(*(smgrsw[i].smgr_sync)) ();
	}
}

/*
 *	smgrpostckpt() -- Post-checkpoint cleanup.
 */
void
smgrpostckpt(void)
{
	int			i;

	for (i = 0; i < NSmgr; i++)
	{
		if (smgrsw[i].smgr_post_ckpt)
			(*(smgrsw[i].smgr_post_ckpt)) ();
	}
}

/*
 * AtEOXact_SMgr
 *
 * This routine is called during transaction commit or abort (it doesn't
 * particularly care which).  All transient SMgrRelation objects are closed.
 *
 * We do this as a compromise between wanting transient SMgrRelations to
 * live awhile (to amortize the costs of blind writes of multiple blocks)
 * and needing them to not live forever (since we're probably holding open
 * a kernel file descriptor for the underlying file, and we need to ensure
 * that gets closed reasonably soon if the file gets deleted).
 */
void
AtEOXact_SMgr(void)
{
	/*
	 * Zap all unowned SMgrRelations.  We rely on smgrclose() to remove each
	 * one from the list.
	 */
	while (first_unowned_reln != NULL)
	{
		Assert(first_unowned_reln->smgr_owner == NULL);
		smgrclose(first_unowned_reln);
	}
}

bool
is_primary_mode()
{
	int res = access("pg_tmp/primary_mode", F_OK);
	//ereport(TRACE_LEVEL,
		//	(errmsg("Primary Mode: %d", res)));

	if(res == 0)
		return true;
	else
		return false;
}

bool
is_standby_mode()
{
	int res = access("pg_tmp/standby_mode", F_OK);
	//ereport(TRACE_LEVEL,
			//(errmsg("Standby Mode: %d", res)));

	if(res == 0)
	{
		return true;
	}
	else
		return false;
}

bool
is_tracked(char *filename)
{
	if(strstr(filename, "pg_tmp") != NULL ||
			(strstr(filename, "base/") == NULL &&
			 strstr(filename, "global/") == NULL))
		return false;

	return true;
}

HTAB*
init_last_block_hash()
{
	HASHCTL info;
	int hash_flags;
	long init_table_size = LASTBLOCKHASHSIZE;
	long max_table_size = init_table_size;

	if(LastBlockHash != NULL)
		return LastBlockHash;

	MemSet(&info, 0, sizeof(info));
	info.keysize = sizeof(RelName);
	info.entrysize = sizeof(RelLastBlockData);
	hash_flags = HASH_ELEM;

	return ShmemInitHash("relation last block",
								init_table_size,
								max_table_size,
								&info,
								hash_flags);
}

void
modify_last_block_hash(char *filename, BlockNumber blocknum, HASHACTION action)
{
	bool found;
	RelName rel_name;
	RelLastBlock val;
	struct timeval tv;


	if(LastBlockHash == NULL)
	{
		LastBlockHash = init_last_block_hash();
		ereport(TRACE_LEVEL,
				(errmsg("modify_last_block_hash:LastBlockHash:%p", LastBlockHash)));
	}

	strcpy(rel_name.filename, filename);
	val = (RelLastBlock) hash_search(LastBlockHash,
									&rel_name,
									action,
									&found);

	if(action == HASH_REMOVE)
		return;

	if(val != NULL)
	{
		val->last_block_num = blocknum;
		gettimeofday(&val->tv, NULL);
	}
	else if(action == HASH_ENTER_NULL)
		ereport(ERROR,
			(errmsg("%ld.%ld:\tOutofMemory for last block hash",
					tv.tv_sec, tv.tv_usec)));
}

BlockNumber
get_last_block_hash(char *filename, HASHACTION action)
{
	bool found;
	RelName rel_name;
	RelLastBlock val;

	if(LastBlockHash == NULL)
	{
		LastBlockHash = init_last_block_hash();
		ereport(TRACE_LEVEL,
				(errmsg("get_last_block_hash: LastBlockHash:%p", LastBlockHash)));
		return InvalidBlockNumber;
	}

	strcpy(rel_name.filename, filename);
	val = (RelLastBlock) hash_search(LastBlockHash,
										&rel_name,
										action,
										&found);

	if(val != NULL)
		return val->last_block_num;
	else
		return InvalidBlockNumber;
}

Size BlockLSNSize()
{
	Size size = 0;
	size = mul_size(BLOCKLSNHASHSIZE*2, sizeof(BlockLSNData));
	//size = add_size(size, sizeof(PrimaryData));
	return size;
}

HTAB*
init_block_lsn_hash()
{
	HASHCTL		info;
	info.keysize = sizeof(BlockTag);
	info.entrysize = sizeof(BlockLSNData);
	info.num_partitions = 16;
	long init_size = BLOCKLSNHASHSIZE;
	long max_size = init_size;
	info.hash = tag_hash;

	if(BlockLSNHash != NULL)
		return BlockLSNHash;

	return ShmemInitHash("block lsn",
							init_size, max_size,
							&info,
							HASH_ELEM | HASH_PARTITION | HASH_FUNCTION);
}

/**
 * HashACTION should be either
 * HASH_REMOVE or
 * HASH_ENTER_NULL
 */
void
update_block_header(RelFileNode rnode, ForkNumber forknum, BlockNumber blocknum,
					PageHeader header, HASHACTION action)
{
	BlockTag blk_tag;
	bool found;
	BlockLSN val;

	if(BlockLSNHash == NULL)
	{
		BlockLSNHash = init_block_lsn_hash();
		ereport(TRACE_LEVEL,
				(errmsg("update_block_lsn:BlockLSNHash:%p", BlockLSNHash)));
	}

	blk_tag.rnode = rnode;
	blk_tag.forkno = forknum;
	blk_tag.blkno = blocknum;
	val = (BlockLSN) hash_search(BlockLSNHash,
									&blk_tag,
									action,
									&found);
	if(action == HASH_REMOVE)
		return;

	if(val != NULL)
		memcpy(&(val->header), header, sizeof(PageHeaderData));
	else if(action == HASH_ENTER_NULL)
		ereport(ERROR,
			(errmsg("OutofMemory for block lsn hash")));
}

bool
get_block_header(RelFileNode rnode, ForkNumber forknum, BlockNumber blocknum, PageHeader header)
{
	bool found;
	BlockTag blk_tag;
	BlockLSN val;

	if(BlockLSNHash == NULL)
	{
		BlockLSNHash = init_block_lsn_hash();
		ereport(TRACE_LEVEL,
				(errmsg("get_block_lsn:BlockLSNHash:%p", BlockLSNHash)));
	}

	blk_tag.rnode = rnode;
	blk_tag.blkno = blocknum;
	blk_tag.forkno = forknum;
	val = (BlockLSN) hash_search(BlockLSNHash,
										&blk_tag,
										HASH_FIND,
										&found);

	if(val != NULL)
	{
		(*header) = val->header;
		return true;
	}
	else
	{
		MemSet(header, 0, sizeof(PageHeaderData));
		return false;
	}
}

int isReadable(int sd, int timeOut) { // milliseconds
  fd_set read_set;
  struct timeval tv;

  FD_ZERO(&read_set);
  FD_SET(sd,&read_set);

  if (timeOut) {
    tv.tv_sec  = timeOut / 1000;
    tv.tv_usec = (timeOut % 1000) * 1000;
  } else {
    tv.tv_sec  = 0;
    tv.tv_usec = 0;
  }
  if (select(sd+1,&read_set,0,0,&tv) < 0) {
    return 0;
  }

  return FD_ISSET(sd,&read_set) != 0;
}

void
network_sync(char* buffer, RelFileNode rnode, ForkNumber forknum, BlockNumber blocknum, XLogRecPtr lsn, bool flush)
{
	static struct sockaddr_in primary_addr;
	static struct sockaddr_in standby_addr;
	int addr_len = sizeof(standby_addr);
	int ret = 0;
	FlushRequest request;

	if(WalSndCtl == NULL)
	{
		ereport(WARNING,
				(errmsg("Attach to WalSndCtl")));
		bool found;
		WalSndCtl = (WalSndCtlData *)
			ShmemInitStruct("Wal Sender Ctl", WalSndShmemSize(), &found);

		if(!found || WalSndCtl == NULL)
			ereport(WARNING,
					(errmsg("WalSndCtl not initiated")));
	}

	if(primary_s < 0) {
		if((primary_s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0)
			ereport(FATAL,
					(errmsg("Unable to create socket")));

		primary_addr.sin_family = AF_INET;
		primary_addr.sin_addr.s_addr = htonl(INADDR_ANY);
		primary_addr.sin_port = htons(0);

		if(bind(primary_s, (struct sockaddr *)&primary_addr, sizeof(primary_addr)) < 0) {
			ereport(FATAL,
					(errmsg("Unable to bind")));
		}

		memset(&standby_addr, 0, sizeof(standby_addr));
		standby_addr.sin_family = AF_INET;
		standby_addr.sin_port = htons(flush_req_port);

		if(inet_aton(WalSndCtl->standby_addr, &standby_addr.sin_addr) == 0)
			ereport(FATAL,
					(errmsg("Unable to inet_aton: %s", strerror(errno))));
	}

	request.rnode = rnode;
	request.forknum = forknum;
	request.blocknum = blocknum;
	request.lsn = lsn;
	request.flush = flush+'0';

	if(sendto(primary_s, &request, req_len, 0, &standby_addr, addr_len) < 0)
	{
		primary_s = -1;
		ereport(ERROR,
				(errmsg("Unable to send: %s", strerror(errno))));
	}
	if(isReadable(primary_s, 100)) {
		ret = recvfrom(primary_s, (char*)buffer, BLCKSZ, 0, &standby_addr, &addr_len);
	}
	if(ret != BLCKSZ)
	{
		primary_s = -1;
		ereport(ERROR,
				(errmsg("Unable to get block: %u", blocknum)));
	}
}

void shutdownHandler()
{
	ereport(WARNING,
			(errmsg("get_block_info: shut down clean up")));
	close(standby_s);
	exit(1);
}

void shutdownHandler2()
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	xp_stack_trace(10, tv);
	shutdownHandler();
}

void
get_block_info()
{
	struct sockaddr_in primary_addr;
	static struct sockaddr_in standby_addr;
	int addr_len = sizeof(primary_addr);
	FlushRequest request;
	int ret = 0;

	pqsignal(SIGTERM, shutdownHandler);	/* request shutdown */
	pqsignal(SIGQUIT, shutdownHandler);	/* hard crash time */
	pqsignal(SIGSEGV, shutdownHandler2);	/* hard crash time */

	/* init the xlog_apply, monitor how far the standby has replayed */
	if(xlog_apply == NULL)
	{
		bool found;
		xlog_apply = ShmemInitStruct("xlog apply", sizeof(XLogApplyData), &found);
		if(!found)
		ereport(WARNING,
				(errmsg("Xlog Apply not inited by walreceiver")));
	}

	if(standby_s < 0) {
		if((standby_s = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
			ereport(FATAL,
					(errmsg("Unable to create socket")));

		memset(&standby_addr, 0, sizeof(standby_addr));
		standby_addr.sin_family = AF_INET;
		standby_addr.sin_port = htons(flush_req_port);
		standby_addr.sin_addr.s_addr = htonl(INADDR_ANY);

		if(bind(standby_s, (struct sockaddr *)&standby_addr, sizeof(standby_addr)) < 0)
		{
			ereport(FATAL,
					(errmsg("Unable to bind: %s", strerror(errno))));
		}
	}


	/* start the loop to receive primary request */
	while(1)
	{
		/* if the parent is dead */
		if (getppid() == 1)
			kill(getpid(), SIGTERM);

		if(isReadable(standby_s, 100)) {
			if((ret = recvfrom(standby_s, &request, req_len, 0, (struct sockaddr*)&primary_addr, &addr_len)) != req_len) {
				ereport(WARNING,
						(errmsg("Wrong request size: %d,%s", ret, strerror(errno))));
			}
			if(request.flush-'0' != 0)
			{
				if(XLByteLT(xlog_apply->apply, request.lsn))
				{
					ereport(WARNING,
							(errmsg("DelaySync: rnode:%u\tblocknum:%u\trequested:%u.%u, applied:%u.%u",
							request.rnode.relNode, request.blocknum,
							request.lsn.xlogid, request.lsn.xrecoff, xlog_apply->apply.xlogid, xlog_apply->apply.xrecoff)));
				}
				else
					sync_block(request, &primary_addr);
			}
		}
	}
}

void
sync_block(FlushRequest request, struct sockaddr_in* primary_addr)
{
	Buffer buf;
	Page page;
	XLogRecPtr pageLSN;
	struct timeval tv;

	buf = XLogReadBufferExtended(request.rnode, request.forknum, request.blocknum, RBM_NORMAL);

	if(BufferIsValid(buf))
	{
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);

		page = (Page) BufferGetPage(buf);
		pageLSN = PageGetLSN(page);

		if(!XLByteEQ(pageLSN, request.lsn) && request.lsn.xrecoff != 0)
		{
			ereport(WARNING,
					(errmsg("Block LSN not match: rnode:%u\tblocknum:%u\trequested:%u.%u\tpageLSN:%u.%u",
					request.rnode.relNode, request.blocknum,
					request.lsn.xlogid, request.lsn.xrecoff, pageLSN.xlogid, pageLSN.xrecoff)));
		}
		gettimeofday(&tv, NULL);
		ereport(WARNING,
			(errmsg("SyncABlock:%ld.%ld:\trnode:%u\tblocknum:%u\trequested:%u.%u\tapplied:%u.%u",
					tv.tv_sec, tv.tv_usec, request.rnode.relNode,
					request.blocknum, request.lsn.xlogid, request.lsn.xrecoff,
					xlog_apply->apply.xlogid, xlog_apply->apply.xrecoff)));

		PageSetLSN(page, request.lsn);
		if(sendto(standby_s, (char*)page, BLCKSZ, 0, primary_addr, sizeof(*primary_addr)) < 0)
			ereport(WARNING,
					(errmsg("Unable to send: %s", strerror(errno))));

		UnlockReleaseBuffer(buf);
	}
}

void
clean_standby_resources()
{
	ereport(WARNING,
			(errmsg("Clean up standby resources")));
	unlink("pg_tmp/standby_mode");
}

