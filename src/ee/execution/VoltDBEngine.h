/* This file is part of VoltDB.
 * Copyright (C) 2008-2020 VoltDB Inc.
 *
 * This file contains original code and/or modifications of original code.
 * Any modifications made by VoltDB Inc. are licensed under the following
 * terms and conditions:
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with VoltDB.  If not, see <http://www.gnu.org/licenses/>.
 */
/* Copyright (C) 2008 by H-Store Project
 * Brown University
 * Massachusetts Institute of Technology
 * Yale University
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#pragma once

#include "common/Pool.hpp"
#include "common/serializeio.h"
#include "common/LoadTableCaller.h"
#include "common/HiddenColumnFilter.h"
#include "common/ThreadLocalPool.h"
#include "common/UndoLog.h"
#include "common/valuevector.h"

#include "logging/LogManager.h"
#include "logging/LogProxy.h"
#include "logging/StdoutLogProxy.h"

#include "stats/StatsAgent.h"

#include "storage/BinaryLogSinkWrapper.h"
#include "storage/SystemTableId.h"

#include "boost/scoped_ptr.hpp"
#include "boost/unordered_map.hpp"

#include <common/debuglog.h>
#include <map>
#include <unordered_map>
#include <string>
#include <vector>
#include <pthread.h>
#include <atomic>

// shorthand for ExecutionEngine versions generated by javah
#define ENGINE_ERRORCODE_SUCCESS 0
#define ENGINE_ERRORCODE_ERROR 1

#define MAX_BATCH_COUNT 1000
#define MAX_PARAM_COUNT 1025 // keep in sync with value in CompiledPlan.java
// keep in sync with value MAX_BUFFER_SIZE in ExecutionEngineJNI.java
#define MAX_UDF_BUFFER_SIZE 50*1024*1024

namespace catalog {
class Catalog;
class Database;
class Table;
}

namespace voltdb {

class AbstractDRTupleStream;
class AbstractExecutor;
class AbstractPlanNode;
class AbstractTempTable;
class EnginePlanSet;  // Locally defined in VoltDBEngine.cpp
class ExecutorContext;
class ExecutorVector;
class PersistentTable;
class StreamedTable;
class Table;
class TableCatalogDelegate;
class TempTableLimits;
class Topend;
class TheHashinator;
class ExportTupleStream;

namespace kipling {
class GroupStore;
}

class TempTableTupleDeleter {
public:
    void operator()(AbstractTempTable* tbl) const;
};

struct UserDefinedFunctionInfo {
    std::vector<ValueType> paramTypes;
    ValueType returnType;
};

// UniqueTempTableResult is a smart pointer wrapper around a temp
// table.  It doesn't delete the temp table, but it will delete the
// contents of the table when it goes out of scope.
using UniqueTempTableResult = std::unique_ptr<AbstractTempTable, TempTableTupleDeleter>;

const int64_t DEFAULT_TEMP_TABLE_MEMORY = 1024 * 1024 * 100;

extern int32_t s_exportFlushTimeout;


/**
 * Represents an Execution Engine which holds catalog objects (i.e. table) and executes
 * plans on the objects. Every operation starts from this object.
 * This class is designed to be single-threaded.
 */
// TODO(evanj): Used by JNI so must be exported. Remove when we only one .so
class __attribute__((visibility("default"))) VoltDBEngine {
    protected:
        void setHashinator(TheHashinator* hashinator);
    public:
        /** The defaults apply to test code which does not enable JNI/IPC callbacks. */
        VoltDBEngine(Topend* topend = nullptr, LogProxy* logProxy = new StdoutLogProxy());

        void initialize(int32_t clusterIndex,
                        int64_t siteId,
                        int32_t partitionId,
                        int32_t sitesPerHost,
                        int32_t hostId,
                        std::string const& hostname,
                        int32_t drClusterId,
                        int32_t defaultDrBufferSize,
                        int64_t tempTableMemoryLimit,
                        bool isLowestSiteId,
                        int32_t compactionThreshold = 95);
        virtual ~VoltDBEngine();

        bool decommission(bool remove, bool promote, int newSitePerHost);

        void cleanup();

