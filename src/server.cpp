#include "server.h"

#include <filesystem>
#include <format>
#include <iostream>

namespace cpp2ls {

  // StdinReader implementation
  StdinReader::StdinReader(std::istream& stream) : m_stream{&stream} {}

  size_t StdinReader::Read(std::byte* out, size_t count) {
    m_stream->read(reinterpret_cast<char*>(out),
                   static_cast<std::streamsize>(count));
    return static_cast<size_t>(m_stream->gcount());
  }

  // StdoutWriter implementation
  StdoutWriter::StdoutWriter(std::ostream& stream) : m_stream{&stream} {}

  langsvr::Result<langsvr::SuccessType> StdoutWriter::Write(const std::byte* in,
                                                            size_t count) {
    m_stream->write(reinterpret_cast<const char*>(in),
                    static_cast<std::streamsize>(count));
    m_stream->flush();
    if (m_stream->good()) {
      return langsvr::Success;
    }
    return langsvr::Failure{"Failed to write to output stream"};
  }

  // Server implementation
  Server::Server(std::istream& input, std::ostream& output)
      : m_reader{input}, m_writer{output} {
    register_handlers();

    // Set up the sender for the session
    m_session.SetSender([this](std::string_view msg) {
      return langsvr::WriteContent(m_writer, msg);
    });
  }

  void Server::register_handlers() {
    // Register initialize request handler
    m_session.Register([this](const langsvr::lsp::InitializeRequest& req) {
      return handle_initialize(req);
    });

    // Register shutdown request handler
    m_session.Register([this](const langsvr::lsp::ShutdownRequest& req) {
      return handle_shutdown(req);
    });

    // Register initialized notification handler
    m_session.Register(
        [this](const langsvr::lsp::InitializedNotification& notif) {
          return handle_initialized(notif);
        });

    // Register exit notification handler
    m_session.Register([this](const langsvr::lsp::ExitNotification& notif) {
      return handle_exit(notif);
    });

    // Register textDocument/didOpen notification handler
    m_session.Register(
        [this](const langsvr::lsp::TextDocumentDidOpenNotification& notif) {
          return handle_did_open(notif);
        });

    // Register textDocument/didChange notification handler
    m_session.Register(
        [this](const langsvr::lsp::TextDocumentDidChangeNotification& notif) {
          return handle_did_change(notif);
        });

    // Register textDocument/didClose notification handler
    m_session.Register(
        [this](const langsvr::lsp::TextDocumentDidCloseNotification& notif) {
          return handle_did_close(notif);
        });

    // Register textDocument/hover request handler
    m_session.Register(
        [this](const langsvr::lsp::TextDocumentHoverRequest& req) {
          return handle_hover(req);
        });

    // Register textDocument/definition request handler
    m_session.Register(
        [this](const langsvr::lsp::TextDocumentDefinitionRequest& req) {
          return handle_definition(req);
        });

    // Register textDocument/references request handler
    m_session.Register(
        [this](const langsvr::lsp::TextDocumentReferencesRequest& req) {
          return handle_references(req);
        });

    // Register textDocument/completion request handler
    m_session.Register(
        [this](const langsvr::lsp::TextDocumentCompletionRequest& req) {
          return handle_completion(req);
        });
  }

  void Server::run() {
    while (m_running) {
      auto content = langsvr::ReadContent(m_reader);
      if (content != langsvr::Success) {
        // EOF or read error - exit the loop
        break;
      }

      auto result = m_session.Receive(content.Get());
      if (result != langsvr::Success) {
        std::cerr << std::format("Error processing message: {}\n",
                                 result.Failure().reason);
      }
    }
  }

