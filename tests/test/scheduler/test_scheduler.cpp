#include <catch.hpp>

#include <faabric/proto/faabric.pb.h>
#include <faabric/scheduler/FunctionCallClient.h>
#include <faabric/scheduler/SnapshotClient.h>
#include <faabric/snapshot/SnapshotRegistry.h>
#include <faabric/util/func.h>
#include <faabric/util/testing.h>
#include <faabric_utils.h>

#include <faabric/redis/Redis.h>
#include <faabric/scheduler/Scheduler.h>
#include <faabric/util/environment.h>

using namespace scheduler;
using namespace redis;

namespace tests {
TEST_CASE("Test scheduler clear-up", "[scheduler]")
{
    cleanFaabric();
    faabric::util::setMockMode(true);

    faabric::Message msg = faabric::util::messageFactory("blah", "foo");

    std::string thisHost = faabric::util::getSystemConfig().endpointHost;
    std::string otherHost = "other";
    std::unordered_set<std::string> expectedHosts = { otherHost };

    faabric::scheduler::Scheduler& sch = faabric::scheduler::getScheduler();

    sch.addHostToGlobalSet(otherHost);

    // Set resources
    int nCores = 5;
    faabric::HostResources res;
    res.set_cores(nCores);
    sch.setThisHostResources(res);

    // Set resources for other host too
    faabric::scheduler::queueResourceResponse(otherHost, res);

    // Initial checks
    REQUIRE(sch.getFunctionFaasletCount(msg) == 0);
    REQUIRE(sch.getFunctionInFlightCount(msg) == 0);
    REQUIRE(sch.getFunctionRegisteredHostCount(msg) == 0);
    REQUIRE(sch.getFunctionRegisteredHosts(msg).empty());

    faabric::HostResources resCheck = sch.getThisHostResources();
    REQUIRE(resCheck.cores() == nCores);
    REQUIRE(resCheck.boundexecutors() == 0);
    REQUIRE(resCheck.functionsinflight() == 0);

    // Make calls
    int nCalls = nCores + 1;
    for (int i = 0; i < nCalls; i++) {
        sch.callFunction(msg);
    }

    REQUIRE(sch.getFunctionFaasletCount(msg) == nCores);
    REQUIRE(sch.getFunctionInFlightCount(msg) == nCores);
    REQUIRE(sch.getFunctionRegisteredHostCount(msg) == 1);
    REQUIRE(sch.getFunctionRegisteredHosts(msg) == expectedHosts);

    resCheck = sch.getThisHostResources();
    REQUIRE(resCheck.cores() == nCores);
    REQUIRE(resCheck.boundexecutors() == nCores);
    REQUIRE(resCheck.functionsinflight() == nCores);

    // Run shutdown
    sch.shutdown();

    // Check scheduler has been cleared
    REQUIRE(sch.getFunctionFaasletCount(msg) == 0);
    REQUIRE(sch.getFunctionInFlightCount(msg) == 0);
    REQUIRE(sch.getFunctionRegisteredHostCount(msg) == 0);
    REQUIRE(sch.getFunctionRegisteredHosts(msg).empty());

    resCheck = sch.getThisHostResources();
    int actualCores = faabric::util::getUsableCores();
    REQUIRE(resCheck.cores() == actualCores);
    REQUIRE(resCheck.boundexecutors() == 0);
    REQUIRE(resCheck.functionsinflight() == 0);

    faabric::util::setMockMode(false);
}

TEST_CASE("Test scheduler available hosts", "[scheduler]")
{
    cleanFaabric();

    faabric::scheduler::Scheduler& sch = faabric::scheduler::getScheduler();

    // Set up some available hosts
    std::string thisHost = faabric::util::getSystemConfig().endpointHost;
    std::string hostA = "hostA";
    std::string hostB = "hostB";
    std::string hostC = "hostC";

    sch.addHostToGlobalSet(hostA);
    sch.addHostToGlobalSet(hostB);
    sch.addHostToGlobalSet(hostC);

    std::unordered_set<std::string> expectedHosts = {
        thisHost, hostA, hostB, hostC
    };
    std::unordered_set<std::string> actualHosts = sch.getAvailableHosts();

    REQUIRE(actualHosts == expectedHosts);

    sch.removeHostFromGlobalSet(hostB);
    sch.removeHostFromGlobalSet(hostC);

    expectedHosts = { thisHost, hostA };
    actualHosts = sch.getAvailableHosts();

    REQUIRE(actualHosts == expectedHosts);
}

TEST_CASE("Test batch scheduling", "[scheduler]")
{
    cleanFaabric();

    std::string expectedSnapshot;
    faabric::BatchExecuteRequest::BatchExecuteType execMode;
    SECTION("Threads")
    {
        execMode = faabric::BatchExecuteRequest::THREADS;
        expectedSnapshot = "threadSnap";
    }
    SECTION("Processes")
    {
        execMode = faabric::BatchExecuteRequest::PROCESSES;
        expectedSnapshot = "procSnap";
    }
    SECTION("Functions") { execMode = faabric::BatchExecuteRequest::FUNCTIONS; }
    bool isThreads = execMode == faabric::BatchExecuteRequest::THREADS;

    // Set up a dummy snapshot if necessary
    faabric::util::SnapshotData snapshot;
    faabric::snapshot::SnapshotRegistry& snapRegistry =
      faabric::snapshot::getSnapshotRegistry();

    if (!expectedSnapshot.empty()) {
        snapshot.size = 1234;
        snapshot.data = new uint8_t[snapshot.size];

        snapRegistry.takeSnapshot(expectedSnapshot, snapshot);
    }

    // Mock everything
    faabric::util::setMockMode(true);

    std::string thisHost = faabric::util::getSystemConfig().endpointHost;

    faabric::scheduler::Scheduler& sch = faabric::scheduler::getScheduler();

    // Set up another host
    std::string otherHost = "beta";
    sch.addHostToGlobalSet(otherHost);

    int nCallsOne = 10;
    int nCallsTwo = 5;
    int thisCores = 5;
    int otherCores = 11;
    int nCallsOffloadedOne = nCallsOne - thisCores;

    faabric::HostResources thisResources;
    thisResources.set_cores(thisCores);

    faabric::HostResources otherResources;
    otherResources.set_cores(otherCores);

    // Prepare two resource responses for other host
    sch.setThisHostResources(thisResources);
    faabric::scheduler::queueResourceResponse(otherHost, otherResources);

    // Set up the messages
    std::vector<faabric::Message> msgsOne;
    std::vector<std::string> expectedHostsOne;
    for (int i = 0; i < nCallsOne; i++) {
        faabric::Message msg = faabric::util::messageFactory("foo", "bar");

        // Set important bind fields
        msg.set_ispython(true);
        msg.set_pythonfunction("baz");
        msg.set_pythonuser("foobar");
        msg.set_issgx(true);

        // Set snapshot key
        msg.set_snapshotkey(expectedSnapshot);

        msgsOne.push_back(msg);

        // Expect this host to handle up to its number of cores
        // If in threads mode, expect it _not_ to execute
        bool isThisHost = i < thisCores;
        if (isThisHost && isThreads) {
            expectedHostsOne.push_back("");
        } else if (isThisHost) {
            expectedHostsOne.push_back(thisHost);
        } else {
            expectedHostsOne.push_back(otherHost);
        }
    }

    // Create the batch request
    faabric::BatchExecuteRequest reqOne =
      faabric::util::batchExecFactory(msgsOne);
    reqOne.set_type(execMode);

    // Schedule the functions
    std::vector<std::string> actualHostsOne = sch.callFunctions(reqOne);

    // Check resource requests have been made to other host
    auto resRequestsOne = faabric::scheduler::getResourceRequests();
    REQUIRE(resRequestsOne.size() == 1);
    REQUIRE(resRequestsOne.at(0).first == otherHost);

    // Check snapshots have been pushed
    auto snapshotPushes = faabric::scheduler::getSnapshotPushes();
    if (expectedSnapshot.empty()) {
        REQUIRE(snapshotPushes.empty());
    } else {
        REQUIRE(snapshotPushes.size() == 1);
        auto pushedSnapshot = snapshotPushes.at(0);
        REQUIRE(pushedSnapshot.first == otherHost);
        REQUIRE(pushedSnapshot.second.size == snapshot.size);
        REQUIRE(pushedSnapshot.second.data == snapshot.data);
    }

    // Check scheduled on expected hosts
    REQUIRE(actualHostsOne == expectedHostsOne);

    faabric::Message m = msgsOne.at(0);

    // Check the bind messages on this host
    auto bindQueue = sch.getBindQueue();
    if (isThreads) {
        // For threads we expect the caller to do the work
        REQUIRE(bindQueue->size() == 0);
        REQUIRE(sch.getFunctionInFlightCount(m) == thisCores);
        REQUIRE(sch.getFunctionFaasletCount(m) == 0);
    } else {
        // Check the scheduler info on this host
        REQUIRE(sch.getFunctionInFlightCount(m) == thisCores);
        REQUIRE(sch.getFunctionFaasletCount(m) == thisCores);

        // For non-threads we expect faaslets to be created
        REQUIRE(bindQueue->size() == thisCores);
        for (int i = 0; i < thisCores; i++) {
            faabric::Message msg = bindQueue->dequeue();

            REQUIRE(msg.user() == m.user());
            REQUIRE(msg.function() == m.function());
            REQUIRE(msg.type() == faabric::Message_MessageType_BIND);
            REQUIRE(msg.ispython());
            REQUIRE(msg.pythonuser() == "foobar");
            REQUIRE(msg.pythonfunction() == "baz");
            REQUIRE(msg.issgx());
        }
    }

    // Check the message is dispatched to the other host
    auto batchRequestsOne = faabric::scheduler::getBatchRequests();
    REQUIRE(batchRequestsOne.size() == 1);
    auto batchRequestOne = batchRequestsOne.at(0);
    REQUIRE(batchRequestOne.first == otherHost);

    // Check the request to the other host
    REQUIRE(batchRequestOne.second.messages_size() == nCallsOffloadedOne);

    // Clear mocks
    faabric::scheduler::clearMockRequests();

    // Set up resource response again
    faabric::scheduler::queueResourceResponse(otherHost, otherResources);

    // Now schedule a second batch and check they're also sent to the other host
    // (which is now warm)
    std::vector<faabric::Message> msgsTwo;
    std::vector<std::string> expectedHostsTwo;

    for (int i = 0; i < nCallsTwo; i++) {
        faabric::Message msg = faabric::util::messageFactory("foo", "bar");
        msg.set_snapshotkey(expectedSnapshot);

        msgsTwo.push_back(msg);
        expectedHostsTwo.push_back(otherHost);
    }

    // Create the batch request
    faabric::BatchExecuteRequest reqTwo =
      faabric::util::batchExecFactory(msgsTwo);
    reqTwo.set_type(execMode);

    // Schedule the functions
    std::vector<std::string> actualHostsTwo = sch.callFunctions(reqTwo);

    // Check resource request made again
    auto resRequestsTwo = faabric::scheduler::getResourceRequests();
    REQUIRE(resRequestsTwo.size() == 1);
    REQUIRE(resRequestsTwo.at(0).first == otherHost);

    // Check scheduled on expected hosts
    REQUIRE(actualHostsTwo == expectedHostsTwo);

    // Check no other functions have been scheduled on this host
    REQUIRE(sch.getFunctionInFlightCount(m) == thisCores);

    if (isThreads) {
        REQUIRE(sch.getFunctionFaasletCount(m) == 0);
    } else {
        REQUIRE(sch.getFunctionFaasletCount(m) == thisCores);
    }

    // Check the second message is dispatched to the other host
    auto batchRequestsTwo = faabric::scheduler::getBatchRequests();
    REQUIRE(batchRequestsTwo.size() == 1);
    auto pTwo = batchRequestsTwo.at(0);
    REQUIRE(pTwo.first == otherHost);

    // Check the request to the other host
    REQUIRE(pTwo.second.messages_size() == nCallsTwo);

    faabric::util::setMockMode(false);
}

TEST_CASE("Test overloaded scheduler", "[scheduler]")
{
    cleanFaabric();
    faabric::util::setMockMode(true);

    faabric::BatchExecuteRequest::BatchExecuteType execMode;
    std::string expectedSnapshot;
    SECTION("Threads")
    {
        execMode = faabric::BatchExecuteRequest::THREADS;
        expectedSnapshot = "threadSnap";
    }
    SECTION("Processes")
    {
        execMode = faabric::BatchExecuteRequest::PROCESSES;
        expectedSnapshot = "procSnap";
    }
    SECTION("Functions") { execMode = faabric::BatchExecuteRequest::FUNCTIONS; }

    // Set up snapshot if necessary
    faabric::util::SnapshotData snapshot;
    faabric::snapshot::SnapshotRegistry& snapRegistry =
      faabric::snapshot::getSnapshotRegistry();

    if (!expectedSnapshot.empty()) {
        snapshot.size = 1234;
        snapshot.data = new uint8_t[snapshot.size];
        snapRegistry.takeSnapshot(expectedSnapshot, snapshot);
    }

    // Set up this host with very low resources
    Scheduler& sch = scheduler::getScheduler();
    std::string thisHost = sch.getThisHost();
    int nCores = 1;
    faabric::HostResources res;
    res.set_cores(nCores);
    sch.setThisHostResources(res);

    // Set up another host with no resources
    std::string otherHost = "other";
    faabric::HostResources resOther;
    resOther.set_cores(0);
    faabric::scheduler::queueResourceResponse(otherHost, resOther);

    // Submit more calls than we have capacity for
    int nCalls = 10;
    std::vector<faabric::Message> msgs;
    for (int i = 0; i < nCalls; i++) {
        faabric::Message msg = faabric::util::messageFactory("foo", "bar");
        msg.set_snapshotkey(expectedSnapshot);
        msgs.push_back(msg);
    }

    // Submit the request
    faabric::BatchExecuteRequest req = faabric::util::batchExecFactory(msgs);
    req.set_type(execMode);
    std::vector<std::string> executedHosts = sch.callFunctions(req);

    // Set up expectations
    std::vector<std::string> expectedHosts;
    int expectedBindQueueSize;
    if (execMode == faabric::BatchExecuteRequest::THREADS) {
        expectedHosts = std::vector<std::string>(nCalls, "");
        expectedBindQueueSize = 0;
    } else {
        expectedHosts = std::vector<std::string>(nCalls, thisHost);
        expectedBindQueueSize = nCalls;
    }

    // Check they're scheduled locally
    faabric::Message firstMsg = req.messages().at(0);
    REQUIRE(sch.getBindQueue()->size() == expectedBindQueueSize);
    REQUIRE(sch.getFunctionFaasletCount(firstMsg) == expectedBindQueueSize);

    // We expect the in flight count to be incremented regardless
    REQUIRE(sch.getFunctionInFlightCount(firstMsg) == nCalls);

    faabric::util::setMockMode(false);
}

TEST_CASE("Test unregistering host", "[scheduler]")
{
    cleanFaabric();
    faabric::util::setMockMode(true);

    Scheduler& sch = scheduler::getScheduler();

    std::string thisHost = faabric::util::getSystemConfig().endpointHost;
    std::string otherHost = "foobar";
    sch.addHostToGlobalSet(otherHost);

    int nCores = 5;
    faabric::HostResources res;
    res.set_cores(nCores);
    sch.setThisHostResources(res);

    // Set up capacity for other host
    faabric::scheduler::queueResourceResponse(otherHost, res);

    faabric::Message msg = faabric::util::messageFactory("foo", "bar");
    for (int i = 0; i < nCores + 1; i++) {
        sch.callFunction(msg);
    }

    // Check other host is added
    std::unordered_set<std::string> expectedHosts = { otherHost };
    REQUIRE(sch.getFunctionRegisteredHosts(msg) == expectedHosts);
    REQUIRE(sch.getFunctionRegisteredHostCount(msg) == 1);

    // Remove host for another function and check host isn't removed
    faabric::Message otherMsg = faabric::util::messageFactory("foo", "qux");
    sch.removeRegisteredHost(otherHost, otherMsg);
    REQUIRE(sch.getFunctionRegisteredHosts(msg) == expectedHosts);
    REQUIRE(sch.getFunctionRegisteredHostCount(msg) == 1);

    // Remove host
    sch.removeRegisteredHost(otherHost, msg);
    REQUIRE(sch.getFunctionRegisteredHosts(msg).empty());
    REQUIRE(sch.getFunctionRegisteredHostCount(msg) == 0);

    faabric::util::setMockMode(false);
}

TEST_CASE("Test host unregisters", "[scheduler]") {}

TEST_CASE("Test counts can't go below zero", "[scheduler]")
{
    cleanFaabric();

    Scheduler& sch = scheduler::getScheduler();
    faabric::Message msg = faabric::util::messageFactory("demo", "echo");

    sch.notifyFaasletFinished(msg);
    sch.notifyFaasletFinished(msg);
    sch.notifyFaasletFinished(msg);
    REQUIRE(sch.getFunctionFaasletCount(msg) == 0);

    sch.notifyCallFinished(msg);
    sch.notifyCallFinished(msg);
    sch.notifyCallFinished(msg);
    sch.notifyCallFinished(msg);
    REQUIRE(sch.getFunctionInFlightCount(msg) == 0);
}

TEST_CASE("Check test mode", "[scheduler]")
{
    cleanFaabric();

    Scheduler& sch = scheduler::getScheduler();

    faabric::Message msgA = faabric::util::messageFactory("demo", "echo");
    faabric::Message msgB = faabric::util::messageFactory("demo", "echo");
    faabric::Message msgC = faabric::util::messageFactory("demo", "echo");

    bool origTestMode = faabric::util::isTestMode();
    SECTION("No test mode")
    {
        faabric::util::setTestMode(false);

        sch.callFunction(msgA);
        REQUIRE(sch.getRecordedMessagesAll().empty());
    }

    SECTION("Test mode")
    {
        faabric::util::setTestMode(true);

        sch.callFunction(msgA);
        sch.callFunction(msgB);
        sch.callFunction(msgC);

        std::vector<unsigned int> expected = { (unsigned int)msgA.id(),
                                               (unsigned int)msgB.id(),
                                               (unsigned int)msgC.id() };
        std::vector<unsigned int> actual = sch.getRecordedMessagesAll();
        REQUIRE(actual == expected);
    }

    faabric::util::setTestMode(origTestMode);
}

TEST_CASE("Global message queue tests", "[scheduler]")
{
    cleanFaabric();

    redis::Redis& redis = redis::Redis::getQueue();
    scheduler::Scheduler& sch = scheduler::getScheduler();

    // Request function
    std::string funcName = "my func";
    std::string userName = "some user";
    std::string inputData = "blahblah";
    faabric::Message call = faabric::util::messageFactory(userName, funcName);
    call.set_inputdata(inputData);

    sch.setFunctionResult(call);

    // Check result has been written to the right key
    REQUIRE(redis.listLength(call.resultkey()) == 1);

    // Check that some expiry has been set
    long ttl = redis.getTtl(call.resultkey());
    REQUIRE(ttl > 10);

    // Check retrieval method gets the same call out again
    faabric::Message actualCall2 = sch.getFunctionResult(call.id(), 1);

    checkMessageEquality(call, actualCall2);
}

TEST_CASE("Check multithreaded function results", "[scheduler]")
{
    cleanFaabric();

    int nWorkers = 5;
    int nWorkerMessages = 8;

    int nWaiters = 10;
    int nWaiterMessages = 4;

    // Sanity check
    REQUIRE((nWaiters * nWaiterMessages) == (nWorkers * nWorkerMessages));

    std::vector<std::thread> waiterThreads;
    std::vector<std::thread> workerThreads;

    // Create waiters that will submit messages and await their results
    for (int i = 0; i < nWaiters; i++) {
        waiterThreads.emplace_back([nWaiterMessages] {
            Scheduler& sch = scheduler::getScheduler();

            faabric::Message msg =
              faabric::util::messageFactory("demo", "echo");

            // Put invocation on local queue and await global result
            for (int m = 0; m < nWaiterMessages; m++) {
                const std::shared_ptr<InMemoryMessageQueue>& queue =
                  sch.getFunctionQueue(msg);
                queue->enqueue(msg);
                sch.getFunctionResult(msg.id(), 5000);
            }
        });
    }

    // Create workers that will dequeue messages and set success
    for (int i = 0; i < nWorkers; i++) {
        workerThreads.emplace_back([nWorkerMessages] {
            Scheduler& sch = scheduler::getScheduler();

            faabric::Message dummyMsg =
              faabric::util::messageFactory("demo", "echo");
            const std::shared_ptr<InMemoryMessageQueue>& queue =
              sch.getFunctionQueue(dummyMsg);

            // Listen to local queue, set result on global bus
            for (int m = 0; m < nWorkerMessages; m++) {
                faabric::Message msg = queue->dequeue(5000);
                sch.setFunctionResult(msg);
            }
        });
    }

    // Wait for all the threads to finish
    for (auto& w : waiterThreads) {
        if (w.joinable()) {
            w.join();
        }
    }

    for (auto& w : workerThreads) {
        if (w.joinable()) {
            w.join();
        }
    }

    // If we get here then things work properly
}

TEST_CASE("Check getting function status", "[scheduler]")
{
    cleanFaabric();

    scheduler::Scheduler& sch = scheduler::getScheduler();

    std::string expectedOutput;
    int expectedReturnValue = 0;
    faabric::Message_MessageType expectedType;
    std::string expectedHost = faabric::util::getSystemConfig().endpointHost;

    faabric::Message msg;
    SECTION("Running")
    {
        msg = faabric::util::messageFactory("demo", "echo");
        expectedReturnValue = 0;
        expectedType = faabric::Message_MessageType_EMPTY;
        expectedHost = "";
    }

    SECTION("Failure")
    {
        msg = faabric::util::messageFactory("demo", "echo");

        expectedOutput = "I have failed";
        msg.set_outputdata(expectedOutput);
        msg.set_returnvalue(1);
        sch.setFunctionResult(msg);

        expectedReturnValue = 1;
        expectedType = faabric::Message_MessageType_CALL;
    }

    SECTION("Success")
    {
        msg = faabric::util::messageFactory("demo", "echo");

        expectedOutput = "I have succeeded";
        msg.set_outputdata(expectedOutput);
        msg.set_returnvalue(0);
        sch.setFunctionResult(msg);

        expectedReturnValue = 0;
        expectedType = faabric::Message_MessageType_CALL;
    }

    // Check status when nothing has been written
    const faabric::Message result = sch.getFunctionResult(msg.id(), 0);

    REQUIRE(result.returnvalue() == expectedReturnValue);
    REQUIRE(result.type() == expectedType);
    REQUIRE(result.outputdata() == expectedOutput);
    REQUIRE(result.executedhost() == expectedHost);
}

TEST_CASE("Check setting long-lived function status", "[scheduler]")
{
    cleanFaabric();
    scheduler::Scheduler& sch = scheduler::getScheduler();
    redis::Redis& redis = redis::Redis::getQueue();

    // Create a message
    faabric::Message msg = faabric::util::messageFactory("demo", "echo");
    faabric::Message expected = msg;
    expected.set_executedhost(util::getSystemConfig().endpointHost);

    sch.setFunctionResult(msg);

    std::vector<uint8_t> actual = redis.get(msg.statuskey());
    REQUIRE(!actual.empty());

    faabric::Message actualMsg;
    actualMsg.ParseFromArray(actual.data(), (int)actual.size());

    // We can't predict the finish timestamp, so have to manually copy here
    REQUIRE(actualMsg.finishtimestamp() > 0);
    expected.set_finishtimestamp(actualMsg.finishtimestamp());

    checkMessageEquality(actualMsg, expected);
}

TEST_CASE("Check logging chained functions", "[scheduler]")
{
    cleanFaabric();

    scheduler::Scheduler& sch = scheduler::getScheduler();

    faabric::Message msg = faabric::util::messageFactory("demo", "echo");
    unsigned int chainedMsgIdA = 1234;
    unsigned int chainedMsgIdB = 5678;
    unsigned int chainedMsgIdC = 9876;

    // Check empty initially
    REQUIRE(sch.getChainedFunctions(msg.id()).empty());

    // Log and check this shows up in the result
    sch.logChainedFunction(msg.id(), chainedMsgIdA);
    std::unordered_set<unsigned int> expected = { chainedMsgIdA };
    REQUIRE(sch.getChainedFunctions(msg.id()) == expected);

    // Log some more and check
    sch.logChainedFunction(msg.id(), chainedMsgIdA);
    sch.logChainedFunction(msg.id(), chainedMsgIdB);
    sch.logChainedFunction(msg.id(), chainedMsgIdC);
    expected = { chainedMsgIdA, chainedMsgIdB, chainedMsgIdC };
    REQUIRE(sch.getChainedFunctions(msg.id()) == expected);
}

TEST_CASE("Test non-master batch request returned to master", "[scheduler]")
{
    cleanFaabric();
    faabric::util::setMockMode(true);

    scheduler::Scheduler& sch = scheduler::getScheduler();

    std::string otherHost = "other";

    faabric::Message msg = faabric::util::messageFactory("blah", "foo");
    msg.set_masterhost(otherHost);

    std::vector<faabric::Message> msgs = { msg };
    faabric::BatchExecuteRequest req = faabric::util::batchExecFactory(msgs);

    std::vector<std::string> expectedHosts = { "" };
    std::vector<std::string> executedHosts = sch.callFunctions(req);
    REQUIRE(executedHosts == expectedHosts);

    // Check forwarded to master
    auto actualReqs = faabric::scheduler::getBatchRequests();
    REQUIRE(actualReqs.size() == 1);
    REQUIRE(actualReqs.at(0).first == otherHost);
    REQUIRE(actualReqs.at(0).second.id() == req.id());

    faabric::util::setMockMode(false);
}

TEST_CASE("Test broadcast snapshot deletion", "[scheduler]")
{
    cleanFaabric();
    faabric::util::setMockMode(true);
    scheduler::Scheduler& sch = scheduler::getScheduler();

    // Set up other hosts
    std::string otherHostA = "otherA";
    std::string otherHostB = "otherB";
    std::string otherHostC = "otherC";

    sch.addHostToGlobalSet(otherHostA);
    sch.addHostToGlobalSet(otherHostB);
    sch.addHostToGlobalSet(otherHostC);

    int nCores = 3;
    faabric::HostResources res;
    res.set_cores(nCores);
    sch.setThisHostResources(res);

    // Set up capacity for other hosts
    faabric::scheduler::queueResourceResponse(otherHostA, res);
    faabric::scheduler::queueResourceResponse(otherHostB, res);
    faabric::scheduler::queueResourceResponse(otherHostC, res);

    // Set up a number of requests that will use this host and two others, but
    // not the third
    faabric::Message msg = faabric::util::messageFactory("foo", "bar");
    int nRequests = 2 * nCores + 1;
    std::vector<faabric::Message> msgs;
    for (int i = 0; i < nRequests; i++) {
        msgs.push_back(msg);
    }
    faabric::BatchExecuteRequest req = faabric::util::batchExecFactory(msgs);
    sch.callFunctions(req);

    // Check other hosts are added
    REQUIRE(sch.getFunctionRegisteredHostCount(msg) == 2);

    std::unordered_set<std::string> expectedHosts =
      sch.getFunctionRegisteredHosts(msg);

    // Broadcast deletion of some snapshot
    std::string snapKey = "blahblah";
    sch.broadcastSnapshotDelete(msg, snapKey);

    std::vector<std::pair<std::string, std::string>> expectedDeleteRequests;
    for (auto h : expectedHosts) {
        expectedDeleteRequests.push_back({ h, snapKey });
    };
    auto actualDeleteRequests = faabric::scheduler::getSnapshotDeletes();

    REQUIRE(actualDeleteRequests == expectedDeleteRequests);
}
}
