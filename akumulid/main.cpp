#include "akumuli.h"
#include "tcp_server.h"
#include "udp_server.h"
#include "httpserver.h"
#include "utility.h"
#include "query_results_pooler.h"
#include "signal_handler.h"
#include "logger.h"

#include <iostream>
#include <fstream>
#include <regex>
#include <thread>

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/ini_parser.hpp>
#include <boost/program_options.hpp>
#include <boost/filesystem.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/exception/diagnostic_information.hpp>
#include <boost/format.hpp>

#include <apr_errno.h>

#include <wordexp.h>
#include <unistd.h>

namespace po=boost::program_options;
using namespace Akumuli;

enum {
    // This is a database size on CI
    AKU_TEST_DB_SIZE = 2*1024*1024,  // 2MB
};

static Logger logger("main");

//! Default configuration for `akumulid`
const char* DEFAULT_CONFIG = R"(# akumulid configuration file (generated automatically).

# path to database files.  Default values is  ~/.akumuli.
path=~/.akumuli

# Number of volumes used  to store data.  Each volume  is
# 4Gb in size by default and allocated beforehand. To change number
# of  volumes  they  should  change  `nvolumes`  value in
# configuration and restart daemon.
nvolumes=%1%

# Size of the individual volume. You can use MB or GB suffix.
# Default value is 4GB (if value is not set).
volume_size=4GB


# HTTP API endpoint configuration

[HTTP]
# port number
port=8181


# TCP ingestion server config (delete to disable)

[TCP]
# port number
port=8282
# worker pool size (0 means that the size of the pool will be chosen automatically)
pool_size=0


# UDP ingestion server config (delete to disable)

[UDP]
# port number
port=8383
# worker pool size
pool_size=1

# OpenTSDB telnet-style data connection enabled (remove this section to disable).

[OpenTSDB]
# port number
port=4242


# Logging configuration
# This is just a log4cxx configuration without any modifications

log4j.rootLogger=all, file
log4j.appender.file=org.apache.log4j.DailyRollingFileAppender
log4j.appender.file.layout=org.apache.log4j.PatternLayout
log4j.appender.file.layout.ConversionPattern=%%d{yyyy-MM-dd HH:mm:ss,SSS} [%%t] %%c [%%p] %%m%%n
log4j.appender.file.filename=/tmp/akumuli.log
log4j.appender.file.datePattern='.'yyyy-MM-dd

)";


const char* WAL_CONFIG = R"(# Write-Ahead-Log section (delete to disable)

[WAL]
# WAL location
path=~/.akumuli

# Max volume size. Log records are added until file size
# will exced configured value.
volume_size=256MB

# Number of log volumes to keep on disk per CPU core. E.g. with `volume_size` = 256MB
# and `nvolumes` = 4 and 4 CPUs WAL will use 4GB at most (4*4*256MB).
nvolumes=4

)";


//! Container class for configuration related functions
struct ConfigFile {
    typedef boost::property_tree::ptree PTree;

    static boost::filesystem::path get_config_path(boost::optional<std::string> config_path) {
        if (config_path) {
            return expand_path(*config_path);
        }
        auto path2cfg = boost::filesystem::path(getenv("HOME"));
        path2cfg /= ".akumulid";
        return path2cfg;
    }

    static void init_config(boost::filesystem::path path, bool disable_wal=false) {
        if (boost::filesystem::exists(path)) {
            std::runtime_error err("configuration file already exists");
            BOOST_THROW_EXCEPTION(err);
        }
        std::ofstream stream(path.c_str());
        int nvolumes = 4;
        std::string config = boost::str(boost::format(DEFAULT_CONFIG) % nvolumes);
        stream << config << std::endl;
        if (!disable_wal) {
            stream << WAL_CONFIG << std::endl;
        }
        stream.close();
    }

    static void init_exp_config(boost::filesystem::path path, bool disable_wal=false) {
        if (boost::filesystem::exists(path)) {
            std::runtime_error err("configuration file already exists");
            BOOST_THROW_EXCEPTION(err);
        }
        std::ofstream stream(path.c_str());
        int nvolumes = 0;
        std::string config = boost::str(boost::format(DEFAULT_CONFIG) % nvolumes);
        stream << config << std::endl;
        if (!disable_wal) {
            stream << WAL_CONFIG << std::endl;
        }
        stream.close();
    }

