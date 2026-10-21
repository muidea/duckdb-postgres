/* Compile libpq's executor with the narrow named-portal adapter in this TU.
 * Keeping this wrapper here avoids changing the downloaded PostgreSQL tree. */
#include "../postgres/src/interfaces/libpq/fe-exec.c"

PGresult *PQfqeBindPortal(PGconn *conn, int nparams, const char *const *values)
{
    PGcmdQueueEntry *entry;
    int i;
    if (!PQexecStart(conn) || !PQsendQueryStart(conn, true) ||
        conn->pipelineStatus != PQ_PIPELINE_OFF || PQtransactionStatus(conn) != PQTRANS_INTRANS)
        return NULL;
    entry = pqAllocCmdQueueEntry(conn);
    if (!entry) return NULL;
    if (pqPutMsgStart('B', conn) < 0 || pqPuts("fqe_guarded_portal", conn) < 0 ||
        pqPuts("fqe_guarded_statement", conn) < 0 || pqPutInt(0, 2, conn) < 0 ||
        pqPutInt(nparams, 2, conn) < 0) goto failed;
    for (i = 0; i < nparams; i++) {
        int len = values[i] ? strlen(values[i]) : -1;
        if (pqPutInt(len, 4, conn) < 0 ||
            (len >= 0 && pqPutnchar(values[i], len, conn) < 0)) goto failed;
    }
    if (pqPutInt(0, 2, conn) < 0 || pqPutMsgEnd(conn) < 0 ||
        pqPutMsgStart('D', conn) < 0 || pqPutc('P', conn) < 0 ||
        pqPuts("fqe_guarded_portal", conn) < 0 || pqPutMsgEnd(conn) < 0 ||
        pqPutMsgStart('S', conn) < 0 || pqPutMsgEnd(conn) < 0) goto failed;
    entry->queryclass = PGQUERY_DESCRIBE;
    if (pqPipelineFlush(conn) < 0) goto failed;
    pqAppendCmdQueueEntry(conn, entry);
    return PQexecFinish(conn);
failed:
    pqRecycleCmdQueueEntry(conn, entry);
    return NULL;
}

PGresult *PQfqeExecutePortal(PGconn *conn)
{
    PGcmdQueueEntry *entry;
    if (!PQexecStart(conn) || !PQsendQueryStart(conn, true) ||
        conn->pipelineStatus != PQ_PIPELINE_OFF || PQtransactionStatus(conn) != PQTRANS_INTRANS)
        return NULL;
    entry = pqAllocCmdQueueEntry(conn);
    if (!entry) return NULL;
    if (pqPutMsgStart('E', conn) < 0 || pqPuts("fqe_guarded_portal", conn) < 0 ||
        pqPutInt(0, 4, conn) < 0 || pqPutMsgEnd(conn) < 0 ||
        pqPutMsgStart('C', conn) < 0 || pqPutc('P', conn) < 0 ||
        pqPuts("fqe_guarded_portal", conn) < 0 || pqPutMsgEnd(conn) < 0 ||
        pqPutMsgStart('C', conn) < 0 || pqPutc('S', conn) < 0 ||
        pqPuts("fqe_guarded_statement", conn) < 0 || pqPutMsgEnd(conn) < 0 ||
        pqPutMsgStart('S', conn) < 0 || pqPutMsgEnd(conn) < 0) goto failed;
    entry->queryclass = PGQUERY_EXTENDED;
    if (pqPipelineFlush(conn) < 0) goto failed;
    pqAppendCmdQueueEntry(conn, entry);
    return PQexecFinish(conn);
failed:
    pqRecycleCmdQueueEntry(conn, entry);
    return NULL;
}
