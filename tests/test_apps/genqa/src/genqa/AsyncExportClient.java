/* This file is part of VoltDB.
 * Copyright (C) 2008-2020 VoltDB Inc.
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
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */
/*
 * This samples uses the native asynchronous request processing protocol
 * to post requests to the VoltDB server, thus leveraging to the maximum
 * VoltDB's ability to run requests in parallel on multiple database
 * partitions, and multiple servers.
 *
 * While asynchronous processing is (marginally) more convoluted to work
 * with and not adapted to all workloads, it is the preferred interaction
 * model to VoltDB as it guarantees blazing performance.
 *
 * Because there is a risk of 'firehosing' a database cluster (if the
 * cluster is too slow (slow or too few CPUs), this sample performs
 * self-tuning to target a specific latency (10ms by default).
 * This tuning process, as demonstrated here, is important and should be
 * part of your pre-launch evalution so you can adequately provision your
 * VoltDB cluster with the number of servers required for your needs.
 */

package genqa;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStreamWriter;
import java.time.Instant;
import java.util.Random;
import java.util.Timer;
import java.util.TimerTask;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.ThreadFactory;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicLongArray;
import java.util.concurrent.atomic.AtomicReference;

import org.voltcore.logging.VoltLogger;
import org.voltdb.VoltDB;
import org.voltdb.VoltTable;
import org.voltdb.client.Client;
import org.voltdb.client.ClientConfig;
import org.voltdb.client.ClientFactory;
import org.voltdb.client.ClientResponse;
import org.voltdb.client.ClientResponseWithPartitionKey;
import org.voltdb.ClientResponseImpl;
import org.voltdb.client.ClientStats;
import org.voltdb.client.ClientStatsContext;
import org.voltdb.client.ClientStatusListenerExt;
import org.voltdb.client.NoConnectionsException;
import org.voltdb.client.NullCallback;
import org.voltdb.client.ProcedureCallback;
import org.voltdb.client.ProcCallException;
import org.voltdb.client.exampleutils.AppHelper;
import org.voltdb.iv2.MpInitiator;
import org.voltdb.iv2.TxnEgo;

public class AsyncExportClient
{
    static VoltLogger log = new VoltLogger("ExportClient");
    // Transactions between catalog swaps.
    public static long CATALOG_SWAP_INTERVAL = 500000;

    static class AsyncCallback implements ProcedureCallback
    {
        private final long m_rowid;
        public AsyncCallback(long rowid)
        {
            super();
            m_rowid = rowid;
        }
        @Override
        public void clientCallback(ClientResponse clientResponse) {
            // Track the result of the request (Success, Failure)
            long now = System.currentTimeMillis();
            if (clientResponse.getStatus() == ClientResponse.SUCCESS)
            {
                TrackingResults.incrementAndGet(0);
                TransactionCounts.incrementAndGet(INSERT);
            }
            else
            {
                TrackingResults.incrementAndGet(1);
            }
        }
    }

    static class TableExportCallback implements ProcedureCallback
    {

        private final long m_row_id;
        private final long m_op;
        public TableExportCallback(long row_id, long op)
        {
            super();
            m_op = op;
            m_row_id = row_id;
        }

        @Override
        public void clientCallback(ClientResponse clientResponse) {
            // Track the result of the request (Success, Failure)
            if (clientResponse.getStatus() == ClientResponse.SUCCESS)
            {
                int transType = clientResponse.getAppStatus(); // get INSERT, DELETE, or UPDATE
                TrackingResults.incrementAndGet(0);
                TransactionCounts.incrementAndGet(transType);
            }
            else
            {
                long now = System.currentTimeMillis();
                TrackingResults.incrementAndGet(1);
                final String trace = String.format("%d:%s\n", now,((ClientResponseImpl)clientResponse).toJSONString());
                log.info("TableExport failed: " + trace);
                // log.info("Failed row_id: " + m_row_id + ", Operation: " + m_op); // it's static so whose rowid anyway?
            }
        }
    }