    static PTree read_config_file(boost::filesystem::path file_path) {

        if (!boost::filesystem::exists(file_path)) {
            std::stringstream fmt;
            fmt << "can't read config file `" << file_path << "`";
            std::runtime_error err(fmt.str());
            BOOST_THROW_EXCEPTION(err);
        }

        PTree conf;
        boost::property_tree::ini_parser::read_ini(file_path.c_str(), conf);
        return conf;
    }

    static boost::filesystem::path expand_path(std::string path) {
        wordexp_t we;
        int err = wordexp(path.c_str(), &we, 0);
        if (err) {
            std::stringstream fmt;
            fmt << "invalid path: `" << path << "`";
            std::runtime_error err(fmt.str());
            BOOST_THROW_EXCEPTION(err);
        }
        if (we.we_wordc != 1) {
            std::stringstream fmt;
            fmt << "expansion error, path: `" << path << "`";
            std::runtime_error err(fmt.str());
            BOOST_THROW_EXCEPTION(err);
        }
        path = std::string(we.we_wordv[0]);
        wordfree(&we);
        auto result = boost::filesystem::path(path);
        return result;
    }

    static boost::filesystem::path get_path(PTree conf) {
        return expand_path(conf.get<std::string>("path"));
    }

    static i32 get_nvolumes(PTree conf) {
        return conf.get<i32>("nvolumes");
    }

    static u64 get_memory_size(std::string strsize) {
        u64 result = 0;
        try {
            result = boost::lexical_cast<u64>(strsize);
        } catch (boost::bad_lexical_cast const&) {
            // Try to read suffix (GB or MB)
            auto throw_decode_error = [strsize]() {
                std::stringstream fmt;
                fmt << "can't decode volume size: `" << strsize << "`";
                std::runtime_error err(fmt.str());
                BOOST_THROW_EXCEPTION(err);
            };
            auto tmp = strsize;
            u64 mul = 1;
            if (tmp.back() != 'B' && tmp.back() != 'b') {
                throw_decode_error();
            }
            tmp.pop_back();
            char symbol = tmp.back();
            tmp.pop_back();
            if (symbol == 'G' || symbol == 'g') {
                mul = 1024*1024*1024;
            } else if (symbol == 'M' || symbol == 'm') {
                mul = 1024*1024;
            } else {
                throw_decode_error();
            }
            try {
                result = boost::lexical_cast<u64>(tmp);
            } catch (boost::bad_lexical_cast const&) {
                throw_decode_error();
            }
            result *= mul;
        }
        return result;

    }

    static u64 get_volume_size(PTree conf) {
        auto strsize = conf.get<std::string>("volume_size", "4GB");
        return get_memory_size(strsize);
    }

    static WALSettings get_wal_settings(PTree conf) {
        WALSettings settings = {};
        if (conf.find("WAL") != conf.not_found()) {
            logger.info() << "WAL is enabled in configuration";
            auto path = expand_path(conf.get<std::string>("WAL.path", ""));
            if (!boost::filesystem::exists(path)) {
                throw std::runtime_error("WAL.path doesn't exist");
            }
            settings.path = path.string();
            settings.nvolumes = conf.get<int>("WAL.nvolumes", 0);
            auto bytes = get_memory_size(conf.get<std::string>("WAL.volume_size", "0"));
            settings.volume_size_bytes = static_cast<int>(bytes);
        } else {
            logger.info() << "WAL is disabled in configuration";
            settings = {};
        }
        return settings;
    }

    static ServerSettings get_http_server(PTree conf) {
        ServerSettings settings;
        settings.name = "HTTP";
        auto ip = conf.get_optional<std::string>("HTTP.bind_addr");
        if (ip) {
            auto addr = boost::asio::ip::address_v4::from_string(*ip);
            boost::asio::ip::tcp::endpoint endpoint(addr, conf.get<unsigned short>("HTTP.port"));
            settings.protocols.push_back({ "HTTP", endpoint});
            settings.nworkers = -1;
        }
        else {
            boost::asio::ip::tcp::endpoint endpoint(boost::asio::ip::tcp::v4(),
                                                    conf.get<unsigned short>("HTTP.port"));
            settings.protocols.push_back({ "HTTP", endpoint });
            settings.nworkers = -1;
        }
        return settings;
    }

