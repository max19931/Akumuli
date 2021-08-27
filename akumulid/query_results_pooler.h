#pragma once
#include "httpserver.h"
#include "storage_api.h"
#include "server.h"
#include <memory>

namespace Akumuli {

//! Output formatter interface
struct OutputFormatter {
    virtual ~OutputFormatter() = default;
    virtual char* format(char* begin, char* end, const aku_Sample& sample) = 0;
};


struct QueryResultsPooler : ReadOperation {

    std::string                      query_text_;
    std::shared_ptr<DbSession>       session_;
    std::shared_ptr<DbCursor>        cursor_;
    std::unique_ptr<OutputFormatter> formatter_;

    std::vector<char>   rdbuf_;      //! Read buffer
    int                 rdbuf_pos_;  //! Read position in buffer
    int                 rdbuf_top_;  //! Last initialized item _index_ in `rdbuf_`
    static const size_t DEFAULT_RDBUF_SIZE_ = 1024;
    static const size_t DEFAULT_ITEM_SIZE_  = sizeof(aku_Sample);
    ApiEndpoint         endpoint_;
    bool                error_produced_;

    QueryResultsPooler(std::shared_ptr<DbSession> session, int readbufsize, ApiEndpoint endpoint);

    void _init_cursor();

    void throw_if_started() const;

    void throw_if_not_started() const;

    virtual void start();

    virtual void append(const char* data, size_t data_size);

    virtual aku_Status get_error();

    virtual const char* get_error_message();

    virtual std::tuple<size_t, bool> read_some(char* buf, size_t buf_size);

    virtual void close();
};

struct QueryProcessor : ReadOperationBuilder {
    std::weak_ptr<DbConnection> con_;
    int                         rdbufsize_;

    QueryProcessor(std::weak_ptr<DbConnection> con, int rdbuf);
    ~QueryProcessor() override;

    virtual ReadOperation* create(ApiEndpoint endpoint) override;

    virtual std::string get_all_stats() override;
    virtual std::string get_resource(std::string name) override;
};

}  // namespace