  langsvr::lsp::InitializeResult Server::handle_initialize(
      const langsvr::lsp::InitializeRequest& req) {
    std::cerr << "Received initialize request\n";

    // Extract workspace root from initialization params
    if (auto* root_uri = req.root_uri.Get<std::string>()) {
      m_workspace_root = *root_uri;
      std::cerr << "Workspace root URI: " << m_workspace_root << "\n";

      // Convert URI to path and set up index
      if (m_workspace_root.starts_with("file://")) {
        auto root_path = std::filesystem::path(m_workspace_root.substr(7));
        m_index.set_workspace_root(root_path);

        // Try to load from cache first
        if (!m_index.load_from_cache()) {
          std::cerr << "No valid cache, scanning workspace...\n";
          m_index.scan_and_index();
          m_index.save_to_cache();
        } else {
          // Check for updated files even if cache loaded
          if (m_index.scan_and_index()) {
            m_index.save_to_cache();
          }
        }
      }
    }

    langsvr::lsp::InitializeResult result;

    // Set server info
    langsvr::lsp::ServerInfo server_info;
    server_info.name = "cpp2ls";
    server_info.version = "0.1.0";
    result.server_info = server_info;

    // Set capabilities
    langsvr::lsp::ServerCapabilities& caps = result.capabilities;

    // Text document sync - we want full document sync for now
    caps.text_document_sync = langsvr::lsp::TextDocumentSyncKind::kFull;

    // Enable hover support
    caps.hover_provider = true;

    // Enable go to definition support
    caps.definition_provider = true;

    // Enable find references support
    caps.references_provider = true;

    // Enable completion support
    langsvr::lsp::CompletionOptions completion_opts;
    completion_opts.trigger_characters = {".", ":"};
    caps.completion_provider = completion_opts;

    // TODO: Add more capabilities as we implement them
    // - completion
    // - find references
    // - etc.

    m_initialized = true;
    return result;
  }

  langsvr::lsp::Null Server::handle_shutdown(
      const langsvr::lsp::ShutdownRequest& req) {
    std::cerr << "Received shutdown request\n";
    m_shutdown_requested = true;
    return langsvr::lsp::Null{};
  }

  langsvr::Result<langsvr::SuccessType> Server::handle_initialized(
      const langsvr::lsp::InitializedNotification& notif) {
    std::cerr << "Client initialized\n";
    return langsvr::Success;
  }

  langsvr::Result<langsvr::SuccessType> Server::handle_exit(
      const langsvr::lsp::ExitNotification& notif) {
    std::cerr << "Received exit notification\n";
    m_running = false;
    return langsvr::Success;
  }

  langsvr::Result<langsvr::SuccessType> Server::handle_did_open(
      const langsvr::lsp::TextDocumentDidOpenNotification& notif) {
    const auto& uri = notif.text_document.uri;
    const auto& text = notif.text_document.text;

    std::cerr << std::format("Document opened: {}\n", uri);

    // Create and parse the document
    auto [it, inserted] = m_documents.try_emplace(uri, uri);
    it->second.update(text);

    // Update the global index with symbols from this document
    auto symbols = it->second.get_indexed_symbols();
    m_index.update_file(uri, symbols);

    // Publish diagnostics
    publish_diagnostics(it->second);

    return langsvr::Success;
  }

  langsvr::Result<langsvr::SuccessType> Server::handle_did_change(
      const langsvr::lsp::TextDocumentDidChangeNotification& notif) {
    const auto& uri = notif.text_document.uri;

    std::cerr << std::format("Document changed: {}\n", uri);

    auto it = m_documents.find(uri);
    if (it == m_documents.end()) {
      return langsvr::Failure{std::format("Document not found: {}", uri)};
    }

    // Since we're using Full sync, we get the entire document content
    for (const auto& change : notif.content_changes) {
      // With Full sync, we expect TextDocumentContentChangeWholeDocument
      if (auto* whole_doc
          = change
                .Get<langsvr::lsp::TextDocumentContentChangeWholeDocument>()) {
        it->second.update(whole_doc->text);
      }
    }

    // Update the global index with symbols from this document
    auto symbols = it->second.get_indexed_symbols();
    m_index.update_file(uri, symbols);

    // Publish diagnostics
    publish_diagnostics(it->second);

    return langsvr::Success;
  }

  langsvr::Result<langsvr::SuccessType> Server::handle_did_close(
      const langsvr::lsp::TextDocumentDidCloseNotification& notif) {
    const auto& uri = notif.text_document.uri;

    std::cerr << std::format("Document closed: {}\n", uri);

    // Clear diagnostics for this document
    langsvr::lsp::TextDocumentPublishDiagnosticsNotification clear_diag;
    clear_diag.uri = uri;
    clear_diag.diagnostics = {};  // Empty diagnostics clears them
    m_session.Send(clear_diag);

    m_documents.erase(uri);

    return langsvr::Success;
  }