    // Connection configuration
    private final static class ConnectionConfig {

        final long displayInterval;
        final long duration;
        final String servers;
        final int port;
        final int poolSize;
        final int rateLimit;
        final boolean autoTune;
        final int latencyTarget;
        final String [] parsedServers;
        final String procedure;
        final int exportTimeout;
        final boolean migrateWithTTL;
        final boolean usetableexport;
        final boolean migrateWithoutTTL;
        final long migrateNoTTLInterval;

        ConnectionConfig( AppHelper apph) {
            displayInterval      = apph.longValue("displayinterval");
            duration             = apph.longValue("duration");
            servers              = apph.stringValue("servers");
            port                 = apph.intValue("port");
            poolSize             = apph.intValue("poolsize");
            rateLimit            = apph.intValue("ratelimit");
            autoTune             = apph.booleanValue("autotune");
            latencyTarget        = apph.intValue("latencytarget");
            procedure            = apph.stringValue("procedure");
            parsedServers        = servers.split(",");
            exportTimeout        = apph.intValue("timeout");
            migrateWithTTL       = apph.booleanValue("migrate-ttl");
            usetableexport       = apph.booleanValue("usetableexport");
            migrateWithoutTTL    = apph.booleanValue("migrate-nottl");
            migrateNoTTLInterval = apph.longValue("nottl-interval");

        }
    }

    // Initialize some common constants and variables
    private static final AtomicLongArray TrackingResults = new AtomicLongArray(2);

    // If testing Table/Export, count inserts, deletes, update fore & aft

    private static int NONE = 0;
    private static int INSERT = 1;
    private static int UPDATE_OLD = 2;
    private static int DELETE = 3;
    private static int UPDATE_NEW = 4;
    // TBD: add MIGRATE (5), though not relevant in this test (yet)
    private static final AtomicLongArray TransactionCounts = new AtomicLongArray(4);
    private static final AtomicLongArray QueuedTransactionCounts = new AtomicLongArray(4);
    private static final AtomicLong NoConnectionsExceptionsCount = new AtomicLong(0);
    private static final AtomicLong OtherProcCallExceptionCount = new AtomicLong(0);

    private static File deployment = new File("deployment.xml");

    // Connection reference
    private static final AtomicReference<Client> clientRef = new AtomicReference<Client>();

    // Shutdown flag
    private static final AtomicBoolean shutdown = new AtomicBoolean(false);

    // Connection Configuration
    private static ConnectionConfig config;

    // Test startup time
    private static long benchmarkStartTS;

    // Statistics manager objects from the client
    private static ClientStatsContext periodicStatsContext;
    private static ClientStatsContext fullStatsContext;

    private static String[] TABLES = { "EXPORT_PARTITIONED_TABLE_JDBC",
                                       "EXPORT_REPLICATED_TABLE_JDBC",
                                       "EXPORT_PARTITIONED_TABLE_KAFKA",
                                       "EXPORT_REPLICATED_TABLE_KAFKA"};

    static {
        VoltDB.setDefaultTimezone();
    }

