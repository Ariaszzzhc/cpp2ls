#ifndef CPP2LS_DOCUMENT_H
#define CPP2LS_DOCUMENT_H

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "index.h"

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
    std::string uri;  // File URI (empty means current file)
    int line{0};      // 0-based line number
    int column{0};    // 0-based column number
  };

  /// Kind of completion item
  enum class CompletionKind {
    Function,
    Variable,
    Parameter,
    Type,
    Namespace,
    Keyword
  };

  /// Completion item information
  struct CompletionInfo {
    std::string label;        // The text shown in the completion list
    std::string detail;       // Additional details (e.g., type signature)
    std::string insert_text;  // Text to insert (defaults to label)
    CompletionKind kind{CompletionKind::Variable};
  };

  /// Parameter information for signature help
  struct ParameterInfo {
    std::string label;  // Parameter name and type (e.g., "name: std::string")
    // Optional: documentation for this parameter
  };

  /// Signature information for a function/method
  struct SignatureInfo {
    std::string label;  // Full function signature
    std::vector<ParameterInfo> parameters;
    int active_parameter{
        0};  // Which parameter is currently being typed (0-based)
  };

  /// Signature help result
  struct SignatureHelpInfo {
    std::vector<SignatureInfo> signatures;
    int active_signature{0};  // Which signature to highlight (for overloads)
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
    /// Uses global index for cross-file symbol lookup
    auto get_hover_info(int line, int col, const ProjectIndex* index) const
        -> std::optional<HoverInfo>;

    /// Get definition location at the given position (0-based line and column)
    /// Uses global index for cross-file symbol lookup
    auto get_definition_location(int line, int col,
                                 const ProjectIndex* index) const
        -> std::optional<LocationInfo>;

    /// Get all references to the symbol at the given position (0-based)
    /// Uses global index for cross-file references
    /// If include_declaration is true, the declaration itself is included
    auto get_references(int line, int col, bool include_declaration,
                        const ProjectIndex* index) const
        -> std::vector<LocationInfo>;

    /// Get completion items at the given position (0-based line and column)
    /// Uses global index for cross-file symbol completion
    auto get_completions(int line, int col, const ProjectIndex* index) const
        -> std::vector<CompletionInfo>;

    /// Get signature help at the given position (0-based line and column)
    /// Shows function signature and parameter info when calling functions
    auto get_signature_help(int line, int col, const ProjectIndex* index) const
        -> std::optional<SignatureHelpInfo>;

    /// Get indexed symbols for this document (for project-wide indexing)
    auto get_indexed_symbols() const -> std::vector<IndexedSymbol>;

    /// Get the document URI
    auto uri() const -> const std::string&;

    /// Check if parsing succeeded
    auto is_valid() const -> bool;

    /// Get diagnostics (converted from cpp2 errors)
    auto diagnostics() const -> std::vector<DiagnosticInfo>;

  private:
    /// Find the token at the given position (1-based line and column)
    auto find_token_at(int line, int col) const -> const cpp2::token*;

    /// Find the nearest identifier token with given name before the position
    /// (1-based line and column)
    auto find_identifier_token_before(const std::string& name, int line,
                                      int col) const -> const cpp2::token*;

    /// Build hover content for a declaration
    auto build_hover_content(const cpp2::declaration_sym& sym) const
        -> std::string;

    /// Build hover content for an indexed symbol
    auto build_hover_content(const IndexedSymbol& sym) const -> std::string;

    std::string m_uri;
    std::string m_content;

    // Cppfront parsing state
    std::vector<cpp2::error_entry>* m_errors{nullptr};
    std::unique_ptr<cpp2::source> m_source;
    std::unique_ptr<cpp2::tokens> m_tokens;
    std::unique_ptr<cpp2::parser> m_parser;
    std::unique_ptr<cpp2::sema> m_sema;
    bool m_valid{false};

    // Cached state from last successful parse (for completion during editing)
    std::unique_ptr<cpp2::source> m_cached_source;
    std::unique_ptr<cpp2::tokens> m_cached_tokens;
    std::unique_ptr<cpp2::parser> m_cached_parser;
    std::unique_ptr<cpp2::sema> m_cached_sema;
  };

}  // namespace cpp2ls

#endif  // !CPP2LS_DOCUMENT_H
