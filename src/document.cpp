#include "document.h"

#include <filesystem>
#include <fstream>
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
        m_valid{other.m_valid},
        m_function_declarations{std::move(other.m_function_declarations)},
        m_function_calls{std::move(other.m_function_calls)} {
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
      m_function_declarations = std::move(other.m_function_declarations);
      m_function_calls = std::move(other.m_function_calls);
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
    m_function_declarations.clear();
    m_function_calls.clear();

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

    // Build function maps for forward reference support
    build_function_maps();

    m_valid = m_errors->empty();
  }

  auto Cpp2Document::get_hover_info(int line, int col) const
      -> std::optional<HoverInfo> {
    if (!m_valid || !m_sema) {
      return std::nullopt;
    }

    // Convert from 0-based (LSP) to 1-based (cppfront)
    const auto* token = find_token_at(line + 1, col + 1);
    if (!token) {
      return std::nullopt;
    }

    // Try to get declaration info for this token
    const auto* decl_sym = m_sema->get_declaration_of(token, true);
    if (!decl_sym || !decl_sym->declaration) {
      return std::nullopt;
    }

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

  auto Cpp2Document::uri() const -> const std::string& { return m_uri; }

  auto Cpp2Document::is_valid() const -> bool { return m_valid; }

  auto Cpp2Document::get_definition_location(int line, int col) const
      -> std::optional<LocationInfo> {
    if (!m_valid || !m_sema) {
      return std::nullopt;
    }

    // Convert from 0-based (LSP) to 1-based (cppfront)
    const auto* token = find_token_at(line + 1, col + 1);
    if (!token) {
      return std::nullopt;
    }

    // Get declaration for this token
    const auto* decl_sym = m_sema->get_declaration_of(token, true);
    if (decl_sym && decl_sym->declaration) {
      // Get the position of the declaration
      auto pos = decl_sym->position();

      LocationInfo loc;
      loc.line = pos.lineno - 1;   // Convert to 0-based
      loc.column = pos.colno - 1;  // Convert to 0-based
      return loc;
    }

    // Fallback: check our custom function declarations map for forward
    // references
    auto name = token->to_string();
    auto it = m_function_declarations.find(name);
    if (it != m_function_declarations.end()) {
      return it->second;
    }

    return std::nullopt;
  }

  auto Cpp2Document::get_references(int line, int col,
                                    bool include_declaration) const
      -> std::vector<LocationInfo> {
    std::vector<LocationInfo> result;

    if (!m_valid || !m_sema) {
      return result;
    }

    // Convert from 0-based (LSP) to 1-based (cppfront)
    const auto* token = find_token_at(line + 1, col + 1);
    if (!token) {
      return result;
    }

    // Get declaration for this token
    const auto* target_decl = m_sema->get_declaration_of(token, true);

    // Determine the function name for our custom maps
    std::string func_name;
    bool use_custom_maps = false;

    if (target_decl && target_decl->declaration) {
      // We have a declaration - include it if requested
      if (include_declaration) {
        auto pos = target_decl->position();
        LocationInfo loc;
        loc.line = pos.lineno - 1;
        loc.column = pos.colno - 1;
        result.push_back(loc);
      }

      // Iterate through declaration_of map to find all references
      for (const auto& [tok, decl_info] : m_sema->declaration_of) {
        if (!tok || !decl_info.sym) {
          continue;
        }

        // Check if this token refers to the same declaration
        if (decl_info.sym == target_decl) {
          // Skip the declaration token itself (we already added it above)
          auto pos = tok->position();
          if (include_declaration
              && pos.lineno - 1 == target_decl->position().lineno - 1
              && pos.colno - 1 == target_decl->position().colno - 1) {
            continue;
          }

          LocationInfo loc;
          loc.line = pos.lineno - 1;
          loc.column = pos.colno - 1;
          result.push_back(loc);
        }
      }

      // Check if this is a function declaration - we may have additional
      // forward references in our custom map
      if (target_decl->declaration->is_function()
          && target_decl->declaration->has_name()) {
        func_name = target_decl->declaration->name()->to_string();
        use_custom_maps = true;
      }
    } else {
      // No declaration found via cppfront - try our custom maps
      func_name = token->to_string();
      auto it = m_function_declarations.find(func_name);
      if (it != m_function_declarations.end()) {
        use_custom_maps = true;
        // Include the declaration if requested
        if (include_declaration) {
          result.push_back(it->second);
        }
      }
    }

    // Add references from our custom function calls map
    if (use_custom_maps && !func_name.empty()) {
      auto range = m_function_calls.equal_range(func_name);
      for (auto it = range.first; it != range.second; ++it) {
        // Check if we already have this location
        bool already_present = false;
        for (const auto& existing : result) {
          if (existing.line == it->second.line
              && existing.column == it->second.column) {
            already_present = true;
            break;
          }
        }
        if (!already_present) {
          result.push_back(it->second);
        }
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
      // Skip fallback errors if there are better ones
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
    if (!m_tokens) {
      return nullptr;
    }

    // Search through all token sections
    for (const auto& [lineno, section_tokens] : m_tokens->get_map()) {
      for (const auto& token : section_tokens) {
        auto pos = token.position();

        // Check if position is within this token
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
      // Function declaration
      if (decl->name()) {
        oss << decl->name()->to_string();
      }
      oss << ": " << decl->signature_to_string();
    } else if (decl->is_object()) {
      // Object/variable declaration
      if (decl->name()) {
        oss << decl->name()->to_string();
      }
      oss << ": " << decl->object_type();
    } else if (decl->is_type()) {
      // Type declaration
      if (decl->name()) {
        oss << decl->name()->to_string();
      }
      oss << ": type";
    } else if (decl->is_namespace()) {
      // Namespace declaration
      if (decl->name()) {
        oss << decl->name()->to_string();
      }
      oss << ": namespace";
    } else if (decl->is_alias()) {
      // Alias declaration
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

    // Add parameter info if this is a parameter
    if (sym.parameter) {
      oss << "\n\n*(parameter)*";
    } else if (sym.member) {
      oss << "\n\n*(member)*";
    } else if (sym.return_param) {
      oss << "\n\n*(return value)*";
    }

    return oss.str();
  }

  void Cpp2Document::build_function_maps() {
    if (!m_parser || !m_tokens) {
      return;
    }

    // First pass: collect all top-level function declarations
    // Use get_parse_tree_declarations_in_range for each token section
    for (const auto& [lineno, section_tokens] : m_tokens->get_map()) {
      if (section_tokens.empty()) {
        continue;
      }

      auto declarations
          = m_parser->get_parse_tree_declarations_in_range(section_tokens);
      for (const auto* decl : declarations) {
        if (!decl || !decl->is_function() || !decl->has_name()) {
          continue;
        }

        auto name = decl->name()->to_string();
        auto pos = decl->position();

        LocationInfo loc;
        loc.line = pos.lineno - 1;   // Convert to 0-based
        loc.column = pos.colno - 1;  // Convert to 0-based
        m_function_declarations[name] = loc;
      }
    }

    // Second pass: scan tokens to find function call sites
    // A function call is an identifier followed by '('
    for (const auto& [lineno, section_tokens] : m_tokens->get_map()) {
      for (size_t i = 0; i + 1 < section_tokens.size(); ++i) {
        const auto& token = section_tokens[i];
        const auto& next_token = section_tokens[i + 1];

        // Check if this is an identifier followed by '('
        if (token.type() == cpp2::lexeme::Identifier
            && next_token.type() == cpp2::lexeme::LeftParen) {
          auto name = token.to_string();

          // Only record if this is a known function
          if (m_function_declarations.contains(name)) {
            auto pos = token.position();

            // Skip if this is the declaration site itself
            const auto& decl_loc = m_function_declarations[name];
            if (pos.lineno - 1 == decl_loc.line
                && pos.colno - 1 == decl_loc.column) {
              continue;
            }

            LocationInfo loc;
            loc.line = pos.lineno - 1;
            loc.column = pos.colno - 1;
            m_function_calls.emplace(name, loc);
          }
        }
      }
    }
  }

}  // namespace cpp2ls