    // Application entry point
    public static void main(String[] args)
    {
        VoltLogger log = new VoltLogger("ExportClient.main");
        try
        {

// ---------------------------------------------------------------------------------------------------------------------------------------------------

            // Use the AppHelper utility class to retrieve command line application parameters

            // Define parameters and pull from command line
            AppHelper apph = new AppHelper(AsyncBenchmark.class.getCanonicalName())
                .add("displayinterval", "display_interval_in_seconds", "Interval for performance feedback, in seconds.", 10)
                .add("duration", "run_duration_in_seconds", "Benchmark duration, in seconds.", 120)
                .add("servers", "comma_separated_server_list", "List of VoltDB servers to connect to.", "localhost")
                .add("port", "port_number", "Client port to connect to on cluster nodes.", 21212)
                .add("poolsize", "pool_size", "Size of the record pool to operate on - larger sizes will cause a higher insert/update-delete rate.", 100000)
                .add("procedure", "procedure_name", "Procedure to call.", "JiggleExportSinglePartition")
                .add("ratelimit", "rate_limit", "Rate limit to start from (number of transactions per second).", 100000)
                .add("autotune", "auto_tune", "Flag indicating whether the benchmark should self-tune the transaction rate for a target execution latency (true|false).", "true")
                .add("latencytarget", "latency_target", "Execution latency to target to tune transaction rate (in milliseconds).", 10)
                .add("timeout","export_timeout","max seconds to wait for export to complete",300)
                .add("migrate-ttl","false","use DDL that includes TTL MIGRATE action","false")
                .add("usetableexport", "usetableexport","use DDL that includes CREATE TABLE with EXPORT ON ... action","false")
                .add("migrate-nottl", "false","use DDL that includes MIGRATE without TTL","false")
                .add("nottl-interval", "milliseconds", "approximate migrate command invocation interval (in milliseconds)", 2500)
                .setArguments(args)
            ;

            config = new ConnectionConfig(apph);

            // Retrieve parameters
            final String csv           = apph.stringValue("statsfile");

            // Validate parameters
            apph.validate("duration", (config.duration > 0))
                .validate("poolsize", (config.poolSize > 0))
                .validate("ratelimit", (config.rateLimit > 0))
                .validate("latencytarget", (config.latencyTarget > 0))
            ;

            // Display actual parameters, for reference
            apph.printActualUsage();

// ---------------------------------------------------------------------------------------------------------------------------------------------------

            // Get a client connection - we retry for a while in case the server hasn't started yet
            createClient();
            connect();


// ---------------------------------------------------------------------------------------------------------------------------------------------------

            // Create a Timer task to display performance data on the procedure
            Timer timer = new Timer(true);
            timer.scheduleAtFixedRate(new TimerTask()
            {
                @Override
                public void run()
                {
                    printStatistics(periodicStatsContext,true);
                }
            }
            , config.displayInterval*1000l
            , config.displayInterval*1000l
            );

            // If migrate without TTL is enabled, set things up so a migrate is triggered
            // roughly every 2.5 seconds, with the first one happening 3 seconds from now
            // Use a separate Timer object to get a dedicated manual migration thread
            Timer migrateTimer = new Timer(true);
            Random migrateInterval = new Random();
            if (config.migrateWithoutTTL) {
                migrateTimer.scheduleAtFixedRate(new TimerTask()
                {
                    @Override
                    public void run()
                    {
                        trigger_migrate(migrateInterval.nextInt(10)); // vary the migrate/delete interval a little
                    }
                }
                , 3000l
                , config.migrateNoTTLInterval
                );
            }

// ---------------------------------------------------------------------------------------------------------------------------------------------------

            benchmarkStartTS = System.currentTimeMillis();
            AtomicLong rowId = new AtomicLong(0);
            // Run the benchmark loop for the requested duration
            final long endTime = benchmarkStartTS + (1000l * config.duration);
            Random r = new Random();
            while (endTime > System.currentTimeMillis())
            {
                long currentRowId = rowId.incrementAndGet();

                // Table with Export
                if (config.usetableexport) {
                    for (long op = 1; op < 4; op++) {
                        long retries = 4;
                        while (retries-- > 0) {
                            try {
                                clientRef.get().callProcedure(
                                        new TableExportCallback(rowId.get(), op),
                                        "TableExport",
                                        currentRowId,
                                        op);
                                QueuedTransactionCounts.incrementAndGet((int) op);
                                break;
                            }
                            catch (NoConnectionsException e) {
                                log.info("Exception: " + e);
                                e.printStackTrace();
                                NoConnectionsExceptionsCount.incrementAndGet();
                            }
                            catch (Exception e) {
                                log.info("Exception: " + e);
                                e.printStackTrace();
                                OtherProcCallExceptionCount.incrementAndGet();
                            }
                            Thread.sleep(3000);
                        }
                    }
                }
               else {
                // Post the request, asynchronously
                    try {
                        clientRef.get().callProcedure(
                                                      new AsyncCallback(currentRowId),
                                                      config.procedure,
                                                      currentRowId,
                                                      0);
                    }
                    catch (Exception e) {
                        log.info("Exception: " + e);
                        e.printStackTrace();
                        // System.exit(-1);
                    }
                }
            }

// ---------------------------------------------------------------------------------------------------------------------------------------------------

            // We're done - stop the performance statistics display task
            timer.cancel();
            // likewise for the migrate task
            migrateTimer.cancel();
            clientRef.get().drain();

            if (config.migrateWithoutTTL) {
                for (String t : TABLES) {
                    log_migrating_counts(t);
                }
                // trigger last "migrate from" cycle and wait a little bit for table to empty, assuming all is working.
                // otherwise, we'll check the table row count at a higher level and fail the test if the table is not empty.
                log.info("triggering final migrate");
                trigger_migrate(0);
                Thread.sleep(7500);
                for (String t : TABLES) {
                    log_migrating_counts(t);
                }
            }

            shutdown.compareAndSet(false, true);


// ---------------------------------------------------------------------------------------------------------------------------------------------------
            clientRef.get().drain();

            Thread.sleep(10000);

            //Write to export table to get count to be expected on other side.
            log.info("Writing export count as: " + TrackingResults.get(0) + " final rowid:" + rowId);
            clientRef.get().callProcedure("InsertExportDoneDetails", TrackingResults.get(0));

            // Now print application results:

            // 1. Tracking statistics
            log.info(
                String.format(
              "-------------------------------------------------------------------------------------\n"
            + " Benchmark Results\n"
            + "-------------------------------------------------------------------------------------\n\n"
            + "A total of %d calls was received...\n"
            + " - %,9d Succeeded\n"
            + " - %,9d Failed (Transaction Error)\n"
            + "\n\n"
            + "-------------------------------------------------------------------------------------\n"
            , TrackingResults.get(0)+TrackingResults.get(1)
            , TrackingResults.get(0)
            , TrackingResults.get(1))
            );
            if ( TrackingResults.get(0) + TrackingResults.get(1) != rowId.longValue() ) {
                log.info("WARNING Tracking results total doesn't match final rowId sequence number " + (TrackingResults.get(0) + TrackingResults.get(1)) + "!=" + rowId );
            }

            // 2. Print TABLE EXPORT stats if that's configured
            if (config.usetableexport) {
                log.info(
                    String.format(
                        "-------------------------------------------------------------------------------------\n"
                      + " Table/Export Committed Counts\n"
                      + "-------------------------------------------------------------------------------------\n\n"
                      + "A total of %d calls were committed...\n"
                      + " - %,9d Committed-Inserts\n"
                      + " - %,9d Committed-Deletes\n"
                      + " - %,9d Committed-Updates/Before\n"
                      + " - %,9d None return from SP"
                      + "\n\n"
                      + "-------------------------------------------------------------------------------------\n"
                      , TrackingResults.get(0)+TrackingResults.get(1)
                      , TransactionCounts.get(INSERT)
                      , TransactionCounts.get(DELETE)
                      , TransactionCounts.get(UPDATE_OLD)
                      , TransactionCounts.get(NONE))
                      // old & new on each update so either = total updates, not the sum of the 2
                      // +TransactionCounts.get(UPDATE_NEW)
                      );
                log.info(
                        String.format(
                            "-------------------------------------------------------------------------------------\n"
                          + " Table/Export Queued Operation Results\n"
                          + "-------------------------------------------------------------------------------------\n\n"
                          + "A total of %d calls were queued...\n"
                          + " - %,9d Queued-Inserts\n"
                          + " - %,9d Queued-Deletes\n"
                          + " - %,9d Queued-Updates/Before"
                          + "\n\n"
                          + "-------------------------------------------------------------------------------------\n"
                          , QueuedTransactionCounts.get(INSERT) + QueuedTransactionCounts.get(DELETE) + QueuedTransactionCounts.get(UPDATE_OLD)
                          , QueuedTransactionCounts.get(INSERT)
                          , QueuedTransactionCounts.get(DELETE)
                          , QueuedTransactionCounts.get(UPDATE_OLD))
                          // old & new on each update so either = total updates, not the sum of the 2
                          // +TransactionCounts.get(UPDATE_NEW)
                          );

                long export_table_count = get_table_count("EXPORT_PARTITIONED_TABLE_CDC");
                log.info("\nEXPORT_PARTITIONED_TABLE_CDC count: " + export_table_count);

                long export_table_expected = TransactionCounts.get(INSERT) - TransactionCounts.get(DELETE);
                if (export_table_count != export_table_expected) {
                    log.info("Insert and delete count " + export_table_expected +
                        " does not match export table count: " + export_table_count + "\n");
                    // System.err.println("Insert and delete count " + export_table_expected +
                    //     " does not match export table count: " + export_table_count + "\n");
                }

            }
            // 3. Performance statistics (we only care about the procedure that we're benchmarking)
            log.info(
              "\n\n-------------------------------------------------------------------------------------\n"
            + " System Statistics\n"
            + "-------------------------------------------------------------------------------------\n\n");
            printStatistics(fullStatsContext,false);

            // Dump statistics to a CSV file
            clientRef.get().writeSummaryCSV(
                    fullStatsContext.getStatsByProc().get(config.procedure),
                    csv
                    );

            clientRef.get().close();

// ---------------------------------------------------------------------------------------------------------------------------------------------------

        }
        catch(Exception x)
        {
            log.fatal("Exception: " + x);
            x.printStackTrace();
        }
        // if we didn't get any successes we need to fail
        if ( TrackingResults.get(0) == 0 ) {
            log.error("No successful transactions");
            System.exit(-1);
        }
    }

