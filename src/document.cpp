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

    // Wrap all parsing in try-catch to handle cppfront exceptions
    // (e.g., "unexpected end of source file")
    try {
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
    } catch (const std::exception& e) {
      // Cppfront threw an exception (e.g., unexpected EOF)
      // Add it as an error and keep cached results if available
      m_errors->emplace_back(cpp2::source_position{1, 1},
                             std::string("Parser exception: ") + e.what());
      m_valid = false;
      // Don't clear cached results - they're still useful
      return;
    }

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
    int target_col = col + 1;

    // Check if we're completing a member access (obj. or obj:)
    // Look backwards in the current line for '.' or ':'
    std::string line_text;
    if (!m_content.empty()) {
      // Extract current line (0-based)
      size_t line_start = 0;
      for (int i = 0; i < line; ++i) {
        line_start = m_content.find('\n', line_start);
        if (line_start == std::string::npos) break;
        ++line_start;
      }
      if (line_start != std::string::npos) {
        size_t line_end = m_content.find('\n', line_start);
        if (line_end == std::string::npos) {
          line_end = m_content.length();
        }
        line_text = m_content.substr(line_start, line_end - line_start);
      }
    }

    // Check for member access pattern
    std::string object_name;
    bool is_member_completion = false;
    bool is_member_only = false;  // true for '..' operator (no UFCS)
    if (col > 0 && col <= static_cast<int>(line_text.length())) {
      std::string prefix = line_text.substr(0, col);

      // Look for '.' or ':' preceded by identifier
      size_t accessor_pos = std::string::npos;
      // Search backwards from cursor to find the most recent '.' or ':'
      for (int i = static_cast<int>(prefix.length()) - 1; i >= 0; --i) {
        if (prefix[i] == '.' || prefix[i] == ':') {
          accessor_pos = i;
          break;
        }
      }

      if (accessor_pos != std::string::npos && accessor_pos > 0) {
        // Check if it's '..' (member-only) by looking at the character before
        if (accessor_pos > 0 && prefix[accessor_pos] == '.' && accessor_pos >= 1
            && prefix[accessor_pos - 1] == '.') {
          is_member_only = true;
        }
        // Extract identifier before accessor (skip if '..')
        int id_end = is_member_only ? accessor_pos - 2 : accessor_pos - 1;
        while (id_end >= 0
               && std::isspace(static_cast<unsigned char>(prefix[id_end]))) {
          --id_end;
        }

        if (id_end >= 0) {
          int id_start = id_end;
          while (
              id_start > 0
              && (std::isalnum(static_cast<unsigned char>(prefix[id_start - 1]))
                  || prefix[id_start - 1] == '_')) {
            --id_start;
          }

          if (id_start <= id_end) {
            object_name = prefix.substr(id_start, id_end - id_start + 1);
            is_member_completion = true;
          }
        }
      }
    }

    // If member completion, find the object's token and get its members
    if (is_member_completion && !object_name.empty()) {
      {  // Separate scope to avoid variable conflicts
        // Use cached sema for member lookup
        const cpp2::sema* sema_to_use = m_sema.get();
        const cpp2::tokens* tokens_to_use = m_tokens.get();

        if (m_cached_sema) {
          bool current_empty = !sema_to_use || sema_to_use->symbols.empty();
          bool cached_has_more
              = m_cached_sema->symbols.size()
                > (sema_to_use ? sema_to_use->symbols.size() : 0);
          if (current_empty || (!m_valid && cached_has_more)) {
            sema_to_use = m_cached_sema.get();
            tokens_to_use = m_cached_tokens.get();  // Use matching tokens!
          }
        }

        if (sema_to_use && tokens_to_use) {
          // Find the token for the object identifier in the matching tokens
          const cpp2::token* obj_token = nullptr;

          // Search backwards from the cursor position
          for (const auto& [lineno, section_tokens] :
               tokens_to_use->get_map()) {
            if (lineno > target_line) {
              continue;
            }

            for (const auto& token : section_tokens) {
              auto pos = token.position();

              // Skip tokens at or after cursor position
              if (lineno == target_line && pos.colno >= target_col) {
                continue;
              }

              // Check if this is an identifier token with matching name
              if (token.type() == cpp2::lexeme::Identifier
                  && token.to_string() == object_name) {
                // Update best match if this is closer to cursor
                if (!obj_token || pos.lineno > obj_token->position().lineno
                    || (pos.lineno == obj_token->position().lineno
                        && pos.colno > obj_token->position().colno)) {
                  obj_token = &token;
                }
              }
            }
          }

          std::cerr << std::format("  Looking for token '{}'\n", object_name);
          if (obj_token) {
            // Get declaration for this token
            auto* decl_sym = sema_to_use->get_declaration_of(obj_token);
            if (decl_sym && decl_sym->declaration) {
              if (decl_sym->declaration->is_object()) {
                auto* obj_decl = decl_sym->declaration;
                // Get type of the object - use safe object_type() method
                std::string type_name = obj_decl->object_type();

                if (!type_name.empty()
                    && type_name.find("(*ERROR*)") == std::string::npos) {
                  // Find the type declaration
                  for (const auto& sym : sema_to_use->symbols) {
                    if (!sym.is_declaration() || !sym.start) continue;

                    const auto& type_sym = sym.as_declaration();
                    if (!type_sym.declaration || !type_sym.identifier) continue;

                    const auto* type_decl = type_sym.declaration;
                    if (type_decl->is_type()
                        && type_sym.identifier->to_string() == type_name) {
                      // Found the type, now get its members
                      for (const auto& member_sym : sema_to_use->symbols) {
                        if (!member_sym.is_declaration() || !member_sym.start)
                          continue;

                        const auto& mem_decl_sym = member_sym.as_declaration();
                        if (!mem_decl_sym.declaration
                            || !mem_decl_sym.identifier)
                          continue;

                        const auto* mem_decl = mem_decl_sym.declaration;
                        // Check if this declaration is a member of our type
                        if (mem_decl->parent_declaration == type_decl) {
                          auto member_name
                              = mem_decl_sym.identifier->to_string();
                          if (!member_name.empty()
                              && !seen_names.contains(member_name)) {
                            seen_names.insert(member_name);

                            CompletionInfo info;
                            info.label = member_name;

                            if (mem_decl->is_function()) {
                              info.kind = CompletionKind::Function;
                              info.detail = mem_decl->signature_to_string();
                              info.insert_text = member_name + "(";
                            } else if (mem_decl->is_object()) {
                              info.kind = CompletionKind::Variable;
                              info.detail = mem_decl->object_type();
                            }

                            result.push_back(std::move(info));
                          }
                        }
                      }

                      // Add UFCS support: find global functions whose first
                      // parameter type matches (but only for single '.' not
                      // '..')
                      if (!is_member_only) {
                        // type_name already defined above
                        for (const auto& func_sym : sema_to_use->symbols) {
                          if (!func_sym.is_declaration() || !func_sym.start)
                            continue;

                          const auto& func_decl_sym = func_sym.as_declaration();
                          if (!func_decl_sym.declaration
                              || !func_decl_sym.identifier)
                            continue;

                          const auto* func_decl = func_decl_sym.declaration;
                          // Only consider global functions (not members)
                          if (func_decl->is_function()
                              && !func_decl->parent_declaration) {
                            // Safely access function parameters
                            auto& func_type_variant = func_decl->type;
                            if (func_type_variant.index()
                                == cpp2::declaration_node::a_function) {
                              auto* func_type
                                  = std::get<
                                        cpp2::declaration_node::a_function>(
                                        func_type_variant)
                                        .get();
                              if (func_type && func_type->parameters
                                  && func_type->parameters->ssize() > 0) {
                                auto* first_param = (*func_type->parameters)[0];
                                if (first_param && first_param->declaration
                                    && first_param->declaration->is_object()) {
                                  // Use safe object_type() method
                                  std::string first_param_type
                                      = first_param->declaration->object_type();

                                  if (!first_param_type.empty()
                                      && first_param_type.find("(*ERROR*)")
                                             == std::string::npos
                                      && first_param_type == type_name) {
                                    auto func_name
                                        = func_decl_sym.identifier->to_string();
                                    if (!func_name.empty()
                                        && !seen_names.contains(func_name)) {
                                      seen_names.insert(func_name);

                                      CompletionInfo info;
                                      info.label = func_name;
                                      info.kind = CompletionKind::Function;
                                      info.detail
                                          = func_decl->signature_to_string();
                                      info.insert_text = func_name + "(";

                                      result.push_back(std::move(info));
                                    }
                                  }
                                }
                              }
                            }
                          }
                        }
                      }

                      break;  // Found the type, done
                    }
                  }
                }
              }
            }
          }
        }
      }

      // Return only member completions
      return result;
    }

    // Regular completion (non-member)

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

        // Global functions, types, namespaces are always visible (cpp2
        // supports forward references)
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

  auto Cpp2Document::get_signature_help(int line, int col,
                                        const ProjectIndex* index) const
      -> std::optional<SignatureHelpInfo> {
    // Use cached sema if current is null or has errors
    const cpp2::sema* sema_to_use = m_sema.get();
    if (m_cached_sema) {
      bool current_empty = !sema_to_use || sema_to_use->symbols.empty();
      bool cached_has_more = m_cached_sema->symbols.size()
                             > (sema_to_use ? sema_to_use->symbols.size() : 0);
      if (current_empty || (!m_valid && cached_has_more)) {
        sema_to_use = m_cached_sema.get();
      }
    }

    if (!sema_to_use) {
      return std::nullopt;
    }

    // Convert to 1-based for cppfront
    int target_line = line + 1;
    int target_col = col + 1;

    // Strategy: Look backwards from cursor to find function call
    // We're looking for a pattern like: function_name( ... cursor is here
    // We need to:
    // 1. Find the opening '(' before cursor
    // 2. Find the function name before '('
    // 3. Count commas to determine active parameter

    // Scan backwards through tokens to find function call context
    const cpp2::tokens* tokens_to_use
        = m_tokens ? m_tokens.get() : m_cached_tokens.get();
    if (!tokens_to_use) {
      return std::nullopt;
    }

    // Find tokens before cursor
    const cpp2::token* function_name_token = nullptr;
    int paren_depth = 0;
    int active_param = 0;
    bool found_open_paren = false;

    // Scan tokens up to cursor position
    for (const auto& [lineno, section_tokens] : tokens_to_use->get_map()) {
      for (const auto& token : section_tokens) {
        auto pos = token.position();

        // Skip tokens after cursor
        if (pos.lineno > target_line
            || (pos.lineno == target_line && pos.colno >= target_col)) {
          break;
        }

        auto token_str = token.to_string();

        // Track parentheses depth
        if (token_str == "(") {
          paren_depth++;
          if (paren_depth == 1 && !found_open_paren) {
            // This is the opening paren of the function call we're in
            found_open_paren = true;
            active_param = 0;
          }
        } else if (token_str == ")") {
          paren_depth--;
          if (paren_depth == 0) {
            // We exited the function call, reset
            found_open_paren = false;
            function_name_token = nullptr;
            active_param = 0;
          }
        } else if (token_str == "," && paren_depth == 1 && found_open_paren) {
          // Count commas at depth 1 to track active parameter
          active_param++;
        } else if (paren_depth == 0
                   && token.type() == cpp2::lexeme::Identifier) {
          // Potential function name before '('
          function_name_token = &token;
        }
      }
    }

    // If we're not inside a function call, no signature help
    if (!found_open_paren || !function_name_token || paren_depth != 1) {
      return std::nullopt;
    }

    std::string func_name = function_name_token->to_string();

    // Look up the function in sema
    auto decl_info = sema_to_use->get_declaration_of(function_name_token, true);
    if (!decl_info || !decl_info->declaration
        || !decl_info->declaration->is_function()) {
      // Try to find function in sema symbols by name
      if (sema_to_use) {
        for (size_t i = 0; i < sema_to_use->symbols.size(); ++i) {
          const auto& sym = sema_to_use->symbols[i];
          if (sym.is_declaration()) {
            const auto& decl_sym = std::get<cpp2::declaration_sym>(sym.sym);
            if (decl_sym.declaration && decl_sym.declaration->has_name()
                && decl_sym.declaration->name()->to_string() == func_name
                && decl_sym.declaration->is_function()) {
              SignatureHelpInfo help;
              SignatureInfo sig;
              sig.label = decl_sym.declaration->signature_to_string();
              sig.active_parameter = active_param;
              help.signatures.push_back(std::move(sig));
              help.active_signature = 0;
              return help;
            }
          }
        }
      }

      // Try index for cross-file functions
      if (index) {
        auto symbols = index->lookup(function_name_token->to_string());
        if (!symbols.empty() && symbols[0]->kind == SymbolKind::Function) {
          SignatureHelpInfo help;
          SignatureInfo sig;
          sig.label = symbols[0]->signature.empty() ? symbols[0]->name
                                                    : symbols[0]->signature;
          sig.active_parameter = active_param;
          // TODO: Parse parameters from signature
          help.signatures.push_back(std::move(sig));
          help.active_signature = 0;
          return help;
        }
      }
      return std::nullopt;
    }

    // Build signature help from declaration
    SignatureHelpInfo help;
    SignatureInfo sig;

    // Get full function signature
    sig.label = decl_info->declaration->signature_to_string();
    sig.active_parameter = active_param;

    // For now, we don't parse individual parameters from the signature
    // The signature string contains all the info, editors will display it
    // TODO: Parse signature_to_string to extract individual parameters

    help.signatures.push_back(std::move(sig));
    help.active_signature = 0;

    return help;
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

  auto Cpp2Document::find_identifier_token_before(const std::string& name,
                                                  int line, int col) const
      -> const cpp2::token* {
    // Use cached tokens if current is null
    const cpp2::tokens* tokens_to_use
        = m_tokens ? m_tokens.get() : m_cached_tokens.get();

    if (!tokens_to_use) {
      return nullptr;
    }

    const cpp2::token* best_match = nullptr;

    // Search backwards from the cursor position
    for (const auto& [lineno, section_tokens] : tokens_to_use->get_map()) {
      // Skip lines after the cursor
      if (lineno > line) {
        continue;
      }

      for (const auto& token : section_tokens) {
        auto pos = token.position();

        // Skip tokens at or after cursor position
        if (lineno == line && pos.colno >= col) {
          continue;
        }

        // Check if this is an identifier token with matching name
        if (token.type() == cpp2::lexeme::Identifier
            && token.to_string() == name) {
          // Update best match if this is closer to cursor
          if (!best_match || pos.lineno > best_match->position().lineno
              || (pos.lineno == best_match->position().lineno
                  && pos.colno > best_match->position().colno)) {
            best_match = &token;
          }
        }
      }
    }

    return best_match;
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
