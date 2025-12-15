#include "document.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>

// Include cppfront headers
// Note: These must be included in a specific order due to dependencies
#include "common.h"
#include "io.h"
#include "lex.h"
#include "parse.h"
#include "sema.h"

namespace cpp2ls {

  Cpp2Document::Cpp2Document(std::string uri) : m_uri{std::move(uri)} {
    m_errors = new std::vector<cpp2::error_entry>();
  }

  Cpp2Document::~Cpp2Document() { delete m_errors; }

  Cpp2Document::Cpp2Document(Cpp2Document&& other) noexcept
      : m_uri{std::move(other.m_uri)},
        m_content{std::move(other.m_content)},
        m_errors{other.m_errors},
        m_source{std::move(other.m_source)},
        m_tokens{std::move(other.m_tokens)},
        m_parser{std::move(other.m_parser)},
        m_sema{std::move(other.m_sema)},
        m_valid{other.m_valid} {
    other.m_errors = nullptr;
  }

  Cpp2Document& Cpp2Document::operator=(Cpp2Document&& other) noexcept {
    if (this != &other) {
      delete m_errors;
      m_uri = std::move(other.m_uri);
      m_content = std::move(other.m_content);
      m_errors = other.m_errors;
      m_source = std::move(other.m_source);
      m_tokens = std::move(other.m_tokens);
      m_parser = std::move(other.m_parser);
      m_sema = std::move(other.m_sema);
      m_valid = other.m_valid;
      other.m_errors = nullptr;
    }
    return *this;
  }

  void Cpp2Document::update(const std::string& content) {
    m_content = content;
    m_valid = false;
    m_errors->clear();

    // Reset parsing state
    m_source.reset();
    m_tokens.reset();
    m_parser.reset();
    m_sema.reset();

    // Create a temporary file for the content
    // (cppfront's source class reads from files)
    auto temp_path
        = std::filesystem::temp_directory_path() / "cpp2ls_temp.cpp2";
    {
      std::ofstream temp_file(temp_path);
      if (!temp_file) {
        m_errors->emplace_back(cpp2::source_position{1, 1},
                               "Failed to create temporary file for parsing");
        return;
      }
      temp_file << content;
    }

    // Initialize cppfront components
    m_source = std::make_unique<cpp2::source>(*m_errors);
    if (!m_source->load(temp_path.string())) {
      // Remove temp file
      std::filesystem::remove(temp_path);
      return;
    }

    // Remove temp file now that we've loaded it
    std::filesystem::remove(temp_path);

    // Check if there's any cpp2 code to parse
    if (!m_source->has_cpp2()) {
      // No cpp2 code - this is valid but there's nothing to parse
      m_valid = true;
      return;
    }

    // Lex the source
    m_tokens = std::make_unique<cpp2::tokens>(*m_errors);
    m_tokens->lex(m_source->get_lines());

    // Parse the tokens
    std::set<std::string> includes;
    m_parser = std::make_unique<cpp2::parser>(*m_errors, includes);

    // Parse each section of cpp2 code
    for (const auto& [lineno, section_tokens] : m_tokens->get_map()) {
      if (!m_parser->parse(section_tokens, m_tokens->get_generated())) {
        // Parse error - continue to collect more errors
        continue;
      }
    }

    // Run semantic analysis
    m_sema = std::make_unique<cpp2::sema>(*m_errors);
    m_parser->visit(*m_sema);
    m_sema->apply_local_rules();

    m_valid = m_errors->empty();

    // Cache successful parse results for use during editing
    // Note: We can't copy sema because it has a reference member,
    // so we only cache when the parse is successful
    if (m_valid && m_sema && !m_sema->symbols.empty()) {
      // Move current results to cache
      m_cached_source = std::move(m_source);
      m_cached_tokens = std::move(m_tokens);
      m_cached_parser = std::move(m_parser);
      m_cached_sema = std::move(m_sema);

      // NOTE: After moving to cache, m_sema is nullptr
      // This is intentional - for valid files, we use cached_sema
    }
  }