        // ------------------------------------------------------------------
        // OBJECT ACCESS FUNCTIONS
        // ------------------------------------------------------------------

        catalog::Catalog* getCatalog() const; // Only used in tests.
        catalog::Database* getDatabase() const {
            return m_database;
        }
        catalog::Table* getCatalogTable(std::string const& name) const;
        Table* getTableById(int32_t tableId) const;
        Table* getTableByName(std::string const& name) const;
        TableCatalogDelegate* getTableDelegate(std::string const& name) const;
        PersistentTable* getSystemTable(const SystemTableId id) const;

        // Serializes table_id to out. Throws a fatal exception if unsuccessful.
        void serializeTable(int32_t tableId, SerializeOutput& out) const;

        bool getIsActiveActiveDREnabled() const {
            return m_isActiveActiveDREnabled;
        }

        static int getDRHiddenColumnSize() {
            return s_drHiddenColumnSize;
        }

        StreamedTable* getPartitionedDRConflictStreamedTable() const {
            return m_drPartitionedConflictStreamedTable;
        }

        StreamedTable* getReplicatedDRConflictStreamedTable() const {
            return m_drReplicatedConflictStreamedTable;
        }

        void enableActiveActiveForTest(StreamedTable* partitionedConflictTable,
                                       StreamedTable* replicatedConflictTable) {
            m_isActiveActiveDREnabled = true;
            m_drPartitionedConflictStreamedTable = partitionedConflictTable;
            m_drReplicatedConflictStreamedTable = replicatedConflictTable;
        }

        void swapDRActions(PersistentTable* table1, PersistentTable* table2);

        ExecutorContext* getExecutorContext() const {
            return m_executorContext;
        }

        int getCurrentIndexInBatch() const {
            return m_currentIndexInBatch;
        }

        // -------------------------------------------------
        // Execution Functions
        // -------------------------------------------------

        /**
         * Execute a list of plan fragments, with the params yet-to-be deserialized.
         */
        int executePlanFragments(int32_t numFragments,
                                 int64_t planfragmentIds[],
                                 int64_t inputDependencyIds[],
                                 ReferenceSerializeInputBE& serialize_in,
                                 int64_t txnId,
                                 int64_t spHandle,
                                 int64_t lastCommittedSpHandle,
                                 int64_t uniqueId,
                                 int64_t undoToken,
                                 bool traceOn);

        /**
         * Execute a single, top-level plan fragment.  This method is
         * used both internally to execute fragments in a batch, and
         * by clients that execute fragments outside of a stored
         * procedure context, e.g., when populating a view during a
         * catalog update.
         *
         * This method will produce a unique_ptr-like wrapper around a
         * temp table, that will automatically delete the contents of
         * the table when it goes out of scope.
         *
         * Callers of this method should take care to call
         * ExecutorContext::cleanupAllExecutors when finished, since
         * if the executed fragment may have produced cached
         * subqueries.
         */
        UniqueTempTableResult executePlanFragment(ExecutorVector* executorVector,
                                                  int64_t* tuplesModified = NULL);

        // Call user-defined function
        NValue callJavaUserDefinedFunction(int32_t functionId, std::vector<NValue>& arguments);
        // Call the start method of the user-defined aggregate function
        void callJavaUserDefinedAggregateStart(int32_t functionId);
        // Call the assemble method of the user-defined aggregate function
        //void callJavaUserDefinedAggregateAssemble(int32_t functionId, const NValue& argument, int udafIndex);
        void callJavaUserDefinedAggregateAssemble(int32_t functionId, std::vector<NValue>& argVector, int32_t argCount, int32_t udafIndex);
        // Deserialize the byte array from each worker and call
        // the combine method of the user-defined aggregate function
        void callJavaUserDefinedAggregateCombine(int32_t functionId, const NValue& argument, int32_t udafIndex);
        // Serialize each worker's object to a byte array and send it to the coordinator
        NValue callJavaUserDefinedAggregateWorkerEnd(int32_t functionId, int32_t udafIndex);
        // Call the end method of the user-defined aggregate function
        NValue callJavaUserDefinedAggregateCoordinatorEnd(int32_t functionId, int32_t udafIndex);


