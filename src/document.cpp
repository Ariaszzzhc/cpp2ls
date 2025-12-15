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

  Cpp2Document::Cpp2Document(std::string uri) : m_uri{std::move(uri)} {}

  Cpp2Document::~Cpp2Document() = default;

  Cpp2Document::Cpp2Document(Cpp2Document&&) noexcept = default;
  Cpp2Document& Cpp2Document::operator=(Cpp2Document&&) noexcept = default;

  void Cpp2Document::update(const std::string& content) {
    m_content = content;
    m_valid = false;
    m_errors.clear();

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
        m_errors.emplace_back(cpp2::source_position{1, 1},
                              "Failed to create temporary file for parsing");
        return;
      }
      temp_file << content;
    }

    // Initialize cppfront components
    m_source = std::make_unique<cpp2::source>(m_errors);
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
    m_tokens = std::make_unique<cpp2::tokens>(m_errors);
    m_tokens->lex(m_source->get_lines());

    // Parse the tokens
    std::set<std::string> includes;
    m_parser = std::make_unique<cpp2::parser>(m_errors, includes);

    // Parse each section of cpp2 code
    for (const auto& [lineno, section_tokens] : m_tokens->get_map()) {
      if (!m_parser->parse(section_tokens, m_tokens->get_generated())) {
        // Parse error - continue to collect more errors
        continue;
      }
    }

    // Run semantic analysis
    m_sema = std::make_unique<cpp2::sema>(m_errors);
    m_parser->visit(*m_sema);
    m_sema->apply_local_rules();

    m_valid = m_errors.empty();
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

  auto Cpp2Document::errors() const -> const std::vector<cpp2::error_entry>& {
    return m_errors;
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

}  // namespace cpp2ls