  auto Cpp2Document::get_hover_info(int line, int col,
                                    const ProjectIndex* index) const
      -> std::optional<HoverInfo> {
    // Use cached sema if current is null
    const cpp2::sema* sema_to_use = m_sema ? m_sema.get() : m_cached_sema.get();
    const cpp2::tokens* tokens_to_use
        = m_tokens ? m_tokens.get() : m_cached_tokens.get();

    if (!sema_to_use || !tokens_to_use) {
      return std::nullopt;
    }

    // Convert from 0-based (LSP) to 1-based (cppfront)
    const auto* token = find_token_at(line + 1, col + 1);
    if (!token) {
      return std::nullopt;
    }

    // Try to get declaration info from cppfront's sema
    const auto* decl_sym = sema_to_use->get_declaration_of(token, true);
    if (decl_sym && decl_sym->declaration) {
      HoverInfo info;
      info.contents = build_hover_content(*decl_sym);

      // Set range from token position (convert back to 0-based)
      auto pos = token->position();
      info.start_line = pos.lineno - 1;
      info.start_col = pos.colno - 1;
      info.end_line = pos.lineno - 1;
      info.end_col = pos.colno - 1 + token->length();

      return info;
    }

    // Fallback: use global index for cross-file and forward reference lookup
    if (index) {
      auto name = token->to_string();
      auto symbols = index->lookup(name);
      if (!symbols.empty()) {
        HoverInfo info;
        info.contents = build_hover_content(*symbols[0]);

        auto pos = token->position();
        info.start_line = pos.lineno - 1;
        info.start_col = pos.colno - 1;
        info.end_line = pos.lineno - 1;
        info.end_col = pos.colno - 1 + token->length();

        return info;
      }
    }

    return std::nullopt;
  }

  auto Cpp2Document::uri() const -> const std::string& { return m_uri; }

  auto Cpp2Document::is_valid() const -> bool { return m_valid; }

  auto Cpp2Document::get_definition_location(int line, int col,
                                             const ProjectIndex* index) const
      -> std::optional<LocationInfo> {
    // Use cached sema if current is null
    const cpp2::sema* sema_to_use = m_sema ? m_sema.get() : m_cached_sema.get();
    const cpp2::tokens* tokens_to_use
        = m_tokens ? m_tokens.get() : m_cached_tokens.get();

    if (!sema_to_use || !tokens_to_use) {
      return std::nullopt;
    }

    // Convert from 0-based (LSP) to 1-based (cppfront)
    const auto* token = find_token_at(line + 1, col + 1);
    if (!token) {
      return std::nullopt;
    }

    // Get declaration from cppfront's sema
    const auto* decl_sym = sema_to_use->get_declaration_of(token, true);
    if (decl_sym && decl_sym->declaration) {
      auto pos = decl_sym->position();

      LocationInfo loc;
      loc.uri = m_uri;  // Same file
      loc.line = pos.lineno - 1;
      loc.column = pos.colno - 1;
      return loc;
    }

    // Fallback: use global index for cross-file lookup
    if (index) {
      auto name = token->to_string();
      auto symbols = index->lookup(name);
      if (!symbols.empty()) {
        LocationInfo loc;
        loc.uri = symbols[0]->file_uri;
        loc.line = symbols[0]->line;
        loc.column = symbols[0]->column;
        return loc;
      }
    }

    return std::nullopt;
  }

  auto Cpp2Document::get_references(int line, int col, bool include_declaration,
                                    const ProjectIndex* index) const
      -> std::vector<LocationInfo> {
    std::vector<LocationInfo> result;

    // Use cached sema if current is null
    const cpp2::sema* sema_to_use = m_sema ? m_sema.get() : m_cached_sema.get();
    const cpp2::tokens* tokens_to_use
        = m_tokens ? m_tokens.get() : m_cached_tokens.get();

    if (!sema_to_use || !tokens_to_use) {
      return result;
    }

    // Convert from 0-based (LSP) to 1-based (cppfront)
    const auto* token = find_token_at(line + 1, col + 1);
    if (!token) {
      return result;
    }

    std::string symbol_name;

    // Get declaration from cppfront's sema
    const auto* target_decl = sema_to_use->get_declaration_of(token, true);

    if (target_decl && target_decl->declaration) {
      // Include declaration if requested
      if (include_declaration) {
        auto pos = target_decl->position();
        LocationInfo loc;
        loc.uri = m_uri;
        loc.line = pos.lineno - 1;
        loc.column = pos.colno - 1;
        result.push_back(loc);
      }

      // Get symbol name for cross-file lookup
      if (target_decl->declaration->has_name()) {
        symbol_name = target_decl->declaration->name()->to_string();
      }

      // Find all references in this file via cppfront's declaration_of map
      for (const auto& [tok, decl_info] : sema_to_use->declaration_of) {
        if (!tok || !decl_info.sym) {
          continue;
        }

        if (decl_info.sym == target_decl) {
          auto pos = tok->position();
          // Skip declaration itself
          if (include_declaration
              && pos.lineno - 1 == target_decl->position().lineno - 1
              && pos.colno - 1 == target_decl->position().colno - 1) {
            continue;
          }

          LocationInfo loc;
          loc.uri = m_uri;
          loc.line = pos.lineno - 1;
          loc.column = pos.colno - 1;
          result.push_back(loc);
        }
      }
    } else {
      // No declaration found via cppfront - use token name for lookup
      symbol_name = token->to_string();

      // Try global index for declaration
      if (index && include_declaration) {
        auto symbols = index->lookup(symbol_name);
        if (!symbols.empty()) {
          LocationInfo loc;
          loc.uri = symbols[0]->file_uri;
          loc.line = symbols[0]->line;
          loc.column = symbols[0]->column;
          result.push_back(loc);
        }
      }
    }

    // TODO: For full cross-file references, we would need to scan all files
    // in the workspace. For now, we only return references in the current file
    // and the declaration from the index.

    return result;
  }