        // Created to transition existing unit tests to context abstraction.
        // If using this somewhere new, consider if you're being lazy.
        void updateExecutorContextUndoQuantumForTest();

        // If an insert will fail due to row limit constraint and user
        // has defined a delete action to make space, this method
        // executes the corresponding fragment.
        //
        // Returns ENGINE_ERRORCODE_SUCCESS on success
        void executePurgeFragment(PersistentTable* table);

        // -------------------------------------------------
        // Dependency Transfer Functions
        // -------------------------------------------------
        void send(Table* dependency);

        int loadNextDependency(Table* destination);

        // -------------------------------------------------
        // Catalog Functions
        // -------------------------------------------------
        bool loadCatalog(int64_t timestamp, std::string const& catalogPayload);

        bool updateCatalog(int64_t timestamp, bool isStreamUpdate, std::string const& catalogPayload);

        bool processCatalogAdditions(int64_t timestamp, bool updateReplicated,
                bool isStreamUpdate, std::map<std::string, ExportTupleStream*> & purgedStreams);
        bool processReplicatedCatalogAdditions(int64_t timestamp, bool isStreamUpdate,
                std::map<std::string, ExportTupleStream*> & purgedStreams) {
            return processCatalogAdditions(timestamp, true, isStreamUpdate, purgedStreams);
        }
        void purgeMissingStreams(std::map<std::string, ExportTupleStream*> & purgedStreams);

        /**
        * Load table data into a persistent table specified by the tableId parameter.
        * This must be called at most only once before any data is loaded in to the table.
        */
        bool loadTable(int32_t tableId,
                       ReferenceSerializeInputBE& serializeIn,
                       int64_t txnId,
                       int64_t spHandle,
                       int64_t lastCommittedSpHandle,
                       int64_t uniqueId,
                       int64_t undoToken,
                       const LoadTableCaller &caller);

        /**
         * Reset the result buffer (use the nextResultBuffer by default)
         */
        void resetReusedResultOutputBuffer(const size_t startingPosition = 0, const int batchIndex = 1) {
            if (batchIndex == 0) {
                m_resultOutput.initializeWithPosition(
                        m_firstReusedResultBuffer, m_firstReusedResultCapacity, startingPosition);
            } else {
                m_resultOutput.initializeWithPosition(
                        m_nextReusedResultBuffer, m_nextReusedResultCapacity, startingPosition);
            }
            m_exceptionOutput.initializeWithPosition(
                    m_exceptionBuffer, m_exceptionBufferCapacity, startingPosition);
            *reinterpret_cast<int32_t*>(m_exceptionBuffer) =
                    static_cast<int32_t>(VoltEEExceptionType::VOLT_EE_EXCEPTION_TYPE_NONE);

        }

        void resetPerFragmentStatsOutputBuffer(int8_t perFragmentTimingEnabled = -1) {
            // The first byte in this buffer is a flag indicating whether the timing is
            // enabled for the current batch.
            // For VoltDB JNI, this byte is set by the Java top end.
            // In this case, we let m_perFragmentStatsOutput initialize skipping this byte,
            // so this byte will not be overwritten by VoltDBEngine.
            // For VoltDB IPC, the per-fragment stats buffer is not shared with the top end.
            // We have to write this byte in EE. This function will help to do that as well.
            // IPC calls will pass 0 or 1 into here instead of sticking with the default -1.
            int headerSize = perFragmentTimingEnabled > -1 ? 0 : sizeof(int8_t);
            m_perFragmentStatsOutput.initializeWithPosition(
                    m_perFragmentStatsBuffer, m_perFragmentStatsBufferCapacity, headerSize);
            if (perFragmentTimingEnabled > -1) {
                m_perFragmentStatsOutput.writeByte(perFragmentTimingEnabled);
            }
        }

        void resetUDFOutputBuffer(const size_t startingPosition = 0) {
            m_udfOutput.initializeWithPosition(m_udfBuffer, m_udfBufferCapacity, startingPosition);
        }

        ReferenceSerializeOutput* getExceptionOutputSerializer() { return &m_exceptionOutput; }

