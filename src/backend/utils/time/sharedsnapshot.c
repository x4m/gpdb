/*-------------------------------------------------------------------------
 *
 * sharedsnapshot.c
 *	  GPDB shared snapshot management.
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/time/sharedsnapshot.c
 *
 * In Greenplum, as part of slice plans, many postgres processes (qExecs, QE)
 * running on a single segment database as part of the same user's SQL
 * statement. All of the qExecs that belong to a particular user on a
 * particular segment database need to have consistent visibility. Idea used
 * is called "Shared Local Snapshot". Shared-memory data structure
 * SharedSnapshotSlot shares session and transaction information among
 * session's gang processes on a particular database instance. The processes
 * are called a SegMate process group.
 *
 * A SegMate process group is a QE (Query Executor) Writer process and 0, 1 or
 * more QE Reader processes. Greenplum needed to invent a SegMate sharing
 * mechanism because in Postgres there is only 1 backend and most needed
 * information is simply available in private memory. With Greenplum session
 * parallelism on database instances, we need to have a way to share
 * not-yet-committed session information among the SegMates. This information
 * includes transaction snapshots, sub-transaction status, so-called combo-cid
 * mapping, etc.
 *
 * An example: the QE readers need to use the same snapshot and command number
 * information as the QE writer so they see the current data written by the QE
 * writer. During a transaction, the QE Writer writes new data into the
 * shared-memory buffered cache. Later in that same transaction, QE Readers
 * will need to recognize which tuples in the shared-memory buffered cache are
 * for its session's transaction to perform correctly.
 *
 * Another example: the QE readers need to know which sub-transactions are
 * active or committed for a session's transaction so they can properly read
 * sub-transaction data written by the QE writer for the transaction.
 *
 * So, the theme is to share private, not-yet-committed session transaction
 * information with the QE Readers so the SegMate process group can all work
 * on the transaction correctly. [We mostly think of QE Writers/Readers being
 * on the segments. However, masters have special purpose QE Reader called the
 * Entry DB Singleton. So, the SegMate module also works on the master.]
 *
 * Each shared snapshot is local only to the segment database. High level
 * Writer gang member establishes a local transaction, acquires the slot in
 * shared snapshot shmem space and copies the snapshot information into shared
 * memory where the other qExecs that are segmates can find it. Following
 * section convers details on how shared memory initialization happens, who
 * writes the snapshot, how its controlled how/when the readers can read the
 * snapshot, locking, etc..
 *
 * Shared Memory Initialization: Shared memory is setup by the postmaster. One
 * slot for every user connection on the QD is kind of needed to store a data
 * structure for a set of segmates to store their snapshot information. In
 * each slot QE writer stores information defined by SharedSnapshotSlot.
 *
 * PQsendMppStatement: Is the same as PQsendQuery except that it also sends a
 * serialized snapshot and xid. postgres.c has been modified to accept this
 * new protocol message. It does pretty much the same stuff as it would for a
 * 'Q' (normal query) except it unpacks the snapshot and xid from the QD and
 * stores it away. All QEs get sent in a QD snapshot during statement
 * dispatch.
 *
 * Global Session ID: The shared snapshot shared memory is split into slots. A
 * set of segmates for a given user requires a single slot. The snapshot
 * information and other information is stored within the snapshot. A unique
 * session id identifies all the components in the system that are working for
 * a single user session. Within a single segment database this essentially
 * defines what it means to be "segmates."  The shared snapshot slot is
 * identified by this unique session id. The unique session id is sent in from
 * the QD as a GUC called "mpp_session_id". So the slot's field "slotid" will
 * store the "mpp_session_id" that WRITER to the slot will use. Readers of the
 * slot will find the correct slot by finding the one that has the slotid
 * equal to their own mpp_session_id.
 *
 * Single Writer: Mechanism is simplified by introducing the restriction of
 * only having a single qExec in a set of segmates capable of writing. Single
 * WRITER qExec is the only qExec amongst all of its segmates that will ever
 * perform database write operations.  Benefits of the approach, Single WRITER
 * qExec is the only member of a set of segmates that need to participate in
 * global transactions. Also... only this WRITER qExec really has to do
 * anything during commit. Assumption seems since they are just reader qExecs
 * that this is not a problem. The single WRITER qExec is the only qExec that
 * is guaranteed to participate in every dispatched statement for a given user
 * (at least to that segdb). Also, it is this WRITER qExec that performs any
 * utility statement.
 *
 * Coordinating Readers and Writers: The coordination is on when the writer
 * has set the snapshot such that the readers can get it and use it. In
 * general, we cannot assume that the writer will get to setting it before the
 * reader needs it and so we need to build a mechanism for the reader to (1)
 * know that its reading the right snapshot and (2) know when it can read.
 * The Mpp_session_id stored in the SharedSnapshotSlot is the piece of
 * information that lets the reader know it has got the right slot. And it
 * knows can read it when the xid and cid in the slot match the transactionid
 * and curid sent in from the QD in the SnapshotInfo field.  Basically QE
 * READERS aren't allowed to read the shared local snapshot until the shared
 * local snapshot has the same QD statement id as the QE Reader. i.e. the QE
 * WRITER updates the local snapshot and then writes the QD statement id into
 * the slot which identifies the "freshness" of that information. Currently QE
 * readers check that value and if its not been set they sleep (gasp!) for a
 * while. Think this approach is definitely not elegant and robust would be
 * great maybe to replace it with latch based approach.
 *
 * Cursor handling through SharedSnapshot: Cursors are funny case because they
 * read through a snapshot taken when the create cursor command was executed,
 * not through the current snapshot. Originally, the SharedSnapshotSlot was
 * designed for just the current command. The default transaction isolation
 * mode is READ COMMITTED, which cause a new snapshot to be created each
 * command. Each command in an explicit transaction started with BEGIN and
 * completed with COMMIT, etc. So, cursors would read through the current
 * snapshot instead of the create cursor snapshot and see data they shouldn't
 * see. The problem turns out to be a little more subtle because of the
 * existence of QE Readers and the fact that QE Readers can be created later –
 * long after the create cursor command. So, the solution was to serialize the
 * current snapshot to a temporary file during create cursor so that
 * subsequently created QE Readers could get the right snapshot to use from
 * the temporary file and ignore the SharedSnapshotSlot.
 *
 * Sub-Transaction handling through SharedSnapshot: QE Readers need to know
 * which sub-transactions the QE Writer has committed and which are active so
 * QE Readers can see the right data. While a sub-transaction may be committed
 * in an active parent transaction, that data is not formally committed until
 * the parent commits. And, active sub-transactions are not even
 * sub-transaction committed yet. So, other transactions cannot see active or
 * committed sub-transaction work yet. Without adding special logic to a QE
 * Reader, it would be considered another transaction and not see the
 * committed or active sub-transaction work. This is because QE Readers do not
 * start their own transaction. We just set a few variables in the xact.c
 * module to fake making it look like there is a current transaction,
 * including which sub-transactions are active or committed. This is a
 * kludge. In order for the QE Reader to fake being part of the QE Writer
 * transaction, we put the current transaction id and the values of all active
 * and committed sub-transaction ids into the SharedSnapshotSlot shared-memory
 * structure. Since shared-memory is not dynamic, poses an arbitrary limit on
 * the number of sub-transaction ids we keep in the SharedSnapshotSlot
 * in-memory. Once this limit is exceeded the sub-transaction ids are written
 * to temp files on disk.  See how the TransactionIdIsCurrentTransactionId
 * procedure in xact.c checks to see if the backend executing is a QE Reader
 * (or Entry DB Singleton), and if it is, walk through the sub-transaction ids
 * in SharedSnapshotSlot.
 *
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/distributedlog.h"
#include "access/twophase.h"  /*max_prepared_xacts*/
#include "access/xact.h"
#include "cdb/cdbtm.h"
#include "cdb/cdbvars.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "lib/stringinfo.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/resowner.h"
#include "utils/sharedsnapshot.h"
#include "utils/snapmgr.h"