  auto Cpp2Document::get_completions(int line, int col,
                                     const ProjectIndex* index) const
      -> std::vector<CompletionInfo> {
    std::vector<CompletionInfo> result;

    std::set<std::string> seen_names;

    // Convert to 1-based for cppfront
    int target_line = line + 1;

    // Use cached sema if:
    // 1. Current sema is null or empty, OR
    // 2. There are parse errors (m_valid is false) and cached sema has more
    // symbols
    const cpp2::sema* sema_to_use = m_sema.get();
    if (m_cached_sema) {
      bool current_empty = !sema_to_use || sema_to_use->symbols.empty();
      bool cached_has_more = m_cached_sema->symbols.size()
                             > (sema_to_use ? sema_to_use->symbols.size() : 0);
      if (current_empty || (!m_valid && cached_has_more)) {
        sema_to_use = m_cached_sema.get();
      }
    }

    // Track which function contains the cursor
    const cpp2::declaration_node* containing_function = nullptr;

    if (sema_to_use) {
      // Find the innermost function containing the cursor by checking depth
      // We iterate through all function declarations and find the one that:
      // 1. Starts before the cursor
      // 2. Has the highest depth (innermost)
      // 3. The cursor is within its scope

      // First, collect all symbols with their positions to understand scope
      // boundaries
      struct FunctionScope {
        const cpp2::declaration_node* decl;
        int start_line;
        int end_line;  // Line of closing brace (0 if unknown)
        int depth;
      };
      std::vector<FunctionScope> function_scopes;

      for (const auto& sym : sema_to_use->symbols) {
        if (!sym.is_declaration() || !sym.start) {
          continue;
        }

        const auto& decl_sym = sym.as_declaration();
        if (!decl_sym.declaration) {
          continue;
        }

        const auto* decl = decl_sym.declaration;
        if (decl->is_function()) {
          auto pos = decl_sym.position();
          int end_line = 0;

          // Get the closing brace position from the function body
          if (decl->initializer) {
            if (auto* compound
                = decl->initializer->get_if<cpp2::compound_statement_node>()) {
              end_line = compound->close_brace.lineno;
            }
          }

          function_scopes.push_back({decl, pos.lineno, end_line, sym.depth});
        }
      }

      // Find the function that contains our cursor
      // A function contains the cursor if:
      // - The function starts before the cursor line
      // - The cursor is before the function's closing brace
      for (const auto& fs : function_scopes) {
        if (fs.start_line > target_line) {
          continue;  // Function starts after cursor
        }

        // Check if cursor is within this function's scope
        // If we have a valid end_line (close_brace), use it
        if (fs.end_line > 0 && target_line > fs.end_line) {
          continue;  // Cursor is after the closing brace
        }

        // For nested functions, prefer the innermost one (highest depth)
        if (containing_function == nullptr) {
          containing_function = fs.decl;
        } else {
          // Check if this function is nested inside the current one
          // by comparing depths and positions
          if (fs.depth > 0
              && fs.start_line > containing_function->position().lineno) {
            containing_function = fs.decl;
          }
        }
      }

      // Collect visible symbols
      for (const auto& sym : sema_to_use->symbols) {
        if (!sym.is_declaration() || !sym.start) {
          continue;
        }

        const auto& decl_sym = sym.as_declaration();
        if (!decl_sym.declaration || !decl_sym.identifier) {
          continue;
        }

        auto decl_pos = decl_sym.position();
        const auto* decl = decl_sym.declaration;
        auto name = decl_sym.identifier->to_string();

        if (name.empty() || seen_names.contains(name)) {
          continue;
        }

        bool is_visible = false;

        // Global functions, types, namespaces are always visible (cpp2 supports
        // forward references)
        if (decl->is_function() || decl->is_type() || decl->is_namespace()) {
          if (decl->is_global()) {
            is_visible = true;
          }
        } else if (decl->is_object()) {
          // For variables/parameters, they must be declared before cursor
          if (decl_pos.lineno > target_line) {
            continue;
          }
          // Check if they're in scope
          if (containing_function == nullptr) {
            // At global scope - only global variables are visible
            is_visible = decl->is_global();
          } else {
            // Inside a function - check if variable belongs to our function
            auto* var_parent = decl->parent_declaration;
            while (var_parent) {
              if (var_parent == containing_function) {
                is_visible = true;
                break;
              }
              var_parent = var_parent->parent_declaration;
            }
          }
        }

        if (!is_visible) {
          continue;
        }

        seen_names.insert(name);

        CompletionInfo info;
        info.label = name;

        if (decl->is_function()) {
          info.kind = CompletionKind::Function;
          info.detail = decl->signature_to_string();
          info.insert_text = name + "(";
        } else if (decl->is_object()) {
          if (decl_sym.parameter) {
            info.kind = CompletionKind::Parameter;
            info.detail = "(parameter) " + decl->object_type();
          } else {
            info.kind = CompletionKind::Variable;
            info.detail = decl->object_type();
          }
        } else if (decl->is_type()) {
          info.kind = CompletionKind::Type;
          info.detail = "type";
        } else if (decl->is_namespace()) {
          info.kind = CompletionKind::Namespace;
          info.detail = "namespace";
        } else {
          continue;
        }

        result.push_back(std::move(info));
      }
    }

    // Add symbols from global index (cross-file completion)
    if (index) {
      for (const auto* sym : index->all_symbols()) {
        if (!sym || seen_names.contains(sym->name)) {
          continue;
        }
        seen_names.insert(sym->name);

        CompletionInfo info;
        info.label = sym->name;

        switch (sym->kind) {
          case SymbolKind::Function:
            info.kind = CompletionKind::Function;
            info.detail = sym->signature;
            info.insert_text = sym->name + "(";
            break;
          case SymbolKind::Type:
            info.kind = CompletionKind::Type;
            info.detail = "type";
            break;
          case SymbolKind::Namespace:
            info.kind = CompletionKind::Namespace;
            info.detail = "namespace";
            break;
          case SymbolKind::Variable:
            info.kind = CompletionKind::Variable;
            info.detail = "variable";
            break;
          case SymbolKind::Alias:
            info.kind = CompletionKind::Type;
            info.detail = "alias";
            break;
        }

        result.push_back(std::move(info));
      }
    }

    // Add cpp2 keywords
    static const std::vector<std::pair<std::string, std::string>> keywords = {
        {"if", "if () { }"},
        {"else", "else { }"},
        {"while", "while () { }"},
        {"for", "for  do { }"},
        {"do", "do { } while ();"},
        {"return", "return"},
        {"break", "break"},
        {"continue", "continue"},
        {"in", "in"},
        {"out", "out"},
        {"inout", "inout"},
        {"copy", "copy"},
        {"move", "move"},
        {"forward", "forward"},
        {"type", "type"},
        {"namespace", "namespace"},
        {"true", "true"},
        {"false", "false"},
        {"nullptr", "nullptr"},
        {"this", "this"},
        {"that", "that"},
        {"inspect", "inspect"},
        {"is", "is"},
        {"as", "as"},
        {"throws", "throws"},
        {"pre", "pre"},
        {"post", "post"},
        {"assert", "assert"},
        {"public", "public"},
        {"protected", "protected"},
        {"private", "private"},
        {"virtual", "virtual"},
        {"override", "override"},
        {"final", "final"},
        {"implicit", "implicit"},
    };

    for (const auto& [kw, detail] : keywords) {
      if (!seen_names.contains(kw)) {
        CompletionInfo info;
        info.label = kw;
        info.kind = CompletionKind::Keyword;
        info.detail = detail;
        result.push_back(std::move(info));
      }
    }

    return result;
  }