    static ServerSettings get_udp_server(PTree conf) {
        ServerSettings settings;
        settings.name = "UDP";
        auto ip = conf.get_optional<std::string>("UDP.bind_addr");
        if (ip) {
            auto addr = boost::asio::ip::address_v4::from_string(*ip);
            boost::asio::ip::tcp::endpoint endpoint(addr, conf.get<unsigned short>("UDP.port"));
            settings.protocols.push_back({ "UDP", endpoint});
            settings.nworkers = conf.get<int>("UDP.pool_size");
        }
        else {
            boost::asio::ip::tcp::endpoint endpoint(boost::asio::ip::tcp::v4(),
                                                    conf.get<unsigned short>("UDP.port"));
            settings.protocols.push_back({ "UDP", endpoint });
            settings.nworkers = conf.get<int>("UDP.pool_size");
        }
        return settings;
    }

    static ServerSettings get_tcp_server(PTree conf) {
        ServerSettings settings;
        settings.name = "TCP";
        auto ip = conf.get_optional<std::string>("TCP.bind_addr");
        if (ip) {
            auto addr = boost::asio::ip::address_v4::from_string(*ip);
            boost::asio::ip::tcp::endpoint endpoint(addr, conf.get<unsigned short>("TCP.port"));
            settings.protocols.push_back({ "RESP", endpoint });
        }
        else {
            boost::asio::ip::tcp::endpoint endpoint(boost::asio::ip::tcp::v4(),
                                                    conf.get<unsigned short>("TCP.port"));
            settings.protocols.push_back({ "RESP", endpoint });
        }

        if (conf.count("OpenTSDB")) {
            auto oip = conf.get_optional<std::string>("OpenTSDB.bind_addr");
            if (oip) {
                auto addr = boost::asio::ip::address_v4::from_string(*oip);
                boost::asio::ip::tcp::endpoint endpoint(addr, conf.get<unsigned short>("OpenTSDB.port"));
                settings.protocols.push_back({ "OpenTSDB", endpoint });
            }
            else {
                boost::asio::ip::tcp::endpoint endpoint(boost::asio::ip::tcp::v4(),
                                                        conf.get<unsigned short>("OpenTSDB.port"));
                settings.protocols.push_back({ "OpenTSDB", endpoint });
            }
        }
        settings.nworkers = conf.get<int>("TCP.pool_size");
        return settings;
    }

    static std::vector<ServerSettings> get_server_settings(PTree conf) {
        std::map<std::string, std::function<ServerSettings(PTree)>> mapping = {
            { "TCP", &get_tcp_server },
            { "UDP", &get_udp_server },
            { "HTTP", &get_http_server },
        };
        std::vector<ServerSettings> result;
        for (auto kv: mapping) {
            if (conf.count(kv.first)) {
                result.push_back(kv.second(conf));
            }
        }
        return result;
    }
};


/** Help message used in CLI. It contains simple markdown formatting.
  * `rich_print function should be used to print this message.
  */
static const char* CLI_HELP_MESSAGE = R"(`akumulid` - time-series database daemon

**SYNOPSIS**
        akumulid

        akumulid --help

        akumulid --init

        akumulid --init-expandable

        akumulid --create

        akumuild --delete

**DESCRIPTION**
        **akumulid** is a time-series database daemon.
        All configuration can be done via `~/.akumulid` configuration
        file.

**OPTIONS**
        **help**
            produce help message and exit

        **init**
            create  configuration  file at `~/.akumulid`  filled with
            default values and exit

        **init-expandable**
            create  configuration  file at `~/.akumulid`  filled with
            default values and exit (sets nvolumes to 0)

        **create**
            generate database files in `~/.akumuli` folder, use with
            --allocate flag to actually allocate disk space

        **delete**
            delete database files in `~/.akumuli` folder

        **(empty)**
            run server

)";


