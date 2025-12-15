#ifndef CPP2LS_DOCUMENT_H
#define CPP2LS_DOCUMENT_H

#include <memory>
#include <optional>
#include <string>
#include <vector>

// Forward declarations from cppfront
namespace cpp2 {
  class source;
  class tokens;
  class parser;
  class sema;
  struct token;
  struct error_entry;
  struct declaration_sym;
}  // namespace cpp2

namespace cpp2ls {

  /// Information about a hover target
  struct HoverInfo {
    std::string contents;  // Markdown content for hover
    int start_line{0};     // 0-based
    int start_col{0};      // 0-based
    int end_line{0};       // 0-based
    int end_col{0};        // 0-based
  };

  /// Diagnostic information (decoupled from cpp2::error_entry)
  struct DiagnosticInfo {
    int line{0};    // 0-based line number
    int column{0};  // 0-based column number
    std::string message;
    bool is_internal{false};  // Internal compiler error
  };

  /// Location information for go-to-definition
  struct LocationInfo {
    int line{0};    // 0-based line number
    int column{0};  // 0-based column number
  };

  /// Manages parsing and semantic analysis for a single cpp2 document
  class Cpp2Document {
  public:
    explicit Cpp2Document(std::string uri);
    ~Cpp2Document();

    // Non-copyable, movable
    Cpp2Document(const Cpp2Document&) = delete;
    Cpp2Document& operator=(const Cpp2Document&) = delete;
    Cpp2Document(Cpp2Document&&) noexcept;
    Cpp2Document& operator=(Cpp2Document&&) noexcept;

    /// Update the document content and re-parse
    void update(const std::string& content);

    /// Get hover information at the given position (0-based line and column)
    auto get_hover_info(int line, int col) const -> std::optional<HoverInfo>;

    /// Get definition location at the given position (0-based line and column)
    auto get_definition_location(int line, int col) const
        -> std::optional<LocationInfo>;

    /// Get the document URI
    auto uri() const -> const std::string&;

    /// Check if parsing succeeded
    auto is_valid() const -> bool;

    /// Get diagnostics (converted from cpp2 errors)
    auto diagnostics() const -> std::vector<DiagnosticInfo>;

  private:
    /// Find the token at the given position (1-based line and column)
    auto find_token_at(int line, int col) const -> const cpp2::token*;

    /// Build hover content for a declaration
    auto build_hover_content(const cpp2::declaration_sym& sym) const
        -> std::string;

    std::string m_uri;
    std::string m_content;

    // Cppfront parsing state
    std::vector<cpp2::error_entry>* m_errors{nullptr};
    std::unique_ptr<cpp2::source> m_source;
    std::unique_ptr<cpp2::tokens> m_tokens;
    std::unique_ptr<cpp2::parser> m_parser;
    std::unique_ptr<cpp2::sema> m_sema;
    bool m_valid{false};
  };

}  // namespace cpp2ls

#endif  // !CPP2LS_DOCUMENT_H