  auto Cpp2Document::diagnostics() const -> std::vector<DiagnosticInfo> {
    std::vector<DiagnosticInfo> result;
    if (!m_errors) {
      return result;
    }
    for (const auto& error : *m_errors) {
      if (error.fallback && m_errors->size() > 1) {
        continue;
      }
      DiagnosticInfo info;
      info.line = std::max(0, error.where.lineno - 1);
      info.column = std::max(0, error.where.colno - 1);
      info.message = error.msg;
      info.is_internal = error.internal;
      result.push_back(std::move(info));
    }
    return result;
  }

  auto Cpp2Document::find_token_at(int line, int col) const
      -> const cpp2::token* {
    // Use cached tokens if current is null
    const cpp2::tokens* tokens_to_use
        = m_tokens ? m_tokens.get() : m_cached_tokens.get();

    if (!tokens_to_use) {
      return nullptr;
    }

    for (const auto& [lineno, section_tokens] : tokens_to_use->get_map()) {
      for (const auto& token : section_tokens) {
        auto pos = token.position();

        if (pos.lineno == line && col >= pos.colno
            && col < pos.colno + token.length()) {
          return &token;
        }
      }
    }

    return nullptr;
  }

  auto Cpp2Document::build_hover_content(const cpp2::declaration_sym& sym) const
      -> std::string {
    const auto* decl = sym.declaration;
    if (!decl) {
      return "";
    }

    std::ostringstream oss;
    oss << "```cpp2\n";

    if (decl->is_function()) {
      oss << decl->signature_to_string();
    } else if (decl->is_object()) {
      if (decl->name()) {
        oss << decl->name()->to_string();
      }
      oss << ": " << decl->object_type();
    } else if (decl->is_type()) {
      if (decl->name()) {
        oss << decl->name()->to_string();
      }
      oss << ": type";
    } else if (decl->is_namespace()) {
      if (decl->name()) {
        oss << decl->name()->to_string();
      }
      oss << ": namespace";
    } else if (decl->is_alias()) {
      if (decl->name()) {
        oss << decl->name()->to_string();
      }
      if (decl->is_type_alias()) {
        oss << ": type ==";
      } else if (decl->is_namespace_alias()) {
        oss << ": namespace ==";
      } else {
        oss << " ==";
      }
    }

    oss << "\n```";

    if (sym.parameter) {
      oss << "\n\n*(parameter)*";
    } else if (sym.member) {
      oss << "\n\n*(member)*";
    } else if (sym.return_param) {
      oss << "\n\n*(return value)*";
    }

    return oss.str();
  }