//! Format text for console. `plain_text` flag removes formatting.
std::string cli_format(std::string dest) {

    bool plain_text = !isatty(STDOUT_FILENO);

    const char* BOLD = "\033[1m";
    const char* EMPH = "\033[3m";
    const char* UNDR = "\033[4m";
    const char* NORM = "\033[0m";

    auto format = [&](std::string& line, const char* pattern, const char* open) {
        size_t pos = 0;
        int token_num = 0;
        while(pos != std::string::npos) {
            pos = line.find(pattern, pos);
            if (pos != std::string::npos) {
                // match
                auto code = (token_num % 2) ? NORM : open;
                line.replace(pos, strlen(pattern), code);
                token_num++;
            }
        }
    };

    if (!plain_text) {
        format(dest, "**", BOLD);
        format(dest, "__", EMPH);
        format(dest, "`",  UNDR);
    } else {
        format(dest, "**", "");
        format(dest, "__", "");
        format(dest, "`",  "");
    }

    return dest;
}

//! Convert markdown subset to console escape codes and print
void rich_print(const char* msg) {

    std::stringstream stream(const_cast<char*>(msg));
    std::string dest;

    while(std::getline(stream, dest)) {
        std::cout << cli_format(dest) << std::endl;
    }
}

/** Logger f-n that shuld be used in libakumuli */
static void static_logger(aku_LogLevel tag, const char * msg) {
    static Logger logger = Logger("Main");
    switch(tag) {
    case AKU_LOG_ERROR:
        logger.error() << msg;
        break;
    case AKU_LOG_INFO:
        logger.info() << msg;
        break;
    case AKU_LOG_TRACE:
        logger.trace() << msg;
        break;
    }
}


/** Create database if database not exists.
  */
void create_db_files(const char* path,
                     i32 nvolumes,
                     u64 volume_size,
                     bool allocate)
{
    auto full_path = boost::filesystem::path(path) / "db.akumuli";
    if (!boost::filesystem::exists(full_path)) {
        apr_status_t status = APR_SUCCESS;
        status = aku_create_database_ex("db", path, path, nvolumes, volume_size, allocate);
        if (status != APR_SUCCESS) {
            char buffer[1024];
            apr_strerror(status, buffer, 1024);
            std::stringstream fmt;
            fmt << "can't create database: " << buffer;
            std::runtime_error err(fmt.str());
            BOOST_THROW_EXCEPTION(err);
        } else {
            std::stringstream fmt;
            fmt << "**OK** database created, path: `" << path << "`";
            std::cout << cli_format(fmt.str()) << std::endl;
        }
    } else {
        std::stringstream fmt;
        fmt << "**ERROR** database file already exists";
        std::cout << cli_format(fmt.str()) << std::endl;
    }
}

/** Read configuration file and run server.
  * If config file can't be found - report error.
  */