/*
 * Distributed Snapshot that gets sent in from the QD to processes running
 * in EXECUTE mode.
 */
DtxContext DistributedTransactionContext = DTX_CONTEXT_LOCAL_ONLY;

DtxContextInfo QEDtxContextInfo = DtxContextInfo_StaticInit;

#define DUMP_HASH_SZ    1024
typedef struct DumpEntry
{
	uint32  segmate;
	Snapshot snapshot;
} DumpEntry;

/* local hash table to store cursor snapshot dump*/
static HTAB *dumpHtab = NULL;

/* MPP Shared Snapshot. */
typedef struct SharedSnapshotLockStruct
{
	int 		numSlots;		/* number of valid Snapshot entries */
	int			maxSlots;		/* allocated size of sharedSnapshotArray */
	int 		nextSlot;		/* points to the next avail slot. */

	/*
	 * We now allow direct indexing into this array.
	 *
	 * We allocate the XIPS below.
	 *
	 * Be very careful when accessing fields inside here.
	 */
	SharedSnapshotLockSlot	   *slots;

	TransactionId	   *xips;		/* VARIABLE LENGTH ARRAY */
} SharedSnapshotLockStruct;

static volatile SharedSnapshotLockStruct *sharedSnapshotLockArray;

static dsm_segment *SharedSnapshotSegment = NULL;