        void setBuffers(char* parameter_buffer,       int m_parameterBuffercapacity,
                        char* perFragmentStatsBuffer, int perFragmentStatsBufferCapacity,
                        char* udfBuffer,              int udfBufferCapacity,
                        char* firstResultBuffer,      int firstResultBufferCapacity,
                        char* nextResultBuffer,       int nextResultBufferCapacity,
                        char* exceptionBuffer,        int exceptionBufferCapacity);

        const char* getParameterBuffer() const { return m_parameterBuffer; }

        /** Returns the size of buffer for passing parameters to EE. */
        int getParameterBufferCapacity() const { return m_parameterBufferCapacity; }

        /**
         * Sets the output and exception buffer to be empty, and then
         * serializes the exception. */
        void serializeException(SerializableEEException const& e);

        /**
         * Retrieves the result buffer that could be either a buffer assigned through setBuffers() or
         * the fallback buffer created dynamically for results larger than 10MB
         */
        const unsigned char* getResultsBuffer() const;

        /**
         * Retrieves the size in bytes of the data that has been placed in the reused result buffer
         */
        int getResultsSize() const;

        /** Returns the buffer for receiving result tables from EE. */
        char* getReusedResultBuffer() const {
            return m_nextReusedResultBuffer;
        }

        /** Returns the size of buffer for receiving result tables from EE. */
        int getReusedResultBufferCapacity() const {
            return m_nextReusedResultCapacity;
        }

        int getPerFragmentStatsSize() const;
        char* getPerFragmentStatsBuffer() const {
            return m_perFragmentStatsBuffer;
        }
        int getPerFragmentStatsBufferCapacity() const {
            return m_perFragmentStatsBufferCapacity;
        }

        char* getUDFBuffer() const {
            return m_udfBuffer;
        }
        int getUDFBufferCapacity() const {
            return m_udfBufferCapacity;
        }

        int64_t* getBatchFragmentIdsContainer() {
            return m_batchFragmentIdsContainer;
        }

        int64_t* getBatchDepIdsContainer() {
            return m_batchDepIdsContainer;
        }

        /** check if this value hashes to the local partition */
        bool isLocalSite(NValue const& value) const;

        /** return partitionId for the provided hash */
        int32_t getPartitionForPkHash(int32_t pkHash) const;

        /** check if this hash is in the local partition */
        bool isLocalSite(int32_t pkHash) const;

        /** print out current hashinator */
        std::string dumpCurrentHashinator() const;

        // -------------------------------------------------
        // Non-transactional work methods
        // -------------------------------------------------

        /** Track the table that needs to be flushed at the target time */
        void setStreamFlushTarget(int64_t targetTime, StreamedTable* table);

        /** Perform once per second, non-transactional work. */
        void tick(int64_t timeInMillis, int64_t lastCommittedSpHandle);

        /** flush active work (like EL buffers) */
        void quiesce(int64_t lastCommittedSpHandle);

        std::string debug(void) const;

        /** DML executors call this to indicate how many tuples
         * have been modified */
        void addToTuplesModified(int64_t amount);

        // -------------------------------------------------
        // Statistics functions
        // -------------------------------------------------
        voltdb::StatsAgent& getStatsManager() {
            return m_statsManager;
        }

        /**
         * Retrieve a set of statistics and place them into the result buffer as a set of VoltTables.
         * @param selector StatisticsSelectorType indicating what set of statistics should be retrieved
         * @param locators Integer identifiers specifying what subset of possible statistical sources
         *                 should be polled. Probably a CatalogId.
         *                 Can be NULL in which case all possible sources for the selector should be included.
         * @param numLocators Size of locators array.
         * @param interval Whether to return counters since the beginning or since the last time this was called
         * @param Timestamp to embed in each row
         * @return Number of result tables, 0 on no results, -1 on failure.
         */
        int getStats(int selector, int locators[], int numLocators, bool interval, int64_t now);

        Pool* getStringPool() { return &m_stringPool; }

        LogManager* getLogManager() { return &m_logManager; }

        void setUndoToken(int64_t nextUndoToken) {
            if (nextUndoToken == INT64_MAX) {
                return;
            } else if (m_currentUndoQuantum != nullptr) {
                if (m_currentUndoQuantum->getUndoToken() == nextUndoToken) {
                    return;
                }

                vassert(nextUndoToken > m_currentUndoQuantum->getUndoToken());
            }
            setCurrentUndoQuantum(m_undoLog.generateUndoQuantum(nextUndoToken));
        }