void cmd_run_server(boost::optional<std::string> cmd_config_path) {

    auto config_path            = ConfigFile::get_config_path(cmd_config_path);
    auto config                 = ConfigFile::read_config_file(config_path);
    auto path                   = ConfigFile::get_path(config);
    auto ingestion_servers      = ConfigFile::get_server_settings(config);
    auto wal_config             = ConfigFile::get_wal_settings(config);
    auto full_path              = boost::filesystem::path(path) / "db.akumuli";

    if (!boost::filesystem::exists(full_path)) {
        std::stringstream fmt;
        fmt << "**ERROR** database file doesn't exists at " << path;
        std::cout << cli_format(fmt.str()) << std::endl;
    } else {
        aku_FineTuneParams params = {};
        if (!wal_config.path.empty() && wal_config.nvolumes != 0 && wal_config.volume_size_bytes != 0) {
            unsigned log_ccr = 0;
            for (auto settings: ingestion_servers) {
                unsigned ccr = settings.nworkers < 0 ? std::thread::hardware_concurrency()
                                                     : static_cast<unsigned>(settings.nworkers);
                log_ccr = std::max(log_ccr, ccr);
            }
            bool use_wal = true;
            if (wal_config.nvolumes < 0 || wal_config.nvolumes > 1000 || wal_config.nvolumes == 1) {
                std::stringstream fmt;
                fmt << "**ERROR** invalid configuration value WAL.nvolumes = " << wal_config.nvolumes
                    << ", value should not exceed 1000 or be equal to 1";
                std::cout << cli_format(fmt.str()) << std::endl;
                use_wal = false;
            }
            if (wal_config.volume_size_bytes < 1048576 /*1MB*/ ||
                wal_config.volume_size_bytes > 1073741824 /*1GB*/) {
                std::stringstream fmt;
                fmt << "**ERROR** invalid configuration value WAL.volume_size = " << wal_config.volume_size_bytes
                    << ", size should be in 1MB-1GB range";
                std::cout << cli_format(fmt.str()) << std::endl;
                use_wal = false;
            }
            if (!boost::filesystem::exists(wal_config.path)) {
                std::stringstream fmt;
                fmt << "**ERROR** invalid configuration value WAL.path = " << wal_config.path
                    << ", directory doesn't exist";
                std::cout << cli_format(fmt.str()) << std::endl;
                use_wal = false;
            }
            if (use_wal) {
                params.input_log_concurrency = log_ccr;
                params.input_log_path        = wal_config.path.data();
                params.input_log_volume_numb = static_cast<u64>(wal_config.nvolumes);
                params.input_log_volume_size = static_cast<u64>(wal_config.volume_size_bytes);
            }
        }

        auto connection  = std::make_shared<AkumuliConnection>(full_path.c_str(), params);
        auto qproc       = std::make_shared<QueryProcessor>(connection, 2048);

        SignalHandler sighandler;
        int srvid = 0;
        std::map<int, std::string> srvnames;
        for(auto settings: ingestion_servers) {
            auto srv = ServerFactory::instance().create(connection, qproc, settings);
            assert(srv != nullptr);
            srvnames[srvid] = settings.name;
            srv->start(&sighandler, srvid);
            logger.info() << "Starting " << settings.name << " index " << srvid;
            if (settings.protocols.size() == 1) {
                std::cout << cli_format("**OK** ") << settings.name
                          << " server started, endpoint: " << settings.protocols[0].endpoint << std::endl;
            } else {
                std::cout << cli_format("**OK** ") << settings.name
                          << " server started";
                for (const auto& protocol: settings.protocols) {
                    std::cout << ", " << protocol.name << " endpoint: " << protocol.endpoint;
                    logger.info() << "Protocol: " << protocol.name << " endpoint: " << protocol.endpoint;
                }
                std::cout << std::endl;
            }
            srvid++;
        }
        auto srvids = sighandler.wait();

        for(int id: srvids) {
            std::cout << cli_format("**OK** ") << srvnames[id] << " server stopped" << std::endl;
        }
    }
}

/** Create database command.
  */
void cmd_create_database(boost::optional<std::string> cmd_config_path, bool test_db=false, bool allocate=false) {
    auto config_path = ConfigFile::get_config_path(cmd_config_path);
    auto config      = ConfigFile::read_config_file(config_path);
    auto path        = ConfigFile::get_path(config);
    auto volumes     = ConfigFile::get_nvolumes(config);
    auto volsize     = ConfigFile::get_volume_size(config);

    if (test_db) {
        volsize = AKU_TEST_DB_SIZE;
    }

    create_db_files(path.c_str(), volumes, volsize, allocate);
}

void cmd_delete_database(boost::optional<std::string> cmd_config_path) {
    auto config_path = ConfigFile::get_config_path(cmd_config_path);
    auto config      = ConfigFile::read_config_file(config_path);
    auto path        = ConfigFile::get_path(config);
    auto wal_path    = ConfigFile::get_wal_settings(config).path;

    auto full_path = boost::filesystem::path(path) / "db.akumuli";
    if (boost::filesystem::exists(full_path)) {
        // TODO: don't delete database if it's not empty
        // FIXME: add command line argument --force to delete nonempty database
        auto status = aku_remove_database(full_path.c_str(), wal_path.c_str(), true);
        if (status != APR_SUCCESS) {
            char buffer[1024];
            apr_strerror(status, buffer, 1024);
            std::stringstream fmt;
            fmt << "can't delete database: " << buffer;
            std::runtime_error err(fmt.str());
            BOOST_THROW_EXCEPTION(err);
        } else {
            std::stringstream fmt;
            fmt << "**OK** database at `" << path << "` deleted";
            std::cout << cli_format(fmt.str()) << std::endl;
        }
    } else {
        std::stringstream fmt;
        fmt << "**ERROR** database file doesn't exists";
        std::cout << cli_format(fmt.str()) << std::endl;
    }
}