struct SharedSnapshotData SharedSnapshot = {0};

static Size slotCount = 0;
static Size xipEntryCount = 0;


/* prototypes for internal functions */
static SharedSnapshotLockSlot *SharedSnapshotAddLock(int32 sessionId);
static SharedSnapshotLockSlot *SharedSnapshotLookupLock(int32 sessionId);

/*
 * Report shared-memory space needed by CreateSharedSnapshot.
 */
Size
SharedSnapshotShmemSize(void)
{
	Size		size;
	Size        slotSize;

	/* should be the same as PROCARRAY_MAXPROCS */
	xipEntryCount = MaxBackends + max_prepared_xacts;

	slotSize = sizeof(SharedSnapshotLockSlot);
	slotSize = MAXALIGN(slotSize);

	/*
	 * We only really need max_prepared_xacts; but for safety we
	 * multiply that by two (to account for slow de-allocation on
	 * cleanup, for instance).
	 */
	slotCount = NUM_SHARED_SNAPSHOT_SLOTS;

	size = offsetof(SharedSnapshotLockStruct, xips);
	size = add_size(size, mul_size(slotSize, slotCount));

	RequestNamedLWLockTranche("SharedSnapshotLocks", slotCount);

	return MAXALIGN(size);
}

/*
 * Initialize the sharedSnapshot array.  This array is used to communicate
 * snapshots between qExecs that are segmates.
 */