    private static void log_migrating_counts(String table) {
        try {
            VoltTable[] results = clientRef.get().callProcedure("@AdHoc",
                                                                "SELECT COUNT(*) FROM " + table + " WHERE MIGRATING; " +
                                                                "SELECT COUNT(*) FROM " + table + " WHERE NOT MIGRATING; " +
                                                                "SELECT COUNT(*) FROM " + table
                                                                ).getResults();
            long migrating = results[0].asScalarLong();
            long not_migrating = results[1].asScalarLong();
            long total = results[2].asScalarLong();

            log.info("row counts for " + table +
                     ": total: " + total +
                     ", migrating: " + migrating +
                     ", not migrating: " + not_migrating);
        }
        catch (Exception e) {
            // log it and otherwise ignore it.  it's not fatal to fail if the
            // SELECTS due to a migrate or some other exception
            log.fatal("log_migrating_counts exception: " + e);
            e.printStackTrace();
        }
    }

    private static void trigger_migrate(int time_window) {
        try {
            VoltTable[] results;
            if (config.procedure.equals("JiggleExportGroupSinglePartition")) {
                ClientResponseWithPartitionKey[] responses  = clientRef.get().callAllPartitionProcedure("MigratePartitionedExport",
                        time_window);
                for (ClientResponseWithPartitionKey resp : responses) {
                    if (ClientResponse.SUCCESS == resp.response.getStatus()){
                        VoltTable res = resp.response.getResults()[0];
                        log.info("Partitioned Migrate - window: " + time_window + " seconds" +
                                ", kafka: " + res.asScalarLong() +
                                ", rabbit: " + res.asScalarLong() +
                                ", file: " + res.asScalarLong() +
                                ", jdbc: " + res.asScalarLong() +
                                ", on partition " + resp.partitionKey
                                );
                    } else {
                        log.info("WARNING: fail to migrate on partition:" + resp.partitionKey);
                    }
                }
            } else {
                results = clientRef.get().callProcedure("MigrateReplicatedExport", time_window).getResults();
                log.info("Replicated Migrate - window: " + time_window + " seconds" +
                         ", kafka: " + results[0].asScalarLong() +
                         ", rabbit: " + results[1].asScalarLong() +
                         ", file: " + results[2].asScalarLong() +
                         ", jdbc: " + results[3].asScalarLong()
                         );
            }
        }
        catch (ProcCallException e1) {
            if (e1.getMessage().contains("was lost before a response was received")) {
                log.warn("Possible problem executing " + config.procedure + ", procedure may not have completed");
            } else {
                log.fatal("Exception: " + e1);
                e1.printStackTrace();
                System.exit(-1);
            }
        }
        catch (Exception e) {
            log.fatal("Exception: " + e);
            e.printStackTrace();
            System.exit(-1);
        }
    }

