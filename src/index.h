#ifndef CPP2LS_INDEX_H
#define CPP2LS_INDEX_H

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace cpp2ls {

  /// Symbol kind for indexed symbols
  enum class SymbolKind { Function, Type, Namespace, Variable, Alias };

  /// Indexed symbol information
  struct IndexedSymbol {
    std::string name;  // Symbol name
    SymbolKind kind{SymbolKind::Function};
    std::string signature;  // For functions: parameter list
    std::string file_uri;   // URI of the file containing the symbol
    int line{0};            // 0-based line number
    int column{0};          // 0-based column number
  };

  /// Index data for a single file
  struct FileIndex {
    std::string uri;                        // File URI
    std::filesystem::file_time_type mtime;  // Last modification time
    std::vector<IndexedSymbol> symbols;     // Symbols defined in this file
  };

  /// Project-wide index for cross-file symbol resolution
  class ProjectIndex {
  public:
    ProjectIndex() = default;

    /// Set the workspace root directory
    void set_workspace_root(const std::filesystem::path& root);

    /// Get the workspace root
    auto workspace_root() const -> const std::filesystem::path&;

    /// Get the cache directory path (.cache/cpp2ls)
    auto cache_dir() const -> std::filesystem::path;

    /// Get the index file path
    auto index_file_path() const -> std::filesystem::path;

    /// Scan the workspace for cpp2 files and build/update the index
    /// Returns true if any files were indexed
    bool scan_and_index();

    /// Load index from cache file
    /// Returns true if successfully loaded
    bool load_from_cache();

    /// Save index to cache file
    bool save_to_cache() const;

    /// Look up a symbol by name
    /// Returns all matching symbols across all files
    auto lookup(const std::string& name) const
        -> std::vector<const IndexedSymbol*>;

    /// Look up a function by name
    /// Returns the first matching function (for go-to-definition)
    auto lookup_function(const std::string& name) const
        -> std::optional<IndexedSymbol>;

    /// Get all symbols (for completion)
    auto all_symbols() const -> std::vector<const IndexedSymbol*>;

    /// Update index for a single file
    /// Called when a file is modified
    void update_file(const std::string& uri,
                     const std::vector<IndexedSymbol>& symbols);

    /// Remove a file from the index
    void remove_file(const std::string& uri);

    /// Check if a file needs re-indexing based on modification time
    bool needs_reindex(const std::string& uri) const;

    /// Mark the index as dirty (needs saving)
    void mark_dirty();

    /// Check if index has unsaved changes
    bool is_dirty() const;

  private:
    /// Scan a directory recursively for .cpp2 files
    auto find_cpp2_files(const std::filesystem::path& dir) const
        -> std::vector<std::filesystem::path>;

    /// Index a single file
    auto index_file(const std::filesystem::path& path)
        -> std::optional<FileIndex>;

    /// Convert file path to URI
    static auto path_to_uri(const std::filesystem::path& path) -> std::string;

    /// Convert URI to file path
    static auto uri_to_path(const std::string& uri) -> std::filesystem::path;

    std::filesystem::path m_workspace_root;
    std::unordered_map<std::string, FileIndex>
        m_file_indices;  // URI -> FileIndex
    std::unordered_multimap<std::string, const IndexedSymbol*>
        m_symbol_map;  // name -> symbol
    bool m_dirty{false};
  };

}  // namespace cpp2ls

#endif  // !CPP2LS_INDEX_H