        void releaseUndoToken(int64_t undoToken, bool isEmptyDRTxn);

        void undoUndoToken(int64_t undoToken);

        voltdb::UndoQuantum* getCurrentUndoQuantum() const {
            return m_currentUndoQuantum;
        }

        Topend* getTopend() const {
            return m_topend;
        }

        bool isLowestSite() const {
            return m_isLowestSite;
        }

        void setLowestSiteForTest() {
            m_isLowestSite = true;
        }

        int getSnapshotSchema(CatalogId tableId, HiddenColumnFilter::Type hiddenColumnFilterType);

        /**
         * Activate a table stream of the specified type for the specified table.
         * Returns true on success and false on failure
         */
        bool activateTableStream(CatalogId tableId, TableStreamType streamType,
                HiddenColumnFilter::Type hiddenColumnFilterType, int64_t undoToken,
                ReferenceSerializeInputBE& serializeIn);

        /**
         * Serialize tuples to output streams from a table in COW mode.
         * Overload that serializes a stream position array.
         * Return remaining tuple count, 0 if done, or TABLE_STREAM_SERIALIZATION_ERROR on error.
         */
        int64_t tableStreamSerializeMore(CatalogId tableId,
                TableStreamType streamType, ReferenceSerializeInputBE& serializeIn);

        /**
         * Serialize tuples to output streams from a table in COW mode.
         * Overload that populates a position vector provided by the caller.
         * Return remaining tuple count, 0 if done, or TABLE_STREAM_SERIALIZATION_ERROR on error.
         */
        int64_t tableStreamSerializeMore(CatalogId tableId, TableStreamType streamType,
                ReferenceSerializeInputBE& serializeIn, std::vector<int>& retPositions);

        /**
         * Set the export stream positions for the given stream
         *
         * @param ackOffset the reference to the USO of the next row inserted in the stream
         * @param seqNo the reference to the sequenceNumber of the next inserted row
         * @param generationIdCreated the reference to the initial creation generation ID of the export stream
         * @param streamName the name of the stream we want to update the state for
         */
        void setExportStreamPositions(int64_t ackOffset, int64_t seqNo, int64_t generationIdCreated,
                std::string streamName);

        /**
         * Complete the deletion of the Migrated Table rows.
         *
         * @param txnId The transactionId of the currently executing stored procedure
         * @param spHandle The spHandle of the currently executing stored procedure
         * @param uniqueId The uniqueId of the currently executing stored procedure
         * @param mTableName The name of the table that the deletes should be applied to
         * @param deletableTxnId The transactionId of the last row that can be deleted
         * @param undoToken Commit/Rollback token for this delete call
         * @return true if more rows to be deleted
         */
        bool deleteMigratedRows(int64_t txnId, int64_t spHandle, int64_t uniqueId,
                std::string const& tableName, int64_t deletableTxnId, int64_t undoToken);

        void getUSOForExportTable(size_t& ackOffset, int64_t& seqNo, int64_t &genId, std::string const& streamName);

        /**
         * Retrieve a hash code for the specified table
         */
        size_t tableHashCode(int32_t tableId);

        void updateHashinator(char const* config, int32_t* configPtr, uint32_t numTokens);

        /**
         * Apply multiple binary logs which can either be one log with multiple transactions to one partition or
         * multiple logs which are one multi-partition transaction
         */
        int64_t applyBinaryLog(int64_t txnId, int64_t spHandle, int64_t lastCommittedSpHandle, int64_t uniqueId,
                int32_t remoteClusterId, int64_t undoToken, char const* logs);

        /*
         * Execute an arbitrary task represented by the task id and serialized parameters.
         * Returns serialized representation of the results
         */
        void executeTask(TaskType taskType, ReferenceSerializeInputBE& taskInfo);

        void rebuildTableCollections(bool updateReplicated = false, bool fromScratch = true);
        void rebuildReplicatedTableCollections() {
            rebuildTableCollections(true);
        }

        int64_t tempTableMemoryLimit() const { return m_tempTableMemoryLimit; }