void
CreateSharedSnapshotArray(void)
{
	bool	found;
	int		i;

	/* Create or attach to the SharedSnapshot shared structure */
	sharedSnapshotLockArray = (SharedSnapshotLockStruct *)
		ShmemInitStruct("Shared Snapshot", SharedSnapshotShmemSize(), &found);

	Assert(slotCount != 0);

	if (!found)
	{
		/*
		 * We're the first - initialize.
		 */
		LWLockPadded *lock_base;

		sharedSnapshotLockArray->numSlots = 0;

		/* TODO:  MaxBackends is only somewhat right.  What we really want here
		 *        is the MaxBackends value from the QD.  But this is at least
		 *		  safe since we know we dont need *MORE* than MaxBackends.  But
		 *        in general MaxBackends on a QE is going to be bigger than on a
		 *		  QE by a good bit.  or at least it should be.
		 *
		 * But really, max_prepared_transactions *is* what we want (it
		 * corresponds to the number of connections allowed on the
		 * master).
		 *
		 * slotCount is initialized in SharedSnapshotShmemSize().
		 */
		sharedSnapshotLockArray->maxSlots = slotCount;
		sharedSnapshotLockArray->nextSlot = 0;

		/*
		 * Set slots to point to the next byte beyond what was allocated for
		 * SharedSnapshotStruct. xips is the last element in the struct but is
		 * not included in SharedSnapshotShmemSize allocation.
		 */
		sharedSnapshotLockArray->slots = (SharedSnapshotLockSlot *)&sharedSnapshotLockArray->xips;

		lock_base = GetNamedLWLockTranche("SharedSnapshotLocks");
		for (i=0; i < sharedSnapshotLockArray->maxSlots; i++)
		{
			SharedSnapshotLockSlot *tmpSlot = &sharedSnapshotLockArray->slots[i];

			tmpSlot->session_id = -1;
			tmpSlot->slotindex = i;
			tmpSlot->lock = &lock_base[i].lock;
		}
	}
}

/*
 * Used to dump the internal state of the shared slots for debugging.
 */
char *
SharedSnapshotDump(void)
{
	StringInfoData str;

	initStringInfo(&str);

	appendStringInfo(&str, "session: %d/%d,is QD = %d, is writer = %d ",
	                 gp_session_id,
	                 SharedSnapshot.lockSlot->session_id,
	                 Gp_role == GP_ROLE_DISPATCH, Gp_is_writer);
	for (int i = 0; i < SNAPSHOTDUMPARRAYSZ; i++)
	{
		appendStringInfo(&str, "syncmateSync: %u \n",
			SharedSnapshot.desc->dump[i].segmateSync);
	}

	if(dumpHtab != NULL)
	{
		appendStringInfo(&str, "hashtable contain: \n");
		HASH_SEQ_STATUS hash_seq;
		DumpEntry *entry = NULL;
		hash_seq_init(&hash_seq, dumpHtab);

		while ((entry = hash_seq_search(&hash_seq)) != NULL)
		{
			appendStringInfo(&str, "syncmateSync: %u \n", entry->segmate);
		}
	}

	return str.data;
}

/* Acquires an available slot in the sharedSnapshotArray.  The slot is then
 * marked with the supplied slotId.  This slotId is what others will use to
 * find this slot.  This should only ever be called by the "writer" qExec.
 *
 * The slotId should be something that is unique amongst all the possible
 * "writer" qExecs active on a segment database at a given moment.  It also
 * will need to be communicated to the "reader" qExecs so that they can find
 * this slot.
 */
