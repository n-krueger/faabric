#include <faabric/mpi/mpi.h>

#include <faabric/scheduler/FunctionCallClient.h>
#include <faabric/scheduler/MpiThreadPool.h>
#include <faabric/scheduler/MpiWorld.h>
#include <faabric/scheduler/Scheduler.h>
#include <faabric/state/State.h>
#include <faabric/util/environment.h>
#include <faabric/util/gids.h>
#include <faabric/util/logging.h>
#include <faabric/util/macros.h>
#include <faabric/util/timing.h>

static thread_local std::unordered_map<int, std::future<void>> futureMap;

namespace faabric::scheduler {
MpiWorld::MpiWorld()
  : id(-1)
  , size(-1)
  , thisHost(faabric::util::getSystemConfig().endpointHost)
  , creationTime(faabric::util::startTimer())
  , cartProcsPerDim(2)
{}

std::string getWorldStateKey(int worldId)
{
    if (worldId <= 0) {
        throw std::runtime_error(
          fmt::format("World ID must be bigger than zero ({})", worldId));
    }
    return "mpi_world_" + std::to_string(worldId);
}

std::string getRankStateKey(int worldId, int rankId)
{
    if (worldId <= 0 || rankId < 0) {
        throw std::runtime_error(
          fmt::format("World ID must be >0 and rank ID must be >=0 ({}, {})",
                      worldId,
                      rankId));
    }
    return "mpi_rank_" + std::to_string(worldId) + "_" + std::to_string(rankId);
}

std::string getWindowStateKey(int worldId, int rank, size_t size)
{
    return "mpi_win_" + std::to_string(worldId) + "_" + std::to_string(rank) +
           "_" + std::to_string(size);
}

void MpiWorld::setUpStateKV()
{
    if (stateKV == nullptr) {
        state::State& state = state::getGlobalState();
        std::string stateKey = getWorldStateKey(id);
        stateKV = state.getKV(user, stateKey, sizeof(MpiWorldState));
    }
}

std::shared_ptr<state::StateKeyValue> MpiWorld::getRankHostState(int rank)
{
    state::State& state = state::getGlobalState();
    std::string stateKey = getRankStateKey(id, rank);
    return state.getKV(user, stateKey, MPI_HOST_STATE_LEN);
}

int MpiWorld::getMpiThreadPoolSize()
{
    auto logger = faabric::util::getLogger();
    int usableCores = faabric::util::getUsableCores();
    int worldSize = size;

    if ((worldSize > usableCores) && (worldSize % usableCores != 0)) {
        logger->warn("Over-provisioning threads in the MPI thread pool.");
        logger->warn("To avoid this, set an MPI world size multiple of the "
                     "number of cores per machine.");
    }
    return std::min<int>(worldSize, usableCores);
}

void MpiWorld::create(const faabric::Message& call, int newId, int newSize)
{
    id = newId;
    user = call.user();
    function = call.function();

    size = newSize;
    threadPool = std::make_shared<faabric::scheduler::MpiAsyncThreadPool>(
      getMpiThreadPoolSize());

    // Write this to state
    setUpStateKV();
    pushToState();

    // Register this as the master
    registerRank(0);

    // Dispatch all the chained calls
    // NOTE - with the master being rank zero, we want to spawn
    // (size - 1) new functions starting with rank 1
    scheduler::Scheduler& sch = scheduler::getScheduler();
    for (int i = 1; i < size; i++) {
        faabric::Message msg = faabric::util::messageFactory(user, function);
        msg.set_ismpi(true);
        msg.set_mpiworldid(id);
        msg.set_mpirank(i);
        msg.set_cmdline(call.cmdline());
    }

        sch.callFunction(msg);
    }
}

void MpiWorld::destroy()
{
    setUpStateKV();
    state::getGlobalState().deleteKV(stateKV->user, stateKV->key);

    for (auto& s : rankHostMap) {
        const std::shared_ptr<state::StateKeyValue>& rankState =
          getRankHostState(s.first);
        state::getGlobalState().deleteKV(rankState->user, rankState->key);
    }

    localQueueMap.clear();
}

void MpiWorld::initialiseFromState(const faabric::Message& msg, int worldId)
{
    id = worldId;
    user = msg.user();
    function = msg.function();

    setUpStateKV();

    // Read from state
    MpiWorldState s{};
    stateKV->pull();
    stateKV->get(BYTES(&s));
    size = s.worldSize;
    threadPool = std::make_shared<faabric::scheduler::MpiAsyncThreadPool>(
      getMpiThreadPoolSize());
}

void MpiWorld::pushToState()
{
    // Write to state
    MpiWorldState s{
        .worldSize = this->size,
    };

    stateKV->set(BYTES(&s));
    stateKV->pushFull();
}

void MpiWorld::registerRank(int rank)
{
    {
        faabric::util::FullLock lock(worldMutex);
        rankHostMap[rank] = thisHost;
    }

    // Note that the host name may be shorter than the buffer, so we need to pad
    // with nulls
    uint8_t hostBytesBuffer[MPI_HOST_STATE_LEN];
    memset(hostBytesBuffer, '\0', MPI_HOST_STATE_LEN);
    ::strcpy((char*)hostBytesBuffer, thisHost.c_str());

    const std::shared_ptr<state::StateKeyValue>& kv = getRankHostState(rank);
    kv->set(hostBytesBuffer);
    kv->pushFull();
}