void cmd_dump_debug_information(boost::optional<std::string> cmd_config_path, const char* outfname) {
    auto config_path = ConfigFile::get_config_path(cmd_config_path);
    auto config      = ConfigFile::read_config_file(config_path);
    auto path        = ConfigFile::get_path(config);

    auto full_path = boost::filesystem::path(path) / "db.akumuli";
    if (boost::filesystem::exists(full_path)) {
        auto status = aku_debug_report_dump(full_path.c_str(), outfname);
        if (status != APR_SUCCESS) {
            char buffer[1024];
            apr_strerror(status, buffer, 1024);
            std::stringstream fmt;
            fmt << "can't dump debug info: " << buffer;
            std::runtime_error err(fmt.str());
            BOOST_THROW_EXCEPTION(err);
        } else {
            if (outfname) {
                // Don't generate this message if output was written to stdout. User
                // should be able to use this command this way:
                // ./akumulid --debug-dump=stdout >> outfile.xml
                std::stringstream fmt;
                fmt << "**OK** `" << outfname << "` successfully generated for `" << path << "`";
                std::cout << cli_format(fmt.str()) << std::endl;
            }
        }
    } else {
        std::stringstream fmt;
        fmt << "**ERROR** database file doesn't exists";
        std::cout << cli_format(fmt.str()) << std::endl;
    }
}

void cmd_dump_recovery_debug_information(boost::optional<std::string> cmd_config_path, const char* outfname) {
    auto config_path = ConfigFile::get_config_path(cmd_config_path);
    auto config      = ConfigFile::read_config_file(config_path);
    auto path        = ConfigFile::get_path(config);

    auto full_path = boost::filesystem::path(path) / "db.akumuli";
    if (boost::filesystem::exists(full_path)) {
        auto status = aku_debug_recovery_report_dump(full_path.c_str(), outfname);
        if (status != APR_SUCCESS) {
            char buffer[1024];
            apr_strerror(status, buffer, 1024);
            std::stringstream fmt;
            fmt << "can't dump debug info: " << buffer;
            std::runtime_error err(fmt.str());
            BOOST_THROW_EXCEPTION(err);
        } else {
            if (outfname) {
                // Don't generate this message if output was written to stdout. User
                // should be able to use this command this way:
                // ./akumulid --debug-recovery-dump=stdout >> outfile.xml
                std::stringstream fmt;
                fmt << "**OK** `" << outfname << "` successfully generated for `" << path << "`";
                std::cout << cli_format(fmt.str()) << std::endl;
            }
        }
    } else {
        std::stringstream fmt;
        fmt << "**ERROR** database file doesn't exists";
        std::cout << cli_format(fmt.str()) << std::endl;
    }
}


/** Panic handler for libakumuli.
  * Shouldn't be called directly, writes error message and
  * writes coredump (this depends on system configuration)
  */
void panic_handler(const char * msg) {
    // write error message
    static_logger(AKU_LOG_ERROR, msg);
    static_logger(AKU_LOG_ERROR, "Terminating (core dumped)");
    // this should generate SIGABORT and triger coredump
    abort();
}