static SharedSnapshotLockSlot *
SharedSnapshotAddLock(int32 sessionId)
{
	SharedSnapshotLockSlot *slot;
	volatile SharedSnapshotLockStruct *arrayP = sharedSnapshotLockArray;
	int nextSlot = -1;
	int i;
	int retryCount = gp_snapshotadd_timeout * 10; /* .1 s per wait */

retry:
	LWLockAcquire(SharedSnapshotLock, LW_EXCLUSIVE);

	slot = NULL;

	for (i=0; i < arrayP->maxSlots; i++)
	{
		SharedSnapshotLockSlot *testSlot = &arrayP->slots[i];

		if (testSlot->slotindex > arrayP->maxSlots)
			elog(ERROR, "Shared Local Snapshots Array appears corrupted: %s", SharedSnapshotDump());

		if (testSlot->session_id == sessionId)
		{
			slot = testSlot;
			break;
		}
	}

	if (slot != NULL)
	{
		elog(DEBUG1, "SharedSnapshotAddLock: found existing entry for our session-id. id %d retry %d ", sessionId, retryCount);
		LWLockRelease(SharedSnapshotLock);

		if (retryCount > 0)
		{
			retryCount--;

			pg_usleep(100000); /* 100ms, wait gp_snapshotadd_timeout seconds max. */
			goto retry;
		}
		else
		{
			elog(ERROR, "writer segworker group shared snapshot collision on session_id %d", sessionId);
		}
	}

	if (arrayP->numSlots >= arrayP->maxSlots || arrayP->nextSlot == -1)
	{
		/*
		 * Ooops, no room.  this shouldn't happen as something else should have
		 * complained if we go over MaxBackends.
		 */
		LWLockRelease(SharedSnapshotLock);
		ereport(FATAL,
				(errcode(ERRCODE_TOO_MANY_CONNECTIONS),
				 errmsg("sorry, too many clients already."),
				 errdetail("There are no more available slots in the sharedSnapshotArray."),
				 errhint("Another piece of code should have detected that we have too many clients."
						 " this probably means that someone isn't releasing their slot properly.")));
	}

	slot = &arrayP->slots[arrayP->nextSlot];

	slot->slotindex = arrayP->nextSlot;

	/*
	 * find the next available slot
	 */
	for (i=arrayP->nextSlot+1; i < arrayP->maxSlots; i++)
	{
		SharedSnapshotLockSlot *tmpSlot = &arrayP->slots[i];

		if (tmpSlot->session_id == -1)
		{
			nextSlot = i;
			break;
		}
	}

	/* mark that there isn't a nextSlot if the above loop didn't find one */
	if (nextSlot == arrayP->nextSlot)
		arrayP->nextSlot = -1;
	else
		arrayP->nextSlot = nextSlot;

	arrayP->numSlots += 1;

	/* initialize some things */
	slot->session_id = sessionId;

	LWLockRelease(SharedSnapshotLock);

	return slot;
}

/*
 * Used by "reader" qExecs to find the slot in the sharedsnapshotArray with the
 * specified slotId.  In general, we should always be able to find the specified
 * slot unless something unexpected.  If the slot is not found, then NULL is
 * returned.
 *
 * MPP-4599: retry in the same pattern as the writer.
 */
static SharedSnapshotLockSlot *
SharedSnapshotLookupLock(int32 sessionId)
{
	SharedSnapshotLockSlot *slot = NULL;
	volatile SharedSnapshotLockStruct *arrayP = sharedSnapshotLockArray;
	int retryCount = gp_snapshotadd_timeout * 10; /* .1 s per wait */
	int index;

	for (;;)
	{
		CHECK_FOR_INTERRUPTS();

		LWLockAcquire(SharedSnapshotLock, LW_SHARED);

		for (index=0; index < arrayP->maxSlots; index++)
		{
			SharedSnapshotLockSlot *testSlot;

			testSlot = &arrayP->slots[index];

			if (testSlot->slotindex > arrayP->maxSlots)
			{
				LWLockRelease(SharedSnapshotLock);
				elog(ERROR, "Shared Local Snapshots Array appears corrupted: %s", SharedSnapshotDump());
			}

			if (testSlot->session_id == sessionId)
			{
				slot = testSlot;
				break;
			}
		}

		LWLockRelease(SharedSnapshotLock);

		if (slot != NULL)
		{
			break;
		}
		else
		{
			if (retryCount > 0)
			{
				retryCount--;

				pg_usleep(100000); /* 100ms, wait gp_snapshotadd_timeout seconds max. */
			}
			else
			{
				break;
			}
		}
	}

	return slot;
}


/*
 * Used by the "writer" qExec to "release" the slot it had been using.
 *
 */