std::string MpiWorld::getHostForRank(int rank)
{
    // Pull from state if not present
    if (rankHostMap.count(rank) == 0) {
        faabric::util::FullLock lock(worldMutex);

        if (rankHostMap.count(rank) == 0) {
            auto buffer = new uint8_t[MPI_HOST_STATE_LEN];
            const std::shared_ptr<state::StateKeyValue>& kv =
              getRankHostState(rank);
            kv->get(buffer);

            char* bufferChar = reinterpret_cast<char*>(buffer);
            if (bufferChar[0] == '\0') {
                // No entry for other rank
                throw std::runtime_error(
                  fmt::format("No host entry for rank {}", rank));
            }

            // Note - we rely on C strings detecting the null terminator here,
            // assuming the host will either be an IP or string of alphanumeric
            // characters and dots
            std::string otherHost(bufferChar);
            rankHostMap[rank] = otherHost;
        }
    }

    return rankHostMap[rank];
}

void MpiWorld::getCartesianRank(int rank,
                                int maxDims,
                                const int* dims,
                                int* periods,
                                int* coords)
{
    if (rank > this->size - 1) {
        throw std::runtime_error(
          fmt::format("Rank {} bigger than world size {}", rank, this->size));
    }
    // Pre-requisite: dims[0] * dims[1] == nprocs
    // Note: we don't support 3-dim grids
    if ((dims[0] * dims[1]) != this->size) {
        throw std::runtime_error(
          fmt::format("Product of ranks across dimensions not equal to world "
                      "size, {} x {} != {}",
                      dims[0],
                      dims[1],
                      this->size));
    }

    // Store the cartesian dimensions for further use. All ranks have the same
    // vector.
    // Note that we could only store one of the two, and derive the other
    // from the world size.
    this->cartProcsPerDim[0] = dims[0];
    this->cartProcsPerDim[1] = dims[1];

    // Compute the coordinates in a 2-dim grid of the original process rank.
    // As input we have a vector containing the number of processes per
    // dimension (dims).
    // We have dims[0] x dims[1] = N slots, thus:
    coords[0] = rank / dims[1];
    coords[1] = rank % dims[1];

    // LAMMPS always uses periodic grids. So do we.
    periods[0] = 1;
    periods[1] = 1;

    // The remaining dimensions should be 1, and the coordinate of our rank 0
    for (int i = 2; i < maxDims; i++) {
        if (dims[i] != 1) {
            throw std::runtime_error(
              fmt::format("Non-zero number of processes in dimension greater "
                          "than 2. {} -> {}",
                          i,
                          dims[i]));
        }
        coords[i] = 0;
        periods[i] = 1;
    }
}

void MpiWorld::getRankFromCoords(int* rank, int* coords)
{
    // Note that we only support 2 dim grids. In each dimension we have
    // cartProcsPerDim[0] and cartProcsPerDim[1] processes respectively.

    // Pre-requisite: cartProcsPerDim[0] * cartProcsPerDim[1] == nprocs
    if ((this->cartProcsPerDim[0] * this->cartProcsPerDim[1]) != this->size) {
        throw std::runtime_error(fmt::format(
          "Processors per dimension don't match world size: {} x {} != {}",
          this->cartProcsPerDim[0],
          this->cartProcsPerDim[1],
          this->size));
    }

    // This is the inverse of finding the coordinates for a rank
    *rank = coords[1] + coords[0] * cartProcsPerDim[1];
}

void MpiWorld::shiftCartesianCoords(int rank,
                                    int direction,
                                    int disp,
                                    int* source,
                                    int* destination)
{
    // rank: is the process the method is being called from (i.e. me)
    // source: the rank that reaches me moving <disp> units in <direction>
    // destination: is the rank I reach moving <disp> units in <direction>

    // Get the coordinates for my rank
    std::vector<int> coords = { rank / cartProcsPerDim[1],
                                rank % cartProcsPerDim[1] };

    // Move <disp> units in <direction> forward with periodicity
    // Note: we always use periodicity and 2 dimensions because LAMMMPS does.
    std::vector<int> dispCoordsFwd;
    if (direction == 0) {
        dispCoordsFwd = { (coords[0] + disp) % cartProcsPerDim[0], coords[1] };
    } else if (direction == 1) {
        dispCoordsFwd = { coords[0], (coords[1] + disp) % cartProcsPerDim[1] };
    } else {
        dispCoordsFwd = { coords[0], coords[1] };
    }
    // If direction >=2 we are in a dimension we don't use, hence we are the
    // only process, and we always land in our coordinates (due to periodicity)

    // Fill the destination variable
    getRankFromCoords(destination, dispCoordsFwd.data());

    // Move <disp> units in <direction> backwards with periodicity
    // Note: as subtracting may yield a negative result, we add a full loop
    // to prevent taking the modulo of a negative value.
    std::vector<int> dispCoordsBwd;
    if (direction == 0) {
        dispCoordsBwd = { (coords[0] - disp + cartProcsPerDim[0]) %
                            cartProcsPerDim[0],
                          coords[1] };
    } else if (direction == 1) {
        dispCoordsBwd = { coords[0],
                          (coords[1] - disp + cartProcsPerDim[1]) %
                            cartProcsPerDim[1] };
    } else {
        dispCoordsBwd = { coords[0], coords[1] };
    }

    // Fill the source variable
    getRankFromCoords(source, dispCoordsBwd.data());
}