        int64_t tempTableLogLimit() const {
            return (m_tempTableMemoryLimit * 3) / 4;
        }

        void setPartitionIdForTest(int32_t partitionId) {
            m_partitionId = partitionId;
        }
        int32_t getPartitionId() const {
            return m_partitionId;
        }

        void setViewsEnabled(const std::string& viewNames, bool value);

        virtual ExportTupleStream** getNewestExportStreamWithPendingRowsForAssignment() {
            return &m_newestExportStreamWithPendingRows;
        }

        virtual ExportTupleStream** getOldestExportStreamWithPendingRowsForAssignment() {
            return &m_oldestExportStreamWithPendingRows;
        }

        void disableExternalStreams();

        bool externalStreamsEnabled();

        /**
         * Store a kipling group in the system tables
         */
        int32_t storeKiplingGroup(int64_t undoToken, SerializeInputBE& in);

        /**
         * Delete a kipling group and all related metadata
         */
        int32_t deleteKiplingGroup(int64_t undoToken, const NValue& groupId);

        /**
         * Start or continue a fetch of all kipling groups.
         * Return 1 if there are more groups to fetch, 0 if there are no more groups or -1 if there was an error
         */
        int32_t fetchKiplingGroups(int32_t maxResultSize, const NValue& startGroupId);

        /**
         * Store topic parition offsets for a kipling group
         */
        int32_t commitKiplingGroupOffsets(int64_t spUniqueId, int64_t undoToken, int16_t requestVersion,
                const NValue& groupId, SerializeInputBE& in);

        /**
         * Fetch topic parition offsets for a kipling group
         */
        int32_t fetchKiplingGroupOffsets(int16_t requestVersion, const NValue& groupId, SerializeInputBE& in);

        /*
         * Delete expired offsets of standalone groups
         */
        int32_t deleteExpiredKiplingOffsets(int64_t undoToken, int64_t deleteOlderThan);

    private:
        /*
         * Tasks dispatched by executeTask
         */
        void dispatchValidatePartitioningTask(ReferenceSerializeInputBE& taskInfo);

        void collectDRTupleStreamStateInfo();

        void setCurrentUndoQuantum(voltdb::UndoQuantum* undoQuantum);

        // -------------------------------------------------
        // Initialization Functions
        // -------------------------------------------------
        void processCatalogDeletes(int64_t timestamp, bool updateReplicated,
                std::map<std::string, ExportTupleStream*> & purgedStreams);
        void processReplicatedCatalogDeletes(int64_t timestamp, std::map<std::string, ExportTupleStream*> & purgedStreams) {
            processCatalogDeletes(timestamp, true, purgedStreams);
        }

        void initMaterializedViewsAndLimitDeletePlans(bool updateReplicated = false);
        void initReplicatedMaterializedViewsAndLimitDeletePlans() {
            initMaterializedViewsAndLimitDeletePlans(true);
        }

        template<class TABLE>
        void initMaterializedViews(catalog::Table* catalogTable, TABLE* storageTable, bool updateReplicated);

        bool updateCatalogDatabaseReference();

        void resetDRConflictStreamedTables();

        /**
         * Execute a single plan fragment.
         */
        int executePlanFragment(int64_t planfragmentId, int64_t inputDependencyId, bool traceOn);

        /**
         * Set up the vector of executors for a given fragment id.
         * Get the vector from the cache if the fragment id is there.
         * If not, get a plan from the Java topend and load it up,
         * putting it in the cache and possibly bumping something else.
         */
        void setExecutorVectorForFragmentId(int64_t fragId);

        bool checkTempTableCleanup(ExecutorVector* execsForFrag);

        void loadBuiltInJavaFunctions();

        void attachTupleStream(StreamedTable* streamedTable, const std::string& streamName,
                std::map<std::string, ExportTupleStream*> & purgedStreams, int64_t timestamp);

