#ifndef CPP2LS_SERVER_H
#define CPP2LS_SERVER_H

#include <iostream>
#include <string>
#include <unordered_map>

#include "document.h"
#include "langsvr/content_stream.h"
#include "langsvr/lsp/lsp.h"
#include "langsvr/reader.h"
#include "langsvr/session.h"
#include "langsvr/writer.h"

namespace cpp2ls {

  /// A Reader implementation that reads from std::istream
  class StdinReader : public langsvr::Reader {
  public:
    explicit StdinReader(std::istream& stream);
    size_t Read(std::byte* out, size_t count) override;

  private:
    std::istream* m_stream;
  };

  /// A Writer implementation that writes to std::ostream
  class StdoutWriter : public langsvr::Writer {
  public:
    explicit StdoutWriter(std::ostream& stream);
    langsvr::Result<langsvr::SuccessType> Write(const std::byte* in,
                                                size_t count) override;

  private:
    std::ostream* m_stream;
  };

  /// The cpp2ls Language Server
  class Server {
  public:
    Server(std::istream& input, std::ostream& output);

    /// Run the server main loop
    void run();

  private:
    /// Register all LSP request/notification handlers
    void register_handlers();

    /// Handler for initialize request
    langsvr::lsp::InitializeResult handle_initialize(
        const langsvr::lsp::InitializeRequest& req);

    /// Handler for shutdown request
    langsvr::lsp::Null handle_shutdown(
        const langsvr::lsp::ShutdownRequest& req);

    /// Handler for initialized notification
    langsvr::Result<langsvr::SuccessType> handle_initialized(
        const langsvr::lsp::InitializedNotification& notif);

    /// Handler for exit notification
    langsvr::Result<langsvr::SuccessType> handle_exit(
        const langsvr::lsp::ExitNotification& notif);

    /// Handler for textDocument/didOpen notification
    langsvr::Result<langsvr::SuccessType> handle_did_open(
        const langsvr::lsp::TextDocumentDidOpenNotification& notif);

    /// Handler for textDocument/didChange notification
    langsvr::Result<langsvr::SuccessType> handle_did_change(
        const langsvr::lsp::TextDocumentDidChangeNotification& notif);

    /// Handler for textDocument/didClose notification
    langsvr::Result<langsvr::SuccessType> handle_did_close(
        const langsvr::lsp::TextDocumentDidCloseNotification& notif);

    /// Handler for textDocument/hover request
    langsvr::lsp::TextDocumentHoverRequest::ResultType handle_hover(
        const langsvr::lsp::TextDocumentHoverRequest& req);

    /// Handler for textDocument/definition request
    langsvr::lsp::TextDocumentDefinitionRequest::ResultType handle_definition(
        const langsvr::lsp::TextDocumentDefinitionRequest& req);

    /// Publish diagnostics for a document
    void publish_diagnostics(const Cpp2Document& doc);

  private:
    StdinReader m_reader;
    StdoutWriter m_writer;
    langsvr::Session m_session;

    bool m_initialized{false};
    bool m_shutdown_requested{false};
    bool m_running{true};

    /// Map of open documents by URI
    std::unordered_map<std::string, Cpp2Document> m_documents;
  };

}  // namespace cpp2ls

#endif  // !CPP2LS_SERVER_H
