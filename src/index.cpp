#include "index.h"

#include <fstream>
#include <iostream>
#include <sstream>

// NOTE: We do NOT include cppfront headers here to avoid duplicate symbol
// errors. Instead, we use Document to index files, which already includes
// cppfront.

// Use nlohmann json for serialization
#include "document.h"
#include "nlohmann/json.hpp"

namespace cpp2ls {

  namespace {
    // JSON serialization helpers
    constexpr const char* kIndexVersion = "1";
    constexpr const char* kCacheDir = ".cache/cpp2ls";
    constexpr const char* kIndexFile = "index.json";

    auto symbol_kind_to_string(SymbolKind kind) -> std::string {
      switch (kind) {
        case SymbolKind::Function:
          return "function";
        case SymbolKind::Type:
          return "type";
        case SymbolKind::Namespace:
          return "namespace";
        case SymbolKind::Variable:
          return "variable";
        case SymbolKind::Alias:
          return "alias";
      }
      return "unknown";
    }

    auto string_to_symbol_kind(const std::string& str) -> SymbolKind {
      if (str == "function") return SymbolKind::Function;
      if (str == "type") return SymbolKind::Type;
      if (str == "namespace") return SymbolKind::Namespace;
      if (str == "variable") return SymbolKind::Variable;
      if (str == "alias") return SymbolKind::Alias;
      return SymbolKind::Function;
    }

    // Convert file_time_type to integer for JSON serialization
    // Use nanoseconds for higher precision
    auto time_to_int(std::filesystem::file_time_type time) -> int64_t {
      auto duration = time.time_since_epoch();
      return std::chrono::duration_cast<std::chrono::nanoseconds>(duration)
          .count();
    }

    auto int_to_time(int64_t ns) -> std::filesystem::file_time_type {
      auto duration = std::chrono::nanoseconds(ns);
      return std::filesystem::file_time_type(
          std::chrono::duration_cast<std::filesystem::file_time_type::duration>(
              duration));
    }
  }  // namespace

  void ProjectIndex::set_workspace_root(const std::filesystem::path& root) {
    m_workspace_root = root;
  }

  auto ProjectIndex::workspace_root() const -> const std::filesystem::path& {
    return m_workspace_root;
  }

  auto ProjectIndex::cache_dir() const -> std::filesystem::path {
    return m_workspace_root / kCacheDir;
  }

  auto ProjectIndex::index_file_path() const -> std::filesystem::path {
    return cache_dir() / kIndexFile;
  }

  auto ProjectIndex::find_cpp2_files(const std::filesystem::path& dir) const
      -> std::vector<std::filesystem::path> {
    std::vector<std::filesystem::path> files;

    if (!std::filesystem::exists(dir)) {
      return files;
    }

    try {
      for (const auto& entry : std::filesystem::recursive_directory_iterator(
               dir,
               std::filesystem::directory_options::skip_permission_denied)) {
        if (!entry.is_regular_file()) {
          continue;
        }

        auto path = entry.path();
        auto ext = path.extension().string();

        // Skip files in hidden directories (like .cache, .git, etc.)
        bool in_hidden_dir = false;
        for (const auto& part : path) {
          auto part_str = part.string();
          if (!part_str.empty() && part_str[0] == '.') {
            in_hidden_dir = true;
            break;
          }
        }
        if (in_hidden_dir) {
          continue;
        }

        if (ext == ".cpp2" || ext == ".h2") {
          files.push_back(path);
        }
      }
    } catch (const std::filesystem::filesystem_error& e) {
      std::cerr << "Error scanning directory: " << e.what() << "\n";
    }

    return files;
  }

  auto ProjectIndex::path_to_uri(const std::filesystem::path& path)
      -> std::string {
    auto abs_path = std::filesystem::absolute(path);
    return "file://" + abs_path.string();
  }

  auto ProjectIndex::uri_to_path(const std::string& uri)
      -> std::filesystem::path {
    // Remove "file://" prefix
    if (uri.starts_with("file://")) {
      return std::filesystem::path(uri.substr(7));
    }
    return std::filesystem::path(uri);
  }