        // user defined aggregate functions helper functions
        /*
         * put buffer size needed, function id, udaf index, row count and a list of rows (if there is any)
         * into the buffer, so that the java side would receive them
         */
        void serializeToUDFOutputBuffer(int32_t functionId, const NValue& argument, ValueType type, int32_t udafIndex);
        void serializeToUDFOutputBuffer(int32_t functionId,
                std::vector<NValue> const& argVector, int32_t argCount, ValueType type, int32_t udafIndex);
        /*
         * if the info related to this functionId is not found, throw an exception
         */
        void checkUserDefinedFunctionInfo(UserDefinedFunctionInfo *info, int32_t functionId);
        /*
         * if the return code from java is not 0 (something went wrong), throw an exception
         */
        void checkJavaFunctionReturnCode(int32_t returnCode, const char* name);
        /*
         * convert the return value from java side to its correct type. If something went wrong
         * for the conversion, throw an exception.
         */
        NValue udfResultHelper(int32_t returnCode, bool partition_table, ValueType type);

        void createSystemTables();

        // -------------------------------------------------
        // Data Members
        // -------------------------------------------------
        /** True if any fragments in a batch have modified any tuples */
        bool m_dirtyFragmentBatch;

        int m_currentIndexInBatch = -1;

        boost::scoped_ptr<EnginePlanSet> m_plans;

        voltdb::UndoLog m_undoLog;

        voltdb::UndoQuantum* m_currentUndoQuantum = nullptr;

        int64_t m_siteId = -1;

        bool m_isLowestSite = false;

        int32_t m_partitionId = -1;
        int32_t m_clusterIndex;

        std::unique_ptr<TheHashinator> m_hashinator;

        size_t m_startOfResultBuffer;

        int64_t m_tempTableMemoryLimit;

        /*
         * Catalog delegates hashed by path.
         */
        std::map<std::string, TableCatalogDelegate*> m_catalogDelegates;

        std::map<std::string, TableCatalogDelegate*> m_delegatesByName;

        // map catalog table id to table pointers
        std::map<CatalogId, Table*> m_tables;

        // map system table id to table pointers
        std::map<SystemTableId, PersistentTable *> m_systemTables;

        // map catalog table name to table pointers
        std::map<std::string, Table*> m_tablesByName;

        // This maps the function Ids to their corresponding UserDefinedFunctionInfo structures,
        // which stores the parameter types and the return type.
        // The VoltDBEngine will use that information to do correct type casting before handing
        // values to the shared UDF buffer.
        std::unordered_map<int, UserDefinedFunctionInfo*> m_functionInfo;

        /*
         * Map of catalog table ids to snapshotting tables.
         * Note that these tableIds are the ids when the snapshot
         * was initiated. The snapshot processor in Java does not
         * update tableIds when the catalog changes. The point of
         * reference, therefore, is consistently the catalog at
         * the point of snapshot initiation. It is always invalid
         * to try to map this tableId back to catalog::Table via
         * the catalog, at least w/o comparing table names.
         */
        std::map<int32_t, PersistentTable*> m_snapshottingTables;

        /*
         * Map of table signatures to exporting tables.
         */
        std::map<std::string, StreamedTable*> m_exportingTables;

        /*
         * Pointer to begin/end export streams that need to be flushed ordered by first row create time
         */
        ExportTupleStream* m_oldestExportStreamWithPendingRows = nullptr;
        ExportTupleStream* m_newestExportStreamWithPendingRows = nullptr;

        /*
         * Only includes non-materialized tables
         */
        std::unordered_map<int64_t, PersistentTable*> m_tablesBySignatureHash;

        /**
         * System Catalog.
         */
        boost::scoped_ptr<catalog::Catalog> m_catalog;

        catalog::Database* m_database;

        bool m_isActiveActiveDREnabled = false;

        /** buffer object for result tables. set when the result table is sent out to localsite. */
        FallbackSerializeOutput m_resultOutput;

        /** buffer object for exceptions generated by the EE **/
        ReferenceSerializeOutput m_exceptionOutput;

        /** buffer object for per-fragment stats numbers generated by the EE **/
        ReferenceSerializeOutput m_perFragmentStatsOutput;

        /** buffer object for exchanging the function ID, the UDF parameters, and the return value. **/
        ReferenceSerializeOutput m_udfOutput;

        /** buffer object to pass parameters to EE. */
        char const* m_parameterBuffer;

        /** size of parameter_buffer. */
        int m_parameterBufferCapacity;