int MpiWorld::isend(int sendRank,
                    int recvRank,
                    const uint8_t* buffer,
                    faabric_datatype_t* dataType,
                    int count,
                    faabric::MPIMessage::MPIMessageType messageType)
{
    int requestId = (int)faabric::util::generateGid();

    std::promise<void> resultPromise;
    std::future<void> resultFuture = resultPromise.get_future();
    threadPool->getMpiReqQueue()->enqueue(
      std::make_tuple(requestId,
                      std::bind(&MpiWorld::send,
                                this,
                                sendRank,
                                recvRank,
                                buffer,
                                dataType,
                                count,
                                messageType),
                      std::move(resultPromise)));

    // Place the promise in a map to wait for it later
    futureMap.emplace(std::make_pair(requestId, std::move(resultFuture)));

    return requestId;
}

int MpiWorld::irecv(int sendRank,
                    int recvRank,
                    uint8_t* buffer,
                    faabric_datatype_t* dataType,
                    int count,
                    faabric::MPIMessage::MPIMessageType messageType)
{
    int requestId = (int)faabric::util::generateGid();

    std::promise<void> resultPromise;
    std::future<void> resultFuture = resultPromise.get_future();
    threadPool->getMpiReqQueue()->enqueue(
      std::make_tuple(requestId,
                      std::bind(&MpiWorld::recv,
                                this,
                                sendRank,
                                recvRank,
                                buffer,
                                dataType,
                                count,
                                nullptr,
                                messageType),
                      std::move(resultPromise)));

    // Place the promise in a map to wait for it later
    futureMap.emplace(std::make_pair(requestId, std::move(resultFuture)));

    return requestId;
}

void MpiWorld::send(int sendRank,
                    int recvRank,
                    const uint8_t* buffer,
                    faabric_datatype_t* dataType,
                    int count,
                    faabric::MPIMessage::MPIMessageType messageType)
{
    const std::shared_ptr<spdlog::logger>& logger = faabric::util::getLogger();

    if (recvRank > this->size - 1) {
        throw std::runtime_error(fmt::format(
          "Rank {} bigger than world size {}", recvRank, this->size));
    }

    // Generate a message ID
    int msgId = (int)faabric::util::generateGid();

    // Create the message
    auto m = std::make_shared<faabric::MPIMessage>();
    m->set_id(msgId);
    m->set_worldid(id);
    m->set_sender(sendRank);
    m->set_destination(recvRank);
    m->set_type(dataType->id);
    m->set_count(count);
    m->set_messagetype(messageType);

    // Work out whether the message is sent locally or to another host
    const std::string otherHost = getHostForRank(recvRank);
    bool isLocal = otherHost == thisHost;

    // Set up message data
    if (count > 0 && buffer != nullptr) {
        m->set_buffer(buffer, dataType->size * count);
    }

    // Dispatch the message locally or globally
    if (isLocal) {
        if (messageType == faabric::MPIMessage::RMA_WRITE) {
            logger->trace("MPI - local RMA write {} -> {}", sendRank, recvRank);
            synchronizeRmaWrite(*m, false);
        } else {
            logger->trace("MPI - send {} -> {}", sendRank, recvRank);
            getLocalQueue(sendRank, recvRank)->enqueue(std::move(m));
        }
    } else {
        logger->trace("MPI - send remote {} -> {}", sendRank, recvRank);

        // TODO - avoid creating a client each time?
        scheduler::FunctionCallClient client(otherHost);
        client.sendMPIMessage(m);
    }
}

void MpiWorld::recv(int sendRank,
                    int recvRank,
                    uint8_t* buffer,
                    faabric_datatype_t* dataType,
                    int count,
                    MPI_Status* status,
                    faabric::MPIMessage::MPIMessageType messageType)
{
    const std::shared_ptr<spdlog::logger>& logger = faabric::util::getLogger();

    // Listen to the in-memory queue for this rank and message type
    logger->trace("MPI - recv {} -> {}", sendRank, recvRank);
    std::shared_ptr<faabric::MPIMessage> m =
      getLocalQueue(sendRank, recvRank)->dequeue();

    if (messageType != m->messagetype()) {
        logger->error(
          "Message types mismatched on {}->{} (expected={}, got={})",
          sendRank,
          recvRank,
          messageType,
          m->messagetype());
        throw std::runtime_error("Mismatched message types");
    }

    if (m->count() > count) {
        logger->error(
          "Message too long for buffer (msg={}, buffer={})", m->count(), count);
        throw std::runtime_error("Message too long");
    }

    // TODO - avoid copy here
    // Copy message data
    if (m->count() > 0) {
        std::move(m->buffer().begin(), m->buffer().end(), buffer);
    }

    // Set status values if required
    if (status != nullptr) {
        status->MPI_SOURCE = m->sender();
        status->MPI_ERROR = MPI_SUCCESS;

        // Note, take the message size here as the receive count may be larger
        status->bytesSize = m->count() * dataType->size;

        // TODO - thread through tag
        status->MPI_TAG = -1;
    }
}