  auto ProjectIndex::index_file(const std::filesystem::path& path)
      -> std::optional<FileIndex> {
    std::cerr << "Indexing: " << path << "\n";

    // Read file content
    std::ifstream file(path);
    if (!file) {
      std::cerr << "  Failed to open file\n";
      return std::nullopt;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();

    // Get modification time
    std::filesystem::file_time_type mtime;
    try {
      mtime = std::filesystem::last_write_time(path);
    } catch (...) {
      mtime = std::filesystem::file_time_type::clock::now();
    }

    // Create file index
    FileIndex file_index;
    file_index.uri = path_to_uri(path);
    file_index.mtime = mtime;

    // Use Cpp2Document to parse and extract symbols
    // This reuses the existing parsing infrastructure
    Cpp2Document doc(file_index.uri);
    doc.update(content);

    // Extract symbols from the document's function declarations map
    // For now, we'll add a method to Cpp2Document to export indexed symbols
    auto symbols = doc.get_indexed_symbols();
    for (auto& sym : symbols) {
      sym.file_uri = file_index.uri;
      file_index.symbols.push_back(std::move(sym));
    }

    std::cerr << "  Found " << file_index.symbols.size() << " symbols\n";
    return file_index;
  }

  bool ProjectIndex::scan_and_index() {
    if (m_workspace_root.empty()) {
      return false;
    }

    std::cerr << "Scanning workspace: " << m_workspace_root << "\n";

    auto files = find_cpp2_files(m_workspace_root);
    std::cerr << "Found " << files.size() << " cpp2 files\n";

    bool any_indexed = false;

    for (const auto& path : files) {
      auto uri = path_to_uri(path);

      // Check if we need to re-index this file
      auto existing = m_file_indices.find(uri);
      if (existing != m_file_indices.end()) {
        try {
          auto current_mtime = std::filesystem::last_write_time(path);
          if (current_mtime <= existing->second.mtime) {
            continue;  // File hasn't changed
          }
        } catch (...) {
          // If we can't get mtime, re-index
        }
      }

      // Index the file
      auto file_index = index_file(path);
      if (file_index) {
        m_file_indices[uri] = std::move(*file_index);
        any_indexed = true;
      }
    }

    // Rebuild symbol map
    if (any_indexed) {
      m_symbol_map.clear();
      for (const auto& [uri, file_index] : m_file_indices) {
        for (const auto& sym : file_index.symbols) {
          m_symbol_map.emplace(sym.name, &sym);
        }
      }
      m_dirty = true;
    }

    return any_indexed;
  }

  bool ProjectIndex::load_from_cache() {
    auto path = index_file_path();
    if (!std::filesystem::exists(path)) {
      return false;
    }

    std::cerr << "Loading index from cache: " << path << "\n";

    std::ifstream file(path);
    if (!file) {
      return false;
    }

    try {
      nlohmann::json j;
      file >> j;

      // Check version
      if (j["version"] != kIndexVersion) {
        std::cerr << "Index version mismatch, rebuilding\n";
        return false;
      }

      // Load file indices
      m_file_indices.clear();
      for (const auto& file_json : j["files"]) {
        FileIndex file_index;
        file_index.uri = file_json["uri"];
        file_index.mtime = int_to_time(file_json["mtime"]);

        for (const auto& sym_json : file_json["symbols"]) {
          IndexedSymbol sym;
          sym.name = sym_json["name"];
          sym.kind = string_to_symbol_kind(sym_json["kind"]);
          sym.signature = sym_json.value("signature", "");
          sym.file_uri = file_index.uri;
          sym.line = sym_json["line"];
          sym.column = sym_json["column"];
          file_index.symbols.push_back(std::move(sym));
        }

        m_file_indices[file_index.uri] = std::move(file_index);
      }

      // Rebuild symbol map
      m_symbol_map.clear();
      for (const auto& [uri, file_index] : m_file_indices) {
        for (const auto& sym : file_index.symbols) {
          m_symbol_map.emplace(sym.name, &sym);
        }
      }

      std::cerr << "Loaded " << m_file_indices.size() << " files from cache\n";
      m_dirty = false;
      return true;

    } catch (const std::exception& e) {
      std::cerr << "Failed to load index: " << e.what() << "\n";
      return false;
    }
  }

  bool ProjectIndex::save_to_cache() const {
    if (!m_dirty) {
      return true;  // Nothing to save
    }

    auto dir = cache_dir();
    try {
      std::filesystem::create_directories(dir);
    } catch (const std::exception& e) {
      std::cerr << "Failed to create cache directory: " << e.what() << "\n";
      return false;
    }

    auto path = index_file_path();
    std::cerr << "Saving index to cache: " << path << "\n";

    try {
      nlohmann::json j;
      j["version"] = kIndexVersion;

      nlohmann::json files_json = nlohmann::json::array();
      for (const auto& [uri, file_index] : m_file_indices) {
        nlohmann::json file_json;
        file_json["uri"] = file_index.uri;
        file_json["mtime"] = time_to_int(file_index.mtime);

        nlohmann::json symbols_json = nlohmann::json::array();
        for (const auto& sym : file_index.symbols) {
          nlohmann::json sym_json;
          sym_json["name"] = sym.name;
          sym_json["kind"] = symbol_kind_to_string(sym.kind);
          if (!sym.signature.empty()) {
            sym_json["signature"] = sym.signature;
          }
          sym_json["line"] = sym.line;
          sym_json["column"] = sym.column;
          symbols_json.push_back(std::move(sym_json));
        }
        file_json["symbols"] = std::move(symbols_json);
        files_json.push_back(std::move(file_json));
      }
      j["files"] = std::move(files_json);

      std::ofstream file(path);
      if (!file) {
        return false;
      }
      file << j.dump(2);

      return true;

    } catch (const std::exception& e) {
      std::cerr << "Failed to save index: " << e.what() << "\n";
      return false;
    }
  }

  auto ProjectIndex::lookup(const std::string& name) const
      -> std::vector<const IndexedSymbol*> {
    std::vector<const IndexedSymbol*> result;
    auto range = m_symbol_map.equal_range(name);
    for (auto it = range.first; it != range.second; ++it) {
      result.push_back(it->second);
    }
    return result;
  }

  auto ProjectIndex::lookup_function(const std::string& name) const
      -> std::optional<IndexedSymbol> {
    auto range = m_symbol_map.equal_range(name);
    for (auto it = range.first; it != range.second; ++it) {
      if (it->second->kind == SymbolKind::Function) {
        return *it->second;
      }
    }
    return std::nullopt;
  }

  auto ProjectIndex::all_symbols() const -> std::vector<const IndexedSymbol*> {
    std::vector<const IndexedSymbol*> result;
    for (const auto& [uri, file_index] : m_file_indices) {
      for (const auto& sym : file_index.symbols) {
        result.push_back(&sym);
      }
    }
    return result;
  }

  void ProjectIndex::update_file(const std::string& uri,
                                 const std::vector<IndexedSymbol>& symbols) {
    // Remove old symbols from map
    auto old_it = m_file_indices.find(uri);
    if (old_it != m_file_indices.end()) {
      // We need to rebuild the symbol map without the old symbols
      // This is inefficient but simple - could be optimized later
    }

    // Update file index
    FileIndex file_index;
    file_index.uri = uri;
    file_index.mtime = std::filesystem::file_time_type::clock::now();

    // Copy symbols and set their file_uri
    file_index.symbols.reserve(symbols.size());
    for (const auto& sym : symbols) {
      IndexedSymbol sym_copy = sym;
      sym_copy.file_uri = uri;
      file_index.symbols.push_back(std::move(sym_copy));
    }

    m_file_indices[uri] = std::move(file_index);

    // Rebuild symbol map
    m_symbol_map.clear();
    for (const auto& [u, fi] : m_file_indices) {
      for (const auto& sym : fi.symbols) {
        m_symbol_map.emplace(sym.name, &sym);
      }
    }

    m_dirty = true;
  }

  void ProjectIndex::remove_file(const std::string& uri) {
    m_file_indices.erase(uri);

    // Rebuild symbol map
    m_symbol_map.clear();
    for (const auto& [u, fi] : m_file_indices) {
      for (const auto& sym : fi.symbols) {
        m_symbol_map.emplace(sym.name, &sym);
      }
    }

    m_dirty = true;
  }

  bool ProjectIndex::needs_reindex(const std::string& uri) const {
    auto it = m_file_indices.find(uri);
    if (it == m_file_indices.end()) {
      return true;  // Not indexed yet
    }

    auto path = uri_to_path(uri);
    try {
      auto current_mtime = std::filesystem::last_write_time(path);
      return current_mtime > it->second.mtime;
    } catch (...) {
      return true;  // If we can't check, assume we need to re-index
    }
  }

  void ProjectIndex::mark_dirty() { m_dirty = true; }

  bool ProjectIndex::is_dirty() const { return m_dirty; }

}  // namespace cpp2ls