  auto Cpp2Document::build_hover_content(const IndexedSymbol& sym) const
      -> std::string {
    std::ostringstream oss;
    oss << "```cpp2\n";

    switch (sym.kind) {
      case SymbolKind::Function:
        oss << sym.signature;
        break;
      case SymbolKind::Type:
        oss << sym.name << ": type";
        break;
      case SymbolKind::Namespace:
        oss << sym.name << ": namespace";
        break;
      case SymbolKind::Variable:
        oss << sym.name;
        break;
      case SymbolKind::Alias:
        oss << sym.name << ": ==";
        break;
    }

    oss << "\n```";

    // Add file info if from another file
    if (sym.file_uri != m_uri) {
      // Extract filename from URI
      auto pos = sym.file_uri.rfind('/');
      if (pos != std::string::npos) {
        oss << "\n\n*from " << sym.file_uri.substr(pos + 1) << "*";
      }
    }

    return oss.str();
  }

  auto Cpp2Document::get_indexed_symbols() const -> std::vector<IndexedSymbol> {
    std::vector<IndexedSymbol> result;

    // Use cached parser/tokens if current is null
    const cpp2::parser* parser_to_use
        = m_parser ? m_parser.get() : m_cached_parser.get();
    const cpp2::tokens* tokens_to_use
        = m_tokens ? m_tokens.get() : m_cached_tokens.get();

    if (!parser_to_use || !tokens_to_use) {
      return result;
    }

    for (const auto& [lineno, section_tokens] : tokens_to_use->get_map()) {
      if (section_tokens.empty()) {
        continue;
      }

      auto declarations
          = parser_to_use->get_parse_tree_declarations_in_range(section_tokens);
      for (const auto* decl : declarations) {
        if (!decl || !decl->has_name()) {
          continue;
        }

        if (!decl->is_global()) {
          continue;
        }

        IndexedSymbol sym;
        sym.name = decl->name()->to_string();
        auto pos = decl->position();
        sym.line = pos.lineno - 1;
        sym.column = pos.colno - 1;

        if (decl->is_function()) {
          sym.kind = SymbolKind::Function;
          sym.signature = decl->signature_to_string();
        } else if (decl->is_type()) {
          sym.kind = SymbolKind::Type;
        } else if (decl->is_namespace()) {
          sym.kind = SymbolKind::Namespace;
        } else if (decl->is_object()) {
          sym.kind = SymbolKind::Variable;
        } else if (decl->is_alias()) {
          sym.kind = SymbolKind::Alias;
        } else {
          continue;
        }

        result.push_back(std::move(sym));
      }
    }

    return result;
  }

}  // namespace cpp2ls