        /** The buffer to pass per-fragment stats to the Topend
            When executing a batch, this buffer will be populated with the following contents:
            {
                int8_t perFragmentTimingEnabled;
                int32_t succeededFragmentsCount;
                int64_t[] fragmentExecutionTimes; // in nanoseconds.
            }
            If the batch execution succeeded, fragmentExecutionTimes will contain (succeededFragmentsCount) time measurements.
            In the case of batch failure, fragmentExecutionTimes will contain (succeededFragmentsCount + 1) time measurements,
                including the execution time for the failing fragment. */
        char* m_perFragmentStatsBuffer;

        /** size of the per-fragment statistics buffer */
        int m_perFragmentStatsBufferCapacity;

        char* m_exceptionBuffer;

        int m_exceptionBufferCapacity;

        /** buffer object to receive all but the final result tables from EE. */
        char* m_firstReusedResultBuffer;

        /** size of m_firstReusedResultBuffer. */
        int m_firstReusedResultCapacity;

        /** buffer object to receive final result tables from EE. */
        char* m_nextReusedResultBuffer;

        /** size of m_finalReusedResultBuffer. */
        int m_nextReusedResultCapacity;

        // The shared buffer for the Java top end and the EE to exchange data that is necessary for UDF execution.
        char* m_udfBuffer;
        int m_udfBufferCapacity;

        // arrays to hold fragment ids and dep ids from java
        // n.b. these are 8k each, should be boost shared arrays?
        int64_t m_batchFragmentIdsContainer[MAX_BATCH_COUNT];

        int64_t m_batchDepIdsContainer[MAX_BATCH_COUNT];

        // used for sending and recieving deps
        // set by the executeQuery / executeFrag type methods
        int m_currentInputDepId = -1;

        /** Stats manager for this execution engine **/
        voltdb::StatsAgent m_statsManager;

        /*
         * Pool for short lived strings that will not live past the return back to Java.
         */
        Pool m_stringPool{16777216, 2};

        /*
         * When executing a plan fragment this is set to the number of result dependencies
         * that have been serialized into the m_resultOutput
         */
        int32_t m_numResultDependencies = 0;

        LogManager m_logManager;

        char* m_templateSingleLongTable = nullptr;

        const static int m_templateSingleLongTableSize =
                4 + // depid
                4 + // table size
                4 + // header size
                2 + // status code
                1 + // column count
                1 + // column type
                4 + 15 + // column name (length + modified_tuples)
                4 + // tuple count
                4 + // first row size
                8;// modified tuples

        Topend* m_topend;

        // For data from engine that must be shared/distributed to
        // other components. (Components MUST NOT depend on VoltDBEngine.h).
        ExecutorContext* m_executorContext = nullptr;

        int32_t m_compactionThreshold;

        /*
         * DR conflict streamed tables
         */
        StreamedTable* m_drPartitionedConflictStreamedTable = nullptr;

        // Todo: ? handle move ReplicatedConflict table
        StreamedTable* m_drReplicatedConflictStreamedTable = nullptr;

        //Stream of DR data generated by this engine, don't use them directly unless you know which mode
        //are we running now, use m_executorContext->drStream() and m_executorContext->drReplicatedStream()
        //instead.
        AbstractDRTupleStream* m_drStream = nullptr;

        AbstractDRTupleStream* m_drReplicatedStream = nullptr;

        static AbstractDRTupleStream* s_drReplicatedStream;

        //Sink for applying DR binary logs
        BinaryLogSinkWrapper m_wrapper;

        /** current ExecutorVector **/
        ExecutorVector* m_currExecutorVec = nullptr;

        // This stateless member acts as a counted reference to keep the ThreadLocalPool alive
        // just while this VoltDBEngine is alive. That simplifies valgrind-compliant process shutdown.
        ThreadLocalPool m_tlPool;

        std::unique_ptr<kipling::GroupStore> m_groupStore;

        // static variable for sharing loadTable result (and exception) across VoltDBEngines
        static VoltEEExceptionType s_loadTableException;
        static int s_drHiddenColumnSize;
};

inline bool startsWith(const string& s1, const string& s2) {
    return s2.size() <= s1.size() && s1.compare(0, s2.size(), s2) == 0;
}

} // namespace voltdb