  langsvr::lsp::TextDocumentHoverRequest::ResultType Server::handle_hover(
      const langsvr::lsp::TextDocumentHoverRequest& req) {
    const auto& uri = req.text_document.uri;
    const auto& pos = req.position;

    std::cerr << std::format("Hover request: {} at ({}, {})\n", uri, pos.line,
                             pos.character);

    // Find the document
    auto it = m_documents.find(uri);
    if (it == m_documents.end()) {
      return langsvr::lsp::Null{};
    }

    // Get hover info from the document
    auto hover_info = it->second.get_hover_info(
        static_cast<int>(pos.line), static_cast<int>(pos.character), &m_index);

    if (!hover_info) {
      return langsvr::lsp::Null{};
    }

    // Build the hover response
    langsvr::lsp::Hover hover;

    // Set content as markdown
    langsvr::lsp::MarkupContent content;
    content.kind = langsvr::lsp::MarkupKind::kMarkdown;
    content.value = hover_info->contents;
    hover.contents = content;

    // Set range
    langsvr::lsp::Range range;
    range.start.line
        = static_cast<langsvr::lsp::Uinteger>(hover_info->start_line);
    range.start.character
        = static_cast<langsvr::lsp::Uinteger>(hover_info->start_col);
    range.end.line = static_cast<langsvr::lsp::Uinteger>(hover_info->end_line);
    range.end.character
        = static_cast<langsvr::lsp::Uinteger>(hover_info->end_col);
    hover.range = range;

    return hover;
  }

  langsvr::lsp::TextDocumentDefinitionRequest::ResultType
  Server::handle_definition(
      const langsvr::lsp::TextDocumentDefinitionRequest& req) {
    const auto& uri = req.text_document.uri;
    const auto& pos = req.position;

    std::cerr << std::format("Definition request: {} at ({}, {})\n", uri,
                             pos.line, pos.character);

    // Find the document
    auto it = m_documents.find(uri);
    if (it == m_documents.end()) {
      return langsvr::lsp::Null{};
    }

    // Get definition location from the document (uses global index)
    auto def_loc = it->second.get_definition_location(
        static_cast<int>(pos.line), static_cast<int>(pos.character), &m_index);

    if (!def_loc) {
      return langsvr::lsp::Null{};
    }

    // Build the location response
    langsvr::lsp::Location location;
    // Use the URI from the location (supports cross-file definitions)
    location.uri = def_loc->uri.empty() ? uri : def_loc->uri;

    langsvr::lsp::Range range;
    range.start.line = static_cast<langsvr::lsp::Uinteger>(def_loc->line);
    range.start.character
        = static_cast<langsvr::lsp::Uinteger>(def_loc->column);
    range.end.line = range.start.line;
    range.end.character = range.start.character + 1;  // Minimal range
    location.range = range;

    // Wrap in Definition type (OneOf<Location, vector<Location>>)
    langsvr::lsp::Definition definition{location};
    return definition;
  }

  langsvr::lsp::TextDocumentReferencesRequest::ResultType
  Server::handle_references(
      const langsvr::lsp::TextDocumentReferencesRequest& req) {
    const auto& uri = req.text_document.uri;
    const auto& pos = req.position;
    bool include_declaration = req.context.include_declaration;

    std::cerr << std::format(
        "References request: {} at ({}, {}), includeDecl={}\n", uri, pos.line,
        pos.character, include_declaration);

    // Find the document
    auto it = m_documents.find(uri);
    if (it == m_documents.end()) {
      return langsvr::lsp::Null{};
    }

    // Get references from the document (uses global index)
    auto refs = it->second.get_references(static_cast<int>(pos.line),
                                          static_cast<int>(pos.character),
                                          include_declaration, &m_index);

    if (refs.empty()) {
      return langsvr::lsp::Null{};
    }

    // Build the locations response
    std::vector<langsvr::lsp::Location> locations;
    locations.reserve(refs.size());

    for (const auto& ref : refs) {
      langsvr::lsp::Location location;
      // Use the URI from the reference (supports cross-file references)
      location.uri = ref.uri.empty() ? uri : ref.uri;

      langsvr::lsp::Range range;
      range.start.line = static_cast<langsvr::lsp::Uinteger>(ref.line);
      range.start.character = static_cast<langsvr::lsp::Uinteger>(ref.column);
      range.end.line = range.start.line;
      range.end.character = range.start.character + 1;  // Minimal range
      location.range = range;

      locations.push_back(std::move(location));
    }

    std::cerr << std::format("Found {} references\n", locations.size());
    return locations;
  }