void MpiWorld::sendRecv(uint8_t* sendBuffer,
                        int sendCount,
                        faabric_datatype_t* sendDataType,
                        int sendRank,
                        uint8_t* recvBuffer,
                        int recvCount,
                        faabric_datatype_t* recvDataType,
                        int recvRank,
                        int myRank,
                        MPI_Status* status)
{
    auto logger = faabric::util::getLogger();
    logger->trace(
      "MPI - Sendrecv. Rank {}. Sending to: {} - Receiving from: {}",
      myRank,
      sendRank,
      recvRank);

    if (recvRank > this->size - 1) {
        throw std::runtime_error(fmt::format(
          "Receive rank {} bigger than world size {}", recvRank, this->size));
    }
    if (sendRank > this->size - 1) {
        throw std::runtime_error(fmt::format(
          "Send rank {} bigger than world size {}", sendRank, this->size));
    }

    // Post async recv
    int recvId = irecv(recvRank,
                       myRank,
                       recvBuffer,
                       recvDataType,
                       recvCount,
                       faabric::MPIMessage::SENDRECV);
    // Then send the message
    send(myRank,
         sendRank,
         sendBuffer,
         sendDataType,
         sendCount,
         faabric::MPIMessage::SENDRECV);
    // And wait
    awaitAsyncRequest(recvId);
}

void MpiWorld::broadcast(int sendRank,
                         const uint8_t* buffer,
                         faabric_datatype_t* dataType,
                         int count,
                         faabric::MPIMessage::MPIMessageType messageType)
{
    const std::shared_ptr<spdlog::logger>& logger = faabric::util::getLogger();
    logger->trace("MPI - bcast {} -> all", sendRank);

    for (int r = 0; r < size; r++) {
        // Skip this rank (it's doing the broadcasting)
        if (r == sendRank) {
            continue;
        }

        // Send to the other ranks
        send(sendRank, r, buffer, dataType, count, messageType);
    }
}

void checkSendRecvMatch(faabric_datatype_t* sendType,
                        int sendCount,
                        faabric_datatype_t* recvType,
                        int recvCount)
{
    if (sendType->id != recvType->id && sendCount == recvCount) {
        const std::shared_ptr<spdlog::logger>& logger =
          faabric::util::getLogger();
        logger->error("Must match type/ count (send {}:{}, recv {}:{})",
                      sendType->id,
                      sendCount,
                      recvType->id,
                      recvCount);
        throw std::runtime_error("Mismatching send/ recv");
    }
}

void MpiWorld::scatter(int sendRank,
                       int recvRank,
                       const uint8_t* sendBuffer,
                       faabric_datatype_t* sendType,
                       int sendCount,
                       uint8_t* recvBuffer,
                       faabric_datatype_t* recvType,
                       int recvCount)
{
    const std::shared_ptr<spdlog::logger>& logger = faabric::util::getLogger();
    checkSendRecvMatch(sendType, sendCount, recvType, recvCount);

    size_t sendOffset = sendCount * sendType->size;

    // If we're the sender, do the sending
    if (recvRank == sendRank) {
        logger->trace("MPI - scatter {} -> all", sendRank);

        for (int r = 0; r < size; r++) {
            // Work out the chunk of the send buffer to send to this rank
            const uint8_t* startPtr = sendBuffer + (r * sendOffset);

            if (r == sendRank) {
                // Copy data directly if this is the send rank
                const uint8_t* endPtr = startPtr + sendOffset;
                std::copy(startPtr, endPtr, recvBuffer);
            } else {
                send(sendRank,
                     r,
                     startPtr,
                     sendType,
                     sendCount,
                     faabric::MPIMessage::SCATTER);
            }
        }
    } else {
        // Do the receiving
        recv(sendRank,
             recvRank,
             recvBuffer,
             recvType,
             recvCount,
             nullptr,
             faabric::MPIMessage::SCATTER);
    }
}

