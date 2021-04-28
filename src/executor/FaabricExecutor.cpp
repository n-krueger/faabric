#include <faabric/executor/FaabricExecutor.h>
#include <faabric/proto/faabric.pb.h>
#include <faabric/state/State.h>
#include <faabric/util/config.h>
#include <faabric/util/environment.h>
#include <faabric/util/func.h>
#include <faabric/util/queue.h>

namespace faabric::executor {
FaabricExecutor::FaabricExecutor(int threadIdxIn)
  : threadIdx(threadIdxIn)
  , scheduler(faabric::scheduler::getScheduler())
{
    const std::shared_ptr<spdlog::logger>& logger = faabric::util::getLogger();

    faabric::util::SystemConfig& conf = faabric::util::getSystemConfig();

    // Note that the main thread will be executing so we want one less
    threadPoolSize = faabric::util::getUsableCores() - 1;

    // Set an ID for this Faaslet
    id = conf.endpointHost + "_" + std::to_string(threadIdx);

    logger->debug("Starting executor thread {}", id);

    // Listen to bind queue by default
    bindQueue = scheduler.getBindQueue();
}

void FaabricExecutor::bindToFunction(const faabric::Message& msg, bool force)
{
    // If already bound, will be an error, unless forced to rebind to the same
    // message
    if (_isBound) {
        if (force) {
            if (msg.user() != boundMessage.user() ||
                msg.function() != boundMessage.function()) {
                throw std::runtime_error(
                  "Cannot force bind to a different function");
            }
        } else {
            throw std::runtime_error(
              "Cannot bind worker thread more than once");
        }
    }

    boundMessage = msg;
    _isBound = true;

    // Get queue from the scheduler
    functionQueue = scheduler.getFunctionQueue(msg);

    // Hook
    this->postBind(msg, force);
}

bool FaabricExecutor::isBound()
{
    return _isBound;
}

void FaabricExecutor::finish()
{
    if (_isBound) {
        // Notify scheduler if this thread was bound to a function
        scheduler.notifyFaasletFinished(boundMessage);
    }

    // Shut down thread pool with a series of kill messages
    for (auto& queuePair : threadQueues) {
        std::shared_ptr<BatchExecuteRequest> killReq =
          faabric::util::batchExecFactory();

        killReq->add_messages()->set_type(faabric::Message::KILL);
        queuePair.second.enqueue(std::make_pair(0, killReq));
    }

    // Wait
    for (auto& t : threads) {
        if (t.second.joinable()) {
            t.second.join();
        }
    }

    // Hook
    this->postFinish();
}

void FaabricExecutor::finishCall(faabric::Message& msg,
                                 bool success,
                                 const std::string& errorMsg)
{
    // Hook
    this->preFinishCall(msg, success, errorMsg);

    const std::shared_ptr<spdlog::logger>& logger = faabric::util::getLogger();

    const std::string funcStr = faabric::util::funcToString(msg, true);
    logger->info("Finished {}", funcStr);
    if (!success) {
        msg.set_outputdata(errorMsg);
    }

    fflush(stdout);

    // Notify the scheduler *before* setting the result. Calls awaiting
    // the result will carry on blocking
    scheduler.notifyCallFinished(msg);

    // Set result
    logger->debug("Setting function result for {}", funcStr);
    scheduler.setFunctionResult(msg);

    // Increment the execution counter
    executionCount++;

    // Hook
    this->postFinishCall();
}

void FaabricExecutor::run()
{
    const std::shared_ptr<spdlog::logger>& logger = faabric::util::getLogger();

    // Wait for next message
    while (true) {
        try {
            logger->debug("{} waiting for next message", this->id);
            std::string errorMessage = this->processNextMessage();

            // Drop out if there's some issue
            if (!errorMessage.empty()) {
                break;
            }
        } catch (faabric::util::ExecutorFinishedException& e) {
            // Executor has notified us it's finished
            logger->debug("{} finished", this->id);
            break;
        } catch (faabric::util::QueueTimeoutException& e) {
            // At this point we've received no message, so die off
            logger->debug("{} got no messages. Finishing", this->id);
            break;
        }
    }

    this->finish();
}

std::string FaabricExecutor::processNextMessage()
{
    const std::shared_ptr<spdlog::logger>& logger = faabric::util::getLogger();
    faabric::util::SystemConfig conf = faabric::util::getSystemConfig();

    std::string errorMessage;
    if (_isBound) {
        // Get the next task
        faabric::scheduler::MessageTask execTask =
          functionQueue->dequeue(conf.boundTimeout);

        std::vector<int> messageIdxs = execTask.first;
        int nMessages = messageIdxs.size();

        std::string funcStr = faabric::util::funcToString(execTask.second);

        // Check if it's a batch of thread calls or not
        if (execTask.second->type() == faabric::BatchExecuteRequest::THREADS) {
            logger->debug("{} batch {} threads of {}", id, nMessages, funcStr);

            batchExecuteThreads(execTask);
        } else if (nMessages == 1) {
            int msgIdx = messageIdxs.at(0);
            logger->debug("{} executing single {} message", id, funcStr);

            // Work out which message we're executing
            faabric::Message msg = execTask.second->messages().at(msgIdx);

            if (msg.type() == faabric::Message_MessageType_FLUSH) {
                flush();
            } else {
                // Do the actual execution
                errorMessage = executeCall(msg);
            }
        } else {
            logger->error("Executing {} x {} messages but not in thread mode "
                          "is unuspported",
                          nMessages,
                          funcStr);

            throw std::runtime_error(
              "Executing multiple messages not in thread mode");
        }
    } else {
        faabric::Message bindMsg = bindQueue->dequeue(conf.unboundTimeout);
        const std::string funcStr = faabric::util::funcToString(bindMsg, false);
        logger->debug("{} binding to {}", id, funcStr);

        try {
            this->bindToFunction(bindMsg);
        } catch (faabric::util::InvalidFunctionException& e) {
            errorMessage = "Invalid function: " + funcStr;
        }
    }
    return errorMessage;
}

void FaabricExecutor::batchExecuteThreads(faabric::scheduler::MessageTask& task)
{
    const auto& logger = faabric::util::getLogger();
    std::vector<int> messageIdxs = task.first;
    int nMessages = messageIdxs.size();
    std::shared_ptr<faabric::BatchExecuteRequest> req = task.second;

    const std::string funcStr = faabric::util::funcToString(req);
    logger->info("Batch executing {}/{} threads of {}",
                 nMessages,
                 req->messages_size(),
                 funcStr);

    // Iterate through and invoke threads
    for (int msgIdx : messageIdxs) {
        const faabric::Message& msg = req->messages().at(msgIdx);
        int threadPoolIdx = msg.appindex() % threadPoolSize;

        std::pair<int, std::shared_ptr<faabric::BatchExecuteRequest>> task =
          std::make_pair(msgIdx, req);
        threadQueues[threadPoolIdx].enqueue(std::move(task));

        if (threads.count(threadPoolIdx) == 0) {
            // Get mutex and re-check
            faabric::util::UniqueLock lock(threadsMutex);

            if (threads.count(threadPoolIdx) == 0) {
                threads.emplace(
                  std::make_pair(threadPoolIdx, [this, threadPoolIdx] {
                      auto logger = faabric::util::getLogger();
                      logger->debug("Thread pool thread {} starting up",
                                    threadPoolIdx);

                      for (;;) {
                          auto task = threadQueues[threadPoolIdx].dequeue();

                          int msgIdx = task.first;
                          std::shared_ptr<faabric::BatchExecuteRequest> req =
                            task.second;

                          faabric::Message& msg =
                            req->mutable_messages()->at(msgIdx);

                          if (msg.type() == faabric::Message::KILL) {
                              break;
                          }

                          int32_t returnValue =
                            executeThread(threadPoolIdx, req, msg);

                          // Set the result for this thread
                          auto& sch = faabric::scheduler::getScheduler();
                          sch.setThreadResult(msg, returnValue);

                          // Notify scheduler finished
                          sch.notifyCallFinished(msg);
                      }

                      logger->debug("Thread pool thread {} shutting down",
                                    threadPoolIdx);
                  }));
            }
        }
    }
}

std::string FaabricExecutor::executeCall(faabric::Message& call)
{
    const std::shared_ptr<spdlog::logger>& logger = faabric::util::getLogger();
    const std::string funcStr = faabric::util::funcToString(call, true);

    // Create and execute the module
    bool success;
    std::string errorMessage;
    try {
        success = this->doExecute(call);
    } catch (const std::exception& e) {
        errorMessage = "Error: " + std::string(e.what());
        logger->error(errorMessage);
        success = false;
        call.set_returnvalue(1);
    }

    if (!success && errorMessage.empty()) {
        errorMessage =
          "Call failed (return value=" + std::to_string(call.returnvalue()) +
          ")";
    }

    this->finishCall(call, success, errorMessage);
    return errorMessage;
}

// ------------------------------------------
// HOOKS
// ------------------------------------------

bool FaabricExecutor::doExecute(faabric::Message& msg)
{
    return true;
}

int32_t FaabricExecutor::executeThread(
  int threadPoolIdx,
  std::shared_ptr<faabric::BatchExecuteRequest> req,
  faabric::Message& msg)
{
    return 0;
}

void FaabricExecutor::postBind(const faabric::Message& msg, bool force) {}

void FaabricExecutor::preFinishCall(faabric::Message& call,
                                    bool success,
                                    const std::string& errorMsg)
{}

void FaabricExecutor::postFinishCall() {}

void FaabricExecutor::postFinish() {}

void FaabricExecutor::flush() {}

}