void
SharedSnapshotRemove( char *creatorDescription)
{
	int sessionid = SharedSnapshot.lockSlot->session_id;

	LWLockAcquire(SharedSnapshotLock, LW_EXCLUSIVE);

	/* determine if we need to modify the next available slot to use.  we
	 * only do this is our slotindex is lower then the existing one.
	 */
	if (sharedSnapshotLockArray->nextSlot == -1 || SharedSnapshot.lockSlot->slotindex < sharedSnapshotLockArray->nextSlot)
	{
		if (SharedSnapshot.lockSlot->slotindex > sharedSnapshotLockArray->maxSlots)
			elog(ERROR, "Shared Local Snapshots slot has a bogus slotindex: %d. slot array dump: %s",
				 SharedSnapshot.lockSlot->slotindex, SharedSnapshotDump());

		sharedSnapshotLockArray->nextSlot = SharedSnapshot.lockSlot->slotindex;
	}

	/* reset the slotid which marks it as being unused. */
	SharedSnapshot.lockSlot->session_id = -1;

	sharedSnapshotLockArray->numSlots -= 1;

	/* we do not need worry SharedSnapshotSegment, it will free when process exits */
	MyProc->sharedSnapshotDescHandle = 0;

	SharedSnapshotSegment = NULL;
	SharedSnapshot.desc = NULL;
	SharedSnapshot.lockSlot = NULL;

	LWLockRelease(SharedSnapshotLock);

	elog((Debug_print_full_dtm ? LOG : DEBUG5),"SharedSnapshotRemove removed slot for slotId = %d, creator = %s",
		 sessionid, creatorDescription);
}

void
addSharedSnapshot(char *creatorDescription, int id)
{
	SharedSnapshotLockSlot *lockSlot;

	lockSlot = SharedSnapshotAddLock(id);

	if (lockSlot == NULL)
	{
		ereport(ERROR,
				(errmsg("%s could not set the Shared Local Snapshot!",
						creatorDescription),
				 errdetail("Tried to set the shared local snapshot slot with id: %d "
						   "and failed. Shared Local Snapshots dump: %s", id,
						   SharedSnapshotDump())));
	}

	SharedSnapshot.lockSlot = lockSlot;

	Assert(xipEntryCount != 0);

	if (IsUnderPostmaster)
	{
		/* create shared memory */
		int slotSize = 0;
		slotSize = sizeof(SharedSnapshotDesc);
		slotSize += mul_size(sizeof(TransactionId), (xipEntryCount));

		dsm_segment *segment = dsm_create(slotSize, 0);
		dsm_pin_mapping(segment);

		SharedSnapshotDesc* tmpSlot = dsm_segment_address(segment);
		MemSet(tmpSlot, 0, sizeof(SharedSnapshotDesc));

		tmpSlot->writer_proc = MyProc;
		tmpSlot->writer_xact = MyPgXact;
		tmpSlot->snapshot.xip = (TransactionId  *)&tmpSlot[1];

		SharedSnapshotSegment = segment;
		SharedSnapshot.desc = tmpSlot;

		pg_write_barrier();
		/* fill out hash entry */
		MyProc->sharedSnapshotDescHandle = dsm_segment_handle(SharedSnapshotSegment);

		elog((Debug_print_full_dtm ? LOG : DEBUG5),"%s added Shared Local Snapshot slot for gp_session_id = %d (address %p)",
		     creatorDescription, id, SharedSnapshot.desc);
	}
}

void
lookupSharedSnapshot(char *lookerDescription, char *creatorDescription, int sessionid)
{
	SharedSnapshotLockSlot *lockslot;

	Assert(lockHolderProcPtr->sharedSnapshotDescHandle != 0);

	lockslot = SharedSnapshotLookupLock(sessionid);

	if (lockslot == NULL)
	{
		ereport(ERROR,
				(errmsg("%s could not find Shared Local Snapshot!",
						lookerDescription),
				 errdetail("Tried to find a shared snapshot slot with id: %d "
						   "and found none. Shared Local Snapshots dump: %s", sessionid,
						   SharedSnapshotDump()),
				 errhint("Either this %s was created before the %s or the %s died.",
						 lookerDescription, creatorDescription, creatorDescription)));
	}

	SharedSnapshot.lockSlot = lockslot;

	SharedSnapshotSegment = dsm_attach(lockHolderProcPtr->sharedSnapshotDescHandle);
	dsm_pin_mapping(SharedSnapshotSegment);

	SharedSnapshot.desc = dsm_segment_address(SharedSnapshotSegment);

	elog((Debug_print_full_dtm ? LOG : DEBUG5),"%s found Shared Local Snapshot slot for gp_session_id = %d created by %s (address %p)",
		 lookerDescription, sessionid, creatorDescription, SharedSnapshot.desc);
}