void MpiWorld::gather(int sendRank,
                      int recvRank,
                      const uint8_t* sendBuffer,
                      faabric_datatype_t* sendType,
                      int sendCount,
                      uint8_t* recvBuffer,
                      faabric_datatype_t* recvType,
                      int recvCount)
{
    const std::shared_ptr<spdlog::logger>& logger = faabric::util::getLogger();
    checkSendRecvMatch(sendType, sendCount, recvType, recvCount);

    size_t sendOffset = sendCount * sendType->size;
    size_t recvOffset = recvCount * recvType->size;

    bool isInPlace = sendBuffer == recvBuffer;

    // If we're the root, do the gathering
    if (sendRank == recvRank) {
        logger->trace("MPI - gather all -> {}", recvRank);

        // Iterate through each rank
        for (int r = 0; r < size; r++) {
            // Work out where in the receive buffer this rank's data goes
            uint8_t* recvChunk = recvBuffer + (r * recvOffset);

            if ((r == recvRank) && isInPlace) {
                // If operating in-place, data for the root rank is already in
                // position
                continue;
            } else if (r == recvRank) {
                // Copy data locally on root
                std::copy(sendBuffer, sendBuffer + sendOffset, recvChunk);
            } else {
                // Receive data from rank if it's not the root
                recv(r,
                     recvRank,
                     recvChunk,
                     recvType,
                     recvCount,
                     nullptr,
                     faabric::MPIMessage::GATHER);
            }
        }
    } else {
        if (isInPlace) {
            // A non-root rank running gather "in place" happens as part of an
            // allgather operation. In this case, the send and receive buffer
            // are the same, and the rank is eventually expecting a broadcast of
            // the gather result into this buffer. This means that this buffer
            // is big enough for the whole gather result, with this rank's data
            // already in place. Therefore we need to send _only_ the part of
            // the send buffer relating to this rank.
            const uint8_t* sendChunk = sendBuffer + (sendRank * sendOffset);
            send(sendRank,
                 recvRank,
                 sendChunk,
                 sendType,
                 sendCount,
                 faabric::MPIMessage::GATHER);
        } else {
            // Normal sending
            send(sendRank,
                 recvRank,
                 sendBuffer,
                 sendType,
                 sendCount,
                 faabric::MPIMessage::GATHER);
        }
    }
}

void MpiWorld::allGather(int rank,
                         const uint8_t* sendBuffer,
                         faabric_datatype_t* sendType,
                         int sendCount,
                         uint8_t* recvBuffer,
                         faabric_datatype_t* recvType,
                         int recvCount)
{
    checkSendRecvMatch(sendType, sendCount, recvType, recvCount);

    int root = 0;

    // Do a gather with a hard-coded root
    gather(rank,
           root,
           sendBuffer,
           sendType,
           sendCount,
           recvBuffer,
           recvType,
           recvCount);

    // Note that sendCount and recvCount here are per-rank, so we need to work
    // out the full buffer size
    int fullCount = recvCount * size;
    if (rank == root) {
        // Broadcast the result
        broadcast(root,
                  recvBuffer,
                  recvType,
                  fullCount,
                  faabric::MPIMessage::ALLGATHER);
    } else {
        // Await the broadcast from the master
        recv(root,
             rank,
             recvBuffer,
             recvType,
             fullCount,
             nullptr,
             faabric::MPIMessage::ALLGATHER);
    }
}

void MpiWorld::awaitAsyncRequest(int requestId)
{
    faabric::util::getLogger()->trace("MPI - await {}", requestId);

    auto it = futureMap.find(requestId);
    if (it == futureMap.end()) {
        throw std::runtime_error(
          fmt::format("Error: waiting for unrecognized request {}", requestId));
    }

    // This call blocks until requestId has finished.
    it->second.wait();
    futureMap.erase(it);

    faabric::util::getLogger()->debug("Finished awaitAsyncRequest on {}",
                                      requestId);
}

void MpiWorld::reduce(int sendRank,
                      int recvRank,
                      uint8_t* sendBuffer,
                      uint8_t* recvBuffer,
                      faabric_datatype_t* datatype,
                      int count,
                      faabric_op_t* operation)
{
    const std::shared_ptr<spdlog::logger>& logger = faabric::util::getLogger();

    // If we're the receiver, await inputs
    if (sendRank == recvRank) {
        logger->trace("MPI - reduce ({}) all -> {}", operation->id, recvRank);

        size_t bufferSize = datatype->size * count;

        bool isInPlace = sendBuffer == recvBuffer;

        // If not receiving in-place, initialize the receive buffer to the send
        // buffer values. This prevents issues when 0-initializing for operators
        // like the minimum, or product.
        // If we're receiving from ourselves and in-place, our work is
        // already done and the results are written in the recv buffer
        if (!isInPlace) {
            memcpy(recvBuffer, sendBuffer, bufferSize);
        }

        uint8_t* rankData = new uint8_t[bufferSize];
        for (int r = 0; r < size; r++) {
            // Work out the data for this rank
            memset(rankData, 0, bufferSize);
            if (r != recvRank) {
                recv(r,
                     recvRank,
                     rankData,
                     datatype,
                     count,
                     nullptr,
                     faabric::MPIMessage::REDUCE);

                op_reduce(operation, datatype, count, rankData, recvBuffer);
            }
        }

        delete[] rankData;

    } else {
        // Do the sending
        send(sendRank,
             recvRank,
             sendBuffer,
             datatype,
             count,
             faabric::MPIMessage::REDUCE);
    }
}

void MpiWorld::allReduce(int rank,
                         uint8_t* sendBuffer,
                         uint8_t* recvBuffer,
                         faabric_datatype_t* datatype,
                         int count,
                         faabric_op_t* operation)
{
    // Rank 0 coordinates the allreduce operation
    if (rank == 0) {
        // Run the standard reduce
        reduce(0, 0, sendBuffer, recvBuffer, datatype, count, operation);

        // Broadcast the result
        broadcast(
          0, recvBuffer, datatype, count, faabric::MPIMessage::ALLREDUCE);
    } else {
        // Run the standard reduce
        reduce(rank, 0, sendBuffer, recvBuffer, datatype, count, operation);

        // Await the broadcast from the master
        recv(0,
             rank,
             recvBuffer,
             datatype,
             count,
             nullptr,
             faabric::MPIMessage::ALLREDUCE);
    }
}