  void Server::publish_diagnostics(const Cpp2Document& doc) {
    langsvr::lsp::TextDocumentPublishDiagnosticsNotification notification;
    notification.uri = doc.uri();

    // Convert cpp2 diagnostics to LSP diagnostics
    for (const auto& diag_info : doc.diagnostics()) {
      langsvr::lsp::Diagnostic diag;

      // Set range - DiagnosticInfo already uses 0-based positions
      langsvr::lsp::Position start_pos;
      start_pos.line = static_cast<langsvr::lsp::Uinteger>(diag_info.line);
      start_pos.character
          = static_cast<langsvr::lsp::Uinteger>(diag_info.column);

      langsvr::lsp::Position end_pos;
      end_pos.line = start_pos.line;
      // Mark a reasonable range (end of line or some characters)
      end_pos.character = start_pos.character + 1;

      diag.range.start = start_pos;
      diag.range.end = end_pos;

      // Set severity - cppfront errors are all errors (no warnings yet)
      diag.severity = langsvr::lsp::DiagnosticSeverity::kError;

      // Set message
      diag.message = diag_info.message;

      // Set source
      diag.source = "cpp2";

      notification.diagnostics.push_back(std::move(diag));
    }

    std::cerr << std::format("Publishing {} diagnostics for {}\n",
                             notification.diagnostics.size(), doc.uri());

    // Send the notification
    auto result = m_session.Send(notification);
    if (result != langsvr::Success) {
      std::cerr << std::format("Failed to send diagnostics: {}\n",
                               result.Failure().reason);
    }
  }

  langsvr::lsp::TextDocumentCompletionRequest::ResultType
  Server::handle_completion(
      const langsvr::lsp::TextDocumentCompletionRequest& req) {
    const auto& uri = req.text_document.uri;
    const auto& pos = req.position;

    std::cerr << std::format("Completion request: {} at ({}, {})\n", uri,
                             pos.line, pos.character);

    // Find the document
    auto it = m_documents.find(uri);
    if (it == m_documents.end()) {
      return langsvr::lsp::Null{};
    }

    // Get completion items from the document (uses global index)
    auto completions = it->second.get_completions(
        static_cast<int>(pos.line), static_cast<int>(pos.character), &m_index);

    if (completions.empty()) {
      return langsvr::lsp::Null{};
    }

    // Convert to LSP completion items
    std::vector<langsvr::lsp::CompletionItem> items;
    items.reserve(completions.size());

    for (const auto& comp : completions) {
      langsvr::lsp::CompletionItem item;
      item.label = comp.label;
      item.detail = comp.detail;

      if (!comp.insert_text.empty()) {
        item.insert_text = comp.insert_text;
      }

      // Map our CompletionKind to LSP CompletionItemKind
      switch (comp.kind) {
        case CompletionKind::Function:
          item.kind = langsvr::lsp::CompletionItemKind::kFunction;
          break;
        case CompletionKind::Variable:
          item.kind = langsvr::lsp::CompletionItemKind::kVariable;
          break;
        case CompletionKind::Parameter:
          item.kind = langsvr::lsp::CompletionItemKind::kVariable;
          break;
        case CompletionKind::Type:
          item.kind = langsvr::lsp::CompletionItemKind::kClass;
          break;
        case CompletionKind::Namespace:
          item.kind = langsvr::lsp::CompletionItemKind::kModule;
          break;
        case CompletionKind::Keyword:
          item.kind = langsvr::lsp::CompletionItemKind::kKeyword;
          break;
      }

      items.push_back(std::move(item));
    }

    std::cerr << std::format("Returning {} completion items\n", items.size());
    return items;
  }

}  // namespace cpp2ls
