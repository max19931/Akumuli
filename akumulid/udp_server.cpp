#include "udp_server.h"

#include <thread>

#include <sys/socket.h>
#include <netinet/ip.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

#include <boost/bind.hpp>
#include <boost/exception/diagnostic_information.hpp>

namespace Akumuli {

UdpServer::UdpServer(std::shared_ptr<DbConnection> db, int nworkers, boost::asio::ip::tcp::endpoint const& endpoint)
    : db_(db)
    , start_barrier_(static_cast<u32>(nworkers + 1))
    , stop_barrier_(static_cast<u32>(nworkers + 1))
    , stop_{0}
    , endpoint_(endpoint)
    , nworkers_(nworkers)
    , sockfd_(-1)
    , logger_("UdpServer")
{
}

void UdpServer::start(SignalHandler *sig, int id) {
    auto self = shared_from_this();
    sig->add_handler(boost::bind(&UdpServer::stop, std::move(self)), id);

    // Create workers
    for (int i = 0; i < nworkers_; i++) {
        auto session = db_->create_session();
        std::thread thread(std::bind(&UdpServer::worker, shared_from_this(), std::move(session)));
        thread.detach();
    }
    start_barrier_.wait();
}

static void sendByteToLocalhost(boost::asio::ip::tcp::endpoint const& endpoint) {
    Logger logger("UdpServer");
    int fd;

    if ((fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        // We can't send the message to the socket thus, we wouldn't be able
        // to stop the program normally anyway.
        logger.error() << "Can't create the socket";
        std::terminate();
    }

    char payload = 0;

    if (sendto(fd, &payload, 1, 0, endpoint.data(), endpoint.size()) < 0) {
        // Same reasoning as previously
        logger.error() << "Can't send the data through the socket";
        std::terminate();
    }
}

void UdpServer::stop() {
    // Set the flag and then send the 1-byte payload to wake up the
    // worker thread. The socket descriptor can be closed afterwards.
    stop_.store(1, std::memory_order_relaxed);
    sendByteToLocalhost(endpoint_);
    stop_barrier_.wait();
    logger_.info() << "UDP server stopped";
    close(sockfd_);
}

#ifdef __APPLE__
int UdpServer::recvmsg_(int fd, UdpServer::mmsghdr* hdr, unsigned, int) {
    auto retval = recvmsg(fd, &hdr[0].msg_hdr, MSG_WAITALL);
    if (retval >= 0) {
        hdr[0].msg_len = static_cast<unsigned int>(retval);
    } else {
        return -1;
    }
    return 1;
}
#endif

void UdpServer::worker(std::shared_ptr<DbSession> spout) {
#ifdef __gnu_linux__
        // Name the thread
        auto thread = pthread_self();
        pthread_setname_np(thread, "UDP-worker");
#endif
    start_barrier_.wait();

    int retval;
    sockaddr_in sa{};

    try {
        // Create socket
        sockfd_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (sockfd_ == -1) {
            const char* msg = strerror(errno);
            std::stringstream fmt;
            fmt << "can't create socket: " << msg;
            std::runtime_error err(fmt.str());
            BOOST_THROW_EXCEPTION(err);
        }

        // Set socket options
        int optval = 1;
        if (setsockopt(sockfd_, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval)) == -1) {
            const char* msg = strerror(errno);
            std::stringstream fmt;
            fmt << "can't set socket options: " << msg;
            std::runtime_error err(fmt.str());
            BOOST_THROW_EXCEPTION(err);
        }

        // Bind socket to port
        if (bind(sockfd_, endpoint_.data(), sizeof(sa)) == -1) {
            const char* msg = strerror(errno);
            std::stringstream fmt;
            fmt << "can't bind socket: " << msg;
            std::runtime_error err(fmt.str());
            BOOST_THROW_EXCEPTION(err);
        }

        auto iobuf = std::make_shared<IOBuf>();

        while(true) {

#ifdef __APPLE__
            retval = recvmsg_(sockfd_, iobuf->msgs, NPACKETS, MSG_WAITALL);
#else
            retval = recvmmsg(sockfd_, iobuf->msgs, NPACKETS, MSG_WAITFORONE, nullptr);
#endif
            if (retval == -1) {
                if (errno == EAGAIN || errno == EINTR) {
                    continue;
                }
                const char* msg = strerror(errno);
                std::stringstream fmt;
                fmt << "socket read error: " << msg;
                std::runtime_error err(fmt.str());
                BOOST_THROW_EXCEPTION(err);
            }
            if (stop_.load(std::memory_order_seq_cst)) {
                break;
            }

            iobuf->pps++;

            RESPProtocolParser parser(spout);
            // Protocol parser should be created for each Udp packet
            // group. Otherwise one bad packet can corrupt the state
            // of the parser and it will be unable to process remaining
            // packets and only restart will help.
            // Also, it's not necessary to call parser.start() since
            // it only writes to the log. This call here will polute the
            // log file.
            for (int i = 0; i < retval; i++) {
                // reset buffer to receive new message
                iobuf->bps += iobuf->msgs[i].msg_len;
                auto mlen = iobuf->msgs[i].msg_len;
                iobuf->msgs[i].msg_len = 0;

                auto buf = parser.get_next_buffer();
                memcpy(buf, iobuf->bufs[i], mlen);
                try {
                    parser.parse_next(buf, mlen);
                } catch (StreamError const& err) {
                    // Catch protocol parsing errors here and continue processing data
                    logger_.error() << err.what();
                    break;
                } catch (DatabaseError const& err) {
                    // Late write detected.
                    logger_.error() << err.what();
                    break;
                }
            }
            if (retval != 0) {
                iobuf = std::make_shared<IOBuf>();
            }
            parser.close();
        }
    } catch(...) {
        logger_.error() << boost::current_exception_diagnostic_information();
    }

    stop_barrier_.wait();
}

static Logger s_logger_("udp-server");

struct UdpServerBuilder {

    UdpServerBuilder() {
        ServerFactory::instance().register_type("UDP", *this);
    }

    std::shared_ptr<Server> operator () (std::shared_ptr<DbConnection> con,
                                         std::shared_ptr<ReadOperationBuilder>,
                                         const ServerSettings& settings) {
        if (settings.protocols.size() != 1) {
            s_logger_.error() << "Can't initialize UDP server, more than one protocol specified";
            BOOST_THROW_EXCEPTION(std::runtime_error("invalid upd-server settings"));
        }
        return std::make_shared<UdpServer>(con, settings.nworkers, settings.protocols.front().endpoint);
    }
};

static UdpServerBuilder reg_type;


}