void MpiWorld::op_reduce(faabric_op_t* operation,
                         faabric_datatype_t* datatype,
                         int count,
                         uint8_t* inBuffer,
                         uint8_t* outBuffer)
{
    const std::shared_ptr<spdlog::logger>& logger = faabric::util::getLogger();

    logger->trace(
      "MPI - reduce op: {} datatype {}", operation->id, datatype->id);
    if (operation->id == faabric_op_max.id) {
        if (datatype->id == FAABRIC_INT) {
            auto inBufferCast = reinterpret_cast<int*>(inBuffer);
            auto outBufferCast = reinterpret_cast<int*>(outBuffer);

            for (int slot = 0; slot < count; slot++) {
                outBufferCast[slot] =
                  std::max<int>(outBufferCast[slot], inBufferCast[slot]);
            }
        } else if (datatype->id == FAABRIC_DOUBLE) {
            auto inBufferCast = reinterpret_cast<double*>(inBuffer);
            auto outBufferCast = reinterpret_cast<double*>(outBuffer);

            for (int slot = 0; slot < count; slot++) {
                outBufferCast[slot] =
                  std::max<double>(outBufferCast[slot], inBufferCast[slot]);
            }
        } else if (datatype->id == FAABRIC_LONG_LONG) {
            auto inBufferCast = reinterpret_cast<long long*>(inBuffer);
            auto outBufferCast = reinterpret_cast<long long*>(outBuffer);

            for (int slot = 0; slot < count; slot++) {
                outBufferCast[slot] =
                  std::max<long long>(outBufferCast[slot], inBufferCast[slot]);
            }
        } else {
            logger->error("Unsupported type for max reduction (datatype={})",
                          datatype->id);
            throw std::runtime_error("Unsupported type for max reduction");
        }
    } else if (operation->id == faabric_op_min.id) {
        if (datatype->id == FAABRIC_INT) {
            auto inBufferCast = reinterpret_cast<int*>(inBuffer);
            auto outBufferCast = reinterpret_cast<int*>(outBuffer);

            for (int slot = 0; slot < count; slot++) {
                outBufferCast[slot] =
                  std::min<int>(outBufferCast[slot], inBufferCast[slot]);
            }
        } else if (datatype->id == FAABRIC_DOUBLE) {
            auto inBufferCast = reinterpret_cast<double*>(inBuffer);
            auto outBufferCast = reinterpret_cast<double*>(outBuffer);

            for (int slot = 0; slot < count; slot++) {
                outBufferCast[slot] =
                  std::min<double>(outBufferCast[slot], inBufferCast[slot]);
            }
        } else if (datatype->id == FAABRIC_LONG_LONG) {
            auto inBufferCast = reinterpret_cast<long long*>(inBuffer);
            auto outBufferCast = reinterpret_cast<long long*>(outBuffer);

            for (int slot = 0; slot < count; slot++) {
                outBufferCast[slot] =
                  std::min<long long>(outBufferCast[slot], inBufferCast[slot]);
            }
        } else {
            logger->error("Unsupported type for min reduction (datatype={})",
                          datatype->id);
            throw std::runtime_error("Unsupported type for min reduction");
        }
    } else if (operation->id == faabric_op_sum.id) {
        if (datatype->id == FAABRIC_INT) {
            auto inBufferCast = reinterpret_cast<int*>(inBuffer);
            auto outBufferCast = reinterpret_cast<int*>(outBuffer);

            for (int slot = 0; slot < count; slot++) {
                outBufferCast[slot] += inBufferCast[slot];
            }
        } else if (datatype->id == FAABRIC_DOUBLE) {
            auto inBufferCast = reinterpret_cast<double*>(inBuffer);
            auto outBufferCast = reinterpret_cast<double*>(outBuffer);

            for (int slot = 0; slot < count; slot++) {
                outBufferCast[slot] += inBufferCast[slot];
            }
        } else if (datatype->id == FAABRIC_LONG_LONG) {
            auto inBufferCast = reinterpret_cast<long long*>(inBuffer);
            auto outBufferCast = reinterpret_cast<long long*>(outBuffer);

            for (int slot = 0; slot < count; slot++) {
                outBufferCast[slot] += inBufferCast[slot];
            }
        } else {
            logger->error("Unsupported type for sum reduction (datatype={})",
                          datatype->id);
            throw std::runtime_error("Unsupported type for sum reduction");
        }
    } else {
        logger->error("Reduce operation not implemented: {}", operation->id);
        throw std::runtime_error("Not yet implemented reduce operation");
    }
}