int main(int argc, char** argv) {
    try {
        std::locale::global(std::locale("C"));

        po::options_description cli_only_options;
        cli_only_options.add_options()
                ("help", "Produce help message")
                ("config", po::value<std::string>(), "Path to configuration file")
                ("create", "Create database")
                ("allocate", "Preallocate disk space")
                ("delete", "Delete database")
                ("CI", "Create database for CI environment (for testing)")
                ("init", "Create default configuration")
                ("init-expandable", "Create configuration for expandable storage")
                ("disable-wal", "Disable WAL in generated configuration file (can be used with --init)")
                ("debug-dump", po::value<std::string>(), "Create debug dump")
                ("debug-recovery-dump", po::value<std::string>(), "Create debug dump of the system after crash recovery")
                ("version", "Print software version")
                ;

        po::variables_map vm;
        po::store(po::parse_command_line(argc, argv, cli_only_options), vm);
        po::notify(vm);

        if (vm.count("help")) {
            rich_print(CLI_HELP_MESSAGE);
            exit(EXIT_SUCCESS);
        }

        boost::optional<std::string> cmd_config_path;
        if (vm.count("config")) {
            cmd_config_path = vm["config"].as<std::string>();
        }

        aku_initialize(&panic_handler, &static_logger);

        // Init logger
        auto path = ConfigFile::get_config_path(cmd_config_path);
        if (boost::filesystem::exists(path)) {
            Logger::init(path.c_str());
        }

        std::stringstream header;
#ifndef AKU_VERSION
        header << "\n\nStarted\n\n";
#else
        header << "\n\nStarted v" << AKU_VERSION << "\n\n";
#endif
        header << "Command line: ";
        for (int i = 0; i < argc; i++) {
            header << argv[i] << ' ';
        }
        header << "\n\n";
        logger.info() << header.str();

        if (vm.count("init")) {
            bool disable_wal = vm.count("disable-wal");
            ConfigFile::init_config(path, disable_wal);

            std::stringstream fmt;
            fmt << "**OK** configuration file created at: `" << path << "`";
            std::cout << cli_format(fmt.str()) << std::endl;
            exit(EXIT_SUCCESS);
        }

        if (vm.count("init-expandable")) {
            bool disable_wal = vm.count("disable-wal");
            ConfigFile::init_exp_config(path, disable_wal);

            std::stringstream fmt;
            fmt << "**OK** configuration file created at: `" << path << "`";
            std::cout << cli_format(fmt.str()) << std::endl;
            exit(EXIT_SUCCESS);
        }

        if (vm.count("create")) {
            bool allocate = false;
            if(vm.count("allocate"))
                allocate = true;
            cmd_create_database(cmd_config_path, false, allocate);
            exit(EXIT_SUCCESS);
        }

        if (vm.count("CI")) {
            cmd_create_database(cmd_config_path, true);
            exit(EXIT_SUCCESS);
        }

        if (vm.count("delete")) {
            cmd_delete_database(cmd_config_path);
            exit(EXIT_SUCCESS);
        }

        if (vm.count("debug-dump")) {
            auto path = vm["debug-dump"].as<std::string>();
            if (path == "stdout") {
                cmd_dump_debug_information(cmd_config_path, nullptr);
            } else {
                cmd_dump_debug_information(cmd_config_path, path.c_str());
            }
            exit(EXIT_SUCCESS);
        }

        if (vm.count("debug-recovery-dump")) {
            auto path = vm["debug-recovery-dump"].as<std::string>();
            if (path == "stdout") {
                cmd_dump_recovery_debug_information(cmd_config_path, nullptr);
            } else {
                cmd_dump_recovery_debug_information(cmd_config_path, path.c_str());
            }
            exit(EXIT_SUCCESS);
        }

        if (vm.count("version")) {
            std::cout << AKU_VERSION << std::endl;
            exit(EXIT_SUCCESS);
        }

        cmd_run_server(cmd_config_path);

        logger.info() << "\n\nClean exit\n\n";

    } catch(const std::exception& e) {
        std::stringstream fmt;
        fmt << "**FAILURE** " << e.what();
        std::cerr << cli_format(fmt.str()) << std::endl;
        exit(EXIT_FAILURE);
    } catch(...) {
        std::stringstream fmt;
        fmt << "**FAILURE** " << boost::current_exception_diagnostic_information();
        std::cerr << cli_format(fmt.str()) << std::endl;
        exit(EXIT_FAILURE);
    }

    exit(EXIT_SUCCESS);
}