    private static long get_table_count(String sqlTable) {
        long count = 0;
        try {
            count = clientRef.get().callProcedure("@AdHoc", "SELECT COUNT(*) FROM " + sqlTable + ";").getResults()[0].asScalarLong();
        }
        catch (Exception e) {
            log.error("Exception in get_table_count: " + e);
            log.error("SELECT COUNT from table " + sqlTable + " failed");
        }
        return count;
    }

    /**
     * @param servers A comma separated list of servers using the hostname:port
     * syntax (where :port is optional).
     * @throws InterruptedException if anything bad happens with the threads.
     */
    static void connect() throws InterruptedException {
        log.info("Connecting to VoltDB...");

        String[] serverArray = config.parsedServers;
        Client client = clientRef.get();
        for (final String server : serverArray) {
        // connect to the first server in list; with TopologyChangeAware set, no need for more
            try {
                client.createConnection(server, config.port);
                break;
            }catch (Exception e) {
                log.error("Connection to " + server + " failed.\n");
            }
        }
    }

    static Client createClient() {
        ClientConfig clientConfig = new ClientConfig("", "");
        // clientConfig.setReconnectOnConnectionLoss(true); **obsolete**
        clientConfig.setClientAffinity(true);
        clientConfig.setTopologyChangeAware(true);

        if (config.autoTune) {
            clientConfig.enableAutoTune();
            clientConfig.setAutoTuneTargetInternalLatency(config.latencyTarget);
        }
        else {
            clientConfig.setMaxTransactionsPerSecond(config.rateLimit);
        }
        Client client = ClientFactory.createClient(clientConfig);
        clientRef.set(client);

        periodicStatsContext = client.createStatsContext();
        fullStatsContext = client.createStatsContext();

        return client;
    }

    /**
     * Prints a one line update on performance that can be printed
     * periodically during a benchmark.
     **
     * @return
     */
    static private synchronized void printStatistics(ClientStatsContext context, boolean resetBaseline) {
        if (resetBaseline) {
            context = context.fetchAndResetBaseline();
        } else {
            context = context.fetch();
        }

        ClientStats stats = context
                .getStatsByProc()
                .get(config.procedure);

        if (stats == null) return;
        // switch from app's runtime to VoltLogger clock time so results line up
        // with apprunner if running in that framework

        String stats_out = String.format(" Throughput %d/s, ", stats.getTxnThroughput());
        stats_out += String.format("Aborts/Failures %d/%d, ",
                stats.getInvocationAborts(), stats.getInvocationErrors());
        stats_out += String.format("Avg/95%% Latency %.2f/%.2fms\n", stats.getAverageLatency(),
                stats.kPercentileLatencyAsDouble(0.95));
        log.info(stats_out);
    }
}