/*
 * Free any shared snapshot files.
 */
void
AtEOXact_SharedSnapshot(void)
{
	dumpHtab = NULL;
}

/*
 * LogDistributedSnapshotInfo
 *   Log the distributed snapshot info in a given snapshot.
 *
 * The 'prefix' is used to prefix the log message.
 */
void
LogDistributedSnapshotInfo(Snapshot snapshot, const char *prefix)
{
	if (!IsMVCCSnapshot(snapshot))
		return;

	StringInfoData buf;
	initStringInfo(&buf);

	DistributedSnapshotWithLocalMapping *mapping =
		&(snapshot->distribSnapshotWithLocalMapping);

	DistributedSnapshot *ds = &mapping->ds;

	appendStringInfo(&buf, "%s Distributed snapshot info: "
			 "xminAllDistributedSnapshots=%d, distribSnapshotId=%d"
			 ", xmin=%d, xmax=%d, count=%d",
			 prefix,
			 ds->xminAllDistributedSnapshots,
			 ds->distribSnapshotId,
			 ds->xmin,
			 ds->xmax,
			 ds->count);

	appendStringInfoString(&buf, ", In progress array: {");

	for (int no = 0; no < ds->count; no++)
	{
		if (no != 0)
		{
			appendStringInfo(&buf, ", (dx%d)",
					 ds->inProgressXidArray[no]);
		}
		else
		{
			appendStringInfo(&buf, " (dx%d)",
					 ds->inProgressXidArray[no]);
		}
	}
	appendStringInfoString(&buf, "}");

	elog(LOG, "%s", buf.data);
	pfree(buf.data);
}

/*
 * Share the given snapshot to QE readers.
 *
 * This is called in the QE writer (or dispatcher) process. It stores the snapshot in
 * DSM segment, so that a subsequent call to syncSharedSnapshot() with the same
 *'segmentSync' value will find it.
 *
 * For cursor declaration, the QD will dispatch twice. The first time, QD ask all
 * writer gang dump snapshot. The second time, reader gang sync snapshot and execute
 * cursor query. But QD dose not wait reader gang sync snapshot done, it just return
 * success. So it is a small gap to trigger race condition in a txn if a bunch of
 * cursor declare.
 * For the solution, we maintain a big enough loop buffer to store the snapshot dump.
 * We suppose the very beginning snapshot must sync finish when we dump the snapshot
 * at the end of the buffer.
 *
 * For other queries, we simply store snapshot into Sharedsnapshot.
 */
