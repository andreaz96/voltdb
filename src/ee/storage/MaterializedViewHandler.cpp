/* This file is part of VoltDB.
 * Copyright (C) 2008-2018 VoltDB Inc.
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

#include "MaterializedViewHandler.h"
#include "catalog/column.h"
#include "catalog/statement.h"
#include "catalog/table.h"
#include "catalog/tableref.h"
#include "common/executorcontext.hpp"
#include "indexes/tableindex.h"
#include "TableCatalogDelegate.hpp"
#include "temptable.h"


ENABLE_BOOST_FOREACH_ON_CONST_MAP(TableRef);
ENABLE_BOOST_FOREACH_ON_CONST_MAP(Statement);
typedef std::pair<std::string, catalog::TableRef*> LabeledTableRef;
typedef std::pair<std::string, catalog::Statement*> LabeledStatement;

namespace voltdb {

    MaterializedViewHandler::MaterializedViewHandler(PersistentTable* destTable,
                                                     catalog::MaterializedViewHandlerInfo* mvHandlerInfo,
                                                     int32_t groupByColumnCount,
                                                     VoltDBEngine* engine) :
            m_destTable(destTable),
            m_index(destTable->primaryKeyIndex()),
            m_groupByColumnCount(groupByColumnCount),
            m_replicatedWrapper(NULL) {
        if (engine == NULL) return;  // Need this when this is an instance of ReplicatedMaterializedViewHandler
        install(mvHandlerInfo, engine);
        ConditionalExecuteWithMpMemory useMpMemoryIfReplicated(m_destTable->isCatalogTableReplicated());
        setUpAggregateInfo(mvHandlerInfo);
        setUpCreateQuery(mvHandlerInfo, engine);
        setUpMinMaxQueries(mvHandlerInfo, engine);
        setUpBackedTuples();
        m_dirty = false;
    }

    MaterializedViewHandler::~MaterializedViewHandler() {
        if (m_sourceTables.empty()) return;  // Need this when this is an instance of ReplicatedMaterializedViewHandler
        bool viewHandlerPartitioned = !m_destTable->isCatalogTableReplicated();
        do {
            dropSourceTable(viewHandlerPartitioned, m_sourceTables.begin());
        } while (!m_sourceTables.empty());
        if (m_replicatedWrapper) {
            delete m_replicatedWrapper;
        }
    }

    void MaterializedViewHandler::addSourceTable(bool viewHandlerPartitioned,
                PersistentTable *sourceTable, int32_t relativeTableIndex, VoltDBEngine* engine) {
        VOLT_DEBUG("Adding source table %s (%p) for view %s (%p)", sourceTable->name().c_str(), sourceTable, m_destTable->name().c_str(), m_destTable);
        if (viewHandlerPartitioned == sourceTable->isCatalogTableReplicated()) {
            assert(viewHandlerPartitioned);
            // We are adding our (partitioned) ViewHandler to a Replicated Table
            if (!m_replicatedWrapper) {
                m_replicatedWrapper = new ReplicatedMaterializedViewHandler(m_destTable, this, engine->getPartitionId());
            }
            SynchronizedThreadLock::lockReplicatedResource();
            sourceTable->addViewHandler(m_replicatedWrapper);
            SynchronizedThreadLock::unlockReplicatedResource();
//            else {
//                assert(false);
//                // We are adding our (replicated) ViewHandler to each partition's instance of the partitioned table
//                assert(SynchronizedThreadLock::isInSingleThreadMode());
//                BOOST_FOREACH (SharedEngineLocalsType::value_type& enginePair, SynchronizedThreadLock::s_enginesByPartitionId) {
//                    EngineLocals& curr = enginePair.second;
//                    VoltDBEngine* currEngine = curr.context->getContextEngine();
//                    ExecutorContext::assignThreadLocals(curr);
//                    auto partitionedTable = dynamic_cast<PersistentTable*>(currEngine->getTableById(relativeTableIndex));
//                    assert(partitionedTable);
//                    partitionedTable->addViewHandler(this);
//                }
//                SynchronizedThreadLock::assumeLowestSiteContext();
//            }
        }
        else {
            sourceTable->addViewHandler(this);
        }
#ifndef NDEBUG
        std::pair<std::map<PersistentTable*, int32_t>::iterator,bool> ret =
#endif
        #if __cplusplus >= 201103L
        m_sourceTables.emplace(sourceTable, relativeTableIndex);
        #else
        m_sourceTables.insert(std::make_pair(sourceTable, relativeTableIndex));
        #endif
        assert(ret.second);

        m_dirty = true;
    }

    void MaterializedViewHandler::dropSourceTable(bool viewHandlerPartitioned,
            std::map<PersistentTable*, int32_t>::iterator it) {
        assert(!m_sourceTables.empty());
        auto sourceTable = it->first;
//        auto relativeTableIndex = it->second;
        if (viewHandlerPartitioned == sourceTable->isCatalogTableReplicated()) {
            assert(viewHandlerPartitioned);
            // We are dropping our (partitioned) ViewHandler to a Replicated Table
            SynchronizedThreadLock::lockReplicatedResource();
            sourceTable->dropViewHandler(m_replicatedWrapper);
            SynchronizedThreadLock::unlockReplicatedResource();
//            else {
//                // We are dropping our (replicated) ViewHandler to each partition's instance of the partitioned table
//                assert(SynchronizedThreadLock::isInSingleThreadMode());
//                BOOST_FOREACH (SharedEngineLocalsType::value_type& enginePair, SynchronizedThreadLock::s_enginesByPartitionId) {
//                    EngineLocals& curr = enginePair.second;
//                    VoltDBEngine* currEngine = curr.context->getContextEngine();
//                    ExecutorContext::assignThreadLocals(curr);
//                    auto partitionedTable = dynamic_cast<PersistentTable*>(currEngine->getTableById(relativeTableIndex));
//                    assert(partitionedTable);
//                    partitionedTable->dropViewHandler(this);
//                }
//                SynchronizedThreadLock::assumeLowestSiteContext();
//            }
        }
        else {
            sourceTable->dropViewHandler(this);
        }
        // The last element is now excess.
        m_sourceTables.erase(it);
        m_dirty = true;
    }

    void MaterializedViewHandler::dropSourceTable(PersistentTable *sourceTable) {
        std::map<PersistentTable*, int32_t>::iterator it = m_sourceTables.find(sourceTable);
        assert(it != m_sourceTables.end());
        dropSourceTable(!m_destTable->isCatalogTableReplicated(), it);
    }

    void MaterializedViewHandler::install(catalog::MaterializedViewHandlerInfo *mvHandlerInfo,
                                          VoltDBEngine *engine) {
        const std::vector<TableIndex*>& targetIndexes = m_destTable->allIndexes();
        BOOST_FOREACH(TableIndex *index, targetIndexes) {
            if (index != m_index) {
                m_updatableIndexList.push_back(index);
            }
        }
        // Delete the existing handler if exists. When the existing handler is destructed,
        // it will automatically removes itself from all the viewsToTrigger lists of its source tables.
        delete m_destTable->m_mvHandler;
        // The handler will not only be installed on the view table, but also the source tables.
        m_destTable->m_mvHandler = this;
        bool viewHandlerPartitioned = !m_destTable->isCatalogTableReplicated();
        BOOST_FOREACH (LabeledTableRef labeledTableRef, mvHandlerInfo->sourceTables()) {
            catalog::TableRef *sourceTableRef = labeledTableRef.second;
            TableCatalogDelegate *sourceTcd =  engine->getTableDelegate(sourceTableRef->table()->name());
            PersistentTable *sourceTable = sourceTcd->getPersistentTable();
            assert(sourceTable);
            int32_t relativeTableIndex = sourceTableRef->table()->relativeIndex();
            addSourceTable(viewHandlerPartitioned, sourceTable, relativeTableIndex, engine);
        }
    }

    void MaterializedViewHandler::setUpAggregateInfo(catalog::MaterializedViewHandlerInfo *mvHandlerInfo) {
        const catalog::CatalogMap<catalog::Column>& columns = mvHandlerInfo->destTable()->columns();
        m_aggColumnCount = columns.size() - m_groupByColumnCount;
        m_aggTypes.resize(m_aggColumnCount);
        for (catalog::CatalogMap<catalog::Column>::field_map_iter colIterator = columns.begin();
                colIterator != columns.end(); colIterator++) {
            const catalog::Column *destCol = colIterator->second;
            if (destCol->index() < m_groupByColumnCount) {
                continue;
            }
            // The index into the per-agg metadata starts as a materialized view column index
            // but needs to be shifted down for each column that has no agg option
            // -- that is, -1 for each "group by"
            std::size_t aggIndex = destCol->index() - m_groupByColumnCount;
            m_aggTypes[aggIndex] = static_cast<ExpressionType>(destCol->aggregatetype());
            switch(m_aggTypes[aggIndex]) {
                case EXPRESSION_TYPE_AGGREGATE_COUNT_STAR:
                    m_countStarColumnIndex = destCol->index();
                case EXPRESSION_TYPE_AGGREGATE_SUM:
                case EXPRESSION_TYPE_AGGREGATE_COUNT:
                case EXPRESSION_TYPE_AGGREGATE_MIN:
                case EXPRESSION_TYPE_AGGREGATE_MAX:
                    break; // legal value
                default: {
                    char message[128];
                    snprintf(message, 128, "Error in materialized view aggregation %d expression type %s",
                             (int)aggIndex, expressionToString(m_aggTypes[aggIndex]).c_str());
                    throw SerializableEEException(VOLT_EE_EXCEPTION_TYPE_EEEXCEPTION,
                                                  message);
                }
            }
        }
    }

    void MaterializedViewHandler::setUpCreateQuery(catalog::MaterializedViewHandlerInfo *mvHandlerInfo,
                                                      VoltDBEngine *engine) {
        catalog::Statement *createQueryStatement = mvHandlerInfo->createQuery().get("createQuery");
        m_createQueryExecutorVector = ExecutorVector::fromCatalogStatement(engine, createQueryStatement);
        m_createQueryExecutorVector->getRidOfSendExecutor();
#ifdef VOLT_TRACE_ENABLED
        if (ExecutorContext::getExecutorContext()->m_siteId == 0) {
            const std::string& hexString = createQueryStatement->explainplan();
            assert(hexString.length() % 2 == 0);
            int bufferLength = (int)hexString.size() / 2 + 1;
            char* explanation = new char[bufferLength];
            boost::shared_array<char> memoryGuard(explanation);
            catalog::Catalog::hexDecodeString(hexString, explanation);
            cout << m_destTable->name() << " MaterializedViewHandler::setUpCreateQuery()\n" << explanation << endl;
        }
#endif
    }

    void MaterializedViewHandler::setUpMinMaxQueries(catalog::MaterializedViewHandlerInfo *mvHandlerInfo,
                                                 VoltDBEngine *engine) {
        m_minMaxExecutorVectors.resize(mvHandlerInfo->fallbackQueryStmts().size());
        BOOST_FOREACH (LabeledStatement labeledStatement, mvHandlerInfo->fallbackQueryStmts()) {
            int key = std::stoi(labeledStatement.first);
            catalog::Statement *stmt = labeledStatement.second;
            boost::shared_ptr<ExecutorVector> execVec = ExecutorVector::fromCatalogStatement(engine, stmt);
            execVec->getRidOfSendExecutor();
            m_minMaxExecutorVectors[key] = execVec;
        }
    }

    // If the source table(s) is not empty when the view is created,
    // or for non-grouped views* we need to execute the plan directly
    // to catch up with the existing data.
    //TODO: *non-grouped views could instead set up a hard-coded initial
    // row as they do in the single-table case to avoid querying empty tables.
    void MaterializedViewHandler::catchUpWithExistingData(bool fallible) {
        ExecutorContext* ec = ExecutorContext::getExecutorContext();
        UniqueTempTableResult viewContent = ec->getEngine()->executePlanFragment(m_createQueryExecutorVector.get());
        std::unique_ptr<TableIterator> ti(viewContent->makeIterator());
        TableTuple tuple(viewContent->schema());
        while (ti->next(tuple)) {
            //* enable to debug */ std::cout << "DEBUG: inserting catchup tuple into " << m_destTable->name() << std::endl;
            m_destTable->insertPersistentTuple(tuple, fallible, true);
        }

        ec->cleanupAllExecutors();
        /* // enable to debug
        std::cout << "DEBUG: join view initially there are "
                  << m_destTable->activeTupleCount()
                  << " tuples in " << m_destTable->name() << std::endl;
        //*/
    }

    void MaterializedViewHandler::setUpBackedTuples() {
        m_existingTuple = TableTuple(m_destTable->schema());
        m_updatedTupleStorage.init(m_destTable->schema());
        m_updatedTuple = m_updatedTupleStorage.tuple();
    }

    bool MaterializedViewHandler::findExistingTuple(const TableTuple &deltaTuple) {
        // For the case where there is no grouping column, like SELECT COUNT(*) FROM T;
        // We directly return the only row in the view. See ENG-7872.
        if (m_groupByColumnCount == 0) {
            TableIterator* iterator = m_destTable->makeIterator();
            iterator->next(m_existingTuple);
            // Please note that if there is no group by columns, the view shall always have one row.
            // This row will be initialized when the view is constructed. We have special code path for that. -yzhang
            assert( ! m_existingTuple.isNullTuple());
            delete iterator;
            return true;
        }

        IndexCursor indexCursor(m_index->getTupleSchema());
        // determine if the row exists (create the empty one if it doesn't)
        m_index->moveToKeyByTuple(&deltaTuple, indexCursor);
        m_existingTuple = m_index->nextValueAtKey(indexCursor);
        return ! m_existingTuple.isNullTuple();
    }

    void MaterializedViewHandler::mergeTupleForInsert(const TableTuple &deltaTuple) {
        // set up the group-by columns
        for (int colindex = 0; colindex < m_groupByColumnCount; colindex++) {
            // note that if the tuple is in the mv's target table,
            // tuple values should be pulled from the existing tuple in
            // that table. This works around a memory ownership issue
            // related to out-of-line strings.
            NValue value = m_existingTuple.getNValue(colindex);
            m_updatedTuple.setNValue(colindex, value);
        }
        // Aggregations
        int aggOffset = m_groupByColumnCount;
        for (int aggIndex = 0, columnIndex = aggOffset; aggIndex < m_aggColumnCount; aggIndex++, columnIndex++) {
            NValue existingValue = m_existingTuple.getNValue(columnIndex);
            NValue newValue = deltaTuple.getNValue(columnIndex);
            if (newValue.isNull()) {
                newValue = existingValue;
            }
            else {
                switch(m_aggTypes[aggIndex]) {
                    case EXPRESSION_TYPE_AGGREGATE_SUM:
                    case EXPRESSION_TYPE_AGGREGATE_COUNT:
                    case EXPRESSION_TYPE_AGGREGATE_COUNT_STAR:
                        if (!existingValue.isNull()) {
                            newValue = existingValue.op_add(newValue);
                        }
                        break;
                    case EXPRESSION_TYPE_AGGREGATE_MIN:
                        // ignore any new value that is not strictly an improvement
                        if (!existingValue.isNull() && newValue.compare(existingValue) >= 0) {
                            newValue = existingValue;
                        }
                        break;
                    case EXPRESSION_TYPE_AGGREGATE_MAX:
                        // ignore any new value that is not strictly an improvement
                        if (!existingValue.isNull() && newValue.compare(existingValue) <= 0) {
                            newValue = existingValue;
                        }
                        break;
                    default:
                        assert(false); // Should have been caught when the matview was loaded.
                        // no break
                }
            }
            m_updatedTuple.setNValue(columnIndex, newValue);
        }
    }

    void MaterializedViewHandler::handleTupleInsert(PersistentTable *sourceTable, bool fallible) {
        // Within the lifespan of this ScopedDeltaTableContext, the changed source table will enter delta table mode.
        ScopedDeltaTableContext dtContext(sourceTable);
        ExecutorContext* ec = ExecutorContext::getExecutorContext();
        vector<AbstractExecutor*> executorList = m_createQueryExecutorVector->getExecutorList();
        UniqueTempTableResult delta = ec->executeExecutors(executorList);
        std::unique_ptr<TableIterator> ti(delta->makeIterator());
        TableTuple deltaTuple(delta->schema());
        while (ti->next(deltaTuple)) {
            bool found = findExistingTuple(deltaTuple);
            if (found) {
                mergeTupleForInsert(deltaTuple);
                // Shouldn't need to update group-key-only indexes such as the primary key
                // since their keys shouldn't ever change, but do update other indexes.
                m_destTable->updateTupleWithSpecificIndexes(m_existingTuple, m_updatedTuple,
                                                            m_updatableIndexList, fallible);
            }
            else {
                m_destTable->insertPersistentTuple(deltaTuple, fallible);
            }
        }
    }

    void MaterializedViewHandler::mergeTupleForDelete(const TableTuple &deltaTuple) {
        // set up the group-by columns
        for (int colindex = 0; colindex < m_groupByColumnCount; colindex++) {
            // note that if the tuple is in the mv's target table,
            // tuple values should be pulled from the existing tuple in
            // that table. This works around a memory ownership issue
            // related to out-of-line strings.
            NValue value = m_existingTuple.getNValue(colindex);
            m_updatedTuple.setNValue(colindex, value);
        }
        // check new count of tuples
        NValue existingCount = m_existingTuple.getNValue(m_countStarColumnIndex);
        NValue deltaCount = deltaTuple.getNValue(m_countStarColumnIndex);
        NValue newCount = existingCount.op_subtract(deltaCount);

        int aggOffset = m_groupByColumnCount;
        NValue newValue;
        if (newCount.isZero()) {
            // no group by key, no rows, aggs will be null except for count().
            for (int aggIndex = 0, columnIndex = aggOffset; aggIndex < m_aggColumnCount; aggIndex++, columnIndex++) {
                if (m_aggTypes[aggIndex] == EXPRESSION_TYPE_AGGREGATE_COUNT
                    || m_aggTypes[aggIndex] == EXPRESSION_TYPE_AGGREGATE_COUNT_STAR) {
                    newValue = ValueFactory::getBigIntValue(0);
                }
                else {
                    newValue = NValue::getNullValue(m_updatedTuple.getSchema()->columnType(columnIndex));
                }
                m_updatedTuple.setNValue(columnIndex, newValue);
            }
        }
        else {
            // Aggregations
            int minMaxColumnIndex = 0;
            for (int aggIndex = 0, columnIndex = aggOffset; aggIndex < m_aggColumnCount; aggIndex++, columnIndex++) {
                NValue existingValue = m_existingTuple.getNValue(columnIndex);
                NValue deltaValue = deltaTuple.getNValue(columnIndex);
                newValue = existingValue;
                ExpressionType aggType = m_aggTypes[aggIndex];

                if (! deltaValue.isNull()) {
                    switch(aggType) {
                        case EXPRESSION_TYPE_AGGREGATE_COUNT_STAR:
                        case EXPRESSION_TYPE_AGGREGATE_SUM:
                        case EXPRESSION_TYPE_AGGREGATE_COUNT:
                            newValue = existingValue.op_subtract(deltaValue);
                            break;
                        case EXPRESSION_TYPE_AGGREGATE_MIN:
                        case EXPRESSION_TYPE_AGGREGATE_MAX:
                            if (existingValue.compare(deltaValue) == 0) {
                                // re-calculate MIN / MAX
                                newValue = fallbackMinMaxColumn(columnIndex, minMaxColumnIndex);
                            }
                            break;
                        default:
                            assert(false); // Should have been caught when the matview was loaded.
                            // no break
                    }
                }

                if (aggType == EXPRESSION_TYPE_AGGREGATE_MIN
                    || aggType == EXPRESSION_TYPE_AGGREGATE_MAX) {
                    minMaxColumnIndex++;
                }

                m_updatedTuple.setNValue(columnIndex, newValue);
            }
        }
    }

    NValue MaterializedViewHandler::fallbackMinMaxColumn(int columnIndex, int minMaxColumnIndex) {
        NValue newValue = NValue::getNullValue(m_destTable->schema()->columnType(columnIndex));
        ExecutorContext* ec = ExecutorContext::getExecutorContext();
        NValueArray &params = ec->getParameterContainer();
        // We first backup the params array and fill it with our parameters.
        // Is it really necessary???
        vector<NValue> backups(m_groupByColumnCount+1);
        for (int i=0; i<m_groupByColumnCount; i++) {
            backups[i] = params[i];
            params[i] = m_existingTuple.getNValue(i);
        }
        backups[m_groupByColumnCount] = params[m_groupByColumnCount];
        params[m_groupByColumnCount] = m_existingTuple.getNValue(columnIndex);
        // Then we get the executor vectors we need to run:
        vector<AbstractExecutor*> executorList = m_minMaxExecutorVectors[minMaxColumnIndex]->getExecutorList();
        UniqueTempTableResult resultTable = ec->executeExecutors(executorList);
        TableIterator* ti = resultTable->makeIterator();
        TableTuple resultTuple(resultTable->schema());
        if (ti->next(resultTuple)) {
            newValue = resultTuple.getNValue(0);
        }
        delete ti;
        // Now put the original parameters back.
        for (int i=0; i<=m_groupByColumnCount; i++) {
            params[i] = backups[i];
        }
        return newValue;
    }

    void MaterializedViewHandler::handleTupleDelete(PersistentTable *sourceTable, bool fallible) {
        // Within the lifespan of this ScopedDeltaTableContext, the changed source table will enter delta table mode.
        ScopedDeltaTableContext *dtContext = new ScopedDeltaTableContext(sourceTable);
        ExecutorContext* ec = ExecutorContext::getExecutorContext();
        vector<AbstractExecutor*> executorList = m_createQueryExecutorVector->getExecutorList();
        UniqueTempTableResult delta = ec->executeExecutors(executorList);
        std::unique_ptr<TableIterator> ti(delta->makeIterator());
        TableTuple deltaTuple(delta->schema());
        // The min/max value may need to be re-calculated, so we should terminate the delta table mode early
        // in order to run other queries.
        delete dtContext;
        while (ti->next(deltaTuple)) {
            bool found = findExistingTuple(deltaTuple);
            if (! found) {
                std::string name = m_destTable->name();
                throwFatalException("MaterializedViewHandler for table %s went"
                                    " looking for a tuple in the view and"
                                    " expected to find it but didn't", name.c_str());
            }
            NValue existingCount = m_existingTuple.getNValue(m_countStarColumnIndex);
            NValue deltaCount = deltaTuple.getNValue(m_countStarColumnIndex);

            if (existingCount.compare(deltaCount) == 0 && m_groupByColumnCount > 0) {
                m_destTable->deleteTuple(m_existingTuple, fallible);
            }
            else {
                mergeTupleForDelete(deltaTuple);
                // Shouldn't need to update group-key-only indexes such as the primary key
                // since their keys shouldn't ever change, but do update other indexes.
                m_destTable->updateTupleWithSpecificIndexes(m_existingTuple, m_updatedTuple,
                                                            m_updatableIndexList, fallible);
            }
        }
    }

    ReplicatedMaterializedViewHandler::ReplicatedMaterializedViewHandler(PersistentTable* destTable,
                                                                         MaterializedViewHandler* partitionedHandler,
                                                                         int32_t handlerPartitionId) :
        MaterializedViewHandler(destTable, NULL, 0, NULL),
        m_partitionedHandler(partitionedHandler),
        m_handlerPartitionId(handlerPartitionId)
    {}

    void ReplicatedMaterializedViewHandler::handleTupleInsert(PersistentTable *sourceTable, bool fallible) {
        assert(SynchronizedThreadLock::isInSingleThreadMode());
        EngineLocals& curr = SynchronizedThreadLock::s_enginesByPartitionId[m_handlerPartitionId];
        ExecutorContext::assignThreadLocals(curr);
        m_partitionedHandler->handleTupleInsert(sourceTable, fallible);
        SynchronizedThreadLock::assumeLowestSiteContext();
    }

    void ReplicatedMaterializedViewHandler::handleTupleDelete(PersistentTable *sourceTable, bool fallible) {
        assert(SynchronizedThreadLock::isInSingleThreadMode());
        EngineLocals& curr = SynchronizedThreadLock::s_enginesByPartitionId[m_handlerPartitionId];
        ExecutorContext::assignThreadLocals(curr);
        m_partitionedHandler->handleTupleDelete(sourceTable, fallible);
        SynchronizedThreadLock::assumeLowestSiteContext();
    }

} // namespace voltdb