void MpiWorld::scan(int rank,
                    uint8_t* sendBuffer,
                    uint8_t* recvBuffer,
                    faabric_datatype_t* datatype,
                    int count,
                    faabric_op_t* operation)
{
    auto logger = faabric::util::getLogger();
    logger->trace("MPI - scan");

    if (rank > this->size - 1) {
        throw std::runtime_error(
          fmt::format("Rank {} bigger than world size {}", rank, this->size));
    }

    bool isInPlace = sendBuffer == recvBuffer;

    // Scan performs an inclusive prefix reduction, so our input values
    // need also to be considered.
    size_t bufferSize = datatype->size * count;
    if (!isInPlace) {
        memcpy(recvBuffer, sendBuffer, bufferSize);
    }

    if (rank > 0) {
        // Receive the current accumulated value
        auto currentAcc = new uint8_t[bufferSize];
        recv(rank - 1,
             rank,
             currentAcc,
             datatype,
             count,
             nullptr,
             faabric::MPIMessage::SCAN);
        // Reduce with our own value
        op_reduce(operation, datatype, count, currentAcc, recvBuffer);
        delete[] currentAcc;
    }

    // If not the last process, send to the next one
    if (rank < this->size - 1) {
        send(rank,
             rank + 1,
             recvBuffer,
             MPI_INT,
             count,
             faabric::MPIMessage::SCAN);
    }
}

void MpiWorld::allToAll(int rank,
                        uint8_t* sendBuffer,
                        faabric_datatype_t* sendType,
                        int sendCount,
                        uint8_t* recvBuffer,
                        faabric_datatype_t* recvType,
                        int recvCount)
{
    checkSendRecvMatch(sendType, sendCount, recvType, recvCount);

    size_t sendOffset = sendCount * sendType->size;

    // Send out messages for this rank
    for (int r = 0; r < size; r++) {
        // Work out what data to send to this rank
        size_t rankOffset = r * sendOffset;
        uint8_t* sendChunk = sendBuffer + rankOffset;

        if (r == rank) {
            // Copy directly
            std::copy(
              sendChunk, sendChunk + sendOffset, recvBuffer + rankOffset);
        } else {
            // Send message to other rank
            send(rank,
                 r,
                 sendChunk,
                 sendType,
                 sendCount,
                 faabric::MPIMessage::ALLTOALL);
        }
    }

    // Await incoming messages from others
    for (int r = 0; r < size; r++) {
        if (r == rank) {
            continue;
        }

        // Work out where to place the result from this rank
        uint8_t* recvChunk = recvBuffer + (r * sendOffset);

        // Do the receive
        recv(r,
             rank,
             recvChunk,
             recvType,
             recvCount,
             nullptr,
             faabric::MPIMessage::ALLTOALL);
    }
}

void MpiWorld::probe(int sendRank, int recvRank, MPI_Status* status)
{
    const std::shared_ptr<InMemoryMpiQueue>& queue =
      getLocalQueue(sendRank, recvRank);
    std::shared_ptr<faabric::MPIMessage> m = *(queue->peek());

    faabric_datatype_t* datatype = getFaabricDatatypeFromId(m->type());
    status->bytesSize = m->count() * datatype->size;
    status->MPI_ERROR = 0;
    status->MPI_SOURCE = m->sender();
}

void MpiWorld::barrier(int thisRank)
{
    const std::shared_ptr<spdlog::logger>& logger = faabric::util::getLogger();

    if (thisRank == 0) {
        // This is the root, hence just does the waiting

        // Await messages from all others
        for (int r = 1; r < size; r++) {
            MPI_Status s{};
            recv(
              r, 0, nullptr, MPI_INT, 0, &s, faabric::MPIMessage::BARRIER_JOIN);
            logger->trace("MPI - recv barrier join {}", s.MPI_SOURCE);
        }

        // Broadcast that the barrier is done
        broadcast(0, nullptr, MPI_INT, 0, faabric::MPIMessage::BARRIER_DONE);
    } else {
        // Tell the root that we're waiting
        logger->trace("MPI - barrier join {}", thisRank);
        send(
          thisRank, 0, nullptr, MPI_INT, 0, faabric::MPIMessage::BARRIER_JOIN);

        // Receive a message saying the barrier is done
        recv(0,
             thisRank,
             nullptr,
             MPI_INT,
             0,
             nullptr,
             faabric::MPIMessage::BARRIER_DONE);
        logger->trace("MPI - barrier done {}", thisRank);
    }
}

void MpiWorld::enqueueMessage(faabric::MPIMessage& msg)
{
    const std::shared_ptr<spdlog::logger>& logger = faabric::util::getLogger();

    if (msg.worldid() != id) {
        logger->error(
          "Queueing message not meant for this world (msg={}, this={})",
          msg.worldid(),
          id);
        throw std::runtime_error("Queueing message not for this world");
    }

    if (msg.messagetype() == faabric::MPIMessage::RMA_WRITE) {
        // NOTE - RMA notifications must be processed synchronously to ensure
        // ordering
        synchronizeRmaWrite(msg, true);
    } else {
        logger->trace(
          "Queueing message locally {} -> {}", msg.sender(), msg.destination());
        getLocalQueue(msg.sender(), msg.destination())
          ->enqueue(std::make_shared<faabric::MPIMessage>(msg));
    }
}