void publishSharedSnapshot(uint32 segmateSync, Snapshot snapshot, bool for_cursor)
{
	Size sz = 0;
	dsm_segment *segment = NULL;

	Assert(SharedSnapshot.desc != NULL);
	Assert(LWLockHeldByMe(SharedSnapshot.lockSlot->lock));
	Assert(Gp_role == GP_ROLE_DISPATCH || (Gp_role == GP_ROLE_EXECUTE && Gp_is_writer));

	if (!for_cursor)
	{
		SharedSnapshot.desc->snapshot.xmin = snapshot->xmin;
		SharedSnapshot.desc->snapshot.xmax = snapshot->xmax;
		SharedSnapshot.desc->snapshot.xcnt = snapshot->xcnt;
		SharedSnapshot.desc->segmateSync = segmateSync;

		if (snapshot->xcnt > 0)
		{
			Assert(snapshot->xip != NULL);

			ereport((Debug_print_full_dtm ? LOG : DEBUG5),
			        (errmsg("updateSharedLocalSnapshot count of in-doubt ids %u",
			                SharedSnapshot.desc->snapshot.xcnt)));

			memcpy(SharedSnapshot.desc->snapshot.xip, snapshot->xip, snapshot->xcnt * sizeof(TransactionId));
		}

		SharedSnapshot.desc->snapshot.curcid = snapshot->curcid;
		return;
	}

	int id = SharedSnapshot.desc->cur_dump_id;
	volatile SnapshotDump *pDump = &SharedSnapshot.desc->dump[id];

	if(pDump->segment)
		dsm_detach(pDump->segment);

	sz = EstimateSnapshotSpace(snapshot);
	segment = dsm_create(sz, 0);

	char *ptr = dsm_segment_address(segment);
	SerializeSnapshot(snapshot,ptr);

	dsm_pin_mapping(segment);

	pDump->segment = segment;
	pDump->handle = dsm_segment_handle(segment);
	pDump->segmateSync = segmateSync;

	elog(LOG, "Dump syncmate : %u snapshot to slot %d", segmateSync, id);

	SharedSnapshot.desc->cur_dump_id =
		(SharedSnapshot.desc->cur_dump_id + 1) % SNAPSHOTDUMPARRAYSZ;
}

/*
 * For cursor, we synchronize the shared snapshot with the given 'segmateSync'
 * ID value.
 *
 * For other queries, we simply grab the snapshot which store in Sharesnapshot.
 *
 * This is used in QE (or entrydb) reader processes, to load the snapshot
 * that was acquired by the writer process.
 *
 */
void syncSharedSnapshot(uint32 segmateSync, bool for_cursor)
{
	volatile SnapshotDump *pDump = NULL;
	bool found = false;

	Assert(!Gp_is_writer);
	Assert(Gp_role == GP_ROLE_EXECUTE);
	Assert(SharedSnapshot.desc != NULL);
	Assert(LWLockHeldByMe(SharedSnapshot.lockSlot->lock));

	if (!for_cursor)
	{
		SharedSnapshot.snapshot = &SharedSnapshot.desc->snapshot;
		return;
	}

	if (dumpHtab == NULL)
	{
		HASHCTL     hash_ctl;
		memset(&hash_ctl, 0, sizeof(hash_ctl));
		hash_ctl.keysize = sizeof(uint32);
		hash_ctl.entrysize = sizeof(DumpEntry);
		hash_ctl.hcxt = TopTransactionContext;
		dumpHtab = hash_create("snapshot dump",
		                       DUMP_HASH_SZ  ,
		                       &hash_ctl,
		                       HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}

	/* check segmate in local memory, only sync from shared memory once */
	DumpEntry *entry = hash_search(dumpHtab, &segmateSync, HASH_ENTER, &found);

	if(found)
	{
		SharedSnapshot.snapshot = entry->snapshot;
		return;
	}


	int search_finish_id = SharedSnapshot.desc->cur_dump_id;
	int search_iter = search_finish_id;

	do{
		if (search_iter < 0)
			search_iter = SNAPSHOTDUMPARRAYSZ - 1;

		if(SharedSnapshot.desc->dump[search_iter].segmateSync == segmateSync)
		{
			pDump = &SharedSnapshot.desc->dump[search_iter];
			found = true;
			break;
		}

		search_iter --;

	} while (search_iter != search_finish_id);

	if (!found)
	{
		ereport(ERROR, (errmsg("could not find Shared Local Snapshot!"),
			errdetail("Tried to set the shared local snapshot slot with segmate: %u "
			          "and failed. Shared Local Snapshots dump: %s", segmateSync,
			          SharedSnapshotDump())));
	}

	Assert(pDump->handle != 0);

	dsm_segment* segment = dsm_attach(pDump->handle);
	char *ptr = dsm_segment_address(segment);

	SharedSnapshot.snapshot = RestoreSnapshot(ptr);

	entry->snapshot = SharedSnapshot.snapshot;

	dsm_detach(segment);
}