std::shared_ptr<InMemoryMpiQueue> MpiWorld::getLocalQueue(int sendRank,
                                                          int recvRank)
{
    checkRankOnThisHost(recvRank);

    std::string key = std::to_string(sendRank) + "_" + std::to_string(recvRank);
    if (localQueueMap.count(key) == 0) {
        faabric::util::FullLock lock(worldMutex);

        if (localQueueMap.count(key) == 0) {
            auto mq = new InMemoryMpiQueue();
            localQueueMap.emplace(
              std::pair<std::string, InMemoryMpiQueue*>(key, mq));
        }
    }

    return localQueueMap[key];
}

void MpiWorld::rmaGet(int sendRank,
                      faabric_datatype_t* sendType,
                      int sendCount,
                      uint8_t* recvBuffer,
                      faabric_datatype_t* recvType,
                      int recvCount)
{
    checkSendRecvMatch(sendType, sendCount, recvType, recvCount);

    // Get the state value that relates to this window
    int buffLen = sendType->size * sendCount;
    const std::string stateKey = getWindowStateKey(id, sendRank, buffLen);
    state::State& state = state::getGlobalState();
    const std::shared_ptr<state::StateKeyValue>& kv =
      state.getKV(user, stateKey, buffLen);

    // If it's remote, do a pull too
    if (getHostForRank(sendRank) != thisHost) {
        kv->pull();
    }

    // Do the read
    kv->get(recvBuffer);
}

void MpiWorld::rmaPut(int sendRank,
                      uint8_t* sendBuffer,
                      faabric_datatype_t* sendType,
                      int sendCount,
                      int recvRank,
                      faabric_datatype_t* recvType,
                      int recvCount)
{
    checkSendRecvMatch(sendType, sendCount, recvType, recvCount);

    // Get the state value for the window to write to
    int buffLen = sendType->size * sendCount;
    const std::string stateKey = getWindowStateKey(id, recvRank, buffLen);
    state::State& state = state::getGlobalState();
    const std::shared_ptr<state::StateKeyValue>& kv =
      state.getKV(user, stateKey, buffLen);

    // Do the write
    kv->set(sendBuffer);

    // If it's remote, do a push too
    if (getHostForRank(recvRank) != thisHost) {
        kv->pushFull();
    }

    // Notify the receiver of the push
    // NOTE - must specify a count here to say how big the change is
    send(sendRank,
         recvRank,
         nullptr,
         MPI_INT,
         sendCount,
         faabric::MPIMessage::RMA_WRITE);
}

void MpiWorld::synchronizeRmaWrite(const faabric::MPIMessage& msg,
                                   bool isRemote)
{
    faabric_datatype_t* datatype = getFaabricDatatypeFromId(msg.type());
    int winSize = msg.count() * datatype->size;
    const std::string key = getWindowStateKey(id, msg.destination(), winSize);

    // Get the state KV
    state::State& state = state::getGlobalState();
    const std::shared_ptr<state::StateKeyValue>& kv =
      state.getKV(user, key, winSize);

    // If remote, pull the state related to the window
    if (isRemote) {
        kv->pull();
    }

    // Write the state to the pointer
    uint8_t* windowPtr = windowPointerMap[key];
    kv->get(windowPtr);
}

long MpiWorld::getLocalQueueSize(int sendRank, int recvRank)
{
    const std::shared_ptr<InMemoryMpiQueue>& queue =
      getLocalQueue(sendRank, recvRank);
    return queue->size();
}

void MpiWorld::checkRankOnThisHost(int rank)
{
    const std::shared_ptr<spdlog::logger>& logger = faabric::util::getLogger();

    // Check if we know about this rank on this host
    if (rankHostMap.count(rank) == 0) {
        logger->error("No mapping found for rank {} on this host", rank);
        throw std::runtime_error("No mapping found for rank");
    } else if (rankHostMap[rank] != thisHost) {
        logger->error("Trying to access rank {} on {} but it's on {}",
                      rank,
                      thisHost,
                      rankHostMap[rank]);
        throw std::runtime_error("Accessing in-memory queue for remote rank");
    }
}

void MpiWorld::createWindow(const int winRank,
                            const int winSize,
                            uint8_t* windowPtr)
{
    const std::string key = getWindowStateKey(id, winRank, winSize);
    state::State& state = state::getGlobalState();
    const std::shared_ptr<state::StateKeyValue> windowKv =
      state.getKV(user, key, winSize);

    // Set initial value
    windowKv->set(windowPtr);
    windowKv->pushFull();

    // Add pointer to map
    {
        faabric::util::FullLock lock(worldMutex);
        windowPointerMap[key] = windowPtr;
    }
}

double MpiWorld::getWTime()
{
    double t = faabric::util::getTimeDiffMillis(creationTime);
    return t / 1000.0;
}

std::string MpiWorld::getUser()
{
    return user;
}

std::string MpiWorld::getFunction()
{
    return function;
}

int MpiWorld::getId()
{
    return id;
}

int MpiWorld::getSize()
{
    return size;
}

void MpiWorld::overrideHost(const std::string& newHost)
{
    thisHost = newHost;
}
}
